/* Container front-ends: NUP, extracted folders, NSP/XCI/NCA, single NSO/NRO. */
#include "mgr_internal.h"
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ======================= common game helpers ======================= */

void game_add_exefs(mgr_game *g, const char *name,
                    int (*read)(mgr_game *, int, uint8_t **, size_t *), int idx) {
    if (g->exefs_n >= 16) return;
    exefs_entry *e = &g->exefs[g->exefs_n++];
    mgr_strlcpy(e->name, name, sizeof e->name);
    e->read = read;
    e->idx = idx;
}

void game_set_platform_from_tid(mgr_game_info *info) {
    uint8_t hi = (uint8_t)(info->title_id >> 56);
    if (hi == 0x01) info->platform = MGR_PLATFORM_SWITCH1;
    else if (hi == 0x04) info->platform = MGR_PLATFORM_SWITCH2;
}

static int has_exefs(const mgr_game *g, const char *n) {
    for (int i = 0; i < g->exefs_n; i++) if (strcmp(g->exefs[i].name, n) == 0) return 1;
    return 0;
}

void game_finish_info(mgr_game *g) {
    mgr_game_info *in = &g->info;
    in->exefs_count = g->exefs_n;
    for (int i = 0; i < g->exefs_n; i++) mgr_strlcpy(in->exefs_names[i], g->exefs[i].name, 32);
    if (in->format == MGR_FMT_NRO || in->format == MGR_FMT_NSO) in->bootable = g->raw != NULL;
    else in->bootable = has_exefs(g, "main") && has_exefs(g, "main.npdm");
    in->has_icon = g->icon != NULL;
    if (in->platform == MGR_PLATFORM_UNKNOWN) game_set_platform_from_tid(in);
    static const char *names[] = { "Unknown", "NUP", "Folder", "NSP", "XCI", "NCA", "NSO", "NRO", "NSZ", "XCZ" };
    mgr_strlcpy(in->format_name, names[in->format], sizeof in->format_name);
}

void name_from_path(mgr_game_info *in, const char *path) {
    if (in->name[0]) return;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    char tmp[256];
    mgr_strlcpy(tmp, b, sizeof tmp);
    size_t l = strlen(tmp);
    while (l && tmp[l - 1] == '/') tmp[--l] = 0;
    char *dot = strrchr(tmp, '.');
    if (dot && dot != tmp && strlen(dot) <= 5) *dot = 0;
    char *br = strchr(tmp, '[');           /* "Game [0100...][v0]" -> "Game" */
    if (br && br != tmp) { *br = 0; l = strlen(tmp); while (l && tmp[l - 1] == ' ') tmp[--l] = 0; }
    mgr_strlcpy(in->name, tmp, sizeof in->name);
}

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

/* ============================ NUP ============================ */

typedef struct { uint32_t name_off, name_len, first, count; } nup_dir;
typedef struct { uint32_t name_off, name_len; uint64_t size, eoff, esz; uint8_t codec; } nup_file;
typedef struct { uint64_t tid; uint32_t root, exefs, romfs, version; uint8_t kind, flags; uint64_t meta_off; uint32_t meta_size; } nup_pkg;

typedef struct {
    int fd;
    uint8_t *toc; uint64_t toc_size;
    uint32_t pkg_n; uint64_t dir_n, file_n;
    nup_pkg *pkgs; nup_dir *dirs; nup_file *files; uint32_t *children; uint64_t child_n;
    const uint8_t *pool; uint64_t pool_size;
    int legacy_tlv;
    int base, upd;          /* package indices or -1 */
    uint32_t exefs_dir;     /* chosen ExeFS dir */
    uint32_t exefs_files[16];
} nup_impl;

static void nup_destroy(mgr_game *g) {
    nup_impl *n = g->impl;
    if (!n) return;
    if (n->fd >= 0) close(n->fd);
    free(n->toc); free(n->pkgs); free(n->dirs); free(n->files); free(n->children);
    free(n);
}

static int nup_read_file(nup_impl *n, uint32_t fi, uint8_t **out, size_t *len) {
    nup_file *f = &n->files[fi];
    if (f->size > (4ull << 30)) return -1;
    uint8_t *dst = malloc(f->size ? f->size : 1);
    if (!dst) return -1;
    if (f->size) {
        uint8_t *src = f->codec == 0 ? dst : malloc(f->esz);
        if (!src) { free(dst); return -1; }
        mgr_src s = { .fd = n->fd, .base = 0, .size = UINT64_MAX };
        if (src_read(&s, f->eoff, src, f->esz)) { if (src != dst) free(src); free(dst); return -1; }
        if (f->codec == 1) {
            int r = zstd_decompress_frame(src, f->esz, dst, f->size);
            free(src);
            if (r) { free(dst); return -1; }
        } else if (f->codec != 0 || f->esz != f->size) { free(dst); return -1; }
    }
    *out = dst; *len = (size_t)f->size;
    return 0;
}

static int nup_child(const nup_impl *n, uint32_t dir, const char *name, size_t nl, int *is_dir_out, uint32_t *idx) {
    const nup_dir *d = &n->dirs[dir];
    uint64_t lo = 0, hi = d->count;
    while (lo < hi) {   /* children are sorted bytewise */
        uint64_t mid = (lo + hi) / 2;
        uint32_t cid = n->children[d->first + mid];
        uint32_t off, ln;
        if (cid < n->dir_n) { off = n->dirs[cid].name_off; ln = n->dirs[cid].name_len; }
        else { off = n->files[cid - n->dir_n].name_off; ln = n->files[cid - n->dir_n].name_len; }
        size_t m = ln < nl ? ln : nl;
        int c = memcmp(n->pool + off, name, m);
        if (c == 0) c = (ln > nl) - (ln < nl);
        if (c < 0) lo = mid + 1; else if (c > 0) hi = mid;
        else {
            *is_dir_out = cid < n->dir_n;
            *idx = cid < n->dir_n ? cid : (uint32_t)(cid - n->dir_n);
            return 0;
        }
    }
    return -1;
}

static int nup_lookup(const nup_impl *n, uint32_t root, const char *path, uint32_t *file_idx) {
    uint32_t cur = root;
    const char *p = path;
    for (;;) {
        while (*p == '/') p++;
        const char *slash = strchr(p, '/');
        size_t l = slash ? (size_t)(slash - p) : strlen(p);
        int isd; uint32_t idx;
        if (nup_child(n, cur, p, l, &isd, &idx)) return -1;
        if (!slash) { if (isd) return -1; *file_idx = idx; return 0; }
        if (!isd) return -1;
        cur = idx;
        p = slash + 1;
    }
}

static int nup_exefs_read(mgr_game *g, int i, uint8_t **out, size_t *len) {
    nup_impl *n = g->impl;
    return nup_read_file(n, n->exefs_files[i], out, len);
}

static int nup_romfs_read(mgr_game *g, const char *path, uint8_t **out, size_t *len) {
    nup_impl *n = g->impl;
    int order[2] = { n->upd, n->base };
    for (int k = 0; k < 2; k++) {
        int pi = order[k];
        if (pi < 0 || n->pkgs[pi].romfs == 0xFFFFFFFFu) continue;
        uint32_t fi;
        if (nup_lookup(n, n->pkgs[pi].romfs, path, &fi) == 0) return nup_read_file(n, fi, out, len);
    }
    return -1;
}

static void nup_walk(const nup_impl *n, uint32_t dir, char *path, size_t plen, int depth,
                     void (*cb)(void *, const char *, uint64_t), void *ctx) {
    if (depth > 64) return;
    const nup_dir *d = &n->dirs[dir];
    for (uint32_t i = 0; i < d->count; i++) {
        uint32_t cid = n->children[d->first + i];
        uint32_t off, ln;
        if (cid < n->dir_n) { off = n->dirs[cid].name_off; ln = n->dirs[cid].name_len; }
        else { off = n->files[cid - n->dir_n].name_off; ln = n->files[cid - n->dir_n].name_len; }
        if (plen + ln + 2 >= 1024) continue;
        size_t np = plen;
        if (np) path[np++] = '/';
        memcpy(path + np, n->pool + off, ln);
        path[np + ln] = 0;
        if (cid < n->dir_n) nup_walk(n, cid, path, np + ln, depth + 1, cb, ctx);
        else cb(ctx, path, n->files[cid - n->dir_n].size);
        path[plen] = 0;
    }
}

static void nup_romfs_list(mgr_game *g, void (*cb)(void *, const char *, uint64_t), void *ctx) {
    nup_impl *n = g->impl;
    char path[1024];
    int order[2] = { n->upd, n->base };
    for (int k = 0; k < 2; k++) {
        int pi = order[k];
        if (pi < 0 || n->pkgs[pi].romfs == 0xFFFFFFFFu) continue;
        path[0] = 0;
        nup_walk(n, n->pkgs[pi].romfs, path, 0, 0, cb, ctx);
    }
}

static uint64_t nup_count_subtree(const nup_impl *n, uint32_t dir) {
    uint64_t total = 0;
    uint32_t *stack = malloc(sizeof(uint32_t) * (n->dir_n + 1));
    if (!stack) return 0;
    uint64_t sp = 0;
    stack[sp++] = dir;
    uint64_t guard = 0;
    while (sp && guard++ < n->dir_n * 2 + 8) {
        const nup_dir *d = &n->dirs[stack[--sp]];
        for (uint32_t i = 0; i < d->count; i++) {
            uint32_t cid = n->children[d->first + i];
            if (cid < n->dir_n) { if (sp < n->dir_n) stack[sp++] = cid; }
            else total++;
        }
    }
    free(stack);
    return total;
}

/* TLV lookup honouring both the spec numbering and the off-by-one numbering
   written by nup tool builds from Nov 2026 (detected by a type-0 record). */
static int nup_tlv(const nup_impl *n, int pkg, int spec_type, uint8_t **out, size_t *len) {
    if (pkg < 0) return -1;
    const nup_pkg *p = &n->pkgs[pkg];
    if (!p->meta_size || p->meta_size > (64u << 20)) return -1;
    uint8_t *blk = malloc(p->meta_size);
    if (!blk) return -1;
    mgr_src s = { .fd = n->fd, .base = 0, .size = UINT64_MAX };
    if (src_read(&s, p->meta_off, blk, p->meta_size)) { free(blk); return -1; }
    int want = n->legacy_tlv ? spec_type - 1 : spec_type;
    uint32_t pos = 0;
    while (pos + 8 <= p->meta_size) {
        uint16_t t = rd16(blk + pos);
        uint32_t sz = rd32(blk + pos + 4);
        if ((uint64_t)pos + 8 + sz > p->meta_size) break;
        if (t == want) {
            uint8_t *o = malloc(sz + 1);
            if (!o) break;
            memcpy(o, blk + pos + 8, sz);
            o[sz] = 0;
            *out = o; *len = sz;
            free(blk);
            return 0;
        }
        pos += 8 + sz;
    }
    free(blk);
    return -1;
}

static int nup_detect_legacy(const nup_impl *n) {
    mgr_src s = { .fd = n->fd, .base = 0, .size = UINT64_MAX };
    for (uint32_t i = 0; i < n->pkg_n; i++) {
        const nup_pkg *p = &n->pkgs[i];
        if (!p->meta_size || p->meta_size > (64u << 20)) continue;
        uint8_t *blk = malloc(p->meta_size);
        if (!blk) continue;
        if (src_read(&s, p->meta_off, blk, p->meta_size) == 0) {
            uint32_t pos = 0;
            while (pos + 8 <= p->meta_size) {
                uint16_t t = rd16(blk + pos);
                uint32_t sz = rd32(blk + pos + 4);
                if (t == 0) { free(blk); return 1; }
                if ((uint64_t)pos + 8 + sz > p->meta_size) break;
                pos += 8 + sz;
            }
        }
        free(blk);
    }
    return 0;
}

int fmt_nup_open(mgr_game *g, const char *path, char *err, size_t errlen) {
    nup_impl *n = calloc(1, sizeof *n);
    if (!n) return -1;
    n->fd = open(path, O_RDONLY);
    g->impl = n; g->destroy = nup_destroy;
    if (n->fd < 0) { mgr_seterr(err, errlen, "cannot open file"); return -1; }
    uint8_t sb[0x80];
    mgr_src s = { .fd = n->fd, .base = 0, .size = UINT64_MAX };
    if (src_read(&s, 0, sb, 0x80) || memcmp(sb, "NUP\x01", 4)) { mgr_seterr(err, errlen, "not a NUP v1 file"); return -1; }
    uint64_t toc_off = rd64(sb + 0x08);
    n->toc_size = rd64(sb + 0x10);
    if (n->toc_size < 0x30 || n->toc_size > (1ull << 31)) { mgr_seterr(err, errlen, "bad TOC size"); return -1; }
    n->toc = malloc(n->toc_size);
    if (!n->toc || src_read(&s, toc_off, n->toc, n->toc_size)) { mgr_seterr(err, errlen, "cannot read TOC"); return -1; }
    const uint8_t *t = n->toc;
    if (memcmp(t, "TOC\x01", 4)) { mgr_seterr(err, errlen, "bad TOC magic"); return -1; }
    n->pkg_n = rd32(t + 4); n->dir_n = rd64(t + 8); n->file_n = rd64(t + 0x10);
    uint64_t pool_off = rd64(t + 0x18); n->pool_size = rd64(t + 0x20);
    uint64_t need = 0x30 + (uint64_t)n->pkg_n * 0x40 + n->dir_n * 0x18 + n->file_n * 0x50;
    if (n->pkg_n == 0 || need > n->toc_size || pool_off < need || pool_off + n->pool_size > n->toc_size) {
        mgr_seterr(err, errlen, "TOC out of bounds"); return -1;
    }
    n->pool = t + pool_off;
    n->pkgs = calloc(n->pkg_n, sizeof *n->pkgs);
    n->dirs = calloc(n->dir_n ? n->dir_n : 1, sizeof *n->dirs);
    n->files = calloc(n->file_n ? n->file_n : 1, sizeof *n->files);
    n->child_n = (pool_off - need) / 4;
    n->children = calloc(n->child_n ? n->child_n : 1, 4);
    if (!n->pkgs || !n->dirs || !n->files || !n->children) { mgr_seterr(err, errlen, "out of memory"); return -1; }
    uint64_t pos = 0x30;
    for (uint32_t i = 0; i < n->pkg_n; i++, pos += 0x40) {
        nup_pkg *p = &n->pkgs[i];
        p->tid = rd64(t + pos); p->root = rd32(t + pos + 8); p->exefs = rd32(t + pos + 0xC);
        p->romfs = rd32(t + pos + 0x10); p->version = rd32(t + pos + 0x14);
        p->kind = t[pos + 0x18]; p->flags = t[pos + 0x19];
        p->meta_off = rd64(t + pos + 0x20); p->meta_size = rd32(t + pos + 0x28);
    }
    for (uint64_t i = 0; i < n->dir_n; i++, pos += 0x18) {
        nup_dir *d = &n->dirs[i];
        d->name_off = rd32(t + pos); d->name_len = rd32(t + pos + 4);
        d->first = rd32(t + pos + 8); d->count = rd32(t + pos + 0xC);
        if ((uint64_t)d->first + d->count > n->child_n || (uint64_t)d->name_off + d->name_len > n->pool_size) {
            mgr_seterr(err, errlen, "corrupt directory record"); return -1;
        }
    }
    for (uint64_t i = 0; i < n->file_n; i++, pos += 0x50) {
        nup_file *f = &n->files[i];
        f->name_off = rd32(t + pos); f->name_len = rd32(t + pos + 4);
        f->size = rd64(t + pos + 8); f->eoff = rd64(t + pos + 0x10); f->esz = rd64(t + pos + 0x18);
        f->codec = t[pos + 0x40];
        if ((uint64_t)f->name_off + f->name_len > n->pool_size) { mgr_seterr(err, errlen, "corrupt file record"); return -1; }
    }
    for (uint64_t i = 0; i < n->child_n; i++, pos += 4) {
        n->children[i] = rd32(t + pos);
        if (n->children[i] >= n->dir_n + n->file_n) { mgr_seterr(err, errlen, "corrupt child slot"); return -1; }
    }
    for (uint32_t i = 0; i < n->pkg_n; i++) {
        nup_pkg *p = &n->pkgs[i];
        if (p->root >= n->dir_n) { mgr_seterr(err, errlen, "corrupt package record"); return -1; }
        if (p->exefs != 0xFFFFFFFFu && p->exefs >= n->dir_n) p->exefs = 0xFFFFFFFFu;
        if (p->romfs != 0xFFFFFFFFu && p->romfs >= n->dir_n) p->romfs = 0xFFFFFFFFu;
    }

    n->base = n->upd = -1;
    for (uint32_t i = 0; i < n->pkg_n; i++) {
        if (n->pkgs[i].kind == 0 && n->base < 0) n->base = (int)i;
        if (n->pkgs[i].kind == 1 && (n->upd < 0 || n->pkgs[i].version > n->pkgs[n->upd].version)) n->upd = (int)i;
    }
    n->legacy_tlv = nup_detect_legacy(n);

    mgr_game_info *in = &g->info;
    int app = n->base >= 0 ? n->base : 0;
    in->title_id = n->pkgs[app].tid;
    in->package_count = (int)n->pkg_n;
    for (uint32_t i = 0; i < n->pkg_n; i++) if (n->pkgs[i].version > in->version && n->pkgs[i].kind <= 1) in->version = n->pkgs[i].version;

    uint8_t *v; size_t vl;
    int meta_pkg = app;
    if (nup_tlv(n, meta_pkg, 5, &v, &vl) == 0) { mgr_strlcpy(in->name, (char *)v, sizeof in->name); free(v); }
    if (nup_tlv(n, meta_pkg, 6, &v, &vl) == 0) { mgr_strlcpy(in->publisher, (char *)v, sizeof in->publisher); free(v); }
    if (nup_tlv(n, meta_pkg, 4, &v, &vl) == 0) { mgr_strlcpy(in->display_version, (char *)v, sizeof in->display_version); free(v); }
    if (nup_tlv(n, meta_pkg, 3, &v, &vl) == 0) { g->icon = v; g->icon_len = vl; }
    if (nup_tlv(n, meta_pkg, 2, &v, &vl) == 0) { nacp_apply(v, vl, in); free(v); }
    if (nup_tlv(n, meta_pkg, 7, &v, &vl) == 0) { if (vl == 32) { memcpy(in->build_id, v, 32); in->has_build_id = 1; } free(v); }

    /* ExeFS: an update's ExeFS replaces the base's entirely */
    int exe_pkg = -1;
    if (n->upd >= 0 && n->pkgs[n->upd].exefs != 0xFFFFFFFFu) { exe_pkg = n->upd; in->update_applied = 1; }
    else if (n->base >= 0 && n->pkgs[n->base].exefs != 0xFFFFFFFFu) exe_pkg = n->base;
    if (exe_pkg >= 0) {
        n->exefs_dir = n->pkgs[exe_pkg].exefs;
        const nup_dir *d = &n->dirs[n->exefs_dir];
        int k = 0;
        for (uint32_t i = 0; i < d->count && k < 16; i++) {
            uint32_t cid = n->children[d->first + i];
            if (cid < n->dir_n) continue;
            const nup_file *f = &n->files[cid - n->dir_n];
            char nm[32];
            size_t l = f->name_len < 31 ? f->name_len : 31;
            memcpy(nm, n->pool + f->name_off, l); nm[l] = 0;
            n->exefs_files[k] = (uint32_t)(cid - n->dir_n);
            game_add_exefs(g, nm, nup_exefs_read, k);
            k++;
        }
        if (exe_pkg != n->base && n->base >= 0 && !in->has_build_id &&
            nup_tlv(n, exe_pkg, 7, &v, &vl) == 0) { if (vl == 32) { memcpy(in->build_id, v, 32); in->has_build_id = 1; } free(v); }
    }
    int rom_pkg = n->base >= 0 && n->pkgs[n->base].romfs != 0xFFFFFFFFu ? n->base
                : (n->upd >= 0 && n->pkgs[n->upd].romfs != 0xFFFFFFFFu ? n->upd : -1);
    if (rom_pkg >= 0) in->romfs_file_count = nup_count_subtree(n, n->pkgs[rom_pkg].romfs);
    g->romfs_read = nup_romfs_read;
    g->romfs_list = nup_romfs_list;
    if (n->legacy_tlv) mgr_strlcpy(in->note, "Packed with a nup build that numbers metadata off by one; read in compatibility mode.", sizeof in->note);
    return 0;
}

/* ============================ Folder ============================ */

typedef struct {
    char exefs[1024];
    char romfs[2][1024];  /* [0] update overlay, [1] base */
    char exefs_names[16][32];
} folder_impl;

static void folder_destroy(mgr_game *g) { free(g->impl); }

static int folder_exefs_read(mgr_game *g, int i, uint8_t **out, size_t *len) {
    folder_impl *f = g->impl;
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", f->exefs, f->exefs_names[i]);
    return read_whole_file(p, out, len);
}

static int folder_romfs_read(mgr_game *g, const char *path, uint8_t **out, size_t *len) {
    folder_impl *f = g->impl;
    if (strstr(path, "..")) return -1;
    for (int k = 0; k < 2; k++) {
        if (!f->romfs[k][0]) continue;
        char p[2300];
        snprintf(p, sizeof p, "%s/%s", f->romfs[k], path);
        if (is_file(p)) return read_whole_file(p, out, len);
    }
    return -1;
}

static void folder_walk(const char *root, const char *rel, int depth, void (*cb)(void *, const char *, uint64_t), void *ctx) {
    if (depth > 64) return;
    char full[2048];
    snprintf(full, sizeof full, "%s%s%s", root, rel[0] ? "/" : "", rel);
    DIR *d = opendir(full);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        char r2[1536], p[2400];
        snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(p, sizeof p, "%s/%s", root, r2);
        struct stat st;
        if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) folder_walk(root, r2, depth + 1, cb, ctx);
        else if (S_ISREG(st.st_mode)) cb(ctx, r2, (uint64_t)st.st_size);
    }
    closedir(d);
}

static void folder_romfs_list(mgr_game *g, void (*cb)(void *, const char *, uint64_t), void *ctx) {
    folder_impl *f = g->impl;
    for (int k = 0; k < 2; k++) if (f->romfs[k][0]) folder_walk(f->romfs[k], "", 0, cb, ctx);
}

static uint64_t count_files(const char *dir, int depth) {
    if (depth > 64) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    uint64_t n = 0;
    struct dirent *e;
    char p[2048];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) n += count_files(p, depth + 1);
        else if (S_ISREG(st.st_mode)) n++;
    }
    closedir(d);
    return n;
}

static int dir_has_content(const char *d) {
    char p[1100];
    snprintf(p, sizeof p, "%s/exefs", d); if (is_dir(p)) return 1;
    snprintf(p, sizeof p, "%s/romfs", d); if (is_dir(p)) return 1;
    snprintf(p, sizeof p, "%s/main.npdm", d); if (is_file(p)) return 1;
    return 0;
}

static void exefs_dir_of(const char *d, char *out, size_t n) {
    char p[1100];
    snprintf(p, sizeof p, "%s/exefs", d);
    if (is_dir(p)) mgr_strlcpy(out, p, n);
    else { snprintf(p, sizeof p, "%s/main.npdm", d); if (is_file(p)) mgr_strlcpy(out, d, n); else out[0] = 0; }
}

int fmt_folder_open(mgr_game *g, const char *path, char *err, size_t errlen) {
    folder_impl *f = calloc(1, sizeof *f);
    if (!f) return -1;
    g->impl = f; g->destroy = folder_destroy; g->romfs_read = folder_romfs_read; g->romfs_list = folder_romfs_list;
    mgr_game_info *in = &g->info;
    char base[1100], tmp[1200];
    snprintf(base, sizeof base, "%s/base", path);
    if (!is_dir(base) || !dir_has_content(base)) mgr_strlcpy(base, path, sizeof base);
    if (!dir_has_content(base)) { mgr_seterr(err, errlen, "folder has no exefs/ or romfs/"); return -1; }

    /* newest updates/<decimal>/ */
    char upd[1100] = "";
    uint32_t best = 0;
    snprintf(tmp, sizeof tmp, "%s/updates", path);
    DIR *d = opendir(tmp);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            char *end;
            unsigned long v = strtoul(e->d_name, &end, 10);
            if (*end || end == e->d_name) continue;
            char cand[1300];
            snprintf(cand, sizeof cand, "%s/%s", tmp, e->d_name);
            if (dir_has_content(cand) && (!upd[0] || v > best)) { best = (uint32_t)v; mgr_strlcpy(upd, cand, sizeof upd); }
        }
        closedir(d);
    }
    in->package_count = 1 + (upd[0] ? 1 : 0);
    if (upd[0]) in->version = best;

    char ux[1100] = "", bx[1100] = "";
    if (upd[0]) exefs_dir_of(upd, ux, sizeof ux);
    exefs_dir_of(base, bx, sizeof bx);
    if (ux[0]) { mgr_strlcpy(f->exefs, ux, sizeof f->exefs); in->update_applied = 1; }
    else mgr_strlcpy(f->exefs, bx, sizeof f->exefs);
    if (upd[0]) { snprintf(tmp, sizeof tmp, "%s/romfs", upd); if (is_dir(tmp)) mgr_strlcpy(f->romfs[0], tmp, sizeof f->romfs[0]); }
    snprintf(tmp, sizeof tmp, "%s/romfs", base);
    if (is_dir(tmp)) { mgr_strlcpy(f->romfs[1], tmp, sizeof f->romfs[1]); in->romfs_file_count = count_files(tmp, 0); }

    if (f->exefs[0]) {
        static const char *order[] = { "rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
                                       "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk", "main.npdm" };
        int k = 0;
        for (size_t i = 0; i < sizeof order / sizeof *order && k < 16; i++) {
            snprintf(tmp, sizeof tmp, "%s/%s", f->exefs, order[i]);
            if (!is_file(tmp)) continue;
            mgr_strlcpy(f->exefs_names[k], order[i], 32);
            game_add_exefs(g, order[i], folder_exefs_read, k);
            k++;
        }
        uint8_t *b; size_t bl;
        snprintf(tmp, sizeof tmp, "%s/main.npdm", f->exefs);
        if (read_whole_file(tmp, &b, &bl) == 0) {
            mgr_npdm np;
            if (npdm_parse(b, bl, &np) == 0 && np.program_id) in->title_id = np.program_id;
            free(b);
        }
        snprintf(tmp, sizeof tmp, "%s/main", f->exefs);
        if (read_whole_file(tmp, &b, &bl) == 0) {
            if (bl >= 0x60 && memcmp(b, "NSO0", 4) == 0) { memcpy(in->build_id, b + 0x40, 32); in->has_build_id = 1; }
            free(b);
        }
    }
    /* optional control data next to the dump */
    static const char *nacps[] = { "control.nacp", "control/control.nacp" };
    for (int i = 0; i < 2; i++) {
        uint8_t *b; size_t bl;
        snprintf(tmp, sizeof tmp, "%s/%s", path, nacps[i]);
        if (read_whole_file(tmp, &b, &bl) == 0) { nacp_apply(b, bl, in); free(b); break; }
    }
    static const char *icons[] = { "icon.jpg", "icon.png", "icon_AmericanEnglish.dat", "control/icon_AmericanEnglish.dat" };
    for (int i = 0; i < 4 && !g->icon; i++) {
        snprintf(tmp, sizeof tmp, "%s/%s", path, icons[i]);
        read_whole_file(tmp, &g->icon, &g->icon_len);
    }
    if (in->title_id && (in->title_id & 0xFFF) == 0x800) in->title_id &= ~0xFFFull;
    name_from_path(in, path);
    return 0;
}

/* ======================= NSP / XCI / NCA ======================= */

typedef struct {
    int fd;
    pfs_img outer;      /* NSP PFS0 or XCI secure HFS0 */
    pfs_img root;       /* XCI root HFS0 */
    pfs_img exefs;      /* program NCA section 0 */
    romfs_img romfs;    /* program NCA section 1 */
    int have_exefs, have_romfs;
} nsp_impl;

static void nsp_destroy(mgr_game *g) {
    nsp_impl *n = g->impl;
    if (!n) return;
    pfs_free(&n->outer); pfs_free(&n->root); pfs_free(&n->exefs);
    if (n->have_romfs) romfs_free(&n->romfs);
    if (n->fd >= 0) close(n->fd);
    free(n);
}

static int nsp_exefs_read(mgr_game *g, int i, uint8_t **out, size_t *len) {
    nsp_impl *n = g->impl;
    return pfs_read_entry(&n->exefs, &n->exefs.e[i], out, len);
}

static int nsp_romfs_raw(mgr_game *g, uint64_t off, void *buf, size_t n) {
    nsp_impl *ni = g->impl;
    if (!ni->have_romfs) return -1;
    return src_read(&ni->romfs.src, off, buf, n);
}

static int nsp_romfs_read(mgr_game *g, const char *path, uint8_t **out, size_t *len) {
    nsp_impl *n = g->impl;
    if (!n->have_romfs) return -1;
    return romfs_read_path(&n->romfs, path, out, len);
}

/* Register title keys from tickets (*.tik) carried inside the container. */
void keys_add_raw(const char *name, const uint8_t *v, int len);
static void absorb_tickets(const pfs_img *p) {
    for (int i = 0; i < p->count; i++) {
        if (!mgr_ends_with_ci(p->e[i].name, ".tik")) continue;
        uint8_t *b; size_t l;
        if (pfs_read_entry(p, &p->e[i], &b, &l)) continue;
        if (l >= 4) {
            uint32_t sig = rd32(b);
            size_t data = 0;
            if (sig == 0x10000 || sig == 0x10003) data = 0x240;
            else if (sig == 0x10001 || sig == 0x10004) data = 0x140;
            else if (sig == 0x10002 || sig == 0x10005) data = 0x80;
            if (data && l >= data + 0x180) {
                const uint8_t *rid = b + data + 0x160;
                const uint8_t *tk = b + data + 0x40;
                char name[33];
                for (int k = 0; k < 16; k++) snprintf(name + k * 2, 3, "%02x", rid[k]);
                keys_add_raw(name, tk, 16);
            }
        }
        free(b);
    }
}

static int nsp_scan_ncas(mgr_game *g, nsp_impl *n, const pfs_img *part, int single_nca_fd, char *err, size_t errlen) {
    mgr_game_info *in = &g->info;
    int found_program = 0, nca_count = 0;
    char last_err[160] = "";
    int total = single_nca_fd >= 0 ? 1 : part->count;
    for (int i = 0; i < total; i++) {
        mgr_src s;
        if (single_nca_fd >= 0) {
            s = (mgr_src){ .fd = single_nca_fd, .base = 0, .size = UINT64_MAX };
        } else {
            const pfs_entry *e = &part->e[i];
            if (!mgr_ends_with_ci(e->name, ".nca") && !mgr_ends_with_ci(e->name, ".ncz")) continue;
            if (mgr_ends_with_ci(e->name, ".ncz")) { mgr_strlcpy(last_err, "contains compressed NCZ content (decompress with nsz first)", sizeof last_err); continue; }
            s = src_sub(&part->src, e->offset, e->size);
        }
        nca_count++;
        nca_info nc;
        if (nca_open(&nc, &s)) { mgr_strlcpy(last_err, nc.err, sizeof last_err); continue; }
        if (nc.err[0]) mgr_strlcpy(last_err, nc.err, sizeof last_err);
        if (nc.content_type == 0 && !found_program) {
            if (nc.sec[0].present && nc.sec[0].fs_type == 1) {
                mgr_src ps = src_sub(&nc.sec[0].src, nc.sec[0].data_off, nc.sec[0].data_size);
                if (pfs_open(&n->exefs, &ps) == 0) {
                    n->have_exefs = 1;
                    found_program = 1;
                    static const char *order[] = { "rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
                                                   "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk", "main.npdm" };
                    for (size_t k = 0; k < sizeof order / sizeof *order; k++)
                        for (int j = 0; j < n->exefs.count; j++)
                            if (strcmp(n->exefs.e[j].name, order[k]) == 0) game_add_exefs(g, order[k], nsp_exefs_read, j);
                }
            }
            if (nc.sec[1].present && nc.sec[1].fs_type == 0 && nc.sec[1].enc_type != 4) {
                mgr_src rs = src_sub(&nc.sec[1].src, nc.sec[1].data_off, nc.sec[1].data_size);
                if (romfs_open(&n->romfs, &rs) == 0) {
                    n->have_romfs = 1; in->romfs_file_count = romfs_count_files(&n->romfs);
                    g->romfs_raw_read = nsp_romfs_raw; g->romfs_raw_size = nc.sec[1].data_size;
                }
            } else if (nc.sec[1].present && nc.sec[1].enc_type == 4) {
                mgr_strlcpy(in->note, "Update RomFS (BKTR patch) needs the base game; only the update ExeFS is used.", sizeof in->note);
            }
            if (!in->title_id) in->title_id = nc.title_id;
            if (!in->has_build_id) {
                uint8_t *m; size_t ml;
                const pfs_entry *me = n->have_exefs ? pfs_find(&n->exefs, "main") : NULL;
                if (me && pfs_read_entry(&n->exefs, me, &m, &ml) == 0) {
                    if (ml >= 0x60) { memcpy(in->build_id, m + 0x40, 32); in->has_build_id = 1; }
                    free(m);
                }
            }
        } else if (nc.content_type == 2 && nc.sec[0].present && nc.sec[0].fs_type == 0) {
            romfs_img cr;
            mgr_src rs = src_sub(&nc.sec[0].src, nc.sec[0].data_off, nc.sec[0].data_size);
            if (romfs_open(&cr, &rs) == 0) {
                uint8_t *b; size_t bl;
                if (romfs_read_path(&cr, "control.nacp", &b, &bl) == 0) { nacp_apply(b, bl, in); free(b); }
                static const char *langs[] = { "AmericanEnglish", "BritishEnglish", "Japanese", "French", "German",
                                               "LatinAmericanSpanish", "Spanish", "Italian", "Dutch", "CanadianFrench",
                                               "Portuguese", "Russian", "Korean", "TraditionalChinese", "SimplifiedChinese",
                                               "BrazilianPortuguese" };
                for (size_t k = 0; k < 16 && !g->icon; k++) {
                    char p[64]; snprintf(p, sizeof p, "icon_%s.dat", langs[k]);
                    romfs_read_path(&cr, p, &g->icon, &g->icon_len);
                }
                romfs_free(&cr);
            }
        } else if (nc.content_type == 1 && nc.sec[0].present && nc.sec[0].fs_type == 1) {
            pfs_img mp;
            mgr_src ps = src_sub(&nc.sec[0].src, nc.sec[0].data_off, nc.sec[0].data_size);
            if (pfs_open(&mp, &ps) == 0) {
                for (int k = 0; k < mp.count; k++) {
                    if (!mgr_ends_with_ci(mp.e[k].name, ".cnmt")) continue;
                    uint8_t hdr[0x10];
                    if (src_read(&mp.src, mp.e[k].offset, hdr, 0x10) == 0) {
                        uint32_t v = rd32(hdr + 8);
                        uint8_t type = hdr[0xC];
                        if (type == 0x80 || type == 0x81) { /* application / patch */
                            if (v > in->version) in->version = v;
                            if (type == 0x80 || !in->title_id) in->title_id = rd64(hdr) & ~0xFFFull;
                        }
                        in->package_count++;
                    }
                }
                pfs_free(&mp);
            }
        }
    }
    if (in->title_id && (in->title_id & 0xFFF) == 0x800) in->title_id &= ~0xFFFull;
    if (!found_program) {
        if (!mgr_keys_have_header_key())
            mgr_strlcpy(in->note, "Encrypted: add your prod.keys in Settings to read this game.", sizeof in->note);
        else if (last_err[0]) snprintf(in->note, sizeof in->note, "No playable program: %s", last_err);
        else if (!nca_count) mgr_strlcpy(in->note, "No NCA content found.", sizeof in->note);
        else mgr_strlcpy(in->note, "No program NCA (update/DLC only?).", sizeof in->note);
    }
    (void)err; (void)errlen;
    return 0;
}

int fmt_nsp_xci_open(mgr_game *g, const char *path, int is_xci, char *err, size_t errlen) {
    nsp_impl *n = calloc(1, sizeof *n);
    if (!n) return -1;
    g->impl = n; g->destroy = nsp_destroy; g->romfs_read = nsp_romfs_read;
    n->fd = open(path, O_RDONLY);
    if (n->fd < 0) { mgr_seterr(err, errlen, "cannot open file"); return -1; }
    mgr_src whole = { .fd = n->fd, .base = 0, .size = UINT64_MAX };
    if (is_xci) {
        uint8_t h[0x200];
        if (src_read(&whole, 0, h, 0x200) || memcmp(h + 0x100, "HEAD", 4)) { mgr_seterr(err, errlen, "not an XCI"); return -1; }
        uint64_t root_off = rd64(h + 0x130);
        mgr_src rs = src_sub(&whole, root_off, UINT64_MAX - root_off);
        if (pfs_open(&n->root, &rs)) { mgr_seterr(err, errlen, "XCI root partition unreadable"); return -1; }
        const pfs_entry *sec = pfs_find(&n->root, "secure");
        if (!sec) { mgr_seterr(err, errlen, "XCI has no secure partition"); return -1; }
        mgr_src ss = src_sub(&n->root.src, sec->offset, sec->size);
        if (pfs_open(&n->outer, &ss)) { mgr_seterr(err, errlen, "XCI secure partition unreadable"); return -1; }
    } else {
        if (pfs_open(&n->outer, &whole)) { mgr_seterr(err, errlen, "not an NSP (PFS0)"); return -1; }
        absorb_tickets(&n->outer);
    }
    g->info.package_count = 0;
    nsp_scan_ncas(g, n, &n->outer, -1, err, errlen);
    if (!g->info.package_count) g->info.package_count = 1;
    name_from_path(&g->info, path);
    return 0;
}

int fmt_nca_open(mgr_game *g, const char *path, char *err, size_t errlen) {
    nsp_impl *n = calloc(1, sizeof *n);
    if (!n) return -1;
    g->impl = n; g->destroy = nsp_destroy; g->romfs_read = nsp_romfs_read;
    n->fd = open(path, O_RDONLY);
    if (n->fd < 0) { mgr_seterr(err, errlen, "cannot open file"); return -1; }
    g->info.package_count = 1;
    nsp_scan_ncas(g, n, NULL, n->fd, err, errlen);
    name_from_path(&g->info, path);
    return 0;
}

/* ======================= single NSO / NRO ======================= */

static int raw_exefs_read(mgr_game *g, int i, uint8_t **out, size_t *len) {
    (void)i;
    uint8_t *b = malloc(g->raw_len);
    if (!b) return -1;
    memcpy(b, g->raw, g->raw_len);
    *out = b; *len = g->raw_len;
    return 0;
}

int fmt_nso_open(mgr_game *g, const char *path, char *err, size_t errlen) {
    if (read_whole_file(path, &g->raw, &g->raw_len) || g->raw_len < 0x100 || memcmp(g->raw, "NSO0", 4)) {
        mgr_seterr(err, errlen, "not an NSO"); return -1;
    }
    memcpy(g->info.build_id, g->raw + 0x40, 32);
    g->info.has_build_id = 1;
    g->info.package_count = 1;
    game_add_exefs(g, "main", raw_exefs_read, 0);
    name_from_path(&g->info, path);
    mgr_strlcpy(g->info.note, "Single NSO module: runs without an NPDM using default settings.", sizeof g->info.note);
    return 0;
}

int fmt_nro_open(mgr_game *g, const char *path, char *err, size_t errlen) {
    if (read_whole_file(path, &g->raw, &g->raw_len) || g->raw_len < 0x80 || memcmp(g->raw + 0x10, "NRO0", 4)) {
        mgr_seterr(err, errlen, "not an NRO"); return -1;
    }
    mgr_game_info *in = &g->info;
    in->platform = MGR_PLATFORM_HOMEBREW;
    in->package_count = 1;
    memcpy(in->build_id, g->raw + 0x40, 32);
    in->has_build_id = 1;
    uint32_t size = rd32(g->raw + 0x18);
    if ((uint64_t)size + 0x38 <= g->raw_len && memcmp(g->raw + size, "ASET", 4) == 0) {
        const uint8_t *a = g->raw + size;
        uint64_t io = rd64(a + 8), is = rd64(a + 0x10), no = rd64(a + 0x18), ns = rd64(a + 0x20);
        if (is && size + io + is <= g->raw_len) {
            g->icon = malloc(is);
            if (g->icon) { memcpy(g->icon, a + io, is); g->icon_len = is; }
        }
        if (ns && size + no + ns <= g->raw_len) nacp_apply(a + no, ns, in);
    }
    game_add_exefs(g, "main", raw_exefs_read, 0);
    name_from_path(in, path);
    return 0;
}
