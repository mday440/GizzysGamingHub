#include "mgr_internal.h"
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* from the vendored zstddeclib.c */
size_t ZSTD_decompress(void *dst, size_t dstCapacity, const void *src, size_t compressedSize);
unsigned ZSTD_isError(size_t code);

const char *mgr_version(void) { return "0.1.0"; }

void mgr_seterr(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || !errlen) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

void mgr_strlcpy(char *dst, const char *src, size_t n) {
    if (!n) return;
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

int mgr_ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (b > a) return 0;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}

int read_whole_file(const char *path, uint8_t **out, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) { close(fd); return -1; }
    size_t n = (size_t)st.st_size;
    uint8_t *b = malloc(n ? n : 1);
    if (!b) { close(fd); return -1; }
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, b + got, n - got, (off_t)got);
        if (r <= 0) { free(b); close(fd); return -1; }
        got += (size_t)r;
    }
    close(fd);
    *out = b; *len = n;
    return 0;
}

/* ---------- LZ4 block format ---------- */
int lz4_decompress_block(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    size_t ip = 0, op = 0;
    while (ip < srclen) {
        uint8_t token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do { if (ip >= srclen) return -1; b = src[ip++]; lit += b; } while (b == 255);
        }
        if (ip + lit > srclen || op + lit > dstlen) return -1;
        memcpy(dst + op, src + ip, lit);
        ip += lit; op += lit;
        if (ip >= srclen) break; /* last sequence has no match */
        if (ip + 2 > srclen) return -1;
        size_t offset = (size_t)src[ip] | ((size_t)src[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op) return -1;
        size_t mlen = token & 15;
        if (mlen == 15) {
            uint8_t b;
            do { if (ip >= srclen) return -1; b = src[ip++]; mlen += b; } while (b == 255);
        }
        mlen += 4;
        if (op + mlen > dstlen) return -1;
        for (size_t i = 0; i < mlen; i++, op++) dst[op] = dst[op - offset];
    }
    return op == dstlen ? 0 : -1;
}

void mgr_zstd_set_zbic(int on);

/* Decompresses a zstd frame, or a ZBIC frame (Nintendo's zstd variant, magic "ZBIC"). */
int zstd_decompress_frame(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    int zbic = srclen >= 4 && memcmp(src, "ZBIC", 4) == 0;
    if (zbic) mgr_zstd_set_zbic(1);
    size_t r = ZSTD_decompress(dst, dstlen, src, srclen);
    if (zbic) mgr_zstd_set_zbic(0);
    if (ZSTD_isError(r) || r != dstlen) return -1;
    return 0;
}

/* ---------- keys ---------- */
typedef struct { char name[64]; uint8_t val[32]; int len; } key_ent;
static key_ent *g_keys;
static int g_nkeys, g_capkeys;

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void key_put(const char *name, const uint8_t *v, int len) {
    for (int i = 0; i < g_nkeys; i++)
        if (strcmp(g_keys[i].name, name) == 0) { memcpy(g_keys[i].val, v, (size_t)len); g_keys[i].len = len; return; }
    if (g_nkeys == g_capkeys) {
        int nc = g_capkeys ? g_capkeys * 2 : 128;
        key_ent *nk = realloc(g_keys, (size_t)nc * sizeof *nk);
        if (!nk) return;
        g_keys = nk; g_capkeys = nc;
    }
    key_ent *k = &g_keys[g_nkeys++];
    mgr_strlcpy(k->name, name, sizeof k->name);
    memcpy(k->val, v, (size_t)len); k->len = len;
}

int mgr_keys_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char name[64]; size_t ni = 0;
        for (char *p = line; *p && ni + 1 < sizeof name; p++)
            if (!isspace((unsigned char)*p)) name[ni++] = (char)tolower((unsigned char)*p);
        name[ni] = 0;
        uint8_t val[32]; int vl = 0, half = -1, bad = 0;
        for (char *p = eq + 1; *p; p++) {
            if (isspace((unsigned char)*p)) continue;
            int h = hexval((unsigned char)*p);
            if (h < 0) { bad = 1; break; }
            if (half < 0) half = h;
            else { if (vl < 32) val[vl++] = (uint8_t)(half << 4 | h); else bad = 1; half = -1; }
        }
        if (bad || half >= 0 || !ni || !vl) continue;
        key_put(name, val, vl);
        n++;
    }
    fclose(f);
    return n;
}

int mgr_keys_count(void) { return g_nkeys; }

int keys_get(const char *name, uint8_t *out, size_t len) {
    for (int i = 0; i < g_nkeys; i++)
        if (strcmp(g_keys[i].name, name) == 0 && (size_t)g_keys[i].len == len) {
            memcpy(out, g_keys[i].val, len);
            return 1;
        }
    return 0;
}

int mgr_keys_have_header_key(void) { uint8_t k[32]; return keys_get("header_key", k, 32); }

int keys_get_titlekey(const uint8_t rights_id[16], uint8_t out[16]) {
    char name[33];
    for (int i = 0; i < 16; i++) snprintf(name + i * 2, 3, "%02x", rights_id[i]);
    return keys_get(name, out, 16);
}

/* ---------- byte source ---------- */
mgr_src src_sub(const mgr_src *s, uint64_t off, uint64_t size) {
    mgr_src r = *s;
    r.base = s->base + off;
    r.size = size;
    r.ctr_base = s->ctr_base + off;
    return r;
}

int src_read(const mgr_src *s, uint64_t off, void *buf, size_t n) {
    if (off > s->size || n > s->size - off) return -1;
    if (!s->ctr) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = pread(s->fd, (uint8_t *)buf + got, n - got, (off_t)(s->base + off + got));
            if (r <= 0) return -1;
            got += (size_t)r;
        }
        return 0;
    }
    /* AES-CTR: align to 16 bytes relative to the NCA */
    uint64_t abs = s->ctr_base + off;
    uint64_t start = abs & ~(uint64_t)15;
    size_t pre = (size_t)(abs - start);
    size_t total = (pre + n + 15) & ~(size_t)15;
    uint8_t *tmp = malloc(total);
    if (!tmp) return -1;
    memset(tmp, 0, total);
    /* read what exists in the file (last block may be short) */
    uint64_t file_start = s->base + off - pre;
    size_t got = 0;
    while (got < total) {
        ssize_t r = pread(s->fd, tmp + got, total - got, (off_t)(file_start + got));
        if (r < 0) { free(tmp); return -1; }
        if (r == 0) break;
        got += (size_t)r;
    }
    if (got < pre + n) { free(tmp); return -1; }
    uint8_t iv[16];
    memcpy(iv, s->ctr_hi, 8);
    uint64_t blk = start >> 4;
    for (int i = 0; i < 8; i++) iv[15 - i] = (uint8_t)(blk >> (8 * i));
    aes128_ctr_xor(&s->aes, iv, tmp, total);
    memcpy(buf, tmp + pre, n);
    free(tmp);
    return 0;
}

void keys_add_raw(const char *name, const uint8_t *v, int len) { key_put(name, v, len); }
