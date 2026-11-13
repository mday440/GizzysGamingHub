/* GPU surfaces: block-linear addressing and PNG capture of presented frames.
 *
 * Block-linear layout (NVIDIA "GOB" tiling, documented in open-gpu-doc and Mesa's nil):
 * a GOB is 64 bytes x 8 rows (512 bytes); GOBs stack vertically into blocks of 2^bh GOBs;
 * blocks are laid out left to right, then top to bottom. Inside a GOB:
 *   offset = (x%64/32)*256 + (y%8/2)*64 + (x%32/16)*32 + (y%2)*16 + x%16      (x in bytes)
 */
#ifndef MGR_GPU_IMAGE_H
#define MGR_GPU_IMAGE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint64_t bl_offset(uint32_t xb, uint32_t y, uint32_t pitch_bytes, uint32_t bh_gobs) {
    uint32_t gobs_x = pitch_bytes / 64, block_rows = 8 * bh_gobs;
    uint64_t block = (uint64_t)(y / block_rows) * gobs_x + xb / 64;
    uint32_t gob_y = (y % block_rows) / 8;
    uint32_t in = (xb % 64 / 32) * 256 + (y % 8 / 2) * 64 + (xb % 32 / 16) * 32 + (y % 2) * 16 + xb % 16;
    return block * 512ull * bh_gobs + gob_y * 512ull + in;
}

/* --- minimal PNG writer (stored deflate blocks: no compression library needed) --- */
static uint32_t png_crc_table[256];
static uint32_t png_crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!png_crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; png_crc_table[i] = v; }
    c = ~c; while (n--) c = png_crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8); return ~c;
}
static void png_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; png_be32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = png_crc(0, (const uint8_t *)type, 4); c = png_crc(c, d, n);
    uint8_t t[4]; png_be32(t, c); fwrite(t, 1, 4, f);
}
/* rgb: w*h*3 bytes */
static int png_write_rgb(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
    uint8_t ihdr[13]; png_be32(ihdr, w); png_be32(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (w * 3 + 1);
    uint8_t *raw = malloc(raw_n); if (!raw) { fclose(f); return -1; }
    for (uint32_t y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; memcpy(raw + y * (w * 3 + 1) + 1, rgb + (size_t)y * w * 3, w * 3); }
    size_t nblk = (raw_n + 65534) / 65535, z_n = 2 + raw_n + nblk * 5 + 4;
    uint8_t *z = malloc(z_n); if (!z) { free(raw); fclose(f); return -1; }
    size_t o = 0; z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t p = 0; p < raw_n; p += 65535) {
        uint32_t len = raw_n - p > 65535 ? 65535 : (uint32_t)(raw_n - p);
        z[o++] = p + len >= raw_n; z[o++] = len & 0xFF; z[o++] = len >> 8; z[o++] = ~len & 0xFF; z[o++] = (~len >> 8) & 0xFF;
        memcpy(z + o, raw + p, len); o += len;
    }
    png_be32(z + o, (b << 16) | a); o += 4;
    png_chunk(f, "IDAT", z, (uint32_t)o); png_chunk(f, "IEND", NULL, 0);
    free(z); free(raw); fclose(f); return 0;
}
#endif
