/*
 * MikesGameRoom - save data and SD card file systems backed by host folders (included by ipc.c).
 *
 * fsp-srv hands a game an IFileSystem for its save data (and the SD card). Here each one is a
 * folder on the Mac:
 *   save: $MGR_SAVE_DIR, or <data>/saves/<title id>
 *   sd:   <data>/sd
 * where <data> is $MGR_DATA_DIR, or ~/Library/Application Support/MikesGameRoom
 * Paths arrive in an X (pointer) buffer as "/dir/file". ".." components are refused.
 */
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#define FS_NOT_FOUND      0x202u      /* fs PathNotFound */
#define FS_ALREADY_EXISTS 0x402u      /* fs PathAlreadyExists */
#define FS_MAX_OPEN       512

static struct { char path[600]; int used; int is_dir; uint32_t pos; } g_fs_open[FS_MAX_OPEN];

static void fs_mkdirs(const char *path) {          /* mkdir -p */
    char tmp[600];
    mgr_strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static const char *fs_root(mgr_emu *e, const char *svc) {
    static char save_root[512], sd_root[512];
    char data[400];
    if (getenv("MGR_DATA_DIR")) mgr_strlcpy(data, getenv("MGR_DATA_DIR"), sizeof data);
    else snprintf(data, sizeof data, "%s/Library/Application Support/MikesGameRoom", getenv("HOME") ? getenv("HOME") : "/tmp");
    if (!strcmp(svc, "IFileSystem(sd)")) {
        if (!sd_root[0]) { snprintf(sd_root, sizeof sd_root, "%s/sd", data); fs_mkdirs(sd_root); }
        return sd_root;
    }
    if (!save_root[0]) {
        if (getenv("MGR_SAVE_DIR")) mgr_strlcpy(save_root, getenv("MGR_SAVE_DIR"), sizeof save_root);
        else snprintf(save_root, sizeof save_root, "%s/saves/%016llx", data,
                      (unsigned long long)mgr_info(e->game)->title_id);
        fs_mkdirs(save_root);
        elog(e, "[fs] save data folder: %s\n", save_root);
    }
    return save_root;
}

/* The k-th X buffer as a host path under the file system's root. Returns 0 if unusable. */
static int fs_path(ipc_ctx *c, int k, const char *svc, char *out, size_t n) {
    if (k >= c->nx || !c->x[k].addr) return 0;
    char p[0x302] = {0};
    size_t len = c->x[k].size < 0x301 ? (size_t)c->x[k].size : 0x301;
    uint8_t *src = mem_ptr(c->e, c->x[k].addr, len);
    if (!src) return 0;
    memcpy(p, src, len);
    p[0x301] = 0;
    if (strstr(p, "..")) return 0;
    const char *rel = p;
    const char *colon = strchr(p, ':');                 /* "save:/x" style mount prefixes */
    if (colon && colon < p + 16) rel = colon + 1;
    while (*rel == '/') rel++;
    snprintf(out, n, "%s/%s", fs_root(c->e, svc), rel);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
    return 1;
}

static int fs_rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char sub[1200]; snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
            fs_rm_rf(sub);
        }
        if (d) closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

static int fs_open_slot(const char *path, int is_dir) {
    for (int i = 0; i < FS_MAX_OPEN; i++)
        if (!g_fs_open[i].used) {
            g_fs_open[i].used = 1; g_fs_open[i].is_dir = is_dir; g_fs_open[i].pos = 0;
            mgr_strlcpy(g_fs_open[i].path, path, sizeof g_fs_open[i].path);
            return i;
        }
    return -1;
}

/* Returns 1 if handled (result in *res). */
static int fs_host_dispatch(ipc_ctx *c, const char *name, uint32_t cmd, uint32_t *res) {
    mgr_emu *e = c->e;
    *res = 0;
    if (!strcmp(name, "IFileSystem(save)") || !strcmp(name, "IFileSystem(sd)")) {
        char path[600], path2[600];
        struct stat st;
        switch (cmd) {
        case 0: {                                                    /* CreateFile(mode, size) */
            if (!fs_path(c, 0, name, path, sizeof path)) { *res = FS_NOT_FOUND; return 1; }
            if (!stat(path, &st)) { *res = FS_ALREADY_EXISTS; return 1; }
            FILE *f = fopen(path, "wb");
            if (!f) { *res = FS_NOT_FOUND; return 1; }
            uint64_t size = arg64(c, 8);
            if (size) { fseeko(f, (off_t)size - 1, SEEK_SET); fputc(0, f); }
            fclose(f);
            return 1;
        }
        case 1:                                                      /* DeleteFile */
            if (!fs_path(c, 0, name, path, sizeof path) || unlink(path)) *res = FS_NOT_FOUND;
            return 1;
        case 2:                                                      /* CreateDirectory */
            if (!fs_path(c, 0, name, path, sizeof path)) { *res = FS_NOT_FOUND; return 1; }
            if (!stat(path, &st)) { *res = FS_ALREADY_EXISTS; return 1; }
            if (mkdir(path, 0755)) *res = FS_NOT_FOUND;
            return 1;
        case 3:                                                      /* DeleteDirectory */
            if (!fs_path(c, 0, name, path, sizeof path) || rmdir(path)) *res = FS_NOT_FOUND;
            return 1;
        case 4:                                                      /* DeleteDirectoryRecursively */
            if (!fs_path(c, 0, name, path, sizeof path) || fs_rm_rf(path)) *res = FS_NOT_FOUND;
            return 1;
        case 13: {                                                   /* CleanDirectoryRecursively */
            if (!fs_path(c, 0, name, path, sizeof path)) { *res = FS_NOT_FOUND; return 1; }
            fs_rm_rf(path); mkdir(path, 0755);
            return 1;
        }
        case 5: case 6:                                              /* RenameFile / RenameDirectory */
            if (!fs_path(c, 0, name, path, sizeof path) || !fs_path(c, 1, name, path2, sizeof path2)) { *res = FS_NOT_FOUND; return 1; }
            if (!stat(path2, &st)) { *res = FS_ALREADY_EXISTS; return 1; }
            if (rename(path, path2)) *res = FS_NOT_FOUND;
            return 1;
        case 7:                                                      /* GetEntryType: 0 directory, 1 file */
            if (!fs_path(c, 0, name, path, sizeof path) || stat(path, &st)) { *res = FS_NOT_FOUND; return 1; }
            out_u32(c, S_ISDIR(st.st_mode) ? 0 : 1);
            return 1;
        case 8: case 9: {                                            /* OpenFile / OpenDirectory */
            if (!fs_path(c, 0, name, path, sizeof path) || stat(path, &st) || (cmd == 8) == S_ISDIR(st.st_mode)) {
                *res = FS_NOT_FOUND; return 1;
            }
            int slot = fs_open_slot(path, cmd == 9);
            if (slot < 0) { *res = 0xD001; return 1; }
            int s = out_object_idx(c, cmd == 8 ? "IFile(host)" : "IDirectory(host)");
            if (s > 0) c->e->svcs[s].state = slot;
            return 1;
        }
        case 10: return 1;                                           /* Commit: writes are already on disk */
        case 11: case 12: out_u64(c, 4ull << 30); return 1;          /* free / total space: 4 GiB */
        case 14: {                                                   /* GetFileTimeStampRaw */
            uint8_t ts[0x20] = {0};
            if (fs_path(c, 0, name, path, sizeof path) && !stat(path, &st)) {
                wr64(ts, (uint64_t)st.st_ctime); wr64(ts + 8, (uint64_t)st.st_mtime); wr64(ts + 16, (uint64_t)st.st_atime); ts[24] = 1;
            } else { *res = FS_NOT_FOUND; return 1; }
            out_bytes(c, ts, sizeof ts);
            return 1;
        }
        default: return 1;
        }
    }
    if (!strcmp(name, "IFile(host)")) {
        int slot = e->svcs[c->target].state;
        if (slot < 0 || slot >= FS_MAX_OPEN || !g_fs_open[slot].used) { *res = FS_NOT_FOUND; return 1; }
        const char *path = g_fs_open[slot].path;
        switch (cmd) {
        case 0: {                                                    /* Read(option, offset, size) -> B */
            uint64_t off = arg64(c, 8), size = arg64(c, 16);
            ipc_buf *b = out_buf(c);
            uint64_t got = 0;
            if (b && b->addr) {
                if (size > b->size) size = b->size;
                uint8_t *p = size ? mem_ptr(e, b->addr, size) : NULL;
                FILE *f = fopen(path, "rb");
                if (f && p) { fseeko(f, (off_t)off, SEEK_SET); got = fread(p, 1, (size_t)size, f); }
                if (f) fclose(f);
            }
            out_u64(c, got);
            return 1;
        }
        case 1: {                                                    /* Write(option, offset, size) <- A */
            uint64_t off = arg64(c, 8), size = arg64(c, 16);
            const ipc_buf *a = c->na ? &c->a[0] : NULL;
            if (a && a->addr && size) {
                if (size > a->size) size = a->size;
                uint8_t *p = mem_ptr(e, a->addr, size);
                FILE *f = fopen(path, "r+b");
                if (f && p) { fseeko(f, (off_t)off, SEEK_SET); fwrite(p, 1, (size_t)size, f); }
                if (f) fclose(f);
            }
            return 1;
        }
        case 2: return 1;                                            /* Flush */
        case 3: if (truncate(path, (off_t)arg64(c, 0))) *res = FS_NOT_FOUND; return 1;   /* SetSize */
        case 4: { struct stat st; out_u64(c, stat(path, &st) ? 0 : (uint64_t)st.st_size); return 1; }  /* GetSize */
        default: return 1;
        }
    }
    if (!strcmp(name, "IDirectory(host)")) {
        int slot = e->svcs[c->target].state;
        if (slot < 0 || slot >= FS_MAX_OPEN || !g_fs_open[slot].used) { *res = FS_NOT_FOUND; return 1; }
        DIR *d = opendir(g_fs_open[slot].path);
        struct dirent *de;
        uint32_t idx = 0, count = 0;
        ipc_buf *b = cmd == 0 ? out_buf(c) : NULL;
        uint64_t max = b ? b->size / 0x310 : 0;
        while (d && (de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (cmd == 1) { count++; continue; }                     /* GetEntryCount */
            if (idx++ < g_fs_open[slot].pos) continue;               /* Read continues where it stopped */
            if (count >= max) break;
            uint8_t ent[0x310] = {0};
            mgr_strlcpy((char *)ent, de->d_name, 0x301);
            char full[1200]; struct stat st;
            snprintf(full, sizeof full, "%s/%s", g_fs_open[slot].path, de->d_name);
            int isdir = !stat(full, &st) && S_ISDIR(st.st_mode);
            ent[0x304] = isdir ? 0 : 1;
            wr64(ent + 0x308, isdir ? 0 : (uint64_t)st.st_size);
            uint8_t *p = mem_ptr(e, b->addr + count * 0x310ull, 0x310);
            if (p) memcpy(p, ent, sizeof ent);
            count++;
        }
        if (d) closedir(d);
        if (cmd == 0) g_fs_open[slot].pos += count;
        out_u64(c, count);
        return 1;
    }
    return 0;
}
