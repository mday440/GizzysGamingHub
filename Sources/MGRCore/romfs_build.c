/*
 * Builds a Nintendo RomFS image (level-6, as returned by DataStorage) on the fly
 * from a list of files, for containers that store loose files (NUP, folders).
 * Metadata (with the standard path-hash buckets) lives in memory; file data is
 * read on demand from the container.
 */
#include "mgr_internal.h"

typedef struct { char *path; uint64_t size; int order; uint64_t data_off; } vfile;

typedef struct {
    char *name;
    int parent;
    int first_child_dir, next_sibling_dir;
    int first_file, last_file;
    int last_child_dir;
    uint32_t off;
} vdir;

struct vrom {
    vfile *files; size_t nfiles, capfiles;
    uint8_t header[0x50];
    uint64_t data_off, data_size, meta_off;
    uint8_t *meta; size_t meta_size;
    uint64_t size;
    /* single-file read cache */
    char *cache_path; uint8_t *cache; size_t cache_len;
    uint32_t *by_off;   /* file indices sorted by data offset */
};
typedef struct vrom vrom;

static uint32_t romfs_hash(uint32_t parent, const char *name, size_t len) {
    uint32_t h = parent ^ 123456789u;
    for (size_t i = 0; i < len; i++) { h = (h >> 5) | (h << 27); h ^= (uint8_t)name[i]; }
    return h;
}

static uint32_t hash_count(uint32_t n) {
    uint32_t c = n;
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}

static void add_file(void *ctx, const char *path, uint64_t size) {
    vrom *v = ctx;
    if (v->nfiles == v->capfiles) {
        size_t nc = v->capfiles ? v->capfiles * 2 : 1024;
        vfile *nf = realloc(v->files, nc * sizeof *nf);
        if (!nf) return;
        v->files = nf; v->capfiles = nc;
    }
    while (*path == '/') path++;
    vfile *f = &v->files[v->nfiles];
    f->path = strdup(path);
    if (!f->path) return;
    f->size = size;
    f->order = (int)v->nfiles;
    v->nfiles++;
}

static int cmp_path(const void *a, const void *b) {
    const vfile *x = a, *y = b;
    int c = strcmp(x->path, y->path);
    return c ? c : x->order - y->order;
}

static int find_or_add_dir(vdir **dirs, int *nd, int *cap, int parent, const char *name, size_t len) {
    for (int c = (*dirs)[parent].first_child_dir; c >= 0; c = (*dirs)[c].next_sibling_dir)
        if (strlen((*dirs)[c].name) == len && memcmp((*dirs)[c].name, name, len) == 0) return c;
    if (*nd == *cap) {
        int ncap = *cap * 2;
        vdir *n = realloc(*dirs, (size_t)ncap * sizeof *n);
        if (!n) return -1;
        *dirs = n; *cap = ncap;
    }
    int id = (*nd)++;
    vdir *d = &(*dirs)[id];
    memset(d, 0, sizeof *d);
    d->name = strndup(name, len);
    d->parent = parent;
    d->first_child_dir = d->next_sibling_dir = d->first_file = d->last_file = d->last_child_dir = -1;
    vdir *p = &(*dirs)[parent];
    if (p->last_child_dir < 0) p->first_child_dir = id;
    else (*dirs)[p->last_child_dir].next_sibling_dir = id;
    p->last_child_dir = id;
    return id;
}

vrom *vrom_build(void (*list)(void *game, void (*cb)(void *, const char *, uint64_t), void *ctx), void *game) {
    vrom *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    list(game, add_file, v);   /* overlay sources first, base last: first occurrence wins */
    qsort(v->files, v->nfiles, sizeof *v->files, cmp_path);
    size_t w = 0;
    for (size_t i = 0; i < v->nfiles; i++) {
        if (w && strcmp(v->files[w - 1].path, v->files[i].path) == 0) { free(v->files[i].path); continue; }
        v->files[w++] = v->files[i];
    }
    v->nfiles = w;

    int cap = 256, nd = 1;
    vdir *dirs = calloc((size_t)cap, sizeof *dirs);
    int *file_dir = calloc(v->nfiles ? v->nfiles : 1, sizeof *file_dir);
    int *file_next = calloc(v->nfiles ? v->nfiles : 1, sizeof *file_next);
    if (!dirs || !file_dir || !file_next) { free(dirs); free(file_dir); free(file_next); return v; }
    dirs[0].name = strdup("");
    dirs[0].parent = 0;
    dirs[0].first_child_dir = dirs[0].next_sibling_dir = dirs[0].first_file = dirs[0].last_file = dirs[0].last_child_dir = -1;

    for (size_t i = 0; i < v->nfiles; i++) {
        const char *p = v->files[i].path;
        int d = 0;
        const char *slash;
        while ((slash = strchr(p, '/'))) {
            if (slash > p) d = find_or_add_dir(&dirs, &nd, &cap, d, p, (size_t)(slash - p));
            if (d < 0) d = 0;
            p = slash + 1;
        }
        file_dir[i] = d;
        file_next[i] = -1;
        if (dirs[d].last_file < 0) dirs[d].first_file = (int)i;
        else file_next[dirs[d].last_file] = (int)i;
        dirs[d].last_file = (int)i;
    }

    /* dir order: BFS; compute dir entry offsets */
    int *order = malloc((size_t)nd * sizeof *order);
    int head = 0, tail = 0;
    order[tail++] = 0;
    while (head < tail) {
        int d = order[head++];
        for (int c = dirs[d].first_child_dir; c >= 0; c = dirs[c].next_sibling_dir) order[tail++] = c;
    }
    uint32_t dir_meta_size = 0;
    for (int k = 0; k < tail; k++) {
        dirs[order[k]].off = dir_meta_size;
        dir_meta_size += (uint32_t)((0x18 + strlen(dirs[order[k]].name) + 3) & ~3u);
    }
    /* file entries in dir order; data offsets 16-aligned */
    uint32_t *file_off = calloc(v->nfiles ? v->nfiles : 1, sizeof *file_off);
    uint32_t file_meta_size = 0;
    uint64_t data = 0;
    for (int k = 0; k < tail; k++)
        for (int f = dirs[order[k]].first_file; f >= 0; f = file_next[f]) {
            const char *nm = strrchr(v->files[f].path, '/');
            nm = nm ? nm + 1 : v->files[f].path;
            file_off[f] = file_meta_size;
            file_meta_size += (uint32_t)((0x20 + strlen(nm) + 3) & ~3u);
            v->files[f].data_off = data;
            data = (data + v->files[f].size + 15) & ~15ull;
        }

    uint32_t dh_n = hash_count((uint32_t)nd), fh_n = hash_count((uint32_t)v->nfiles);
    v->data_off = 0x200;
    v->data_size = data;
    v->meta_off = (v->data_off + data + 3) & ~3ull;
    uint64_t dh_off = 0, dm_off = dh_off + dh_n * 4, fh_off = dm_off + dir_meta_size, fm_off = fh_off + fh_n * 4;
    v->meta_size = (size_t)(fm_off + file_meta_size);
    v->meta = calloc(1, v->meta_size);
    if (!v->meta) goto out;
    memset(v->meta + dh_off, 0xFF, dh_n * 4);
    memset(v->meta + fh_off, 0xFF, fh_n * 4);

    for (int k = 0; k < tail; k++) {
        vdir *d = &dirs[order[k]];
        uint8_t *ent = v->meta + dm_off + d->off;
        size_t nl = strlen(d->name);
        uint32_t parent_off = dirs[d->parent].off;
        wr32(ent, parent_off);
        wr32(ent + 4, d->next_sibling_dir >= 0 ? dirs[d->next_sibling_dir].off : 0xFFFFFFFFu);
        wr32(ent + 8, d->first_child_dir >= 0 ? dirs[d->first_child_dir].off : 0xFFFFFFFFu);
        wr32(ent + 12, d->first_file >= 0 ? file_off[d->first_file] : 0xFFFFFFFFu);
        uint32_t h = romfs_hash(parent_off, d->name, nl) % dh_n;
        wr32(ent + 16, rd32(v->meta + dh_off + h * 4));
        wr32(v->meta + dh_off + h * 4, d->off);
        wr32(ent + 20, (uint32_t)nl);
        memcpy(ent + 0x18, d->name, nl);
    }
    for (size_t f = 0; f < v->nfiles; f++) {
        const char *nm = strrchr(v->files[f].path, '/');
        nm = nm ? nm + 1 : v->files[f].path;
        size_t nl = strlen(nm);
        uint8_t *ent = v->meta + fm_off + file_off[f];
        uint32_t parent_off = dirs[file_dir[f]].off;
        wr32(ent, parent_off);
        wr32(ent + 4, file_next[f] >= 0 ? file_off[file_next[f]] : 0xFFFFFFFFu);
        wr64(ent + 8, v->files[f].data_off);
        wr64(ent + 16, v->files[f].size);
        uint32_t h = romfs_hash(parent_off, nm, nl) % fh_n;
        wr32(ent + 24, rd32(v->meta + fh_off + h * 4));
        wr32(v->meta + fh_off + h * 4, file_off[f]);
        wr32(ent + 28, (uint32_t)nl);
        memcpy(ent + 0x20, nm, nl);
    }
    {
        uint64_t hdr[10] = { 0x50, v->meta_off + dh_off, dh_n * 4ull, v->meta_off + dm_off, dir_meta_size,
                             v->meta_off + fh_off, fh_n * 4ull, v->meta_off + fm_off, file_meta_size, v->data_off };
        for (int k = 0; k < 10; k++) wr64(v->header + k * 8, hdr[k]);
    }
    v->size = v->meta_off + v->meta_size;
    v->by_off = malloc((v->nfiles ? v->nfiles : 1) * sizeof *v->by_off);
    if (v->by_off) {
        size_t k2 = 0;
        for (int k = 0; k < tail; k++)
            for (int f = dirs[order[k]].first_file; f >= 0; f = file_next[f]) v->by_off[k2++] = (uint32_t)f;
    }
out:
    for (int k = 0; k < nd; k++) free(dirs[k].name);
    free(dirs); free(order); free(file_dir); free(file_next); free(file_off);
    return v;
}

uint64_t vrom_size(const vrom *v) { return v ? v->size : 0; }

int vrom_read(vrom *v, int (*read_file)(void *game, const char *path, uint8_t **out, size_t *len), void *game,
              uint64_t off, uint8_t *buf, size_t n) {
    memset(buf, 0, n);
    while (n) {
        size_t chunk;
        if (off < 0x50) {
            chunk = (size_t)(0x50 - off) < n ? (size_t)(0x50 - off) : n;
            memcpy(buf, v->header + off, chunk);
        } else if (off >= v->data_off && off < v->data_off + v->data_size) {
            uint64_t rel = off - v->data_off;
            vfile *f = NULL;
            size_t lo = 0, hi = v->by_off ? v->nfiles : 0;
            while (lo < hi) {               /* last file with data_off <= rel */
                size_t mid = (lo + hi) / 2;
                if (v->files[v->by_off[mid]].data_off <= rel) lo = mid + 1; else hi = mid;
            }
            if (lo) {
                vfile *cand = &v->files[v->by_off[lo - 1]];
                if (rel < cand->data_off + ((cand->size + 15) & ~15ull)) f = cand;
            }
            uint64_t end_rel = f ? f->data_off + ((f->size + 15) & ~15ull) : rel + 16;
            chunk = (size_t)(end_rel - rel) < n ? (size_t)(end_rel - rel) : n;
            if (f && rel - f->data_off < f->size) {
                if (!v->cache_path || strcmp(v->cache_path, f->path) != 0) {
                    free(v->cache); free(v->cache_path);
                    v->cache = NULL; v->cache_path = NULL; v->cache_len = 0;
                    if (read_file(game, f->path, &v->cache, &v->cache_len) != 0) return -1;
                    v->cache_path = strdup(f->path);
                }
                uint64_t fo = rel - f->data_off;
                size_t take = chunk;
                if (fo + take > v->cache_len) take = fo < v->cache_len ? (size_t)(v->cache_len - fo) : 0;
                memcpy(buf, v->cache + fo, take);
            }
        } else if (off >= v->meta_off && off < v->meta_off + v->meta_size) {
            uint64_t rel = off - v->meta_off;
            chunk = (size_t)(v->meta_size - rel) < n ? (size_t)(v->meta_size - rel) : n;
            memcpy(buf, v->meta + rel, chunk);
        } else if (off >= v->size) {
            return -1;
        } else {
            uint64_t next = off < v->data_off ? v->data_off : v->meta_off;
            chunk = (size_t)(next - off) < n ? (size_t)(next - off) : n;
        }
        buf += chunk; off += chunk; n -= chunk;
    }
    return 0;
}

void vrom_free(vrom *v) {
    if (!v) return;
    for (size_t i = 0; i < v->nfiles; i++) free(v->files[i].path);
    free(v->files); free(v->meta); free(v->cache); free(v->cache_path); free(v->by_off);
    free(v);
}
