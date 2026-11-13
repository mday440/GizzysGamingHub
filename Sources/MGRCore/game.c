/* Format detection and the public mgr_game API. */
#include "mgr_internal.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

void name_from_path(mgr_game_info *in, const char *path);

mgr_format mgr_detect(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return MGR_FMT_UNKNOWN;
    if (S_ISDIR(st.st_mode)) return MGR_FMT_FOLDER;
    uint8_t h[0x110] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) return MGR_FMT_UNKNOWN;
    ssize_t n = pread(fd, h, sizeof h, 0);
    close(fd);
    if (n >= 4 && memcmp(h, "NUP\x01", 4) == 0) return MGR_FMT_NUP;
    if (n >= 4 && memcmp(h, "NSO0", 4) == 0) return MGR_FMT_NSO;
    if (n >= 0x14 && memcmp(h + 0x10, "NRO0", 4) == 0) return MGR_FMT_NRO;
    if (n >= 0x104 && memcmp(h + 0x100, "HEAD", 4) == 0)
        return mgr_ends_with_ci(path, ".xcz") ? MGR_FMT_XCZ : MGR_FMT_XCI;
    if (n >= 4 && memcmp(h, "PFS0", 4) == 0)
        return mgr_ends_with_ci(path, ".nsz") ? MGR_FMT_NSZ : MGR_FMT_NSP;
    if (mgr_ends_with_ci(path, ".nca")) return MGR_FMT_NCA;
    if (mgr_ends_with_ci(path, ".nsz")) return MGR_FMT_NSZ;
    if (mgr_ends_with_ci(path, ".xcz")) return MGR_FMT_XCZ;
    return MGR_FMT_UNKNOWN;
}

mgr_game *mgr_open(const char *path, char *err, size_t errlen) {
    mgr_format f = mgr_detect(path);
    if (f == MGR_FMT_UNKNOWN) { mgr_seterr(err, errlen, "unrecognised file type"); return NULL; }
    mgr_game *g = calloc(1, sizeof *g);
    if (!g) { mgr_seterr(err, errlen, "out of memory"); return NULL; }
    g->info.format = f;
    int r;
    switch (f) {
    case MGR_FMT_NUP:    r = fmt_nup_open(g, path, err, errlen); break;
    case MGR_FMT_FOLDER: r = fmt_folder_open(g, path, err, errlen); break;
    case MGR_FMT_NSP:    r = fmt_nsp_xci_open(g, path, 0, err, errlen); break;
    case MGR_FMT_XCI:    r = fmt_nsp_xci_open(g, path, 1, err, errlen); break;
    case MGR_FMT_NCA:    r = fmt_nca_open(g, path, err, errlen); break;
    case MGR_FMT_NSO:    r = fmt_nso_open(g, path, err, errlen); break;
    case MGR_FMT_NRO:    r = fmt_nro_open(g, path, err, errlen); break;
    case MGR_FMT_NSZ:
    case MGR_FMT_XCZ: {
        const char *b = strrchr(path, '/');
        mgr_strlcpy(g->info.name, b ? b + 1 : path, sizeof g->info.name);
        mgr_strlcpy(g->info.note, "Compressed NSZ/XCZ: decompress to NSP/XCI first (e.g. `nsz -D`).", sizeof g->info.note);
        r = 0;
        break;
    }
    default: r = -1;
    }
    if (r != 0) { mgr_close(g); return NULL; }
    name_from_path(&g->info, path);
    game_finish_info(g);
    return g;
}

void mgr_close(mgr_game *g) {
    if (!g) return;
    if (g->destroy) g->destroy(g);
    vrom_free(g->vrom);
    free(g->icon);
    free(g->raw);
    free(g);
}

const mgr_game_info *mgr_info(const mgr_game *g) { return &g->info; }

int mgr_icon(const mgr_game *g, const uint8_t **data, size_t *len) {
    if (!g->icon) return 0;
    *data = g->icon; *len = g->icon_len;
    return 1;
}

int mgr_exefs_read(mgr_game *g, const char *name, uint8_t **out, size_t *len) {
    for (int i = 0; i < g->exefs_n; i++)
        if (strcmp(g->exefs[i].name, name) == 0) return g->exefs[i].read(g, g->exefs[i].idx, out, len);
    return -1;
}

int mgr_romfs_read(mgr_game *g, const char *path, uint8_t **out, size_t *len) {
    if (!g->romfs_read) return -1;
    return g->romfs_read(g, path, out, len);
}

/* ---------------- RomFS image for DataStorage ---------------- */
static void list_tramp(void *game, void (*cb)(void *, const char *, uint64_t), void *ctx) {
    mgr_game *g = game;
    g->romfs_list(g, cb, ctx);
}
static int read_tramp(void *game, const char *path, uint8_t **out, size_t *len) {
    return mgr_romfs_read((mgr_game *)game, path, out, len);
}
static void ensure_vrom(mgr_game *g) {
    if (g->vrom_built) return;
    g->vrom_built = 1;
    if (g->romfs_list) g->vrom = vrom_build(list_tramp, g);
}

uint64_t mgr_romfs_image_size(mgr_game *g) {
    if (g->romfs_raw_read) return g->romfs_raw_size;
    ensure_vrom(g);
    return vrom_size(g->vrom);
}

int mgr_romfs_image_read(mgr_game *g, uint64_t off, void *buf, size_t n) {
    if (g->romfs_raw_read) return g->romfs_raw_read(g, off, buf, n);
    ensure_vrom(g);
    if (!g->vrom) return -1;
    return vrom_read(g->vrom, read_tramp, g, off, buf, n);
}
