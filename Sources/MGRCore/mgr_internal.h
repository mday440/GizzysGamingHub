#ifndef MGR_INTERNAL_H
#define MGR_INTERNAL_H

#define _FILE_OFFSET_BITS 64
#include "include/mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---------- little helpers ---------- */
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

void mgr_seterr(char *err, size_t errlen, const char *fmt, ...);
int  mgr_ends_with_ci(const char *s, const char *suffix);
void mgr_strlcpy(char *dst, const char *src, size_t n);

/* ---------- crypto ---------- */
typedef struct { uint32_t rk[44]; uint32_t drk[44]; } aes128_ctx;
void aes128_init(aes128_ctx *c, const uint8_t key[16]);
void aes128_encrypt_block(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt_block(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]);
void aes128_ecb_decrypt(const uint8_t key[16], uint8_t *buf, size_t len);
/* CTR: iv is the counter for the first block of buf (big-endian 128-bit counter). */
void aes128_ctr_xor(const aes128_ctx *c, const uint8_t iv[16], uint8_t *buf, size_t len);
/* Nintendo AES-XTS (big-endian sector tweak), key = 32 bytes (data key || tweak key). */
void aes128_xts_nintendo_decrypt(const uint8_t key[32], uint8_t *buf, size_t len,
                                 uint64_t first_sector, size_t sector_size);

/* ---------- compression ---------- */
int lz4_decompress_block(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);
int zstd_decompress_frame(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

/* ---------- keys ---------- */
int keys_get(const char *name, uint8_t *out, size_t len); /* 1 if found with exact length */
int keys_get_titlekey(const uint8_t rights_id[16], uint8_t out[16]);

/* ---------- byte source (file range, optionally AES-CTR) ---------- */
typedef struct {
    int      fd;
    uint64_t base;      /* absolute file offset of this range */
    uint64_t size;
    int      ctr;       /* 1 = AES-CTR encrypted */
    aes128_ctx aes;
    uint8_t  ctr_hi[8]; /* upper 8 bytes of counter */
    uint64_t ctr_base;  /* NCA-relative offset of byte 0 of this range */
} mgr_src;

int src_read(const mgr_src *s, uint64_t off, void *buf, size_t n); /* 0 ok, -1 error */
mgr_src src_sub(const mgr_src *s, uint64_t off, uint64_t size);

/* ---------- RomFS (Nintendo romfs image) ---------- */
typedef struct {
    mgr_src  src;
    uint64_t dir_meta_off, dir_meta_size, file_meta_off, file_meta_size, data_off;
    uint8_t *dir_meta;
    uint8_t *file_meta;
} romfs_img;

int  romfs_open(romfs_img *r, const mgr_src *src);
void romfs_free(romfs_img *r);
uint64_t romfs_count_files(const romfs_img *r);
int  romfs_read_path(const romfs_img *r, const char *path, uint8_t **out, size_t *len);

/* ---------- PFS0 / HFS0 ---------- */
typedef struct { char name[256]; uint64_t offset; uint64_t size; } pfs_entry;
typedef struct { pfs_entry *e; int count; mgr_src src; } pfs_img;
int  pfs_open(pfs_img *p, const mgr_src *src); /* accepts PFS0 and HFS0 */
void pfs_free(pfs_img *p);
const pfs_entry *pfs_find(const pfs_img *p, const char *name);
int  pfs_read_entry(const pfs_img *p, const pfs_entry *e, uint8_t **out, size_t *len);

/* ---------- NCA ---------- */
typedef struct {
    int      present;
    int      fs_type;     /* 0 romfs, 1 pfs0 */
    int      enc_type;    /* 1 none, 2 xts, 3 ctr, 4 bktr */
    uint64_t offset;      /* NCA-relative */
    uint64_t size;
    mgr_src  src;         /* decrypting view of the section */
    uint64_t data_off;    /* PFS0 / romfs level offset inside section */
    uint64_t data_size;
} nca_section;

typedef struct {
    int      ok;
    int      content_type; /* 0 program, 1 meta, 2 control, 3 manual, 4 data, 5 public data */
    uint64_t title_id;
    uint8_t  rights_id[16];
    int      has_rights;
    int      keygen;
    nca_section sec[4];
    char     err[160];
} nca_info;

int nca_open(nca_info *n, const mgr_src *src);

/* ---------- NACP ---------- */
void nacp_apply(const uint8_t *nacp, size_t len, mgr_game_info *info);

/* ---------- game object ---------- */
typedef struct {
    char name[32];
    int (*read)(mgr_game *g, int idx, uint8_t **out, size_t *len);
    int  idx;
} exefs_entry;

struct mgr_game {
    mgr_game_info info;
    uint8_t *icon; size_t icon_len;
    exefs_entry exefs[16];
    int exefs_n;
    void *impl;
    void (*destroy)(mgr_game *g);
    int  (*romfs_read)(mgr_game *g, const char *path, uint8_t **out, size_t *len);
    /* raw program image for single-file NSO/NRO */
    uint8_t *raw; size_t raw_len;
    /* RomFS image for DataStorage: either a raw image (NCA) or built from loose files */
    int  (*romfs_raw_read)(mgr_game *g, uint64_t off, void *buf, size_t n);
    uint64_t romfs_raw_size;
    void (*romfs_list)(mgr_game *g, void (*cb)(void *ctx, const char *path, uint64_t size), void *ctx);
    struct vrom *vrom;
    int vrom_built;
};

struct vrom;
struct vrom *vrom_build(void (*list)(void *game, void (*cb)(void *, const char *, uint64_t), void *ctx), void *game);
uint64_t vrom_size(const struct vrom *v);
int  vrom_read(struct vrom *v, int (*read_file)(void *game, const char *path, uint8_t **out, size_t *len), void *game,
               uint64_t off, uint8_t *buf, size_t n);
void vrom_free(struct vrom *v);
int  mgr_romfs_image_read(mgr_game *g, uint64_t off, void *buf, size_t n);
uint64_t mgr_romfs_image_size(mgr_game *g);

void game_add_exefs(mgr_game *g, const char *name,
                    int (*read)(mgr_game *, int, uint8_t **, size_t *), int idx);
void game_finish_info(mgr_game *g);
void game_set_platform_from_tid(mgr_game_info *info);

int  fmt_nup_open(mgr_game *g, const char *path, char *err, size_t errlen);
int  fmt_folder_open(mgr_game *g, const char *path, char *err, size_t errlen);
int  fmt_nsp_xci_open(mgr_game *g, const char *path, int is_xci, char *err, size_t errlen);
int  fmt_nca_open(mgr_game *g, const char *path, char *err, size_t errlen);
int  fmt_nso_open(mgr_game *g, const char *path, char *err, size_t errlen);
int  fmt_nro_open(mgr_game *g, const char *path, char *err, size_t errlen);

int  read_whole_file(const char *path, uint8_t **out, size_t *len);

/* ---------- program modules (NSO/NRO) and NPDM ---------- */
typedef struct {
    uint8_t *image;      /* text|ro|data|bss laid out at their memory offsets */
    size_t   image_size; /* page aligned, includes bss */
    uint32_t text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    uint8_t  module_id[32];
} mgr_module;

int  nso_load(const uint8_t *buf, size_t len, mgr_module *m, char *err, size_t errlen);
int  nro_load(const uint8_t *buf, size_t len, mgr_module *m, char *err, size_t errlen);
void module_free(mgr_module *m);

typedef struct {
    int      ok;
    char     name[17];
    uint64_t program_id;
    int      is_64bit;
    uint8_t  main_priority;
    uint8_t  main_core;
    uint32_t main_stack_size;
    uint32_t acid_size;
    uint32_t system_resource_size;
} mgr_npdm;

int npdm_parse(const uint8_t *buf, size_t len, mgr_npdm *out);

#endif
