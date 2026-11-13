/* PFS0 / HFS0 partitions, RomFS images, NCA headers + sections, NACP. */
#include "mgr_internal.h"

/* ---------------- PFS0 / HFS0 ---------------- */
int pfs_open(pfs_img *p, const mgr_src *src) {
    memset(p, 0, sizeof *p);
    uint8_t h[16];
    if (src_read(src, 0, h, 16)) return -1;
    int hfs = memcmp(h, "HFS0", 4) == 0;
    if (!hfs && memcmp(h, "PFS0", 4) != 0) return -1;
    uint32_t count = rd32(h + 4), strsz = rd32(h + 8);
    size_t esz = hfs ? 0x40 : 0x18;
    if (count > 100000 || strsz > (64u << 20)) return -1;
    size_t tbl = esz * count;
    uint8_t *buf = malloc(tbl + strsz + 1);
    if (!buf) return -1;
    if (src_read(src, 16, buf, tbl + strsz)) { free(buf); return -1; }
    buf[tbl + strsz] = 0;
    uint64_t data_start = 16 + tbl + strsz;
    p->e = calloc(count ? count : 1, sizeof *p->e);
    if (!p->e) { free(buf); return -1; }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = buf + i * esz;
        uint32_t so = rd32(e + 16);
        if (so >= strsz) continue;
        mgr_strlcpy(p->e[p->count].name, (const char *)buf + tbl + so, sizeof p->e[0].name);
        p->e[p->count].offset = data_start + rd64(e);
        p->e[p->count].size = rd64(e + 8);
        p->count++;
    }
    free(buf);
    p->src = *src;
    return 0;
}

void pfs_free(pfs_img *p) { free(p->e); p->e = NULL; p->count = 0; }

const pfs_entry *pfs_find(const pfs_img *p, const char *name) {
    for (int i = 0; i < p->count; i++) if (strcmp(p->e[i].name, name) == 0) return &p->e[i];
    return NULL;
}

int pfs_read_entry(const pfs_img *p, const pfs_entry *e, uint8_t **out, size_t *len) {
    if (e->size > (1ull << 32)) return -1;
    uint8_t *b = malloc(e->size ? e->size : 1);
    if (!b) return -1;
    if (src_read(&p->src, e->offset, b, e->size)) { free(b); return -1; }
    *out = b; *len = (size_t)e->size;
    return 0;
}

/* ---------------- RomFS ---------------- */
int romfs_open(romfs_img *r, const mgr_src *src) {
    memset(r, 0, sizeof *r);
    uint8_t h[0x50];
    if (src_read(src, 0, h, 0x50)) return -1;
    if (rd64(h) != 0x50) return -1;
    r->dir_meta_off = rd64(h + 0x18);  r->dir_meta_size = rd64(h + 0x20);
    r->file_meta_off = rd64(h + 0x38); r->file_meta_size = rd64(h + 0x40);
    r->data_off = rd64(h + 0x48);
    if (r->dir_meta_size > (256u << 20) || r->file_meta_size > (512u << 20)) return -1;
    r->dir_meta = malloc(r->dir_meta_size + 1);
    r->file_meta = malloc(r->file_meta_size + 1);
    if (!r->dir_meta || !r->file_meta ||
        src_read(src, r->dir_meta_off, r->dir_meta, r->dir_meta_size) ||
        src_read(src, r->file_meta_off, r->file_meta, r->file_meta_size)) {
        romfs_free(r);
        return -1;
    }
    r->src = *src;
    return 0;
}

void romfs_free(romfs_img *r) { free(r->dir_meta); free(r->file_meta); r->dir_meta = r->file_meta = NULL; }

uint64_t romfs_count_files(const romfs_img *r) {
    uint64_t n = 0, off = 0;
    while (off + 0x20 <= r->file_meta_size) {
        uint32_t nl = rd32(r->file_meta + off + 0x1C);
        n++;
        off += (0x20 + nl + 3) & ~3ull;
    }
    return n;
}

#define ROMFS_NONE 0xFFFFFFFFu

static int romfs_name_eq(const uint8_t *ent, uint32_t name_off_in_ent, uint32_t nl, const char *s, size_t sl) {
    return nl == sl && memcmp(ent + name_off_in_ent, s, sl) == 0;
}

int romfs_read_path(const romfs_img *r, const char *path, uint8_t **out, size_t *len) {
    uint32_t dir = 0;
    const char *p = path;
    while (*p == '/') p++;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t sl = slash ? (size_t)(slash - p) : strlen(p);
        if (dir + 0x18 > r->dir_meta_size) return -1;
        const uint8_t *d = r->dir_meta + dir;
        if (slash) {
            uint32_t c = rd32(d + 8);
            int found = 0;
            while (c != ROMFS_NONE && c + 0x18 <= r->dir_meta_size) {
                const uint8_t *cd = r->dir_meta + c;
                if (romfs_name_eq(cd, 0x18, rd32(cd + 0x14), p, sl)) { dir = c; found = 1; break; }
                c = rd32(cd + 4);
            }
            if (!found) return -1;
            p = slash + 1;
            while (*p == '/') p++;
            continue;
        }
        uint32_t f = rd32(d + 0xC);
        while (f != ROMFS_NONE && f + 0x20 <= r->file_meta_size) {
            const uint8_t *fe = r->file_meta + f;
            if (romfs_name_eq(fe, 0x20, rd32(fe + 0x1C), p, sl)) {
                uint64_t off = rd64(fe + 8), sz = rd64(fe + 0x10);
                if (sz > (1ull << 32)) return -1;
                uint8_t *b = malloc(sz ? sz : 1);
                if (!b) return -1;
                if (src_read(&r->src, r->data_off + off, b, sz)) { free(b); return -1; }
                *out = b; *len = (size_t)sz;
                return 0;
            }
            f = rd32(fe + 4);
        }
        return -1;
    }
}

/* ---------------- NCA ---------------- */
static const char *kak_names[3] = { "key_area_key_application", "key_area_key_ocean", "key_area_key_system" };

int nca_open(nca_info *n, const mgr_src *src) {
    memset(n, 0, sizeof *n);
    uint8_t hdr[0xC00];
    uint8_t hk[32];
    if (!keys_get("header_key", hk, 32)) { snprintf(n->err, sizeof n->err, "header_key missing from prod.keys"); return -1; }
    if (src_read(src, 0, hdr, sizeof hdr)) { snprintf(n->err, sizeof n->err, "NCA truncated"); return -1; }
    aes128_xts_nintendo_decrypt(hk, hdr, 0xC00, 0, 0x200);
    if (memcmp(hdr + 0x200, "NCA3", 4) != 0 && memcmp(hdr + 0x200, "NCA2", 4) != 0) {
        snprintf(n->err, sizeof n->err, "NCA header did not decrypt (wrong header_key?)");
        return -1;
    }
    n->content_type = hdr[0x205];
    int kaek_idx = hdr[0x207];
    int gen = hdr[0x206] > hdr[0x220] ? hdr[0x206] : hdr[0x220];
    if (gen > 0) gen--;
    n->keygen = gen;
    n->title_id = rd64(hdr + 0x210);
    memcpy(n->rights_id, hdr + 0x230, 16);
    for (int i = 0; i < 16; i++) if (n->rights_id[i]) n->has_rights = 1;

    uint8_t ctr_key[16];
    int have_key = 0;
    if (n->has_rights) {
        uint8_t tk[16], kek[16];
        char kn[32];
        snprintf(kn, sizeof kn, "titlekek_%02x", gen);
        if (keys_get_titlekey(n->rights_id, tk) && keys_get(kn, kek, 16)) {
            aes128_ecb_decrypt(kek, tk, 16);
            memcpy(ctr_key, tk, 16);
            have_key = 1;
        } else {
            snprintf(n->err, sizeof n->err, "title key for this game not found (add title.keys or keep the .tik in the NSP)");
        }
    } else if (kaek_idx < 3) {
        uint8_t kak[16];
        char kn[48];
        snprintf(kn, sizeof kn, "%s_%02x", kak_names[kaek_idx], gen);
        if (keys_get(kn, kak, 16)) {
            uint8_t area[0x40];
            memcpy(area, hdr + 0x300, 0x40);
            aes128_ecb_decrypt(kak, area, 0x40);
            memcpy(ctr_key, area + 0x20, 16);
            have_key = 1;
        } else {
            snprintf(n->err, sizeof n->err, "%s missing from prod.keys", kn);
        }
    }

    for (int i = 0; i < 4; i++) {
        const uint8_t *ent = hdr + 0x240 + i * 0x10;
        uint32_t ss = rd32(ent), es = rd32(ent + 4);
        if (!ss && !es) continue;
        const uint8_t *fs = hdr + 0x400 + i * 0x200;
        nca_section *s = &n->sec[i];
        s->present = 1;
        s->fs_type = fs[2];
        s->enc_type = fs[4];
        s->offset = (uint64_t)ss * 0x200;
        s->size = ((uint64_t)es - ss) * 0x200;
        s->src = src_sub(src, s->offset, s->size);
        s->src.ctr = 0;
        if (s->enc_type == 3 || s->enc_type == 4) {
            if (!have_key) { s->present = 0; continue; }
            s->src.ctr = 1;
            aes128_init(&s->src.aes, ctr_key);
            for (int j = 0; j < 8; j++) s->src.ctr_hi[j] = fs[0x140 + 7 - j];
            s->src.ctr_base = s->offset;
        } else if (s->enc_type != 1) {
            s->present = 0; /* XTS sections are not used by games */
            continue;
        }
        const uint8_t *hd = fs + 8;
        if (s->fs_type == 1) {           /* PFS0 with sha256 hash table */
            s->data_off = rd64(hd + 0x38);
            s->data_size = rd64(hd + 0x40);
        } else {                          /* RomFS with IVFC; level 6 is data */
            if (memcmp(hd, "IVFC", 4) != 0) { s->present = 0; continue; }
            const uint8_t *lvl = hd + 0x10 + 5 * 0x18;
            s->data_off = rd64(lvl);
            s->data_size = rd64(lvl + 8);
        }
    }
    n->ok = 1;
    return 0;
}

/* ---------------- NACP ---------------- */
void nacp_apply(const uint8_t *nacp, size_t len, mgr_game_info *info) {
    if (len < 0x3070) return;
    for (int lang = 0; lang < 16; lang++) {
        const char *nm = (const char *)nacp + lang * 0x300;
        if (nm[0]) {
            if (!info->name[0]) { size_t l = strnlen(nm, 0x200); if (l >= sizeof info->name) l = sizeof info->name - 1; memcpy(info->name, nm, l); info->name[l] = 0; }
            if (!info->publisher[0]) { const char *pb = nm + 0x200; size_t l = strnlen(pb, 0x100); if (l >= sizeof info->publisher) l = sizeof info->publisher - 1; memcpy(info->publisher, pb, l); info->publisher[l] = 0; }
            break;
        }
    }
    if (!info->display_version[0]) {
        size_t l = strnlen((const char *)nacp + 0x3060, 0x10);
        memcpy(info->display_version, nacp + 0x3060, l);
        info->display_version[l] = 0;
    }
}
