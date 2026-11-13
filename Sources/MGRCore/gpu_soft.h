/* Software rendering of 3D draws (reference path, slow): vertex fetch, the Ampere shader
 * interpreter (sass_exec.h), triangle rasterisation, texture sampling and render-target
 * writes. It runs only for the frames being captured (MGR_FRAME_DUMP) when MGR_SOFT_RENDER
 * is set; the real-time path will be a Metal translation checked against this one.
 * Register layout and formats: NVIDIA open-gpu-doc (clc797.h, clc797tex.h, MIT). */
#ifndef MGR_GPU_SOFT_H
#define MGR_GPU_SOFT_H
#include "gpu_image.h"
#include "gpu_bc7.h"
#include "gpu_astc.h"
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"                                 /* BC6H (bcdec by Sergii Kudlai, MIT / Unlicense) */

#define R3(off) (en->regs[(off) / 4])
typedef struct { uint64_t addr; uint32_t size; } soft_cb;
static soft_cb g_cbs[NX2_MAX_CH][5][18];          /* per channel, shader stage group, slot */
static int g_soft_frame;                           /* 1 while the current frame is being rendered */
static uint64_t g_soft_draws, g_soft_pixels, g_soft_unknown, g_soft_unk_op, g_soft_texfmt[128], g_soft_ztests, g_soft_zfail, g_soft_blends;
#include <time.h>
static double now_seconds(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static uint64_t g_soft_t0_id; static double g_soft_t0, g_soft_budget = 120; static int g_soft_budget_hit;
typedef struct { uint64_t addr; uint32_t w, h, fmt, bh, draws; uint64_t pixels; } soft_rt_seen;
static soft_rt_seen g_soft_rts[256]; static int g_soft_nrts;

static float soft_srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float soft_lin_to_srgb(float c) { c = c < 0 ? 0 : c > 1 ? 1 : c; return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }
static float soft_uf11(uint32_t v, int mb) {       /* unsigned small float: 5-bit exponent, mb-bit mantissa */
    uint32_t e = (v >> mb) & 31, m = v & ((1u << mb) - 1);
    if (e == 0) return ldexpf((float)m, -14 - mb);
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf(1.0f + (float)m / (float)(1u << mb), (int)e - 15);
}
static uint32_t soft_f_to_uf(float f, int mb) {
    if (!(f > 0)) return 0;
    int e; float m = frexpf(f, &e);                /* f = m * 2^e, m in [0.5, 1) */
    int be = e - 1 + 15;
    if (be >= 31) return (31u << mb) - 1;
    if (be <= 0) { uint32_t mm = (uint32_t)(ldexpf(f, 14 + mb) + 0.5f); return mm > ((1u << mb) - 1) ? (1u << mb) : mm; }
    uint32_t mm = (uint32_t)((m * 2 - 1) * (float)(1u << mb) + 0.5f);
    if (mm >> mb) { mm = 0; be++; if (be >= 31) return (31u << mb) - 1; }
    return ((uint32_t)be << mb) | mm;
}

/* ---------------- render targets ---------------- */
typedef struct { uint64_t addr; uint32_t w, h, fmt, bpp, bh, pitch; } soft_rt;
static int soft_fmt_bpp(uint32_t f) {
    switch (f) { case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 16;
    case 0xC6: case 0xC7: case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: return 8;
    case 0xEE: case 0xEF: case 0xF0: case 0xF1: case 0xF2: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xE8: case 0xE9: case 0xF8: return 2;
    case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF7: return 1;
    case 0: return 0;
    default: return 4; }
}
static int soft_rt_get(nx2_engine *en, int j, soft_rt *rt) {
    rt->addr = ((uint64_t)(R3(0x800 + j * 64) & 0xFF) << 32) | R3(0x804 + j * 64);
    rt->w = R3(0x808 + j * 64) & 0xFFFFFFF; rt->h = R3(0x80c + j * 64) & 0x1FFFF; rt->fmt = R3(0x810 + j * 64) & 0xFF;
    uint32_t mem = R3(0x814 + j * 64);
    rt->bpp = (uint32_t)soft_fmt_bpp(rt->fmt); rt->bh = 1u << ((mem >> 4) & 7);
    rt->pitch = (rt->w * rt->bpp + 63) & ~63u;
    return rt->addr && rt->bpp && rt->w && rt->h && rt->w <= 16384 && rt->h <= 16384;
}
static void soft_pack(uint32_t fmt, const float c[4], uint8_t *o) {
    uint32_t v;
    switch (fmt) {
    case 0xD5: case 0xD6: case 0xF9: case 0xFA: case 0xCF: case 0xD0: case 0xE6: case 0xE7: {
        float r = c[0], g = c[1], b = c[2], a = c[3];
        if (fmt == 0xD6 || fmt == 0xFA || fmt == 0xD0 || fmt == 0xE7) { r = soft_lin_to_srgb(r); g = soft_lin_to_srgb(g); b = soft_lin_to_srgb(b); }
        #define U8(x) (uint8_t)((x) < 0 ? 0 : (x) > 1 ? 255 : (uint32_t)((x) * 255.0f + 0.5f))
        if (fmt >= 0xCF && fmt <= 0xD0) { o[0] = U8(b); o[1] = U8(g); o[2] = U8(r); o[3] = U8(a); }
        else if (fmt == 0xE6 || fmt == 0xE7) { o[0] = U8(b); o[1] = U8(g); o[2] = U8(r); o[3] = 255; }
        else { o[0] = U8(r); o[1] = U8(g); o[2] = U8(b); o[3] = (fmt == 0xF9 || fmt == 0xFA) ? 255 : U8(a); }
        return;
    }
    case 0xE0: v = soft_f_to_uf(c[0], 6) | (soft_f_to_uf(c[1], 6) << 11) | (soft_f_to_uf(c[2], 5) << 22); memcpy(o, &v, 4); return;
    case 0xD1: { uint32_t r = (uint32_t)(fminf(fmaxf(c[0], 0), 1) * 1023 + .5f), g = (uint32_t)(fminf(fmaxf(c[1], 0), 1) * 1023 + .5f), b = (uint32_t)(fminf(fmaxf(c[2], 0), 1) * 1023 + .5f), a = (uint32_t)(fminf(fmaxf(c[3], 0), 1) * 3 + .5f); v = r | g << 10 | b << 20 | a << 30; memcpy(o, &v, 4); return; }
    case 0xCA: case 0xCE: for (int i = 0; i < 4; i++) { uint16_t h = sx_f_to_half(c[i]); memcpy(o + i * 2, &h, 2); } return;
    case 0xDE: for (int i = 0; i < 2; i++) { uint16_t h = sx_f_to_half(c[i]); memcpy(o + i * 2, &h, 2); } return;
    case 0xF2: { uint16_t h = sx_f_to_half(c[0]); memcpy(o, &h, 2); return; }
    case 0xC0: case 0xC3: memcpy(o, c, 16); return;
    case 0xCB: memcpy(o, c, 8); return;
    case 0xE5: case 0xFF: memcpy(o, c, 4); return;
    case 0xF3: o[0] = U8(c[0]); return;
    case 0xEA: o[0] = U8(c[0]); o[1] = U8(c[1]); return;
    case 0xC6: for (int i = 0; i < 4; i++) { uint16_t u = (uint16_t)(fminf(fmaxf(c[i], 0), 1) * 65535 + .5f); memcpy(o + i * 2, &u, 2); } return;
    default: { uint32_t raw = 0; memcpy(&raw, c, 4); memcpy(o, &raw, (size_t)soft_fmt_bpp(fmt) > 4 ? 4 : (size_t)soft_fmt_bpp(fmt)); return; }
    }
    #undef U8
}
static void soft_unpack(uint32_t fmt, const uint8_t *p, float c[4]) {
    c[0] = c[1] = c[2] = 0; c[3] = 1;
    uint32_t u; memcpy(&u, p, 4);
    switch (fmt) {
    case 0xD5: case 0xD6: case 0xF9: case 0xFA: for (int i = 0; i < 4; i++) c[i] = p[i] / 255.0f; break;
    case 0xCF: case 0xD0: case 0xE6: case 0xE7: c[0] = p[2] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[0] / 255.0f; break;
    case 0xE0: c[0] = soft_uf11(u & 2047, 6); c[1] = soft_uf11((u >> 11) & 2047, 6); c[2] = soft_uf11(u >> 22, 5); break;
    case 0xCA: case 0xCE: for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, p + i * 2, 2); c[i] = sx_half_to_f(h); } break;
    case 0xC0: case 0xC3: memcpy(c, p, 16); break;
    case 0xD1: c[0] = (u & 1023) / 1023.0f; c[1] = ((u >> 10) & 1023) / 1023.0f; c[2] = ((u >> 20) & 1023) / 1023.0f; break;
    case 0xE5: case 0xFF: memcpy(c, p, 4); c[1] = c[2] = c[0]; break;
    case 0xF2: { uint16_t h; memcpy(&h, p, 2); c[0] = c[1] = c[2] = sx_half_to_f(h); break; }
    case 0xF3: c[0] = c[1] = c[2] = p[0] / 255.0f; break;
    default: c[0] = p[0] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[2] / 255.0f; break;
    }
}
static uint8_t *soft_rt_ptr(mgr_emu *e, const soft_rt *rt, uint32_t x, uint32_t y) {
    uint64_t off = bl_offset(x * rt->bpp, y, rt->pitch, rt->bh);
    uint64_t ga = rt->addr + off, cpu = nx2_gpu_xlate(e, ga);
    if (cpu) return mem_ptr(e, cpu, rt->bpp);
    if (gpu_only_ok(e, ga)) { uint8_t *g = gpu_only_page(ga, 1); return g ? g + (ga & 0xFFF) : NULL; }   /* GPU-only memory */
    return NULL;
}

/* ---------------- textures ---------------- */
typedef struct { uint64_t addr; uint32_t comps, types, swz, w, h, bl, bh_log2, pitch, srgb, bytes, blockw, blockh, depth, type, bd_log2, maxmip, base; uint64_t layer_stride; } soft_tex;
static soft_tex g_cs_in_big, g_cs_out_big; static int g_cs_track, g_cs_hmma;   /* compute: largest image read / written */
static int soft_tex_get(mgr_emu *e, nx2_engine *en, uint32_t handle, soft_tex *t) {
    uint64_t pool = ((uint64_t)(R3(0x1574) & 0xFF) << 32) | R3(0x1578);
    uint32_t idx = handle & 0xFFFFF;
    if (!pool) return 0;
    uint32_t w[8]; gread(e, pool + idx * 32ull, w, 32);
    #define TB(lo, hi) (uint32_t)sass_bits(w, lo, (hi) + 1)
    t->comps = TB(0, 6); t->types = TB(7, 18); t->swz = TB(19, 30);
    uint32_t ver = TB(85, 87);
    t->bl = ver >= 3;
    if (t->bl) { t->addr = ((uint64_t)TB(41, 63) << 9) | ((uint64_t)TB(64, 80) << 32); t->bh_log2 = TB(99, 101); }
    else { t->addr = ((uint64_t)TB(37, 63) << 5) | ((uint64_t)TB(64, 80) << 32); t->pitch = TB(96, 111) << 5; }
    t->w = TB(128, 144) + 1; t->h = TB(160, 175) + 1 + (TB(146, 146) << 16); t->srgb = TB(150, 150);
    t->depth = TB(176, 189) + 1 + (TB(145, 145) << 14); t->type = TB(151, 154); t->bd_log2 = t->bl ? TB(102, 104) : 0; t->maxmip = TB(124, 127);
    #undef TB
    t->blockw = t->blockh = 1;
    switch (t->comps) {
    case 0x01: t->bytes = 16; break; case 0x02: t->bytes = 12; break; case 0x03: case 0x04: t->bytes = 8; break;
    case 0x07: case 0x08: case 0x09: case 0x0c: case 0x0f: case 0x21: case 0x20: case 0x29: case 0x2a: case 0x2b: case 0x2f: t->bytes = 4; break;
    case 0x30: t->bytes = 8; break;
    case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x18: case 0x1b: case 0x3a: t->bytes = 2; break;
    case 0x1d: case 0x1e: t->bytes = 1; break;
    case 0x24: case 0x27: t->bytes = 8; t->blockw = t->blockh = 4; break;
    case 0x25: case 0x26: case 0x28: case 0x10: case 0x11: case 0x17: t->bytes = 16; t->blockw = t->blockh = 4; break;
    default:
        if (t->comps >= 0x40 && t->comps <= 0x57) {                 /* ASTC: block footprint per format (not decoded yet) */
            static const uint8_t aw[24] = {4,5,6,0,8,10,12,0,0,0,0,0,0,0,0,0, 5,6,8,10,12,8,10,10}, ah[24] = {4,5,6,0,8,10,12,0,0,0,0,0,0,0,0,0, 4,5,6,8,10,5,5,6};
            uint32_t k = t->comps - 0x40; t->bytes = 16; t->blockw = aw[k] ? aw[k] : 4; t->blockh = ah[k] ? ah[k] : 4; break; }
        return 0;
    }
    if (t->bl) t->pitch = ((t->w + t->blockw - 1) / t->blockw * t->bytes + 63) & ~63u;
    t->layer_stride = 0;
    t->base = t->bl ? (w[7] & 15) : 0;                                /* streamed textures: the view starts at a smaller mip (RES_VIEW_MIN_MIP_LEVEL) */
    { uint32_t clamp = (w[7] >> 12) & 0xFFF; uint32_t cl = (clamp + 255) >> 8; if (t->bl && cl > t->base) t->base = cl > t->maxmip ? t->maxmip : cl; }   /* MIN_LOD_CLAMP (4.8 fixed) */
    if (t->base > t->maxmip) t->base = 0;
    if (t->bl && t->type != 2) {                                     /* arrays and cubes: each layer holds its whole mip chain */
        uint64_t sum = 0;
        for (uint32_t l = 0; l <= t->maxmip && l < 16; l++) {
            uint32_t wl = t->w >> l ? t->w >> l : 1, hl = t->h >> l ? t->h >> l : 1;
            uint32_t bw = (wl + t->blockw - 1) / t->blockw, bhr = (hl + t->blockh - 1) / t->blockh, bh = 1u << t->bh_log2;
            while (bh > 1 && bhr <= 4 * bh) bh >>= 1;
            uint64_t pl = ((uint64_t)bw * t->bytes + 63) & ~63ull, rows = (bhr + 8 * bh - 1) / (8 * bh) * (8 * bh);
            sum += pl * rows;
        }
        uint64_t al = 512ull << t->bh_log2; t->layer_stride = (sum + al - 1) / al * al;
    }
    if (t->bl && t->base && (t->type != 2 || t->depth == 1)) {           /* move to the base level: skip the larger levels */
        uint64_t off = 0; uint32_t bh = 1u << t->bh_log2;
        for (uint32_t l = 0; l <= t->base; l++) {
            uint32_t wl = t->w >> l ? t->w >> l : 1, hl = t->h >> l ? t->h >> l : 1;
            uint32_t bw = (wl + t->blockw - 1) / t->blockw, bhr = (hl + t->blockh - 1) / t->blockh, b2 = 1u << t->bh_log2;
            while (b2 > 1 && bhr <= 4 * b2) b2 >>= 1;
            uint64_t pl = ((uint64_t)bw * t->bytes + 63) & ~63ull, rows = (bhr + 8 * b2 - 1) / (8 * b2) * (8 * b2);
            if (l == t->base) { bh = b2; break; }
            off += pl * rows;
        }
        uint32_t lg = 0; while ((1u << lg) < bh) lg++;
        t->addr += off; t->w = t->w >> t->base ? t->w >> t->base : 1; t->h = t->h >> t->base ? t->h >> t->base : 1; t->bh_log2 = lg;
        t->pitch = ((t->w + t->blockw - 1) / t->blockw * t->bytes + 63) & ~63u;
    }
    return t->addr && t->w && t->h;
}
static float soft_unorm(uint32_t v, int bits) { return (float)v / (float)((1u << bits) - 1); }
static void soft_bc_endpoints(uint16_t c0, uint16_t c1, float pal[4][4], int dxt1) {
    float a[3] = { ((c0 >> 11) & 31) / 31.0f, ((c0 >> 5) & 63) / 63.0f, (c0 & 31) / 31.0f };
    float b[3] = { ((c1 >> 11) & 31) / 31.0f, ((c1 >> 5) & 63) / 63.0f, (c1 & 31) / 31.0f };
    for (int i = 0; i < 3; i++) {
        pal[0][i] = a[i]; pal[1][i] = b[i];
        if (!dxt1 || c0 > c1) { pal[2][i] = (2 * a[i] + b[i]) / 3; pal[3][i] = (a[i] + 2 * b[i]) / 3; }
        else { pal[2][i] = (a[i] + b[i]) / 2; pal[3][i] = 0; }
    }
    for (int k = 0; k < 4; k++) pal[k][3] = 1;
    if (dxt1 && c0 <= c1) pal[3][3] = 0;
}
static float soft_bc_alpha(const uint8_t *b, int i) {     /* BC3/BC4 interpolated channel */
    uint32_t a0 = b[0], a1 = b[1];
    uint64_t bits = 0; for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
    uint32_t s = (bits >> (3 * i)) & 7;
    if (s == 0) return a0 / 255.0f; if (s == 1) return a1 / 255.0f;
    if (a0 > a1) return ((8 - s) * a0 + (s - 1) * a1) / 7.0f / 255.0f;
    if (s == 6) return 0; if (s == 7) return 1;
    return ((6 - s) * a0 + (s - 1) * a1) / 5.0f / 255.0f;
}
static void soft_texel_z(mgr_emu *e, const soft_tex *t, int x, int y, int z, float c[4]);
static void soft_texel(mgr_emu *e, const soft_tex *t, int x, int y, float c[4]) { soft_texel_z(e, t, x, y, 0, c); }
/* the sampler (TSC) of the texture instruction being run: address modes, border colour, depth compare */
typedef struct { int on, au, av, dc, func; float border[4], dref; } soft_smp;
static soft_smp g_smp;
static int soft_addr(int x, int n, int mode, int *border) {
    switch (mode) {
    case 0: x %= n; if (x < 0) x += n; return x;                                        /* WRAP */
    case 1: { int p = 2 * n; x %= p; if (x < 0) x += p; return x < n ? x : p - 1 - x; }  /* MIRROR */
    case 3: case 6: if (x < 0 || x >= n) { *border = 1; return 0; } return x;           /* BORDER */
    case 5: case 7: if (x < 0) x = -1 - x; return x >= n ? n - 1 : x;                  /* MIRROR_ONCE */
    default: return x < 0 ? 0 : x >= n ? n - 1 : x;                                     /* CLAMP */
    }
}
static int soft_zcmp_f(float a, float b, int f) {
    switch (f & 7) { case 0: return 0; case 1: return a < b; case 2: return a == b; case 3: return a <= b; case 4: return a > b; case 5: return a != b; case 6: return a >= b; default: return 1; }
}
static void soft_texel_z_raw(mgr_emu *e, const soft_tex *t, int x, int y, int z, float c[4]);
static void soft_texel_z(mgr_emu *e, const soft_tex *t, int x, int y, int z, float c[4]) {
    if (g_smp.on) {
        int bd = 0; x = soft_addr(x, (int)t->w, g_smp.au, &bd); y = soft_addr(y, (int)t->h, g_smp.av, &bd);
        if (bd) { memcpy(c, g_smp.border, 16); }
        else soft_texel_z_raw(e, t, x, y, z, c);
        if (g_smp.dc) { float r = soft_zcmp_f(g_smp.dref, c[0], g_smp.func) ? 1.0f : 0.0f; c[0] = c[1] = c[2] = c[3] = r; }
        return;
    }
    soft_texel_z_raw(e, t, x, y, z, c);
}
static void soft_texel_z_raw(mgr_emu *e, const soft_tex *t, int x, int y, int z, float c[4]) {
    if (x < 0) x = 0; if (y < 0) y = 0; if ((uint32_t)x >= t->w) x = (int)t->w - 1; if ((uint32_t)y >= t->h) y = (int)t->h - 1;
    uint32_t bx = (uint32_t)x / t->blockw, by = (uint32_t)y / t->blockh;
    uint64_t off;
    { uint32_t nz = (t->type == 3 || t->type == 8) ? t->depth * 6 : t->depth; if (z < 0) z = 0; if ((uint32_t)z >= nz) z = (int)nz - 1; }
    if (t->bl && t->type == 2 && t->depth > 1) {                     /* 3D: blocks are 2^bh x 2^bd GOBs deep */
        uint32_t bh = 1u << t->bh_log2, bd = 1u << t->bd_log2, rows = 8 * bh, xb = bx * t->bytes, gx = t->pitch / 64;
        uint32_t blocks_y = (t->h + rows - 1) / rows;
        uint64_t blk = ((uint64_t)((uint32_t)z / bd) * blocks_y + by / rows) * gx + xb / 64;
        uint32_t gob = ((uint32_t)z % bd) * bh + (by % rows) / 8;
        uint32_t in = (xb % 64 / 32) * 256 + (by % 8 / 2) * 64 + (xb % 32 / 16) * 32 + (by % 2) * 16 + xb % 16;
        off = blk * 512ull * bh * bd + gob * 512ull + in;
    } else {
        off = t->bl ? bl_offset(bx * t->bytes, by, t->pitch, 1u << t->bh_log2) : (uint64_t)by * t->pitch + bx * t->bytes;
        off += (uint64_t)z * t->layer_stride;
    }
    uint8_t b[16] = {0}; gread(e, t->addr + off, b, t->bytes);
    uint32_t u; memcpy(&u, b, 4);
    int ft = (t->types & 7) == 7;            /* red channel float? */
    c[0] = c[1] = c[2] = 0; c[3] = 1;
    switch (t->comps) {
    case 0x08: case 0x07: for (int i = 0; i < 4; i++) c[i] = b[i] / 255.0f; if (t->comps == 0x07) c[3] = 1; break;
    case 0x09: c[0] = soft_unorm(u & 1023, 10); c[1] = soft_unorm((u >> 10) & 1023, 10); c[2] = soft_unorm((u >> 20) & 1023, 10); c[3] = soft_unorm(u >> 30, 2); break;
    case 0x21: c[0] = soft_uf11(u & 2047, 6); c[1] = soft_uf11((u >> 11) & 2047, 6); c[2] = soft_uf11(u >> 22, 5); break;
    case 0x03: for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, b + i * 2, 2); c[i] = ft ? sx_half_to_f(h) : h / 65535.0f; } break;
    case 0x0c: for (int i = 0; i < 2; i++) { uint16_t h; memcpy(&h, b + i * 2, 2); c[i] = ft ? sx_half_to_f(h) : h / 65535.0f; } break;
    case 0x1b: { uint16_t h; memcpy(&h, b, 2); c[0] = ft ? sx_half_to_f(h) : h / 65535.0f; break; }
    case 0x01: memcpy(c, b, 16); break;
    case 0x04: memcpy(c, b, 8); break;
    case 0x0f: memcpy(c, b, 4); if (!ft) c[0] = (float)u; break;
    case 0x18: c[0] = b[0] / 255.0f; c[1] = b[1] / 255.0f; break;
    case 0x1d: c[0] = b[0] / 255.0f; break;
    case 0x15: { uint16_t h; memcpy(&h, b, 2); c[0] = soft_unorm(h >> 11, 5); c[1] = soft_unorm((h >> 5) & 63, 6); c[2] = soft_unorm(h & 31, 5); break; }
    case 0x24: case 0x25: case 0x26: {       /* BC1 / BC2 / BC3 */
        const uint8_t *cb = t->comps == 0x24 ? b : b + 8;
        uint16_t c0, c1; memcpy(&c0, cb, 2); memcpy(&c1, cb + 2, 2);
        float pal[4][4]; soft_bc_endpoints(c0, c1, pal, t->comps == 0x24);
        int i = (y & 3) * 4 + (x & 3);
        uint32_t idx; memcpy(&idx, cb + 4, 4); idx = (idx >> (2 * i)) & 3;
        for (int k = 0; k < 4; k++) c[k] = pal[idx][k];
        c[0] = pal[idx][0]; c[1] = pal[idx][1]; c[2] = pal[idx][2];
        if (t->comps == 0x25) c[3] = ((b[i / 2] >> ((i & 1) * 4)) & 15) / 15.0f;
        if (t->comps == 0x26) c[3] = soft_bc_alpha(b, i);
        break;
    }
    case 0x17: {                                                                               /* BC7 */
        static uint8_t cache[16][4]; static uint8_t last[16]; static int valid;
        if (!valid || memcmp(last, b, 16)) { bc7_decode(b, cache); memcpy(last, b, 16); valid = 1; }
        const uint8_t *q = cache[(y & 3) * 4 + (x & 3)]; for (int k = 0; k < 4; k++) c[k] = q[k] / 255.0f; break;
    }
    case 0x10: case 0x11: {                                                                    /* BC6H */
        static float cache[16 * 3]; static uint8_t last[16]; static int valid, lsg;
        int sg = t->comps == 0x10;
        if (!valid || lsg != sg || memcmp(last, b, 16)) { bcdec_bc6h_float(b, cache, 12, sg); memcpy(last, b, 16); valid = 1; lsg = sg; }
        const float *q = cache + ((y & 3) * 4 + (x & 3)) * 3; c[0] = q[0]; c[1] = q[1]; c[2] = q[2]; break;
    }
    case 0x2f: case 0x30: memcpy(&c[0], b, 4); c[1] = c[2] = c[0]; break;                    /* ZF32 (depth) */
    case 0x20: { float sc = ldexpf(1.0f, (int)(u >> 27) - 24); c[0] = (u & 511) * sc; c[1] = ((u >> 9) & 511) * sc; c[2] = ((u >> 18) & 511) * sc; break; }   /* E5B9G9R9 */
    case 0x3a: { uint16_t h; memcpy(&h, b, 2); c[0] = c[1] = c[2] = h / 65535.0f; break; }   /* Z16 */
    case 0x29: case 0x2a: case 0x2b: c[0] = c[1] = c[2] = (u & 0xFFFFFF) / 16777215.0f; break;
    case 0x27: c[0] = soft_bc_alpha(b, (y & 3) * 4 + (x & 3)); break;                       /* BC4 */
    case 0x28: c[0] = soft_bc_alpha(b, (y & 3) * 4 + (x & 3)); c[1] = soft_bc_alpha(b + 8, (y & 3) * 4 + (x & 3)); break; /* BC5 */
    case 0x40: case 0x41: case 0x42: case 0x44: case 0x45: case 0x46: case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: {                                                /* ASTC (LDR) */
        static uint16_t cache[144 * 4]; static uint8_t last[16]; static uint32_t lkey; static int valid;
        uint32_t key = t->blockw | t->blockh << 8 | t->srgb << 16;
        if (!valid || lkey != key || memcmp(last, b, 16)) { unsigned long long before = astc_err_count[3];
            astc_decode_block(b, (int)t->blockw, (int)t->blockh, (int)t->srgb, cache); memcpy(last, b, 16); lkey = key; valid = 1;
            if (astc_err_count[3] != before) { static uint64_t seen[8]; static int ns; int k; for (k = 0; k < ns && seen[k] != t->addr; k++) {}
                if (k == ns && ns < 8) { seen[ns++] = t->addr;
                    elog(e, "[soft]   ASTC bad block: tex %llx %ux%ux%u comps %x type %u base %u maxmip %u bh %u layer_stride %llx at %d,%d,%d off %llx: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
                         (unsigned long long)t->addr, t->w, t->h, t->depth, t->comps, t->type, t->base, t->maxmip, t->bh_log2, (unsigned long long)t->layer_stride, x, y, z, (unsigned long long)off,
                         b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]); } } }
        const uint16_t *q = cache + ((uint32_t)y % t->blockh * t->blockw + (uint32_t)x % t->blockw) * 4;
        if (t->srgb) for (int k = 0; k < 4; k++) c[k] = (q[k] >> 8) / 255.0f;
        else for (int k = 0; k < 4; k++) c[k] = q[k] / 65535.0f;
        if (t->srgb) c[3] = (q[3] >> 8) / 255.0f;
        break;
    }
    default: c[0] = c[1] = c[2] = 0.5f; g_soft_texfmt[t->comps & 127]++; break;                                 /* BC6H/BC7/ASTC: not decoded yet */
    }
    if (t->srgb) for (int i = 0; i < 3; i++) c[i] = soft_srgb_to_lin(c[i]);
}
static void soft_sample_z(mgr_emu *e, const soft_tex *t, float u, float v, int z, float out[4]) {
    float fx = u * t->w - 0.5f, fy = v * t->h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy); float ax = fx - x0, ay = fy - y0;
    float a[4], b[4], c[4], d[4];
    soft_texel_z(e, t, x0, y0, z, a); soft_texel_z(e, t, x0 + 1, y0, z, b); soft_texel_z(e, t, x0, y0 + 1, z, c); soft_texel_z(e, t, x0 + 1, y0 + 1, z, d);
    for (int i = 0; i < 4; i++) out[i] = (a[i] * (1 - ax) + b[i] * ax) * (1 - ay) + (c[i] * (1 - ax) + d[i] * ax) * ay;
}
static void soft_sample3(mgr_emu *e, const soft_tex *t, float u, float v, float w, float out[4]) {   /* trilinear across slices */
    float fz = w * t->depth - 0.5f; int z0 = (int)floorf(fz); float az = fz - z0; float a[4], b[4];
    soft_sample_z(e, t, u, v, z0, a); soft_sample_z(e, t, u, v, z0 + 1, b);
    for (int i = 0; i < 4; i++) out[i] = a[i] * (1 - az) + b[i] * az;
}
static void soft_cube(float x, float y, float z, int *face, float *u, float *v) {
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z), ma, sc, tc;
    if (ax >= ay && ax >= az) { ma = ax; *face = x >= 0 ? 0 : 1; sc = x >= 0 ? -z : z; tc = -y; }
    else if (ay >= az) { ma = ay; *face = y >= 0 ? 2 : 3; sc = x; tc = y >= 0 ? z : -z; }
    else { ma = az; *face = z >= 0 ? 4 : 5; sc = z >= 0 ? x : -x; tc = -y; }
    if (ma <= 0) ma = 1;
    *u = (sc / ma + 1) * 0.5f; *v = (tc / ma + 1) * 0.5f;
}
static void soft_sample(mgr_emu *e, const soft_tex *t, float u, float v, float out[4]) {
    float fx = u * t->w - 0.5f, fy = v * t->h - 0.5f;         /* bilinear, clamp to edge */
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy); float ax = fx - x0, ay = fy - y0;
    float a[4], b[4], c[4], d[4];
    soft_texel(e, t, x0, y0, a); soft_texel(e, t, x0 + 1, y0, b); soft_texel(e, t, x0, y0 + 1, c); soft_texel(e, t, x0 + 1, y0 + 1, d);
    for (int i = 0; i < 4; i++) out[i] = (a[i] * (1 - ax) + b[i] * ax) * (1 - ay) + (c[i] * (1 - ax) + d[i] * ax) * ay;
}
static void soft_swizzle(uint32_t swz, const float in[4], float out[4]) {
    for (int i = 0; i < 4; i++) {
        uint32_t s = (swz >> (3 * i)) & 7;
        out[i] = s == 2 ? in[0] : s == 3 ? in[1] : s == 4 ? in[2] : s == 5 ? in[3] : s == 6 || s == 7 ? 1.0f : 0.0f;
    }
}

/* ---------------- shader environment ---------------- */
typedef struct {
    sass_env env;
    mgr_emu *e; nx2_engine *en; int ch, group;
    uint32_t in[0x200 / 4], out[0x200 / 4];        /* attribute space (VS in / out) */
    const float *bary; const uint32_t (*vout[3])[0x200 / 4]; float invw[3]; float frag[4];
    int compute; uint64_t cba[8]; uint32_t cbs[8]; uint32_t tid[3], ctaid[3], lin;   /* compute dispatch */
} soft_ctx;
static uint32_t soft_cbuf(sass_env *ev, uint32_t idx, uint32_t off) {
    soft_ctx *c = (soft_ctx *)ev;
    if (c->compute) { if (idx >= 8 || !c->cba[idx] || off >= c->cbs[idx] + 0x10000) return 0; return gread32(c->e, c->cba[idx] + off); }
    if (idx >= 18) return 0;
    soft_cb *b = &g_cbs[c->ch][c->group][idx];
    if (!b->addr || off + 4 > b->size + 0x10000) return 0;
    return gread32(c->e, b->addr + off);
}
static uint32_t soft_vs_in(sass_env *ev, uint32_t a) { soft_ctx *c = (soft_ctx *)ev; return a < 0x200 ? c->in[a / 4] : 0; }
static void soft_vs_out(sass_env *ev, uint32_t a, uint32_t v) { soft_ctx *c = (soft_ctx *)ev; if (a < 0x200) c->out[a / 4] = v; }
static uint32_t soft_ps_in(sass_env *ev, uint32_t a) {         /* IPA: perspective-weighted attribute / w */
    soft_ctx *c = (soft_ctx *)ev;
    if (a >= 0x70 && a < 0x7c) return sx_u(c->frag[(a - 0x70) / 4]);
    if (a == 0x7c) return sx_u(c->bary[0] * c->invw[0] + c->bary[1] * c->invw[1] + c->bary[2] * c->invw[2]);
    if (a >= 0x200) return 0;
    float s = 0; for (int k = 0; k < 3; k++) s += c->bary[k] * c->invw[k] * sx_f((*c->vout[k])[a / 4]);
    return sx_u(s);
}
static uint32_t soft_sysreg(sass_env *ev, uint32_t i) {
    soft_ctx *c = (soft_ctx *)ev;
    if (!c->compute) return 0;
    uint32_t lane = c->lin & 31;
    switch (i) {
    case 0x00: return lane;
    case 0x20: return c->tid[0] | c->tid[1] << 16 | c->tid[2] << 26;
    case 0x21: case 0x22: case 0x23: return c->tid[i - 0x21];
    case 0x25: case 0x26: case 0x27: return c->ctaid[i - 0x25];
    case 0x38: return 1u << lane; case 0x39: return (1u << lane) - 1; case 0x3a: return (2u << lane) - 1;
    case 0x3b: return ~((2u << lane) - 1); case 0x3c: return ~((1u << lane) - 1);
    default: return 0;
    }
}
static uint32_t soft_memld(sass_env *ev, uint64_t a) { soft_ctx *c = (soft_ctx *)ev; return gread32(c->e, a); }
static int g_soft_scale = 0; static int g_log_tex;
static void soft_texop(sass_env *ev, uint32_t op, uint32_t handle, const uint32_t *co, int nc, uint32_t dim, uint32_t lodm, uint32_t out[4]) {
    soft_ctx *c = (soft_ctx *)ev; (void)lodm; (void)dim;
    soft_tex t; float px[4] = {0, 0, 0, 1}, sw[4];
    int k = 0;
    if (handle == 0xFFFFFFFFu) { handle = nc ? co[0] : 0; k = 1; }   /* TXQ bindless: handle first */
    int okt = soft_tex_get(c->e, c->en, handle, &t);
    if (okt && c->compute && g_cs_hmma) {                               /* upscaler inputs */
        static uint32_t seenh[32]; static int nh; int k; for (k = 0; k < nh && seenh[k] != handle; k++) {}
        if (k == nh && nh < 32) { seenh[nh++] = handle; float v[4], v2[4]; soft_texel(c->e, &t, (int)t.w / 2, (int)t.h / 2, v); soft_texel(c->e, &t, (int)t.w / 3, (int)t.h / 3, v2);
            elog(c->e, "[soft]   upscaler reads %x: %llx %ux%u comps %x types %x (op %x dim %u) centre %g %g %g %g third %g %g %g %g\n", handle, (unsigned long long)t.addr, t.w, t.h, t.comps, t.types, op, dim,
                 v[0], v[1], v[2], v[3], v2[0], v2[1], v2[2], v2[3]);
            if (t.w >= 512 && t.blockw == 1) {                          /* where are the bad (huge) input pixels? 32x18 map */
                char map[18][33]; uint64_t nbad = 0, nbig = 0; double sum = 0; uint64_t nsum = 0;
                for (int gy = 0; gy < 18; gy++) { for (int gx = 0; gx < 32; gx++) {
                    int bad = 0, tot = 0;
                    for (uint32_t y = gy * t.h / 18; y < (gy + 1) * t.h / 18; y += 3) for (uint32_t x = gx * t.w / 32; x < (gx + 1) * t.w / 32; x += 3) {
                        float p[4]; soft_texel(c->e, &t, (int)x, (int)y, p); float m = fmaxf(p[0], fmaxf(p[1], p[2])); tot++;
                        if (!(m < 1000)) { bad++; nbad++; } else { if (m > 10) nbig++; sum += m; nsum++; } }
                    map[gy][gx] = bad == 0 ? '.' : bad * 10 < tot ? 'o' : bad * 2 < tot ? 'O' : '#'; }
                    map[gy][32] = 0; }
                elog(c->e, "[soft]   upscaler input %llx: %llu huge (>=1000), %llu >10, mean of the rest %g; map ('#' mostly huge):\n", (unsigned long long)t.addr, (unsigned long long)nbad, (unsigned long long)nbig, nsum ? sum / nsum : 0);
                for (int gy = 0; gy < 18; gy++) elog(c->e, "[soft]     %s\n", map[gy]);
            } }
    }
    if (okt && c->compute && (uint64_t)t.w * t.h <= 64) {             /* tiny textures in compute (exposure and the like) */
        static uint64_t seen[64]; static int ns; int k; for (k = 0; k < ns && seen[k] != t.addr; k++) {}
        if (k == ns && ns < 64) { seen[ns++] = t.addr; float v[4]; soft_texel(c->e, &t, 0, 0, v);
            elog(c->e, "[soft]   compute reads small texture%llu  %llx %ux%u comps %x: %g %g %g %g\n", (unsigned long long)0, (unsigned long long)t.addr, t.w, t.h, t.comps, v[0], v[1], v[2], v[3]); }
    }
    if (okt && c->compute && g_cs_track && t.blockw == 1 && (uint64_t)t.w * t.h > (uint64_t)g_cs_in_big.w * g_cs_in_big.h) g_cs_in_big = t;
    if (g_log_tex > 0 && okt && t.w >= 1024 && t.blockw == 1) {          /* dump the big textures the final passes read */
        static uint64_t dumped[8], dframe[8]; static int nd; int k; for (k = 0; k < nd && !(dumped[k] == t.addr && dframe[k] == g_soft_t0_id); k++) {}
        if (k == nd) { if (nd == 8) nd = 0; dframe[nd] = g_soft_t0_id; dumped[nd++] = t.addr;
            uint32_t ow = t.w / 2, oh = t.h / 2; uint8_t *rgb = malloc((size_t)ow * oh * 3); float *fl = malloc((size_t)ow * oh * 3 * 4);
            if (rgb && fl) {
                float mx[4096]; int nm = 0;
                for (uint32_t y = 0; y < oh; y++) for (uint32_t x = 0; x < ow; x++) { float px[4]; soft_texel(c->e, &t, (int)x * 2, (int)y * 2, px);
                    for (int i = 0; i < 3; i++) fl[((size_t)y * ow + x) * 3 + i] = isfinite(px[i]) ? px[i] : 0; }
                for (size_t i = 0; i < (size_t)ow * oh && nm < 4096; i += (size_t)ow * oh / 4096 + 1) { float m = fmaxf(fl[i * 3], fmaxf(fl[i * 3 + 1], fl[i * 3 + 2])); if (m > 0) mx[nm++] = m; }
                float ex = 1; if (nm) { for (int i = 1; i < nm; i++) { float v = mx[i]; int j = i - 1; while (j >= 0 && mx[j] > v) { mx[j + 1] = mx[j]; j--; } mx[j + 1] = v; } ex = 1.0f / fmaxf(fminf(mx[nm * 98 / 100], 6.0f * mx[nm / 2]), 1e-12f); }   /* bright highlights (the sun) don't set the exposure */
                for (size_t i = 0; i < (size_t)ow * oh * 3; i++) { float v = soft_lin_to_srgb(fl[i] * ex); rgb[i] = (uint8_t)(fminf(fmaxf(v, 0), 1) * 255 + 0.5f); }
                char path[600]; const char *dir = getenv("MGR_DUMP_DIR") ? getenv("MGR_DUMP_DIR") : "/tmp/mgr_dump/frames";
                snprintf(path, sizeof path, "%s/soft_tex_f%llu_%llx_%ux%u_comps%x.png", dir, (unsigned long long)g_soft_t0_id, (unsigned long long)t.addr, t.w, t.h, t.comps);
                png_write_rgb(path, rgb, ow, oh); elog(c->e, "[soft]   dumped texture %s (98th percentile value %g)\n", path, 1.0f / ex);
            }
            free(rgb); free(fl);
        }
    }
    if (g_log_tex > 0) { g_log_tex--; float cv[4] = {0, 0, 0, 0}; if (okt) soft_texel(c->e, &t, (int)t.w / 2, (int)t.h / 2, cv);
        elog(c->e, "[soft]   final-pass texture %x (op %x dim %u): %s addr %llx %ux%ux%u type %u comps %x types %x swz %x bl %u bh %u; centre texel %g %g %g %g; coords %08x %08x %08x\n", handle, op, dim, okt ? "ok" : "unusable",
             (unsigned long long)t.addr, t.w, t.h, t.depth, t.type, t.comps, t.types, t.swz, t.bl, t.bh_log2, cv[0], cv[1], cv[2], cv[3], nc > 0 ? co[0] : 0, nc > 1 ? co[1] : 0, nc > 2 ? co[2] : 0); }
    if (g_soft_draws <= 3) { static uint64_t last; if (last != g_soft_draws) { last = g_soft_draws;
        elog(c->e, "[soft]   draw %llu tex handle %x: %s addr %llx %ux%u comps %x types %x swz %x bl %u\n", (unsigned long long)g_soft_draws, handle, okt ? "ok" : "unusable",
             (unsigned long long)t.addr, t.w, t.h, t.comps, t.types, t.swz, t.bl); } }
    if (op == 0xb6f || op == 0x370) {                                   /* TXQ: dimensions at the given level (integers) */
        uint32_t lod = nc > k ? co[k] : 0;
        if (!okt) { out[0] = out[1] = out[2] = out[3] = 0; return; }
        if (dim == 0) { out[0] = t.w >> lod ? t.w >> lod : 1; out[1] = t.h >> lod ? t.h >> lod : 1; out[2] = 1; out[3] = 1; }
        else { out[0] = out[1] = out[2] = out[3] = 0; }
        return;
    }
    int is_fetch = op == 0xb66 || op == 0xd67 || op == 0x367;
    if (okt && !is_fetch) {                                              /* sampler state for this lookup */
        nx2_engine *en = c->en; uint64_t sp = ((uint64_t)(R3(0x155c) & 0xFF) << 32) | R3(0x1560);
        uint32_t sw2[8] = {0}; if (sp) gread(c->e, sp + (uint64_t)((handle >> 20) & 0xFFF) * 32, sw2, 32);
        g_smp.on = sp != 0; g_smp.au = (int)(sw2[0] & 7); g_smp.av = (int)((sw2[0] >> 3) & 7);
        g_smp.dc = (int)((lodm >> 8) & 1); g_smp.func = (int)((sw2[0] >> 10) & 7); if (!((sw2[0] >> 9) & 1) && g_smp.dc) g_smp.func = 3;
        for (int i = 0; i < 4; i++) memcpy(&g_smp.border[i], &sw2[4 + i], 4);
        g_smp.dref = g_smp.dc ? sx_f(co[6]) : 0;                         /* the executor puts the reference in slot 6 */
    }
    if (okt && (op == 0xb63 || op == 0xd64 || op == 0x364)) {           /* TLD4: gather the red channel of the 2x2 footprint */
        float u = nc > k ? sx_f(co[k]) : 0, v = nc > k + 1 ? sx_f(co[k + 1]) : 0, fx = u * t.w - 0.5f, fy = v * t.h - 0.5f;
        int x0 = (int)floorf(fx), y0 = (int)floorf(fy); float a[4], b2[4], cc[4], d[4];
        soft_texel(c->e, &t, x0, y0 + 1, a); soft_texel(c->e, &t, x0 + 1, y0 + 1, b2); soft_texel(c->e, &t, x0 + 1, y0, cc); soft_texel(c->e, &t, x0, y0, d);
        { int cs = (int)(lodm & 3); float A[4], B[4], C[4], D[4]; soft_swizzle(t.swz, a, A); soft_swizzle(t.swz, b2, B); soft_swizzle(t.swz, cc, C); soft_swizzle(t.swz, d, D);
          out[0] = sx_u(A[cs]); out[1] = sx_u(B[cs]); out[2] = sx_u(C[cs]); out[3] = sx_u(D[cs]); } g_smp.on = 0; return;
    }
    if (okt) {
        #define CO(i) (nc > k + (i) ? co[k + (i)] : 0u)
        int arr = dim == 4 || dim == 5 || dim == 7;                     /* TEX dim: 0 1D, 1 2D, 2 3D, 3 cube, 4/5/7 arrays */
        int kk = arr ? 1 : 0; uint32_t layer = arr ? CO(0) & 0xFFFF : 0;
        if (op == 0xb66 || op == 0xd67 || op == 0x367) {               /* TLD: integer coords */
            if (dim == 2) soft_texel_z(c->e, &t, (int)CO(0), (int)CO(1), (int)CO(2), px);
            else soft_texel_z(c->e, &t, (int)CO(kk), dim == 0 || dim == 4 ? 0 : (int)CO(kk + 1), (int)layer, px);
        } else if (dim == 2) soft_sample3(c->e, &t, sx_f(CO(0)), sx_f(CO(1)), sx_f(CO(2)), px);
        else if (dim == 3 || dim == 7) { int f; float uu, vv; soft_cube(sx_f(CO(kk)), sx_f(CO(kk + 1)), sx_f(CO(kk + 2)), &f, &uu, &vv); soft_sample_z(c->e, &t, uu, vv, (int)layer * 6 + f, px); }
        else soft_sample_z(c->e, &t, sx_f(CO(kk)), dim == 0 || dim == 4 ? 0.5f : sx_f(CO(kk + 1)), (int)layer, px);
        #undef CO
        soft_swizzle(t.swz, px, sw);
    } else { sw[0] = sw[1] = sw[2] = 1; sw[3] = 1; }
    g_smp.on = 0;
    for (int i = 0; i < 4; i++) out[i] = sx_u(sw[i]);
}

static int soft_load_code(mgr_emu *e, uint64_t a, uint32_t **code, uint32_t *bytes, sass_ins **decp);
static int soft_load_program(mgr_emu *e, nx2_engine *en, int stage, uint32_t **code, uint32_t *bytes, sass_ins **decp) {
    if (!(R3(0x2000 + stage * 64) & 1)) return 0;
    uint64_t a = ((uint64_t)(R3(0x2014 + stage * 64) & 0xFF) << 32) | R3(0x2018 + stage * 64);
    return soft_load_code(e, a + 0x80, code, bytes, decp);
}
static int soft_load_code(mgr_emu *e, uint64_t a, uint32_t **code, uint32_t *bytes, sass_ins **decp) {   /* a: first instruction */
    static uint64_t cache_addr[64]; static uint32_t *cache_code[64], cache_len[64]; static sass_ins *cache_dec[64]; static int ncache, next;
    for (int i = 0; i < ncache; i++) if (cache_addr[i] == a) { *code = cache_code[i]; *bytes = cache_len[i]; *decp = cache_dec[i]; return 1; }
    uint32_t cap = 0x10000; uint32_t *buf = malloc(cap);
    if (!buf) return 0;
    uint32_t n = 0;
    for (; n + 16 <= cap; n += 16) {
        gread(e, a + n, buf + n / 4, 16);
        if ((buf[n / 4] & 0xFFF) == 0x947 && (sass_bits(buf + n / 4, 34, 82) == ((1ull << 48) - 4) || sass_bits(buf + n / 4, 34, 82) == ((1ull << 48) - 16))) { n += 16; break; }
    }
    int slot = ncache < 64 ? ncache++ : next++ % 64;
    if (cache_code[slot]) { free(cache_code[slot]); free(cache_dec[slot]); }
    sass_ins *dec = calloc(n / 16 + 1, sizeof *dec);
    if (dec) for (uint32_t i = 0; i < n / 16; i++) sass_decode(buf + i * 4, &dec[i]);
    cache_addr[slot] = a; cache_code[slot] = buf; cache_len[slot] = n; cache_dec[slot] = dec;
    { const char *dd = getenv("MGR_SHADER_DUMP"); if (dd) {                   /* corpus for the Metal translator tests */
        char path[600]; snprintf(path, sizeof path, "%s/sh_%llx.bin", dd, (unsigned long long)a);
        FILE *f = fopen(path, "rb"); if (f) fclose(f); else if ((f = fopen(path, "wb"))) { fwrite(buf, 1, n, f); fclose(f); } } }
    *code = buf; *bytes = n; *decp = dec; return 1;
}

/* vertex attribute fetch (SET_VERTEX_ATTRIBUTE_A / SET_VERTEX_STREAM_A_*) */
static void soft_fetch(mgr_emu *e, nx2_engine *en, uint32_t vtx, uint32_t *in) {
    for (int i = 0; i < 32; i++) {
        uint32_t a = R3(0x1160 + i * 4);
        float f[4] = {0, 0, 0, 1};
        uint32_t *o = in + (0x80 + i * 16) / 4;
        if ((a >> 6) & 1) { memcpy(o, f, 16); continue; }
        uint32_t st = a & 31, off = (a >> 7) & 0x3FFF, wd = (a >> 21) & 63, ty = (a >> 27) & 7;
        uint32_t fmtw = R3(0x1c00 + st * 16);
        uint64_t base = ((uint64_t)(R3(0x1c04 + st * 16) & 0xFF) << 32) | R3(0x1c08 + st * 16);
        if (!((fmtw >> 12) & 1) || !base) { memcpy(o, f, 16); continue; }
        uint8_t b[16] = {0}; gread(e, base + (uint64_t)vtx * (fmtw & 0xFFF) + off, b, 16);
        int n = 4, bits = 32;
        switch (wd) { case 0x01: n = 4; bits = 32; break; case 0x02: n = 3; bits = 32; break; case 0x04: n = 2; bits = 32; break; case 0x12: n = 1; bits = 32; break;
        case 0x03: n = 4; bits = 16; break; case 0x05: n = 3; bits = 16; break; case 0x0f: n = 2; bits = 16; break; case 0x1b: n = 1; bits = 16; break;
        case 0x0a: case 0x2f: case 0x33: n = 4; bits = 8; break; case 0x13: n = 3; bits = 8; break; case 0x18: case 0x32: n = 2; bits = 8; break; case 0x1d: n = 1; bits = 8; break;
        case 0x30: { uint32_t u; memcpy(&u, b, 4); f[0] = (u & 1023) / 1023.0f; f[1] = ((u >> 10) & 1023) / 1023.0f; f[2] = ((u >> 20) & 1023) / 1023.0f; f[3] = (u >> 30) / 3.0f; memcpy(o, f, 16); continue; }
        case 0x31: { uint32_t u; memcpy(&u, b, 4); f[0] = soft_uf11(u & 2047, 6); f[1] = soft_uf11((u >> 11) & 2047, 6); f[2] = soft_uf11(u >> 22, 5); memcpy(o, f, 16); continue; }
        default: memcpy(o, f, 16); continue; }
        for (int k = 0; k < n; k++) {
            uint32_t raw = bits == 32 ? ((uint32_t *)b)[k] : bits == 16 ? ((uint16_t *)b)[k] : b[k];
            float v;
            switch (ty) {
            case 7: v = bits == 32 ? sx_f(raw) : bits == 16 ? sx_half_to_f((uint16_t)raw) : (float)raw; break;
            case 2: v = (float)raw / (float)(bits == 32 ? 4294967295.0 : (double)((1u << bits) - 1)); break;
            case 1: { int32_t s = bits == 32 ? (int32_t)raw : bits == 16 ? (int16_t)raw : (int8_t)raw; v = fmaxf((float)s / (float)((1u << (bits - 1)) - 1), -1.0f); break; }
            case 3: { int32_t s = bits == 32 ? (int32_t)raw : bits == 16 ? (int16_t)raw : (int8_t)raw; memcpy(&v, &s, 4); if (0) v = 0; break; }
            case 4: memcpy(&v, &raw, 4); break;
            case 5: v = (float)raw; break;
            case 6: v = (float)(bits == 16 ? (int16_t)raw : bits == 8 ? (int8_t)raw : (int32_t)raw); break;
            default: v = 0;
            }
            if (ty == 3 || ty == 4) o[k] = (ty == 3 && bits < 32) ? (uint32_t)(bits == 16 ? (int16_t)raw : (int8_t)raw) : raw;
            else o[k] = sx_u(v);
        }
        for (int k = n; k < 4; k++) o[k] = sx_u(k == 3 ? 1.0f : 0.0f);
        if ((a >> 31) & 1) { uint32_t tmp = o[0]; o[0] = o[2]; o[2] = tmp; }
    }
}


/* ---------------- per-draw fixed-function state (clc797.h) ---------------- */
/* depth target (SET_ZT_*): read and written in guest memory so later passes can sample it */
static int soft_zb_get(nx2_engine *en, soft_rt *z) {
    if (!(R3(0x1538) & 1)) return 0;                                      /* SET_ZT_SELECT: no depth target */
    z->addr = ((uint64_t)(R3(0xfe0) & 0xFF) << 32) | R3(0xfe4);
    z->w = R3(0x1228) & 0xFFFFFFF; z->h = R3(0x122c) & 0x3FFFF; z->fmt = R3(0xfe8) & 0xFF;
    z->bpp = z->fmt == 0x13 ? 2 : (z->fmt == 0x19 || (z->fmt >= 0x1d && z->fmt <= 0x1f)) ? 8 : 4;
    z->bh = 1u << ((R3(0xfec) >> 4) & 7);
    z->pitch = (z->w * z->bpp + 63) & ~63u;
    return z->addr && z->w && z->h && z->w <= 16384 && z->h <= 16384;
}
static float soft_zget(const soft_rt *z, const uint8_t *p) {
    uint32_t u; memcpy(&u, p, 4);
    if (z->fmt == 0x0a || z->fmt == 0x19 || z->fmt == 0x1e || z->fmt == 0x1f) return sx_f(u);
    if (z->fmt == 0x13) return (u & 0xFFFF) / 65535.0f;
    return (u & 0xFFFFFF) / 16777215.0f;
}
static void soft_zput(const soft_rt *z, uint8_t *p, float v) {
    if (z->fmt == 0x0a || z->fmt == 0x19 || z->fmt == 0x1e || z->fmt == 0x1f) { memcpy(p, &v, 4); return; }
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    if (z->fmt == 0x13) { uint16_t h = (uint16_t)(v * 65535 + .5f); memcpy(p, &h, 2); return; }
    uint32_t u; memcpy(&u, p, 4); u = (u & 0xFF000000u) | (uint32_t)(v * 16777215 + .5f); memcpy(p, &u, 4);
}
typedef struct {
    int reg[8], mask[8];          /* shader output register base and component mask per colour output */
    uint32_t cw[8];               /* SET_CT_WRITE */
    int blend[8]; uint32_t bop[8][6];  /* colour op, src, dst, alpha op, src, dst */
    float bconst[4];
    int depth_reg;                /* shader-written depth, or -1 */
    soft_rt zb; int zon, zfunc, zwrite;
} soft_state;
/* per-frame statistics by draw state, for the largest targets (MGR_SOFT_STATS) */
typedef struct { uint32_t key; uint64_t draws, tests, fails, shaded, killed, black; float maxc, zmin, zmax, dmin, dmax; uint32_t w, h; uint64_t zaddr, rtaddr; } soft_stat;
static soft_stat g_sstat[96]; static int g_nsstat;
static soft_stat *g_cur_stat;
static uint64_t g_zwritten[64]; static int g_nzwritten; static uint64_t g_soft_zequal_fallback;
static int soft_zwritten(uint64_t a) { for (int i = 0; i < g_nzwritten; i++) if (g_zwritten[i] == a) return 1; return 0; }
static void soft_zmark(uint64_t a) { if (!soft_zwritten(a) && g_nzwritten < 64) g_zwritten[g_nzwritten++] = a; }
static int soft_zcmp(int f, float z, float d) {
    /* EQUAL / LEQUAL / GEQUAL get a small tolerance: a depth pre-pass and the pass that matches it
       may use different vertex shaders, whose positions the interpreter does not reproduce bit-exactly */
    static float eps = -1; if (eps < 0) eps = getenv("MGR_SOFT_ZEPS") ? (float)atof(getenv("MGR_SOFT_ZEPS")) : 1e-5f;
    switch (f) { case 0: return 0; case 1: return z < d; case 2: return fabsf(z - d) <= eps; case 3: return z <= d + eps;
    case 4: return z > d; case 5: return z != d; case 6: return z >= d - eps; default: return 1; }
}
static void soft_state_get(mgr_emu *e, nx2_engine *en, soft_state *st) {
    memset(st, 0, sizeof *st);
    uint64_t pa = ((uint64_t)(R3(0x2014 + 5 * 64) & 0xFF) << 32) | R3(0x2018 + 5 * 64);
    uint32_t sph[20] = {0}; if (R3(0x2000 + 5 * 64) & 1) gread(e, pa, sph, sizeof sph);
    int base = 0;                                                          /* SPH OmapTarget: 4 bits per output */
    for (int i = 0; i < 8; i++) { st->mask[i] = (sph[18] >> (i * 4)) & 15; st->reg[i] = base; if (st->mask[i]) base += 4; }
    st->depth_reg = (sph[19] >> 1) & 1 ? base + 1 : -1;
    int any = 0; for (int i = 0; i < 8; i++) any |= R3(0x1a00 + i * 4) != 0;
    int single = R3(0xf90) & 1;
    for (int i = 0; i < 8; i++) st->cw[i] = !any ? 0x1111 : R3(0x1a00 + (single ? 0 : i) * 4) & 0x1111;
    int per = R3(0x12e4) & 1;
    for (int i = 0; i < 8; i++) {
        st->blend[i] = R3(0x1360 + i * 4) & 1;
        if (per) { uint32_t b = 0x1e00 + i * 32; uint32_t sep = R3(b) & 1;
            st->bop[i][0] = R3(b + 4); st->bop[i][1] = R3(b + 8); st->bop[i][2] = R3(b + 12);
            st->bop[i][3] = sep ? R3(b + 16) : st->bop[i][0]; st->bop[i][4] = sep ? R3(b + 20) : st->bop[i][1]; st->bop[i][5] = sep ? R3(b + 24) : st->bop[i][2]; }
        else { uint32_t sep = R3(0x133c) & 1;
            st->bop[i][0] = R3(0x1340); st->bop[i][1] = R3(0x1344); st->bop[i][2] = R3(0x1348);
            st->bop[i][3] = sep ? R3(0x134c) : st->bop[i][0]; st->bop[i][4] = sep ? R3(0x1350) : st->bop[i][1]; st->bop[i][5] = sep ? R3(0x1358) : st->bop[i][2]; }
    }
    for (int i = 0; i < 4; i++) st->bconst[i] = sx_f(R3(0x131c + i * 4));
    if (R3(0x12cc) & 1) {
        uint32_t f = R3(0x130c); st->zfunc = f >= 0x200 ? (int)(f - 0x200) & 7 : (int)(f - 1) & 7;
        st->zwrite = R3(0x12e8) & 1;
        st->zon = soft_zb_get(en, &st->zb);
        /* An EQUAL pass whose depth buffer nothing has drawn into this frame: its pre-pass reached it
           by a route not reproduced here, so test and write it like an ordinary depth pass instead. */
        if (st->zon && st->zfunc == 2 && (!soft_zwritten(st->zb.addr) || soft_zwritten(st->zb.addr ^ 1))) {
            st->zfunc = 3; st->zwrite = 1; g_soft_zequal_fallback++; soft_zmark(st->zb.addr ^ 1);   /* addr^1 marks a fallback buffer */
        }
        if (st->zon && st->zwrite) soft_zmark(st->zb.addr);
    }
}
/* blend factor (OGL 0x4000.. / 0xC00x codes, or D3D 1..15) for one channel */
static float soft_bfac(uint32_t code, const float *s, const float *d, const float *k, int ch) {
    switch (code) {
    case 0x4000: case 1: return 0; case 0x4001: case 2: return 1;
    case 0x4300: case 3: return s[ch]; case 0x4301: case 4: return 1 - s[ch];
    case 0x4302: case 5: return s[3]; case 0x4303: case 6: return 1 - s[3];
    case 0x4304: case 7: return d[3]; case 0x4305: case 8: return 1 - d[3];
    case 0x4306: case 9: return d[ch]; case 0x4307: case 10: return 1 - d[ch];
    case 0x4308: case 11: return ch == 3 ? 1 : fminf(s[3], 1 - d[3]);
    case 0xC001: case 14: return k[ch]; case 0xC002: case 15: return 1 - k[ch];
    case 0xC003: return k[3]; case 0xC004: return 1 - k[3];
    default: return 1;
    }
}
static float soft_bop(uint32_t op, float a, float b) {
    switch (op) { case 0x800A: case 2: return a - b; case 0x800B: case 3: return b - a;
    case 0x8007: case 4: return fminf(a, b); case 0x8008: case 5: return fmaxf(a, b); default: return a + b; }
}

typedef struct { uint32_t out[0x200 / 4]; float sx, sy, sz, invw; int ok; } soft_vert;

static void soft_trace_fwd(sass_env *ev, void *tv, uint32_t pc, const sass_ins *in);
static void soft_trace_reset(void);
static const uint32_t *g_tr_code_fwd;
static void soft_tri(mgr_emu *e, nx2_engine *en, int ch, const soft_vert *v0, const soft_vert *v1, const soft_vert *v2,
                     uint32_t *ps, uint32_t ps_bytes, const sass_ins *psd, soft_rt *rts, int nrt, const soft_state *st) {
    if (!v0->ok || !v1->ok || !v2->ok) return;
    float area = (v1->sx - v0->sx) * (v2->sy - v0->sy) - (v2->sx - v0->sx) * (v1->sy - v0->sy);
    if (fabsf(area) < 1e-12f) return;
    float minx = fminf(v0->sx, fminf(v1->sx, v2->sx)), maxx = fmaxf(v0->sx, fmaxf(v1->sx, v2->sx));
    float miny = fminf(v0->sy, fminf(v1->sy, v2->sy)), maxy = fmaxf(v0->sy, fmaxf(v1->sy, v2->sy));
    int x0 = (int)fmaxf(floorf(minx), 0), x1 = (int)fminf(ceilf(maxx), (float)rts[0].w - 1);
    int y0 = (int)fmaxf(floorf(miny), 0), y1 = (int)fminf(ceilf(maxy), (float)rts[0].h - 1);
    if (x1 < x0 || y1 < y0) return;
    if ((uint64_t)(x1 - x0 + 1) * (uint64_t)(y1 - y0 + 1) > 8000000ull) return;
    static soft_ctx c;
    memset(&c.env, 0, sizeof c.env);
    c.env.cbuf = soft_cbuf; c.env.attr_in = soft_ps_in; c.env.attr_out = soft_vs_out; c.env.sysreg = soft_sysreg; c.env.tex = soft_texop; c.env.mem_ld = soft_memld;
    c.e = e; c.en = en; c.ch = ch; c.group = 4;
    c.vout[0] = &v0->out; c.vout[1] = &v1->out; c.vout[2] = &v2->out;
    c.invw[0] = v0->invw; c.invw[1] = v1->invw; c.invw[2] = v2->invw;
    static sass_thread th;
    if (!g_soft_scale) g_soft_scale = getenv("MGR_SOFT_SCALE") ? atoi(getenv("MGR_SOFT_SCALE")) : 1;
    int sc = g_soft_scale < 1 ? 1 : g_soft_scale > 8 ? 8 : g_soft_scale;     /* shade one pixel per sc x sc block */
    for (int y = y0 - y0 % sc; y <= y1; y += sc)
        for (int x = x0 - x0 % sc; x <= x1; x += sc) {
            float px = x + sc * 0.5f, py = y + sc * 0.5f;
            float w0 = ((v1->sx - px) * (v2->sy - py) - (v2->sx - px) * (v1->sy - py)) / area;
            float w1 = ((v2->sx - px) * (v0->sy - py) - (v0->sx - px) * (v2->sy - py)) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float bary[3] = { w0, w1, w2 };
            float fz = w0 * v0->sz + w1 * v1->sz + w2 * v2->sz;
            c.bary = bary; c.frag[0] = px; c.frag[1] = py; c.frag[2] = fz; c.frag[3] = 1;
            uint8_t *zp = NULL;
            if (st->zon) {
                if ((uint32_t)x < st->zb.w && (uint32_t)y < st->zb.h) zp = soft_rt_ptr(e, &st->zb, (uint32_t)x, (uint32_t)y);
                if (zp && st->depth_reg < 0) { g_soft_ztests++; if (g_cur_stat) { float dd = soft_zget(&st->zb, zp); g_cur_stat->tests++; g_cur_stat->zmin = fminf(g_cur_stat->zmin, fz); g_cur_stat->zmax = fmaxf(g_cur_stat->zmax, fz); g_cur_stat->dmin = fminf(g_cur_stat->dmin, dd); g_cur_stat->dmax = fmaxf(g_cur_stat->dmax, dd); } if (!soft_zcmp(st->zfunc, fz, soft_zget(&st->zb, zp))) { g_soft_zfail++; if (g_cur_stat) g_cur_stat->fails++; continue; } }   /* early depth test */
            }
            memset(&th, 0, offsetof(sass_thread, local));
            static uint64_t wrt = 1; static int wx, wy; if (wrt == 1) { wrt = 0; const char *ws = getenv("MGR_SOFT_WATCH"); if (ws) sscanf(ws, "%llx,%d,%d", (unsigned long long *)&wrt, &wx, &wy); }
            int watch = wrt && rts[0].addr == wrt && wx >= x && wx < x + sc && wy >= y && wy < y + sc;
            if (watch) g_log_tex = 8;
            static long wdraw = -2; if (wdraw == -2) wdraw = getenv("MGR_SOFT_WATCH_DRAW") ? atol(getenv("MGR_SOFT_WATCH_DRAW")) : -1;
            static int wtn; int wtrace = watch && ((long)g_soft_draws == wdraw || (wdraw == -5 && st->blend[0] && wtn < 3)); if (wtrace) wtn++;
            if (wtrace) { c.env.trace = soft_trace_fwd; c.env.trace_t = &th; g_tr_code_fwd = ps; soft_trace_reset(); elog(e, "[soft] tracing pixel shader of draw %ld\n", wdraw); }
            if (ps) sass_run_dec(&th, &c.env, ps, psd, ps_bytes, 0);
            if (wtrace) { soft_trace_fwd(&c.env, &th, 0xFFFFFFFFu, 0); c.env.trace = 0; }
            if (watch) { g_log_tex = 0; int rb = st->reg[0];
                elog(e, "[soft] WATCH draw %llu px %d,%d: out %g %g %g %g killed %d blend %d cw %x z %g ps-first %08x %08x\n", (unsigned long long)g_soft_draws, wx, wy,
                     sx_f(th.r[rb]), sx_f(th.r[rb + 1]), sx_f(th.r[rb + 2]), sx_f(th.r[rb + 3]), th.killed, st->blend[0], st->cw[0], fz, ps ? ps[0] : 0, ps ? ps[1] : 0); }
            if (th.killed) { if (g_cur_stat) g_cur_stat->killed++; continue; }
            if (g_cur_stat) { g_cur_stat->shaded++; int rb = st->reg[0]; float m = fmaxf(fmaxf(sx_f(th.r[rb]), sx_f(th.r[rb + 1])), sx_f(th.r[rb + 2]));
                if (!(m > 0)) g_cur_stat->black++; else if (m > g_cur_stat->maxc) g_cur_stat->maxc = m; }
            if (zp && st->depth_reg >= 0) { fz = sx_f(th.r[st->depth_reg & 255]); g_soft_ztests++; if (!soft_zcmp(st->zfunc, fz, soft_zget(&st->zb, zp))) { g_soft_zfail++; continue; } }
            if (zp && st->zwrite)
                for (int dy = 0; dy < sc; dy++) for (int dx = 0; dx < sc; dx++) {
                    uint32_t xx = (uint32_t)(x + dx), yy = (uint32_t)(y + dy);
                    uint8_t *q = xx < st->zb.w && yy < st->zb.h ? soft_rt_ptr(e, &st->zb, xx, yy) : NULL;
                    if (q) soft_zput(&st->zb, q, fz);
                }
            for (int r = 0; r < nrt; r++) {
                if (!rts[r].addr || !st->mask[r] || !st->cw[r]) continue;
                int rb = st->reg[r];
                float col[4] = { sx_f(th.r[rb]), sx_f(th.r[rb + 1]), sx_f(th.r[rb + 2]), sx_f(th.r[rb + 3]) };
                int plain = !st->blend[r] && st->cw[r] == 0x1111;
                uint8_t px8[16]; if (plain) soft_pack(rts[r].fmt, col, px8);
                for (int dy = 0; dy < sc; dy++) for (int dx = 0; dx < sc; dx++) {
                    uint32_t xx = (uint32_t)(x + dx), yy = (uint32_t)(y + dy);
                    if (xx >= rts[r].w || yy >= rts[r].h) continue;
                    uint8_t *p = soft_rt_ptr(e, &rts[r], xx, yy);
                    if (!p) continue;
                    if (plain) { memcpy(p, px8, rts[r].bpp); continue; }
                    float d[4], o[4]; soft_unpack(rts[r].fmt, p, d);
                    if (st->blend[r]) {
                        g_soft_blends++;
                        const uint32_t *b = st->bop[r];
                        for (int k = 0; k < 4; k++) {
                            int a = k == 3; uint32_t op = b[a ? 3 : 0], sf = b[a ? 4 : 1], df = b[a ? 5 : 2];
                            float sv = col[k] * soft_bfac(sf, col, d, st->bconst, k), dv = d[k] * soft_bfac(df, col, d, st->bconst, k);
                            if (op == 0x8007 || op == 0x8008 || op == 4 || op == 5) { sv = col[k]; dv = d[k]; }
                            o[k] = soft_bop(op, sv, dv);
                        }
                    } else memcpy(o, col, sizeof o);
                    for (int k = 0; k < 4; k++) if (!((st->cw[r] >> (k * 4)) & 1)) o[k] = d[k];
                    uint8_t q[16]; soft_pack(rts[r].fmt, o, q); memcpy(p, q, rts[r].bpp);
                }
            }
            g_soft_pixels++;
            if (g_soft_draws <= 3 && (x == x0 + (x1 - x0) / 2) && (y == y0 + (y1 - y0) / 2))
                elog(e, "[soft]   draw %llu pixel %d,%d: R0-R3 %f %f %f %f (rt %llx fmt %x)%s\n", (unsigned long long)g_soft_draws, x, y,
                     sx_f(th.r[0]), sx_f(th.r[1]), sx_f(th.r[2]), sx_f(th.r[3]), (unsigned long long)rts[0].addr, rts[0].fmt, th.killed ? " killed" : "");
        }
    if (c.env.unknown_ops && !g_soft_unk_op) g_soft_unk_op = c.env.unknown_ops;
}

/* A draw: topology, first vertex/index, count, indexed or not. */
static void soft_draw(mgr_emu *e, nx2_channel *chn, nx2_engine *en, uint32_t topo, uint32_t first, uint32_t count, int indexed) {
    if (!g_soft_frame || !count || count > 3000000) return;
    uint32_t *vs, vs_bytes, *ps, ps_bytes; sass_ins *vsd, *psd;
    if (!soft_load_program(e, en, 1, &vs, &vs_bytes, &vsd)) return;
    if (!soft_load_program(e, en, 5, &ps, &ps_bytes, &psd)) { ps = NULL; ps_bytes = 0; psd = NULL; }   /* depth-only draw */
    soft_rt rts[8]; int nrt = 0;
    uint32_t ctsel = R3(0x121c), ntargets = ctsel & 15;
    for (uint32_t i = 0; i < 8; i++) { memset(&rts[i], 0, sizeof rts[i]); }
    for (uint32_t i = 0; i < ntargets && i < 8; i++) { uint32_t j = (ctsel >> (4 + 3 * i)) & 7; if (soft_rt_get(en, (int)j, &rts[i])) nrt = (int)i + 1; }
    if (!nrt) {                                                         /* no colour targets: a depth-only pass (pre-pass, shadow map) */
        soft_rt z; if (!(R3(0x12cc) & 1) || !(R3(0x12e8) & 1) || !soft_zb_get(en, &z)) return;
        rts[0].w = z.w; rts[0].h = z.h; rts[0].addr = 0; rts[0].fmt = 0;
    }
    if (!g_soft_budget_hit && now_seconds() - g_soft_t0 > g_soft_budget) { g_soft_budget_hit = 1; elog(e, "[soft] time budget reached after %llu draws: only screen-size passes from now\n", (unsigned long long)g_soft_draws); }
    if (g_soft_budget_hit && rts[0].w < 2560) return;
    g_soft_draws++;
    if (rts[0].w >= 2560) g_log_tex = 24;
    {   static uint64_t probe = 1; if (probe == 1) probe = getenv("MGR_SOFT_PROBE_RT") ? strtoull(getenv("MGR_SOFT_PROBE_RT"), 0, 16) : 0;
        static uint64_t seenp[4]; static int nsp;
        uint64_t pa = ((uint64_t)(R3(0x2014 + 5 * 64) & 0xFF) << 32) | R3(0x2018 + 5 * 64);
        if (probe && rts[0].addr == probe && !(R3(0x12cc) & 1) && ps) { int k; for (k = 0; k < nsp && seenp[k] != pa; k++) {}
            if (k == nsp && nsp < 4) { seenp[nsp++] = pa; char line[200]; g_log_tex = 12;
                elog(e, "[soft] probe: fullscreen-ish draw %llu to %llx (%ux%u fmt %x bh %u), PS %llx, count %u topo %u\n", (unsigned long long)g_soft_draws, (unsigned long long)probe, rts[0].w, rts[0].h, rts[0].fmt, rts[0].bh, (unsigned long long)pa, count, topo);
                for (uint32_t o = 0; o < ps_bytes && o < 0x3000; o += 16) { sass_disasm(ps + o / 4, o, line, sizeof line); elog(e, "  probeps %04x: %s\n", o, line); if ((ps[o / 4] & 0xFFF) == 0x94d && !(ps[o / 4] & 0x8000) && ((ps[o / 4] >> 12) & 7) == 7) break; } } } }
    if (rts[0].w >= 2560 && ps) { static int done; if (!done++) { char line[200];
        for (uint32_t o = 0; o < ps_bytes; o += 16) { sass_disasm(ps + o / 4, o, line, sizeof line); elog(e, "  finalps %04x: %s  [%08x %08x %08x %08x]\n", o, line, ps[o / 4], ps[o / 4 + 1], ps[o / 4 + 2], ps[o / 4 + 3]); }
        for (int g = 0; g < 18; g++) if (g_cbs[(int)(chn - e->nx2_ch) & (NX2_MAX_CH - 1)][4][g].addr) { soft_cb *b = &g_cbs[(int)(chn - e->nx2_ch) & (NX2_MAX_CH - 1)][4][g];
            elog(e, "  finalps cb%d %llx size %u:", g, (unsigned long long)b->addr, b->size); for (uint32_t k = 0; k < 64 && k * 4 < b->size; k++) elog(e, " %08x", gread32(e, b->addr + k * 4)); elog(e, "\n"); } } }
    e->nx2_last_draw_rt = rts[0].addr;
    static soft_state st; soft_state_get(e, en, &st);
    for (int r = 0; r < nrt; r++) {
        if (!rts[r].addr || !st.mask[r]) continue;
        int k = 0; while (k < g_soft_nrts && g_soft_rts[k].addr != rts[r].addr) k++;
        if (k == g_soft_nrts && k < 256) { g_soft_rts[k].addr = rts[r].addr; g_soft_rts[k].w = rts[r].w; g_soft_rts[k].h = rts[r].h; g_soft_rts[k].fmt = rts[r].fmt; g_soft_rts[k].bh = rts[r].bh; g_soft_rts[k].draws = 0; g_soft_nrts++; }
        if (k < 256) g_soft_rts[k].draws++;
    }
    {   uint32_t key = (uint32_t)st.zon | (uint32_t)st.zfunc << 1 | (uint32_t)st.zwrite << 4 | (uint32_t)(st.mask[0] != 0) << 5 | (uint32_t)(st.cw[0] != 0) << 6 | (uint32_t)st.blend[0] << 7 | (uint32_t)(st.depth_reg >= 0) << 8 | (rts[0].fmt << 12) | ((uint32_t)nrt << 20);
        int k = 0; uint64_t za = st.zon ? st.zb.addr : 0; while (k < g_nsstat && !(g_sstat[k].key == key && g_sstat[k].w == rts[0].w && g_sstat[k].h == rts[0].h && g_sstat[k].zaddr == za && g_sstat[k].rtaddr == rts[0].addr)) k++;
        if (k == g_nsstat && k < 96) { memset(&g_sstat[k], 0, sizeof g_sstat[k]); g_sstat[k].zmin = g_sstat[k].dmin = 1e30f; g_sstat[k].zmax = g_sstat[k].dmax = -1e30f; g_sstat[k].key = key; g_sstat[k].zaddr = za; g_sstat[k].rtaddr = rts[0].addr; g_sstat[k].w = rts[0].w; g_sstat[k].h = rts[0].h; g_nsstat++; }
        g_cur_stat = k < 96 ? &g_sstat[k] : NULL; if (g_cur_stat) g_cur_stat->draws++;
    }
    if (g_soft_draws <= 3) elog(e, "[soft]   draw %llu state: omap %x/%x depth-out %d cw %x blend %d op %x %x %x ztest %d func %d write %d zbuf %llx %ux%u\n",
        (unsigned long long)g_soft_draws, st.mask[0], st.mask[1], st.depth_reg, st.cw[0], st.blend[0], st.bop[0][0], st.bop[0][1], st.bop[0][2],
        st.zon, st.zfunc, st.zwrite, (unsigned long long)st.zb.addr, st.zb.w, st.zb.h);
    int ch = (int)(chn - e->nx2_ch); if (ch < 0 || ch >= NX2_MAX_CH) ch = 0;
    uint64_t ib = ((uint64_t)(R3(0x17c8) & 0xFF) << 32) | R3(0x17cc); uint32_t isz = R3(0x17d8) & 3;
    float vsx = sx_f(R3(0xa00)), vsy = sx_f(R3(0xa04)), vsz = sx_f(R3(0xa08)), vox = sx_f(R3(0xa0c)), voy = sx_f(R3(0xa10)), voz = sx_f(R3(0xa14));
    soft_vert *verts = calloc(count, sizeof *verts);
    if (!verts) return;
    static soft_ctx c; static sass_thread th;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = first + i;
        if (indexed) { uint8_t b[4] = {0}; gread(e, ib + (uint64_t)idx * (1u << isz), b, 1u << isz); idx = isz == 0 ? b[0] : isz == 1 ? (uint32_t)(b[0] | b[1] << 8) : (uint32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24); }
        memset(&c.env, 0, sizeof c.env);
        c.env.cbuf = soft_cbuf; c.env.attr_in = soft_vs_in; c.env.attr_out = soft_vs_out; c.env.sysreg = soft_sysreg; c.env.tex = soft_texop; c.env.mem_ld = soft_memld;
        c.e = e; c.en = en; c.ch = ch; c.group = 0;
        memset(c.in, 0, sizeof c.in); memset(c.out, 0, sizeof c.out);
        soft_fetch(e, en, idx, c.in);
        memset(&th, 0, offsetof(sass_thread, local));
        sass_run_dec(&th, &c.env, vs, vsd, vs_bytes, 0);
        memcpy(verts[i].out, c.out, sizeof c.out);
        if (g_soft_draws <= 3 && i < 3)
            elog(e, "[soft]   draw %llu vertex %u (index %u): in %f %f %f %f  out pos %f %f %f %f  out[0x80] %f %f  cb0[0] %llx/%u cb3 %llx\n",
                 (unsigned long long)g_soft_draws, i, idx, sx_f(c.in[0x80 / 4]), sx_f(c.in[0x84 / 4]), sx_f(c.in[0x88 / 4]), sx_f(c.in[0x8c / 4]),
                 sx_f(c.out[0x70 / 4]), sx_f(c.out[0x74 / 4]), sx_f(c.out[0x78 / 4]), sx_f(c.out[0x7c / 4]), sx_f(c.out[0x80 / 4]), sx_f(c.out[0x84 / 4]),
                 (unsigned long long)g_cbs[ch][0][0].addr, g_cbs[ch][0][0].size, (unsigned long long)g_cbs[ch][0][3].addr);
        float x = sx_f(c.out[0x70 / 4]), y = sx_f(c.out[0x74 / 4]), z = sx_f(c.out[0x78 / 4]), w = sx_f(c.out[0x7c / 4]);
        if (!(w > 1e-7f)) { verts[i].ok = 0; continue; }       /* behind the eye: dropped (no clipping yet) */
        verts[i].invw = 1.0f / w; verts[i].ok = 1;
        verts[i].sx = x / w * vsx + vox; verts[i].sy = y / w * vsy + voy; verts[i].sz = z / w * vsz + voz;
        if (c.env.unknown_ops && !g_soft_unk_op) g_soft_unk_op = c.env.unknown_ops;
    }
    switch (topo) {
    case 4: for (uint32_t i = 0; i + 2 < count; i += 3) soft_tri(e, en, ch, &verts[i], &verts[i + 1], &verts[i + 2], ps, ps_bytes, psd, rts, nrt, &st); break;
    case 5: for (uint32_t i = 0; i + 2 < count; i++) soft_tri(e, en, ch, &verts[i], &verts[i + 1 + (i & 1)], &verts[i + 2 - (i & 1)], ps, ps_bytes, psd, rts, nrt, &st); break;
    case 6: for (uint32_t i = 1; i + 1 < count; i++) soft_tri(e, en, ch, &verts[0], &verts[i], &verts[i + 1], ps, ps_bytes, psd, rts, nrt, &st); break;
    case 7: for (uint32_t i = 0; i + 3 < count; i += 4) { soft_tri(e, en, ch, &verts[i], &verts[i + 1], &verts[i + 2], ps, ps_bytes, psd, rts, nrt, &st); soft_tri(e, en, ch, &verts[i], &verts[i + 2], &verts[i + 3], ps, ps_bytes, psd, rts, nrt, &st); } break;
    default: break;
    }
    free(verts);
}

static void soft_clear(mgr_emu *e, nx2_engine *en, uint32_t v) {
    if (!g_soft_frame) return;
    if (v & 1) { float zc = sx_f(R3(0xd90)); soft_rt z;
        { static int nlog; if (nlog++ < 12) elog(e, "[soft]   depth clear to %g (zt %llx fmt %x %ux%u)\n", zc, (unsigned long long)(((uint64_t)(R3(0xfe0) & 0xFF) << 32) | R3(0xfe4)), R3(0xfe8) & 0xFF, R3(0x1228), R3(0x122c)); }
        if (soft_zb_get(en, &z)) for (uint32_t y = 0; y < z.h; y++) for (uint32_t x = 0; x < z.w; x++) { uint8_t *q = soft_rt_ptr(e, &z, x, y); if (q) soft_zput(&z, q, zc); } }
    if (!(v & 0x3C)) return;
    soft_rt rt; if (!soft_rt_get(en, (int)((v >> 6) & 15), &rt)) return;
    float c[4] = { sx_f(R3(0xd80)), sx_f(R3(0xd84)), sx_f(R3(0xd88)), sx_f(R3(0xd8c)) };
    uint8_t px[16]; soft_pack(rt.fmt, c, px);
    for (uint32_t y = 0; y < rt.h; y++)
        for (uint32_t x = 0; x < rt.w; x++) { uint8_t *p = soft_rt_ptr(e, &rt, x, y); if (p) memcpy(p, px, rt.bpp); }
}

/* ---------------- compute dispatches (captured frames only) ---------------- */
static uint64_t g_soft_sustfmt[128]; static uint64_t g_cs_stuck, g_cs_partial, g_cs_resolves, g_cs_barrel; static uint64_t g_cs_st_addr[4]; static uint32_t g_cs_st_info[4]; static int g_cs_nst;
static uint64_t g_soft_cs, g_soft_cs_skipped, g_soft_cs_threads; static uint32_t g_soft_cs_unk[64];
static int g_surf_fail_log;
static int soft_surf_at(soft_ctx *c, uint32_t h, const uint32_t *co, uint32_t dim, soft_tex *t, uint32_t *x, uint32_t *y, uint64_t *ga) {
    if (!soft_tex_get(c->e, c->en, h, t) || t->blockw != 1) {
        if (g_surf_fail_log < 20) { g_surf_fail_log++; elog(c->e, "[soft]   image access with unusable handle %x (dim %u, coords %u %u %u): comps %x addr %llx\n", h, dim, co[0], co[1], co[2], t->comps, (unsigned long long)t->addr); }
        return 0; }
    uint32_t xx = co[0], yy = 0, layer = 0;
    switch (dim) { case 2: layer = co[1]; break; case 3: yy = co[1]; break; case 4: case 5: yy = co[1]; layer = co[2]; break; default: break; }
    if (layer || xx >= t->w || yy >= t->h) return 0;                     /* layers and 3D slices beyond the first: not yet */
    *x = xx; *y = yy;
    *ga = t->addr + (t->bl ? bl_offset(xx * t->bytes, yy, t->pitch, 1u << t->bh_log2) : (uint64_t)yy * t->pitch + (uint64_t)xx * t->bytes);
    return 1;
}
static void soft_tex_pack(const soft_tex *t, const uint32_t d[4], const float f[4], uint8_t *o) {
    uint32_t ty = t->types & 7;                                          /* 2 unorm, 1 snorm, 3 sint, 4 uint, 7 float */
    #define UN(x, m) (uint32_t)(fminf(fmaxf((x), 0), 1) * (m) + 0.5f)
    uint32_t v;
    switch (t->comps) {
    case 0x08: for (int i = 0; i < 4; i++) o[i] = ty == 3 || ty == 4 ? (uint8_t)d[i] : (uint8_t)UN(t->srgb && i < 3 ? soft_lin_to_srgb(f[i]) : f[i], 255); break;
    case 0x18: for (int i = 0; i < 2; i++) o[i] = ty == 3 || ty == 4 ? (uint8_t)d[i] : (uint8_t)UN(f[i], 255); break;
    case 0x1d: o[0] = ty == 3 || ty == 4 ? (uint8_t)d[0] : (uint8_t)UN(f[0], 255); break;
    case 0x21: v = soft_f_to_uf(f[0], 6) | (soft_f_to_uf(f[1], 6) << 11) | (soft_f_to_uf(f[2], 5) << 22); memcpy(o, &v, 4); break;
    case 0x09: v = UN(f[0], 1023) | UN(f[1], 1023) << 10 | UN(f[2], 1023) << 20 | UN(f[3], 3) << 30; memcpy(o, &v, 4); break;
    case 0x20: {                                                         /* E5B9G9R9 shared exponent */
        float r = fminf(fmaxf(f[0], 0), 65408), g = fminf(fmaxf(f[1], 0), 65408), b = fminf(fmaxf(f[2], 0), 65408);
        if (!(r == r)) r = 0; if (!(g == g)) g = 0; if (!(b == b)) b = 0;
        float mx = fmaxf(r, fmaxf(g, b));
        int e = mx > 0 ? (int)floorf(log2f(mx)) : -16; if (e < -16) e = -16; e += 16;      /* biased: 15 + 1 */
        float den = ldexpf(1.0f, e - 15 - 9);
        if ((uint32_t)(mx / den + 0.5f) == 512) { e++; den *= 2; }
        if (e > 31) e = 31;
        v = (uint32_t)(r / den + 0.5f) | (uint32_t)(g / den + 0.5f) << 9 | (uint32_t)(b / den + 0.5f) << 18 | (uint32_t)e << 27; memcpy(o, &v, 4); break;
    }
    case 0x03: case 0x0c: case 0x1b: { int n = t->comps == 0x03 ? 4 : t->comps == 0x0c ? 2 : 1;
        for (int i = 0; i < n; i++) { uint16_t h = ty == 7 ? sx_f_to_half(f[i]) : ty == 2 ? (uint16_t)UN(f[i], 65535) : (uint16_t)d[i]; memcpy(o + i * 2, &h, 2); } break; }
    case 0x01: memcpy(o, d, 16); break;
    case 0x04: memcpy(o, d, 8); break;
    case 0x0f: memcpy(o, d, 4); break;
    default: g_soft_sustfmt[t->comps & 127]++; memcpy(o, d, t->bytes > 16 ? 16 : t->bytes); break;
    }
    #undef UN
}
static void soft_surf_st(sass_env *ev, uint32_t h, const uint32_t *co, uint32_t dim, const uint32_t *data, int formatted, uint32_t mt) {
    soft_ctx *c = (soft_ctx *)ev; soft_tex t; uint32_t x, y; uint64_t ga;
    if (!soft_surf_at(c, h, co, dim, &t, &x, &y, &ga)) return;
    if ((uint64_t)t.w * t.h > (uint64_t)g_cs_out_big.w * g_cs_out_big.h) g_cs_out_big = t;
    { int k; for (k = 0; k < g_cs_nst && g_cs_st_addr[k] != t.addr; k++) {} if (k == g_cs_nst && k < 4) { g_cs_st_addr[k] = t.addr; g_cs_st_info[k] = t.w | t.h << 16; g_cs_nst++; g_cs_st_info[k] = (t.w & 0xFFFF) | (t.h << 16); } }
    uint8_t o[16] = {0};
    if (!formatted) { uint32_t n = mt == 5 ? 8 : mt == 6 ? 16 : 4; memcpy(o, data, n); gwrite(c->e, ga, o, n < t.bytes ? n : t.bytes); return; }
    uint32_t d[4] = {0, 0, 0, 0}; float f[4] = {0, 0, 0, 1};
    if (mt != 15) { float old[4]; soft_texel(c->e, &t, (int)x, (int)y, old); for (int i = 0; i < 4; i++) d[i] = sx_u(old[i]); }
    for (int i = 0, k = 0; i < 4; i++) if ((mt >> i) & 1) d[i] = data[k++];
    for (int i = 0; i < 4; i++) f[i] = sx_f(d[i]);
    soft_tex_pack(&t, d, f, o);
    gwrite(c->e, ga, o, t.bytes);
}
static void soft_surf_ld(sass_env *ev, uint32_t h, const uint32_t *co, uint32_t dim, uint32_t *out, int formatted, uint32_t mt) {
    soft_ctx *c = (soft_ctx *)ev; soft_tex t; uint32_t x, y; uint64_t ga;
    if (!soft_surf_at(c, h, co, dim, &t, &x, &y, &ga)) return;
    if (!formatted) { uint32_t n = mt == 5 ? 8 : mt == 6 ? 16 : 4; uint8_t b[16] = {0}; gread(c->e, ga, b, n); memcpy(out, b, n); return; }
    float px[4]; soft_texel(c->e, &t, (int)x, (int)y, px);
    for (int i = 0; i < 4; i++) out[i] = sx_u(px[i]);
}
static void soft_memst(sass_env *ev, uint64_t a, uint32_t v) { soft_ctx *c = (soft_ctx *)ev; gwrite32(c->e, a, v); }

/* MGR_SOFT_TRACE=n: log the instructions of one thread (the first lane of the first CTA) of the n-th compute
   dispatch of the captured frame, with the destination register value after each */
static uint32_t g_tr_pc, g_tr_dst, g_tr_n; static int g_tr_have; static const uint32_t *g_tr_code;
static void soft_trace(sass_env *ev, void *tv, uint32_t pc, const sass_ins *in) {
    soft_ctx *c = (soft_ctx *)ev; sass_thread *t = (sass_thread *)tv;
    if (pc == 0xFFFFFFFFu) {
        if (g_tr_have && g_tr_n < 20000) { char line[200]; sass_disasm(g_tr_code + g_tr_pc / 4, g_tr_pc, line, sizeof line);
            uint32_t d = g_tr_dst;
            elog(c->e, "  tr %04x: %-60s -> R%u=%08x (%g) R+1=%08x R+2=%08x R+3=%08x\n", g_tr_pc, line, d, d < 255 ? t->r[d] : 0, d < 255 ? sx_f(t->r[d]) : 0.0f,
                 d < 254 ? t->r[d + 1] : 0, d < 253 ? t->r[d + 2] : 0, d < 252 ? t->r[d + 3] : 0); g_tr_n++; }
        g_tr_have = 0; return;
    }
    g_tr_pc = pc; g_tr_dst = in->dst; g_tr_have = 1;
}
/* save a colour target as PNG (float formats tone-mapped so the 98th percentile of lit pixels is white) */
static int soft_save_rt_png(mgr_emu *e, const soft_rt_seen *r, const char *path) {
    soft_rt rt = { r->addr, r->w, r->h, r->fmt, (uint32_t)soft_fmt_bpp(r->fmt), r->bh, (r->w * (uint32_t)soft_fmt_bpp(r->fmt) + 63) & ~63u };
    uint32_t sc = r->w > 1920 ? 2 : 1, ow = r->w / sc, oh = r->h / sc;
    uint8_t *rgb = malloc((size_t)ow * oh * 3); if (!rgb) return 0;
    int hdr = r->fmt == 0xE0 || r->fmt == 0xCA || r->fmt == 0xC0 || r->fmt == 0xCE;
    float *fl = malloc((size_t)ow * oh * 3 * sizeof(float)); if (!fl) { free(rgb); return 0; }
    for (uint32_t y = 0; y < oh; y++) for (uint32_t x = 0; x < ow; x++) {
        uint8_t *p = soft_rt_ptr(e, &rt, x * sc, y * sc); float c[4] = {0, 0, 0, 1};
        if (p) soft_unpack(r->fmt, p, c);
        for (int i = 0; i < 3; i++) fl[((size_t)y * ow + x) * 3 + i] = isfinite(c[i]) ? c[i] : 0;
    }
    float expo = 1;
    if (hdr) {
        static float hist[4096]; int nh = 0; size_t np = (size_t)ow * oh;
        for (size_t i = 0; i < np && nh < 4096; i += np / 4096 + 1) { float m = fmaxf(fl[i * 3], fmaxf(fl[i * 3 + 1], fl[i * 3 + 2])); if (m > 0) hist[nh++] = m; }
        for (int i = 1; i < nh; i++) { float v = hist[i]; int j = i - 1; while (j >= 0 && hist[j] > v) { hist[j + 1] = hist[j]; j--; } hist[j + 1] = v; }
        if (nh) expo = 1.0f / fmaxf(fminf(hist[(nh * 98) / 100], 6.0f * hist[nh / 2]), 1e-12f);
    }
    for (size_t i = 0; i < (size_t)ow * oh * 3; i++) { float v = fl[i] * expo; if (hdr) v = soft_lin_to_srgb(v); v = v < 0 ? 0 : v > 1 ? 1 : v; rgb[i] = (uint8_t)(v * 255 + 0.5f); }
    free(fl);
    png_write_rgb(path, rgb, ow, oh); free(rgb);
    return 1;
}
static void soft_trace_fwd(sass_env *ev, void *tv, uint32_t pc, const sass_ins *in) { g_tr_code = g_tr_code_fwd; soft_trace(ev, tv, pc, in); }
static void soft_trace_reset(void) { g_tr_n = 0; g_tr_have = 0; }
static void soft_compute(mgr_emu *e, nx2_engine *en, const uint32_t *q) {
    if (!g_soft_frame || g_soft_budget_hit) return;
    static int on = -1; if (on < 0) on = getenv("MGR_SOFT_NOCOMPUTE") == NULL;
    if (!on) return;
    uint32_t gx = q[12], gy = q[13] & 0xFFFF, gz = q[14] & 0xFFFF;
    uint32_t bx = q[18] >> 16, by = q[19] & 0xFFFF, bz = q[19] >> 16, smem = q[17] & 0x3FFFF;
    uint64_t pa = ((uint64_t)(q[49] & 0x1FFFF) << 32) | q[48];
    uint64_t nthr = (uint64_t)bx * by * bz, total = nthr * gx * gy * gz;
    if (!gx || !gy || !gz || !nthr || nthr > 1024 || total > 40000000ull || !pa) { g_soft_cs_skipped++;
        { static int nl; if (nl++ < 30) elog(e, "[soft] compute skipped: program %llx grid %ux%ux%u block %ux%ux%u (QMD words 12-19: %x %x %x %x %x %x %x %x)\n", (unsigned long long)pa, gx, gy, gz, bx, by, bz, q[12], q[13], q[14], q[15], q[16], q[17], q[18], q[19]); }
        { static int nd; if (nd++ < 2) { elog(e, "[soft] skipped QMD words:"); for (int i = 0; i < 64; i++) elog(e, " %x", q[i]); elog(e, "\n"); } }
        return; }
    if (now_seconds() - g_soft_t0 > g_soft_budget) { g_soft_budget_hit = 1; elog(e, "[soft] time budget reached in compute after %llu draws\n", (unsigned long long)g_soft_draws); return; }
    { static int nd; if (nd++ < 2) { elog(e, "[soft] valid QMD words:"); for (int i = 0; i < 64; i++) elog(e, " %x", q[i]); elog(e, "\n"); } }
    uint32_t *code, bytes; sass_ins *dec;
    if (!soft_load_code(e, pa, &code, &bytes, &dec)) return;
    { static int want = -2; if (want == -2) want = getenv("MGR_SOFT_DUMP_BEFORE_CS") ? atoi(getenv("MGR_SOFT_DUMP_BEFORE_CS")) : -1;
      if (want >= 0 && (int)g_soft_cs + 1 == want) {
          const char *dir = getenv("MGR_DUMP_DIR") ? getenv("MGR_DUMP_DIR") : "/tmp/mgr_dump/frames";
          for (int k = 0; k < g_soft_nrts; k++) { char path[600];
              snprintf(path, sizeof path, "%s/pre%d_f%llu_rt%d_%llx_%ux%u_fmt%x.png", dir, want, (unsigned long long)g_soft_t0_id, k, (unsigned long long)g_soft_rts[k].addr, g_soft_rts[k].w, g_soft_rts[k].h, g_soft_rts[k].fmt);
              if (g_soft_rts[k].w >= 640 && soft_save_rt_png(e, &g_soft_rts[k], path)) elog(e, "[soft] saved %s before compute %d\n", path, want); } } }
    static soft_ctx c; memset(&c, 0, sizeof c);
    c.env.cbuf = soft_cbuf; c.env.sysreg = soft_sysreg; c.env.tex = soft_texop; c.env.mem_ld = soft_memld; c.env.mem_st = soft_memst;
    c.env.surf_st = soft_surf_st; c.env.surf_ld = soft_surf_ld; c.env.bar_yield = 1;
    { static int want = -2; if (want == -2) want = getenv("MGR_SOFT_TRACE") ? atoi(getenv("MGR_SOFT_TRACE")) : -1;
      if (want >= 0 && (int)g_soft_cs + 1 == want) { c.env.trace = soft_trace; g_tr_n = 0; g_tr_have = 0; elog(e, "[soft] tracing compute %d\n", want); } } c.env.warp_sync = getenv("MGR_SOFT_NOWARP") == NULL;
    c.e = e; c.en = en; c.compute = 1;
    for (int i = 0; i < 8; i++) if ((q[20] >> i) & 1) { c.cba[i] = ((uint64_t)(q[33 + 2 * i] & 0x1FFFF) << 32) | q[32 + 2 * i]; c.cbs[i] = (q[33 + 2 * i] >> 19) << 4; }
    static sass_thread *ths; if (!ths) ths = calloc(1024, sizeof *ths);
    if (c.env.trace) { c.env.trace_t = &ths[0]; g_tr_code = code; }
    uint32_t sw = smem / 4 + 64; uint32_t *sh = calloc(sw, 4);
    if (!ths || !sh) { free(sh); return; }
    c.env.shared = sh; c.env.shared_words = sw;
    double t0 = now_seconds(); g_cs_nst = 0; g_cs_stuck = g_cs_partial = g_cs_resolves = g_cs_barrel = 0; g_cs_track = 1; memset(&g_cs_in_big, 0, sizeof g_cs_in_big); memset(&g_cs_out_big, 0, sizeof g_cs_out_big);
    g_cs_hmma = 0; for (uint32_t i = 0; i < bytes / 16 && dec; i++) if (dec[i].op12 == 0x23c) { g_cs_hmma = 1; break; }
    if (g_cs_hmma) { for (int i = 0; i < 8; i++) if (c.cba[i]) { elog(e, "[soft]   upscaler cb%d %llx:", i, (unsigned long long)c.cba[i]); for (uint32_t k = 0; k < 48 && k * 4 < c.cbs[i]; k++) elog(e, " %08x", gread32(e, c.cba[i] + k * 4)); elog(e, "\n"); } }
    c.env.code_delta = 0;                                              /* from "MOV Rx, <return address>" just before a CALL.ABS */
    for (uint32_t i = 1; i < bytes / 16; i++) if ((code[i * 4] & 0xFFF) == 0x944) {
        for (uint32_t d = 1; d <= 4 && d <= i; d++) if ((code[(i - d) * 4] & 0xFFF) == 0x802) { c.env.code_delta = (int64_t)(i * 16 + 16) - (int64_t)code[(i - d) * 4 + 1]; break; }
        break;
    }
    { static int nl; for (uint32_t i = 0; i < bytes / 16 && nl < 12; i++) if (code[i * 4] && ((code[i * 4] & 0xFFF) == 0x944 || (code[i * 4] & 0xFFF) == 0x950 || (code[i * 4] & 0xFFF) == 0x943)) {
        nl++; elog(e, "[soft] call/ret in %llx at %04x: %08x %08x %08x %08x (code bytes %u)\n", (unsigned long long)pa, i * 16, code[i * 4], code[i * 4 + 1], code[i * 4 + 2], code[i * 4 + 3], bytes);
        char line[200];
        for (int d = -4; d <= 0; d++) if ((int)i + d >= 0) { sass_disasm(code + (i + d) * 4, (i + d) * 16, line, sizeof line); elog(e, "    %s\n", line); }
        if ((code[i * 4] & 0xFFF) == 0x944) { uint32_t tg = code[i * 4 + 1];
            for (int cand = 0; cand < 2; cand++) { int64_t off = (int64_t)tg - (cand ? 0x1000 : 0);
                elog(e, "    candidate target %llx (+%llx):\n", (unsigned long long)(pa + off), (unsigned long long)off);
                for (int d = -2; d <= 2; d++) { int64_t o2 = off + d * 16; if (o2 >= 0 && o2 + 16 <= bytes) { sass_disasm(code + o2 / 4, (uint64_t)o2, line, sizeof line); elog(e, "      %s\n", line); } } } }
        } }
    for (uint32_t cz = 0; cz < gz; cz++) for (uint32_t cy = 0; cy < gy; cy++) for (uint32_t cx = 0; cx < gx; cx++) {
        c.ctaid[0] = cx; c.ctaid[1] = cy; c.ctaid[2] = cz;
        { static int tx = -2, ty; if (tx == -2) { const char *s2 = getenv("MGR_SOFT_TRACE_CTA"); tx = 0; ty = 0; if (s2) sscanf(s2, "%d,%d", &tx, &ty); }
          static void (*tfn)(sass_env *, void *, uint32_t, const sass_ins *); if (c.env.trace) tfn = c.env.trace;
          int hit = (int)cx == tx && (int)cy == ty && cz == 0;
          if (tfn && !hit && c.env.trace) { soft_trace(&c.env, &ths[0], 0xFFFFFFFFu, 0); c.env.trace = 0; if ((int)cy > ty || ((int)cy == ty && (int)cx > tx)) tfn = 0; }
          if (tfn && hit && !c.env.trace) { c.env.trace = tfn; elog(e, "[soft] tracing CTA %u,%u\n", cx, cy); }
          if (tfn && !hit) c.env.trace = 0; }
        memset(sh, 0, (size_t)sw * 4);
        for (uint32_t i = 0; i < nthr; i++) memset(&ths[i], 0, offsetof(sass_thread, local));
        for (int round = 0; round < 100000; round++) {
            int ran = 0;
            for (uint32_t i = 0; i < nthr; i++) {
                sass_thread *th = &ths[i];
                if (th->exited || th->at_bar || th->at_sync) continue;
                c.lin = i; c.tid[0] = i % bx; c.tid[1] = (i / bx) % by; c.tid[2] = i / (bx * by);
                sass_run_dec(th, &c.env, code, dec, bytes, th->resume_pc);
                if (!th->at_bar && !th->at_sync) th->exited = 1;
                ran++;
            }
            /* warp operations: once every live thread of a warp has stopped, combine the values of
               the threads stopped at the same instruction */
            int resolved = 0, waiting_sync = 0;
            for (uint32_t w0 = 0; w0 < nthr; w0 += 32) {
                uint32_t w1 = w0 + 32 < nthr ? w0 + 32 : nthr; int busy = 0, any = 0;
                for (uint32_t i = w0; i < w1; i++) { if (!ths[i].exited && !ths[i].at_bar && !ths[i].at_sync) busy = 1; if (ths[i].at_sync) any = 1; }
                if (!any) continue;
                waiting_sync = 1;
                if (busy) continue;
                uint32_t pc0 = 0; int have = 0;
                for (uint32_t i = w0; i < w1; i++) if (ths[i].at_sync) { pc0 = ths[i].sync_pc; have = 1; break; }
                if (!have) continue;
                uint32_t act = 0; for (uint32_t i = w0; i < w1; i++) if (ths[i].at_sync && ths[i].sync_pc == pc0) act |= 1u << (i - w0);
                for (uint32_t i = w0; i < w1; i++) {
                    sass_thread *th = &ths[i]; if (!((act >> (i - w0)) & 1)) continue;
                    uint32_t lane = i - w0;
                    if (th->sync_kind == 1) {                              /* SHFL: idx, up, down, bfly */
                        uint32_t b = th->sync_lane & 31, cl = th->sync_c & 31, seg = (th->sync_c >> 8) & 31;
                        uint32_t minl = lane & seg, maxl = minl | (cl & ~seg), j; int ok;
                        switch (th->sync_mode) { case 0: j = minl | (b & ~seg); ok = j <= maxl; break; case 1: j = lane - b; ok = (int32_t)j >= (int32_t)maxl; break;
                        case 2: j = lane + b; ok = j <= maxl; break; default: j = lane ^ b; ok = j <= maxl; break; }
                        if (!ok || j >= 32 || !((act >> j) & 1)) { j = lane; }
                        th->sync_res = ths[w0 + j].sync_val; th->sync_pres = ok;
                    } else if (th->sync_kind == 2) {                       /* VOTE: all, any, eq; ballot */
                        uint32_t bal = 0; for (uint32_t k = w0; k < w1; k++) if (((act >> (k - w0)) & 1) && ths[k].sync_val) bal |= 1u << (k - w0);
                        th->sync_res = bal;
                        th->sync_pres = th->sync_mode == 0 ? bal == act : th->sync_mode == 1 ? bal != 0 : (bal == act || bal == 0);
                    } else if (th->sync_kind == 4 || th->sync_kind == 5) {
                        /* done for the whole warp at its first lane below */
                    } else {                                               /* REDUX: and, or, xor, sum, min, max */
                        uint32_t op = th->sync_mode & 15, sg = th->sync_mode >> 4, r = 0; int first = 1;
                        for (uint32_t k = w0; k < w1; k++) { if (!((act >> (k - w0)) & 1)) continue; uint32_t v = ths[k].sync_val;
                            if (first) { r = v; first = 0; continue; }
                            switch (op) { case 0: r &= v; break; case 1: r |= v; break; case 2: r ^= v; break; case 3: r += v; break;
                            case 4: r = sg ? ((int32_t)v < (int32_t)r ? v : r) : (v < r ? v : r); break; default: r = sg ? ((int32_t)v > (int32_t)r ? v : r) : (v > r ? v : r); break; } }
                        th->sync_res = r;
                    }
                }
                {   sass_thread *t0 = NULL; for (uint32_t i = w0; i < w1; i++) if ((act >> (i - w0)) & 1) { t0 = &ths[i]; break; }
                    g_cs_resolves++; if (act != (w1 - w0 == 32 ? 0xFFFFFFFFu : ((1u << (w1 - w0)) - 1))) g_cs_partial++;
                    if (t0 && t0->sync_kind == 4) {                        /* HMMA m16n8k8/k16: D = A x B + C (PTX mma fragment layout) */
                        int k16 = t0->sync_mode & 1, f32 = (t0->sync_mode >> 1) & 1, K = k16 ? 16 : 8;
                        static float A[16][16], B[16][8], C[16][8];
                        memset(A, 0, sizeof A); memset(B, 0, sizeof B); memset(C, 0, sizeof C);
                        for (uint32_t l = 0; l < 32 && w0 + l < w1; l++) {
                            sass_thread *x = &ths[w0 + l]; uint32_t g = l >> 2, tq = l & 3;
                            for (int r = 0; r < (k16 ? 4 : 2); r++) {             /* A: row g (+8 for odd regs), cols 2t (+8 for regs 2,3) */
                                uint32_t v = x->sync_in[r]; int row = (int)g + ((r & 1) ? 8 : 0), col = (int)tq * 2 + ((r & 2) ? 8 : 0);
                                A[row][col] = sx_half_to_f((uint16_t)v); A[row][col + 1] = sx_half_to_f((uint16_t)(v >> 16)); }
                            for (int r = 0; r < (k16 ? 2 : 1); r++) {             /* B: rows 2t (+8), col g */
                                uint32_t v = x->sync_in[4 + r]; int row = (int)tq * 2 + r * 8;
                                B[row][g] = sx_half_to_f((uint16_t)v); B[row + 1][g] = sx_half_to_f((uint16_t)(v >> 16)); }
                            if (f32) for (int r = 0; r < 4; r++) C[g + ((r & 2) ? 8 : 0)][tq * 2 + (r & 1)] = sx_f(x->sync_in[6 + r]);
                            else for (int r = 0; r < 2; r++) { uint32_t v = x->sync_in[6 + r]; C[g + r * 8][tq * 2] = sx_half_to_f((uint16_t)v); C[g + r * 8][tq * 2 + 1] = sx_half_to_f((uint16_t)(v >> 16)); }
                        }
                        float D[16][8];
                        for (int i2 = 0; i2 < 16; i2++) for (int j = 0; j < 8; j++) { float acc = C[i2][j]; for (int k2 = 0; k2 < K; k2++) acc += A[i2][k2] * B[k2][j]; D[i2][j] = acc; }
                        for (uint32_t l = 0; l < 32 && w0 + l < w1; l++) {
                            sass_thread *x = &ths[w0 + l]; uint32_t g = l >> 2, tq = l & 3;
                            if (f32) for (int r = 0; r < 4; r++) x->sync_out[r] = sx_u(D[g + ((r & 2) ? 8 : 0)][tq * 2 + (r & 1)]);
                            else for (int r = 0; r < 2; r++) x->sync_out[r] = (uint32_t)sx_f_to_half(D[g + r * 8][tq * 2]) | (uint32_t)sx_f_to_half(D[g + r * 8][tq * 2 + 1]) << 16;
                        }
                    } else if (t0 && t0->sync_kind == 5) {                 /* MOVM.16.MT88: transpose an 8x8 matrix of halves */
                        uint16_t M[8][8];
                        for (uint32_t l = 0; l < 32 && w0 + l < w1; l++) { uint32_t v = ths[w0 + l].sync_in[0]; M[l >> 2][(l & 3) * 2] = (uint16_t)v; M[l >> 2][(l & 3) * 2 + 1] = (uint16_t)(v >> 16); }
                        for (uint32_t l = 0; l < 32 && w0 + l < w1; l++) { uint32_t r = l >> 2, c0 = (l & 3) * 2; ths[w0 + l].sync_out[0] = (uint32_t)M[c0][r] | (uint32_t)M[c0 + 1][r] << 16; }
                    }
                }
                for (uint32_t i = w0; i < w1; i++) if ((act >> (i - w0)) & 1) { ths[i].at_sync = 0; ths[i].sync_done = 1; resolved++; }
            }
            if (ran || resolved) continue;
            /* nothing ran or resolved: release a barrier when every live thread is at it */
            int at_bar = 0, live = 0; for (uint32_t i = 0; i < nthr; i++) if (!ths[i].exited) { live++; if (ths[i].at_bar) at_bar++; }
            if (!live) break;
            if (at_bar && !waiting_sync) { g_cs_barrel++; if (at_bar != live) g_cs_stuck += 1000000; for (uint32_t i = 0; i < nthr; i++) ths[i].at_bar = 0; continue; }
            /* stuck (threads waiting at different points): let everyone go on */
            g_cs_stuck++;
            for (uint32_t i = 0; i < nthr; i++) { if (ths[i].at_sync) { ths[i].at_sync = 0; ths[i].sync_done = 1; ths[i].sync_res = ths[i].sync_val; ths[i].sync_pres = 1; } ths[i].at_bar = 0; }
        }
    }
    free(sh); g_cs_track = 0;
    /* A tensor-core (HMMA) upscaler: until its network output is reproduced, fill its output
       image with a bilinear upscale of its largest input (only with MGR_SOFT_FAKE_UPSCALER=1; the network itself now runs correctly). */
    if (g_cs_hmma && g_cs_in_big.w && g_cs_out_big.w > g_cs_in_big.w && getenv("MGR_SOFT_FAKE_UPSCALER")) {
        soft_tex *si = &g_cs_in_big, *so = &g_cs_out_big;
        elog(e, "[soft] upscaler fallback: %llx %ux%u (comps %x) -> %llx %ux%u (comps %x)\n", (unsigned long long)si->addr, si->w, si->h, si->comps,
             (unsigned long long)so->addr, so->w, so->h, so->comps);
        for (uint32_t y = 0; y < so->h; y++) for (uint32_t x = 0; x < so->w; x++) {
            float px[4]; soft_sample(e, si, (x + 0.5f) / so->w, (y + 0.5f) / so->h, px);
            if (si->comps == 0x20) for (int i = 0; i < 3; i++) px[i] *= px[i];   /* the network input is square-root encoded (E5B9G9R9 of sqrt(colour)) */
            uint32_t d[4]; for (int i = 0; i < 4; i++) d[i] = sx_u(px[i]);
            uint8_t o[16] = {0}; soft_tex_pack(so, d, px, o);
            uint64_t ga = so->addr + (so->bl ? bl_offset(x * so->bytes, y, so->pitch, 1u << so->bh_log2) : (uint64_t)y * so->pitch + (uint64_t)x * so->bytes);
            gwrite(e, ga, o, so->bytes);
        }
    }
    g_soft_cs++; g_soft_cs_threads += total;
    for (int i = 0; i < 64 && c.env.op_hist_unknown[i]; i++) {
        uint32_t op = c.env.op_hist_unknown[i] & 0xFFFF, n = c.env.op_hist_unknown[i] >> 16; int k;
        for (k = 0; k < 64 && g_soft_cs_unk[k] && (g_soft_cs_unk[k] & 0xFFFF) != op; k++) {}
        if (k < 64) { uint32_t cnt = (g_soft_cs_unk[k] >> 16) + n; g_soft_cs_unk[k] = op | (cnt > 0xFFFF ? 0xFFFF : cnt) << 16; }
    }
    if (total >= 100000) {                                         /* big dispatches: list unsupported ops, disassemble once */
        elog(e, "[soft] big compute %llu: program %llx grid %ux%ux%u block %ux%ux%u shared %u cb0 %llx: %.2f s, unsupported:", (unsigned long long)g_soft_cs, (unsigned long long)pa,
             gx, gy, gz, bx, by, bz, smem, (unsigned long long)c.cba[0], now_seconds() - t0);
        for (int i = 0; i < 64 && c.env.op_hist_unknown[i]; i++) elog(e, " %03x", c.env.op_hist_unknown[i] & 0xFFF);
        elog(e, "; warp ops %llu (%llu partial), barriers %llu, stuck %llu; writes", (unsigned long long)g_cs_resolves, (unsigned long long)g_cs_partial, (unsigned long long)g_cs_barrel, (unsigned long long)g_cs_stuck);
        for (int i = 0; i < g_cs_nst; i++) elog(e, " %llx (%ux%u)", (unsigned long long)g_cs_st_addr[i], g_cs_st_info[i] & 0xFFFF, g_cs_st_info[i] >> 16);
        elog(e, "\n");
        static uint64_t seen[16]; static int nseen; int k; for (k = 0; k < nseen && seen[k] != pa; k++) {}
        if (k == nseen && nseen < 16 && getenv("MGR_SOFT_CSDUMP")) { seen[nseen++] = pa;
            for (uint32_t off = 0; off < bytes; off += 16) { char line[200]; sass_disasm(code + off / 4, off, line, sizeof line); elog(e, "  cs %llx %04x: %s  [%08x %08x %08x %08x]\n", (unsigned long long)pa, off, line, code[off / 4], code[off / 4 + 1], code[off / 4 + 2], code[off / 4 + 3]); } }
    }
    if (g_soft_cs <= 40)
        elog(e, "[soft] compute %llu: program %llx grid %ux%ux%u block %ux%ux%u shared %u: %.2f s%s\n", (unsigned long long)g_soft_cs, (unsigned long long)pa,
             gx, gy, gz, bx, by, bz, smem, now_seconds() - t0, c.env.unknown_ops ? " (unsupported ops)" : "");
}

/* 2D engine PIXELS_FROM_MEMORY (cl902d.h): scaled copy between surfaces (captured frames only) */
static void soft_blit2d(mgr_emu *e, const uint32_t *r) {
    #define R2(o) r[(o) / 4]
    soft_rt d, s;
    d.fmt = R2(0x200) & 0xFF; d.addr = ((uint64_t)(R2(0x220) & 0xFF) << 32) | R2(0x224); d.w = R2(0x218); d.h = R2(0x21c);
    s.fmt = R2(0x230) & 0xFF; s.addr = ((uint64_t)(R2(0x250) & 0xFF) << 32) | R2(0x254); s.w = R2(0x248); s.h = R2(0x24c);
    d.bpp = (uint32_t)soft_fmt_bpp(d.fmt); s.bpp = (uint32_t)soft_fmt_bpp(s.fmt);
    int dpitch = R2(0x204) & 1, spitch = R2(0x234) & 1;
    d.bh = 1u << ((R2(0x208) >> 4) & 7); s.bh = 1u << ((R2(0x238) >> 4) & 7);
    d.pitch = dpitch ? R2(0x214) : (d.w * d.bpp + 63) & ~63u; s.pitch = spitch ? R2(0x244) : (s.w * s.bpp + 63) & ~63u;
    int32_t dx0 = (int32_t)R2(0x8b0), dy0 = (int32_t)R2(0x8b4); uint32_t dw = R2(0x8b8), dh = R2(0x8bc);
    double dudx = (int32_t)R2(0x8c4) + R2(0x8c0) / 4294967296.0, dvdy = (int32_t)R2(0x8cc) + R2(0x8c8) / 4294967296.0;
    double sx0 = (int32_t)R2(0x8d4) + R2(0x8d0) / 4294967296.0, sy0 = (int32_t)R2(0x8dc) + R2(0x8d8) / 4294967296.0;
    static int nl; if (nl++ < 30) elog(e, "[2d] blit %llx (%ux%u fmt %x %s) -> %llx (%ux%u fmt %x %s): dst %d,%d %ux%u from %.1f,%.1f step %.3f,%.3f%s\n",
        (unsigned long long)s.addr, s.w, s.h, s.fmt, spitch ? "pitch" : "block", (unsigned long long)d.addr, d.w, d.h, d.fmt, dpitch ? "pitch" : "block",
        dx0, dy0, dw, dh, sx0, sy0, dudx, dvdy, g_soft_frame ? " (rendered)" : "");
    if (!g_soft_frame || !d.bpp || !s.bpp || !d.addr || !s.addr || dw > 16384 || dh > 16384) return;
    for (uint32_t y = 0; y < dh; y++) for (uint32_t x = 0; x < dw; x++) {
        int32_t tx = dx0 + (int32_t)x, ty = dy0 + (int32_t)y;
        int32_t ux = (int32_t)(sx0 + x * dudx), uy = (int32_t)(sy0 + y * dvdy);
        if (tx < 0 || ty < 0 || (uint32_t)tx >= d.w || (uint32_t)ty >= d.h || ux < 0 || uy < 0 || (uint32_t)ux >= s.w || (uint32_t)uy >= s.h) continue;
        uint8_t *sp = spitch ? NULL : soft_rt_ptr(e, &s, (uint32_t)ux, (uint32_t)uy), *dp = dpitch ? NULL : soft_rt_ptr(e, &d, (uint32_t)tx, (uint32_t)ty);
        uint8_t px[16] = {0};
        if (spitch) gread(e, s.addr + (uint64_t)uy * s.pitch + (uint64_t)ux * s.bpp, px, s.bpp); else if (sp) memcpy(px, sp, s.bpp); else continue;
        uint8_t out[16];
        if (s.fmt == d.fmt || s.bpp == d.bpp) memcpy(out, px, d.bpp);
        else { float c[4]; soft_unpack(s.fmt, px, c); soft_pack(d.fmt, c, out); }
        if (dpitch) gwrite(e, d.addr + (uint64_t)ty * d.pitch + (uint64_t)tx * d.bpp, out, d.bpp); else if (dp) memcpy(dp, out, d.bpp);
    }
    #undef R2
}
static int soft_frame_active(void) { return g_soft_frame; }
#undef R3
#endif
