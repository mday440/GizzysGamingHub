/* gpu_astc.h - ASTC LDR block decoder for MikesGameRoom.
 * Written from the Khronos ASTC specification (KHR_texture_compression_astc_ldr).
 * Decodes one 128-bit block into RGBA 16-bit values (UNORM16, 0..65535).
 * HDR endpoint modes and illegal blocks decode to the error colour (magenta). */
#ifndef MGR_GPU_ASTC_H
#define MGR_GPU_ASTC_H
#include <stdint.h>
#include <string.h>

typedef struct { const uint8_t *p; int pos, end; } astc_bits;
static uint32_t astc_rd(astc_bits *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++, b->pos++) if (b->pos < b->end && (b->p[b->pos >> 3] >> (b->pos & 7) & 1)) v |= 1u << i;
    return v;
}
static uint32_t astc_field(const uint8_t *p, int lo, int n) { astc_bits b = { p, lo, 128 }; return astc_rd(&b, n); }

/* quantisation levels 2..256 in order, with their (trits, quints, bits) */
static const uint16_t astc_levels[21] = { 2,3,4,5,6,8,10,12,16,20,24,32,40,48,64,80,96,128,160,192,256 };
static void astc_qinfo(int levels, int *t, int *q, int *bits) {
    *t = *q = 0; int n = levels;
    if (n % 3 == 0) { *t = 1; n /= 3; } else if (n % 5 == 0) { *q = 1; n /= 5; }
    int k = 0; while ((1 << k) < n) k++; *bits = k;
}
static int astc_ise_size(int count, int levels) {
    int t, q, b; astc_qinfo(levels, &t, &q, &b);
    return b * count + (t ? (8 * count + 4) / 5 : 0) + (q ? (7 * count + 2) / 3 : 0);
}
/* decode count ISE values into out[] (value = (trit/quint << bits) | m), out raw parts in tq[]/m[] */
static void astc_ise(astc_bits *s, int count, int levels, int *tq, int *m) {
    int t, q, nb; astc_qinfo(levels, &t, &q, &nb);
    for (int i = 0; i < count;) {
        if (t) {
            int mm[5]; uint32_t T = 0;
            mm[0] = astc_rd(s, nb); T |= astc_rd(s, 2);
            mm[1] = astc_rd(s, nb); T |= astc_rd(s, 2) << 2;
            mm[2] = astc_rd(s, nb); T |= astc_rd(s, 1) << 4;
            mm[3] = astc_rd(s, nb); T |= astc_rd(s, 2) << 5;
            mm[4] = astc_rd(s, nb); T |= astc_rd(s, 1) << 7;
            int tt[5]; uint32_t C;
            #define B_(x, n) (((x) >> (n)) & 1)
            if (((T >> 2) & 7) == 7) { C = ((T >> 5) & 7) << 2 | (T & 3); tt[4] = tt[3] = 2; }
            else { C = T & 31; if (((T >> 5) & 3) == 3) { tt[4] = 2; tt[3] = B_(T, 7); } else { tt[4] = B_(T, 7); tt[3] = (T >> 5) & 3; } }
            if ((C & 3) == 3) { tt[2] = 2; tt[1] = B_(C, 4); tt[0] = (B_(C, 3) << 1) | (B_(C, 2) & !B_(C, 3)); }
            else if (((C >> 2) & 3) == 3) { tt[2] = 2; tt[1] = 2; tt[0] = C & 3; }
            else { tt[2] = B_(C, 4); tt[1] = (C >> 2) & 3; tt[0] = (B_(C, 1) << 1) | (B_(C, 0) & !B_(C, 1)); }
            for (int k = 0; k < 5 && i < count; k++, i++) { tq[i] = tt[k]; m[i] = mm[k]; }
        } else if (q) {
            int mm[3]; uint32_t Q = 0;
            mm[0] = astc_rd(s, nb); Q |= astc_rd(s, 3);
            mm[1] = astc_rd(s, nb); Q |= astc_rd(s, 2) << 3;
            mm[2] = astc_rd(s, nb); Q |= astc_rd(s, 2) << 5;
            int qq[3];
            if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
                qq[2] = (B_(Q, 0) << 2) | ((B_(Q, 4) & !B_(Q, 0)) << 1) | (B_(Q, 3) & !B_(Q, 0)); qq[1] = qq[0] = 4;
            } else {
                uint32_t C;
                if (((Q >> 1) & 3) == 3) { qq[2] = 4; C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1); }
                else { qq[2] = (Q >> 5) & 3; C = Q & 31; }
                if ((C & 7) == 5) { qq[1] = 4; qq[0] = (C >> 3) & 3; } else { qq[1] = (C >> 3) & 3; qq[0] = C & 7; }
            }
            #undef B_
            for (int k = 0; k < 3 && i < count; k++, i++) { tq[i] = qq[k]; m[i] = mm[k]; }
        } else { tq[i] = 0; m[i] = astc_rd(s, nb); i++; }
    }
}
static int astc_rep(uint32_t v, int from, int to) {     /* bit replication */
    if (from == 0) return 0;
    int r = 0, have = 0;
    while (have < to) { int sh = to - have - from; r |= sh >= 0 ? (int)(v << sh) : (int)(v >> -sh); have += from; }
    return r & ((1 << to) - 1);
}
static int astc_unq_color(int tq, int m, int levels) {
    int t, q, nb; astc_qinfo(levels, &t, &q, &nb);
    if (!t && !q) return astc_rep((uint32_t)m, nb, 8);
    int A = (m & 1) ? 0x1FF : 0, B = 0, C = 0;
    #define b_(k) ((m >> (k)) & 1)
    if (t) switch (nb) {
        case 1: C = 204; break;
        case 2: { int b = b_(1); B = (b << 8) | (b << 4) | (b << 2) | (b << 1); C = 93; break; }
        case 3: { int c = b_(2), b = b_(1); B = (c << 8) | (b << 7) | (c << 3) | (b << 2) | (c << 1) | b; C = 44; break; }
        case 4: { int x = (m >> 1) & 7; B = (x << 6) | x; C = 22; break; }
        case 5: { int x = (m >> 1) & 15; B = (x << 5) | (x >> 2); C = 11; break; }
        case 6: { int x = (m >> 1) & 31; B = (x << 4) | (x >> 4); C = 5; break; }
    } else switch (nb) {
        case 1: C = 113; break;
        case 2: { int b = b_(1); B = (b << 8) | (b << 3) | (b << 2); C = 54; break; }
        case 3: { int c = b_(2), b = b_(1); B = (c << 8) | (b << 7) | (c << 2) | (b << 1) | c; C = 26; break; }
        case 4: { int x = (m >> 1) & 7; B = (x << 6) | (x >> 1); C = 13; break; }
        case 5: { int x = (m >> 1) & 15; B = (x << 5) | (x >> 3); C = 6; break; }
    }
    #undef b_
    int T = tq * C + B; T ^= A; return (A & 0x80) | (T >> 2);
}
static int astc_unq_weight(int tq, int m, int levels) {
    int t, q, nb; astc_qinfo(levels, &t, &q, &nb); int T;
    if (!t && !q) T = astc_rep((uint32_t)m, nb, 6);
    else if (nb == 0) T = t ? (int[]){0, 32, 63}[tq] : (int[]){0, 16, 32, 47, 63}[tq];
    else {
        int A = (m & 1) ? 0x7F : 0, B = 0, C = 0, b = (m >> 1) & 1, c = (m >> 2) & 1;
        if (t) { if (nb == 1) C = 50; else if (nb == 2) { B = (b << 6) | (b << 2) | b; C = 23; } else { B = (c << 6) | (b << 5) | (c << 1) | b; C = 11; } }
        else { if (nb == 1) C = 28; else { B = (b << 6) | (b << 1); C = 13; } }
        T = tq * C + B; T ^= A; T = (A & 0x20) | (T >> 2);
    }
    return T > 32 ? T + 1 : T;
}
static uint32_t astc_hash52(uint32_t p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4; p ^= p >> 5; p += p << 16; p ^= p >> 7; p ^= p >> 3; p ^= p << 6; p ^= p >> 17; return p;
}
static int astc_partition(uint32_t seed, int x, int y, int z, int count, int small) {
    if (count == 1) return 0;
    if (small) { x <<= 1; y <<= 1; z <<= 1; }
    seed += (uint32_t)(count - 1) * 1024;
    uint32_t r = astc_hash52(seed);
    uint32_t s[13];
    s[1] = r & 15; s[2] = (r >> 4) & 15; s[3] = (r >> 8) & 15; s[4] = (r >> 12) & 15; s[5] = (r >> 16) & 15; s[6] = (r >> 20) & 15;
    s[7] = (r >> 24) & 15; s[8] = (r >> 28) & 15; s[9] = (r >> 18) & 15; s[10] = (r >> 22) & 15; s[11] = (r >> 26) & 15; s[12] = ((r >> 30) | (r << 2)) & 15;
    for (int i = 1; i <= 12; i++) s[i] *= s[i];
    int sh1, sh2;
    if (seed & 1) { sh1 = (seed & 2) ? 4 : 5; sh2 = count == 3 ? 6 : 5; } else { sh1 = count == 3 ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
    int sh3 = (seed & 0x10) ? sh1 : sh2;
    for (int i = 1; i <= 8; i++) s[i] >>= (i & 1) ? sh1 : sh2;
    for (int i = 9; i <= 12; i++) s[i] >>= sh3;
    int a = (int)((s[1] * x + s[2] * y + s[11] * z + (r >> 14)) & 63);
    int b = (int)((s[3] * x + s[4] * y + s[12] * z + (r >> 10)) & 63);
    int c = (int)((s[5] * x + s[6] * y + s[9] * z + (r >> 6)) & 63);
    int d = (int)((s[7] * x + s[8] * y + s[10] * z + (r >> 2)) & 63);
    if (count < 4) d = 0; if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0; if (b >= c && b >= d) return 1; if (c >= d) return 2; return 3;
}
static int astc_clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void astc_bts(int *a, int *b) { *b = (*b >> 1) | (*a & 0x80); *a = (*a >> 1) & 0x3F; if (*a & 0x20) *a -= 0x40; }
/* returns 0 for HDR / unsupported modes */
static int astc_endpoints(int cem, const int *v, int e0[4], int e1[4]) {
    int a = 255;
    switch (cem) {
    case 0: e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1]; e0[3] = e1[3] = 255; return 1;
    case 1: { int l0 = (v[0] >> 2) | (v[1] & 0xC0), l1 = l0 + (v[1] & 0x3F); if (l1 > 255) l1 = 255;
              e0[0] = e0[1] = e0[2] = l0; e1[0] = e1[1] = e1[2] = l1; e0[3] = e1[3] = 255; return 1; }
    case 4: e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1]; e0[3] = v[2]; e1[3] = v[3]; return 1;
    case 5: { int v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3]; astc_bts(&v1, &v0); astc_bts(&v3, &v2);
              e0[0] = e0[1] = e0[2] = v0; e0[3] = v2; e1[0] = e1[1] = e1[2] = astc_clamp8(v0 + v1); e1[3] = astc_clamp8(v2 + v3); return 1; }
    case 6: for (int i = 0; i < 3; i++) { e0[i] = (v[i] * v[3]) >> 8; e1[i] = v[i]; } e0[3] = e1[3] = 255; return 1;
    case 10: for (int i = 0; i < 3; i++) { e0[i] = (v[i] * v[3]) >> 8; e1[i] = v[i]; } e0[3] = v[4]; e1[3] = v[5]; return 1;
    case 8: case 12: {
        int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
        int a0 = cem == 12 ? v[6] : a, a1 = cem == 12 ? v[7] : a;
        if (s1 >= s0) { e0[0] = v[0]; e0[1] = v[2]; e0[2] = v[4]; e0[3] = a0; e1[0] = v[1]; e1[1] = v[3]; e1[2] = v[5]; e1[3] = a1; }
        else { e0[0] = (v[1] + v[5]) >> 1; e0[1] = (v[3] + v[5]) >> 1; e0[2] = v[5]; e0[3] = a1;
               e1[0] = (v[0] + v[4]) >> 1; e1[1] = (v[2] + v[4]) >> 1; e1[2] = v[4]; e1[3] = a0; }
        return 1; }
    case 9: case 13: {
        int w[8]; for (int i = 0; i < 8; i++) w[i] = v[i];
        astc_bts(&w[1], &w[0]); astc_bts(&w[3], &w[2]); astc_bts(&w[5], &w[4]); if (cem == 13) astc_bts(&w[7], &w[6]);
        int a0 = cem == 13 ? w[6] : 255, a1 = cem == 13 ? w[6] + w[7] : 255;
        if (w[1] + w[3] + w[5] >= 0) {
            e0[0] = w[0]; e0[1] = w[2]; e0[2] = w[4]; e0[3] = a0;
            e1[0] = w[0] + w[1]; e1[1] = w[2] + w[3]; e1[2] = w[4] + w[5]; e1[3] = a1;
        } else {
            int r = w[0] + w[1], g = w[2] + w[3], b = w[4] + w[5];
            e0[0] = (r + b) >> 1; e0[1] = (g + b) >> 1; e0[2] = b; e0[3] = a1;
            e1[0] = (w[0] + w[4]) >> 1; e1[1] = (w[2] + w[4]) >> 1; e1[2] = w[4]; e1[3] = a0;
        }
        for (int i = 0; i < 4; i++) { e0[i] = astc_clamp8(e0[i]); e1[i] = astc_clamp8(e1[i]); }
        return 1; }
    }
    return 0;
}
static unsigned long long astc_err_count[16];
static void astc_error_r(uint16_t *out, int n, int why) { astc_err_count[why & 15]++;
    if (why == 3) { memset(out, 0, (size_t)n * 8); return; }   /* all-zero (reserved) blocks: transparent black; seen in the padding of partial edge blocks */ for (int i = 0; i < n; i++) { out[i * 4] = 0xFFFF; out[i * 4 + 1] = 0; out[i * 4 + 2] = 0xFFFF; out[i * 4 + 3] = 0xFFFF; } }

/* Decode one block of bw x bh texels into out[bw*bh*4] (UNORM16). srgb selects the sRGB endpoint expansion. */
static void astc_decode_block(const uint8_t blk[16], int bw, int bh, int srgb, uint16_t *out) {
    int nt = bw * bh;
    uint32_t bm = astc_field(blk, 0, 11);
    if ((bm & 0x1FF) == 0x1FC) {                                  /* void extent */
        if (bm & 0x200) { astc_error_r(out, nt, 1); return; }         /* HDR void extent */
        uint16_t c[4]; for (int i = 0; i < 4; i++) c[i] = (uint16_t)astc_field(blk, 64 + 16 * i, 16);
        if (srgb) for (int i = 0; i < 4; i++) c[i] = (uint16_t)((c[i] & 0xFF00) | 0x80);
        for (int i = 0; i < nt; i++) memcpy(out + i * 4, c, 8);
        return;
    }
    int W, H, R, Hp, D;
    if (bm & 3) {
        R = (int)(((bm >> 4) & 1) | ((bm & 3) << 1)); int A = (bm >> 5) & 3, B = (bm >> 7) & 3;
        switch ((bm >> 2) & 3) {
        case 0: W = B + 4; H = A + 2; break;
        case 1: W = B + 8; H = A + 2; break;
        case 2: W = A + 2; H = B + 8; break;
        default: B &= 1; if (bm & 0x100) { W = B + 2; H = A + 2; } else { W = A + 2; H = B + 6; } break;
        }
        Hp = (bm >> 9) & 1; D = (bm >> 10) & 1;
    } else {
        R = (int)(((bm >> 4) & 1) | (((bm >> 2) & 3) << 1)); int A = (bm >> 5) & 3;
        Hp = (bm >> 9) & 1; D = (bm >> 10) & 1;
        switch ((bm >> 7) & 3) {
        case 0: W = 12; H = A + 2; break;
        case 1: W = A + 2; H = 12; break;
        case 3: if (bm & 0x20) { W = 10; H = 6; } else { W = 6; H = 10; } if (A & 2) { astc_error_r(out, nt, 2); return; } break;
        default: W = A + 6; H = (int)((bm >> 9) & 3) + 6; Hp = 0; D = 0; break;
        }
        if ((bm & 0xF) == 0) { astc_error_r(out, nt, 3); return; }
    }
    if (R < 2) { astc_error_r(out, nt, 4); return; }
    static const int wl[2][8] = { {0,0,2,3,4,5,6,8}, {0,0,10,12,16,20,24,32} };
    int wlev = wl[Hp][R];
    int nweights = W * H * (D + 1);
    if (W > bw || H > bh || nweights > 64) { astc_error_r(out, nt, 5); return; }
    int wbits = astc_ise_size(nweights, wlev);
    if (wbits < 24 || wbits > 96) { astc_error_r(out, nt, 6); return; }
    int parts = (int)astc_field(blk, 11, 2) + 1;
    if (parts == 4 && D) { astc_error_r(out, nt, 7); return; }
    int cem[4], cstart, extra = 0; uint32_t pseed = 0;
    if (parts == 1) { cem[0] = (int)astc_field(blk, 13, 4); cstart = 17; }
    else {
        pseed = astc_field(blk, 13, 10); cstart = 29;
        uint32_t f = astc_field(blk, 23, 6);
        if ((f & 3) == 0) { for (int i = 0; i < parts; i++) cem[i] = (int)(f >> 2); }
        else {
            extra = 3 * parts - 4;
            uint32_t all = (f >> 2) | (astc_field(blk, 128 - wbits - extra, extra) << 4);
            int base = (int)(f & 3) - 1;
            for (int i = 0; i < parts; i++) {
                int c = (int)((all >> i) & 1), m = (int)((all >> (parts + 2 * i)) & 3);
                cem[i] = ((base + c) << 2) | m;
            }
        }
    }
    int ccs = D ? (int)astc_field(blk, 128 - wbits - extra - 2, 2) : -1;
    int nvals = 0; for (int i = 0; i < parts; i++) nvals += ((cem[i] >> 2) + 1) * 2;
    if (nvals > 18) { astc_error_r(out, nt, 8); return; }
    int cbits = 128 - wbits - extra - (D ? 2 : 0) - cstart;
    if (cbits < (13 * nvals + 4) / 5) { astc_error_r(out, nt, 9); return; }
    int clev = 0; for (int k = 20; k >= 0; k--) if (astc_levels[k] >= 6 && astc_ise_size(nvals, astc_levels[k]) <= cbits) { clev = astc_levels[k]; break; }
    if (!clev) { astc_error_r(out, nt, 10); return; }
    int tq[64], mm[64], cv[18];
    astc_bits s = { blk, cstart, cstart + cbits };
    astc_ise(&s, nvals, clev, tq, mm);
    for (int i = 0; i < nvals; i++) cv[i] = astc_unq_color(tq[i], mm[i], clev);
    int ep[4][2][4]; int vi = 0;
    for (int p = 0; p < parts; p++) {
        if (!astc_endpoints(cem[p], cv + vi, ep[p][0], ep[p][1])) { astc_error_r(out, nt, 11); return; }
        vi += ((cem[p] >> 2) + 1) * 2;
    }
    uint8_t rev[16]; for (int i = 0; i < 16; i++) { uint8_t x = blk[15 - i]; x = (uint8_t)(((x * 0x0802u & 0x22110u) | (x * 0x8020u & 0x88440u)) * 0x10101u >> 16); rev[i] = x; }
    astc_bits ws = { rev, 0, wbits };
    astc_ise(&ws, nweights, wlev, tq, mm);
    int wq[64]; for (int i = 0; i < nweights; i++) wq[i] = astc_unq_weight(tq[i], mm[i], wlev);
    int Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1);
    int small = nt < 31;
    for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
        int cs = Ds * x, ct = Dt * y;
        int gs = (cs * (W - 1) + 32) >> 6, gt = (ct * (H - 1) + 32) >> 6;
        int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
        int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
        int v0 = js + jt * W;
        int pw[2];
        for (int pl = 0; pl <= D; pl++) {
            #define WQ(i) ((i) < W * H ? wq[(i) * (D + 1) + pl] : 0)
            pw[pl] = (w00 * WQ(v0) + w01 * WQ(v0 + 1) + w10 * WQ(v0 + W) + w11 * WQ(v0 + W + 1) + 8) >> 4;
            #undef WQ
        }
        int p = astc_partition(pseed, x, y, 0, parts, small);
        uint16_t *o = out + (y * bw + x) * 4;
        for (int c = 0; c < 4; c++) {
            int w = (c == ccs) ? pw[1] : pw[0];
            int c0 = ep[p][0][c], c1 = ep[p][1][c];
            if (srgb) { c0 = (c0 << 8) | 0x80; c1 = (c1 << 8) | 0x80; } else { c0 = (c0 << 8) | c0; c1 = (c1 << 8) | c1; }
            int r = (c0 * (64 - w) + c1 * w + 32) >> 6;
            o[c] = (uint16_t)r;
        }
    }
}
#endif
