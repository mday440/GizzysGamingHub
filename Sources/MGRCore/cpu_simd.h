/*
 * Floating-point and Advanced SIMD (NEON) execution for cpu.c.
 * Included once from cpu.c; relies on its mgr_emu definition and helpers.
 *
 * Scalar FP: arithmetic, FMA, compares, selects, rounding, conversions
 * (FP<->FP, FP<->int incl. fixed point), FMOV immediate.
 * Vector: integer & FP three-same, two-register misc, across-lanes,
 * shifts by immediate, widening/narrowing, permutes, EXT, TBL, by-element,
 * DUP, plus scalar pairwise / scalar-vector forms compilers emit.
 */
#include <math.h>

/* ---------- element access ---------- */
static inline uint64_t vget(const mgr_emu *e, int r, int idx, int esz) {
    uint64_t v = 0;
    memcpy(&v, e->q[r].b + idx * esz, (size_t)esz);
    return v;
}
static inline void vset(uint8_t *dst, int idx, int esz, uint64_t v) { memcpy(dst + idx * esz, &v, (size_t)esz); }
static inline void vcommit(mgr_emu *e, int rd, const uint8_t r[16], int Q) {
    memcpy(e->q[rd].b, r, 16);
    if (!Q) memset(e->q[rd].b + 8, 0, 8);
}

/* ---------- half precision ---------- */
static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, ex = (h >> 10) & 0x1F, m = h & 0x3FF, f;
    if (ex == 0) {
        if (!m) f = s;
        else { int sh = 0; while (!(m & 0x400)) { m <<= 1; sh++; } m &= 0x3FF; f = s | ((uint32_t)(113 - sh) << 23) | (m << 13); }
    } else if (ex == 31) f = s | 0x7F800000u | (m << 13);
    else f = s | ((ex + 112) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}
static uint16_t float_to_half(float x) {
    uint32_t f; memcpy(&f, &x, 4);
    uint32_t s = (f >> 16) & 0x8000, ex = (f >> 23) & 0xFF, m = f & 0x7FFFFF;
    if (ex == 0xFF) return (uint16_t)(s | 0x7C00 | (m ? 0x200 | (m >> 13) : 0));
    int e2 = (int)ex - 127 + 15;
    if (e2 >= 31) return (uint16_t)(s | 0x7C00);
    if (e2 <= 0) {
        if (e2 < -10) return (uint16_t)s;
        m |= 0x800000;
        int shift = 14 - e2;
        uint32_t hm = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (hm & 1))) hm++;
        return (uint16_t)(s | hm);
    }
    uint32_t hm = m >> 13, rem = m & 0x1FFF;
    uint32_t r = s | ((uint32_t)e2 << 10) | hm;
    if (rem > 0x1000 || (rem == 0x1000 && (hm & 1))) r++;
    return (uint16_t)r;
}

/* ---------- scalar FP register access (type: 0 single, 1 double, 3 half) ---------- */
static double fget(const mgr_emu *e, int r, int type) {
    if (type == 1) { double d; memcpy(&d, e->q[r].b, 8); return d; }
    if (type == 0) { float f; memcpy(&f, e->q[r].b, 4); return f; }
    uint16_t h; memcpy(&h, e->q[r].b, 2); return half_to_float(h);
}
static void fset(mgr_emu *e, int r, int type, double v) {
    memset(e->q[r].b, 0, 16);
    if (type == 1) memcpy(e->q[r].b, &v, 8);
    else if (type == 0) { float f = (float)v; memcpy(e->q[r].b, &f, 4); }
    else { uint16_t h = float_to_half((float)v); memcpy(e->q[r].b, &h, 2); }
}
static double bits_to_fp(uint64_t b, int esz) {
    if (esz == 8) { double d; memcpy(&d, &b, 8); return d; }
    if (esz == 4) { float f; uint32_t w = (uint32_t)b; memcpy(&f, &w, 4); return f; }
    return half_to_float((uint16_t)b);
}
static uint64_t fp_to_bits(double v, int esz) {
    if (esz == 8) { uint64_t b; memcpy(&b, &v, 8); return b; }
    if (esz == 4) { float f = (float)v; uint32_t w; memcpy(&w, &f, 4); return w; }
    return float_to_half((float)v);
}

static double fp_max(double a, double b) {
    if (isnan(a) || isnan(b)) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? b : a;
    return a > b ? a : b;
}
static double fp_min(double a, double b) {
    if (isnan(a) || isnan(b)) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? a : b;
    return a < b ? a : b;
}
static double fp_maxnm(double a, double b) { if (isnan(a) && !isnan(b)) return b; if (isnan(b) && !isnan(a)) return a; return fp_max(a, b); }
static double fp_minnm(double a, double b) { if (isnan(a) && !isnan(b)) return b; if (isnan(b) && !isnan(a)) return a; return fp_min(a, b); }

/* single-precision results computed in double then rounded once are exact for + - * / sqrt */
static double fp_round_to(double v, int esz) { return esz == 4 ? (double)(float)v : v; }

static double fp_fma(double a, double b, double c, int esz) {
    if (esz == 4) return fmaf((float)a, (float)b, (float)c);
    return fma(a, b, c);
}

static double fp_round_int(double v, int mode) { /* 0 N, 1 P, 2 M, 3 Z, 4 A, 5 current(X/I) */
    switch (mode) {
    case 1: return ceil(v);
    case 2: return floor(v);
    case 3: return trunc(v);
    case 4: return round(v);
    default: return nearbyint(v);
    }
}

static uint64_t fp_to_int(double v, int mode, int is_unsigned, int bits) {
    if (isnan(v)) return 0;
    double r = fp_round_int(v, mode);
    if (is_unsigned) {
        if (r <= 0) return 0;
        double lim = bits == 64 ? 18446744073709551616.0 : (bits == 32 ? 4294967296.0 : (double)(1ull << bits));
        if (r >= lim) return ones(bits);
        return (uint64_t)r;
    }
    double hi = bits == 64 ? 9223372036854775808.0 : (double)(1ull << (bits - 1));
    if (r >= hi) return ones(bits - 1);
    if (r < -hi) return ((uint64_t)1 << (bits - 1)) & ones(bits);
    return (uint64_t)(int64_t)r & ones(bits);
}

/* int -> fp with a single correct rounding to the destination size */
static double int_to_fp(uint64_t v, int is_signed, int src_bits, int esz) {
    if (is_signed) {
        int64_t s = sext(v, src_bits);
        if (esz == 4) return (float)s;
        if (esz == 2) return (float)s;
        return (double)s;
    }
    v &= ones(src_bits);
    if (esz == 4 || esz == 2) return (float)v;
    return (double)v;
}

static void fp_compare(mgr_emu *e, double a, double b) {
    if (isnan(a) || isnan(b)) { e->n = 0; e->z = 0; e->c = 1; e->v = 1; }
    else if (a == b) { e->n = 0; e->z = 1; e->c = 1; e->v = 0; }
    else if (a < b) { e->n = 1; e->z = 0; e->c = 0; e->v = 0; }
    else { e->n = 0; e->z = 0; e->c = 1; e->v = 0; }
}

static double vfp_imm8(uint32_t imm8, int esz) {
    /* VFPExpandImm */
    int sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
    int exp3 = (int)((imm8 >> 4) & 3);
    double frac = 1.0 + (imm8 & 15) / 16.0;
    int expo = b6 ? exp3 - 3 : exp3 + 1; /* exponent in [-3, 4] */
    double v = ldexp(frac, expo);
    (void)esz;
    return sign ? -v : v;
}

/* ================= scalar floating point ================= */
static int exec_fp_scalar(mgr_emu *e, uint32_t i) {
    int rd = (int)(i & 31), rn = (int)((i >> 5) & 31), rm = (int)((i >> 16) & 31);
    int type = (i >> 22) & 3;
    int esz = type == 1 ? 8 : (type == 0 ? 4 : 2);

    if ((i & 0x5F000000) == 0x1F000000) { /* FMADD family (bit 31 = M = 0) */
        int o1 = (i >> 21) & 1, o0 = (i >> 15) & 1, ra = (int)((i >> 10) & 31);
        double a = fget(e, ra, type), n = fget(e, rn, type), m = fget(e, rm, type);
        double r;
        if (!o1 && !o0) r = fp_fma(n, m, a, esz);
        else if (!o1 && o0) r = fp_fma(-n, m, a, esz);
        else if (o1 && !o0) r = fp_fma(-n, m, -a, esz);
        else r = fp_fma(n, m, -a, esz);
        fset(e, rd, type, r);
        return 1;
    }
    if ((i & 0x5F200000) != 0x1E200000) {
        /* fixed-point <-> FP conversion (bit 21 = 0) */
        if ((i & 0x7F200000) == 0x1E000000) {
            int sf = (int)(i >> 31), rmode = (i >> 19) & 3, opcode = (i >> 16) & 7, scale = (i >> 10) & 0x3F;
            int fbits = 64 - scale, bits = sf ? 64 : 32;
            double mul = ldexp(1.0, fbits);
            if (rmode == 0 && (opcode == 2 || opcode == 3)) { /* SCVTF/UCVTF fixed */
                uint64_t v = W(X(e, rn), sf);
                double r = esz == 8 ? (opcode == 2 ? (double)sext(v, bits) : (double)v) / mul
                                    : (double)((opcode == 2 ? (float)sext(v, bits) : (float)v) / (float)mul);
                fset(e, rd, type, r);
                return 1;
            }
            if (rmode == 3 && (opcode == 0 || opcode == 1)) { /* FCVTZS/FCVTZU fixed */
                SETX(e, rd, fp_to_int(fget(e, rn, type) * mul, 3, opcode == 1, bits));
                return 1;
            }
        }
        return 0;
    }
    int sf = (int)(i >> 31);
    uint32_t bits1510 = (i >> 10) & 0x3F;

    if (bits1510 == 0) { /* FP <-> integer */
        int rmode = (i >> 19) & 3, opcode = (i >> 16) & 7, bits = sf ? 64 : 32;
        if (opcode == 2 || opcode == 3) { /* SCVTF / UCVTF */
            fset(e, rd, type, int_to_fp(X(e, rn), opcode == 2, bits, esz));
            return 1;
        }
        if (opcode <= 1) { /* FCVT{N,P,M,Z}{S,U} */
            static const int map[4] = { 0, 1, 2, 3 };
            SETX(e, rd, fp_to_int(fget(e, rn, type), map[rmode], opcode == 1, bits));
            return 1;
        }
        if ((opcode == 4 || opcode == 5) && rmode == 0) { /* FCVTA{S,U} */
            SETX(e, rd, fp_to_int(fget(e, rn, type), 4, opcode == 5, bits));
            return 1;
        }
        if (opcode == 6 || opcode == 7) { /* FMOV general <-> FP */
            if (rmode == 0 && opcode == 6) { uint64_t v = 0; memcpy(&v, e->q[rn].b, (size_t)(type == 3 ? 2 : (sf ? 8 : 4))); SETX(e, rd, v); return 1; }
            if (rmode == 0 && opcode == 7) { uint64_t v = X(e, rn); memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, &v, (size_t)(type == 3 ? 2 : (sf ? 8 : 4))); return 1; }
            if (sf && type == 2 && rmode == 1 && opcode == 6) { uint64_t v; memcpy(&v, e->q[rn].b + 8, 8); SETX(e, rd, v); return 1; }
            if (sf && type == 2 && rmode == 1 && opcode == 7) { uint64_t v = X(e, rn); memcpy(e->q[rd].b + 8, &v, 8); return 1; }
        }
        return 0;
    }
    if ((bits1510 & 0x1F) == 0x10) { /* 1-source */
        int opcode = (i >> 15) & 0x3F;
        double a = fget(e, rn, type);
        switch (opcode) {
        case 0: { uint8_t tmp[16]; memcpy(tmp, e->q[rn].b, 16); memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, tmp, (size_t)esz); return 1; }
        case 1: fset(e, rd, type, fabs(a)); return 1;
        case 2: { uint64_t b = fp_to_bits(a, esz) ^ (1ull << (esz * 8 - 1)); memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, &b, (size_t)esz); return 1; }
        case 3: fset(e, rd, type, fp_round_to(sqrt(a), esz)); return 1;
        case 4: case 5: case 7: { int dt = opcode == 4 ? 0 : (opcode == 5 ? 1 : 3); fset(e, rd, dt, a); return 1; }
        case 8: case 9: case 10: case 11: case 12: case 14: case 15: {
            static const int m[8] = { 0, 1, 2, 3, 4, 5, 5, 5 };
            fset(e, rd, type, fp_round_int(a, m[opcode - 8])); return 1;
        }
        default: return 0;
        }
    }
    if ((bits1510 & 0xF) == 0x8 && (i & 0x1F) <= 0x18) { /* FCMP / FCMPE */
        int zero = (i >> 3) & 1;
        fp_compare(e, fget(e, rn, type), zero ? 0.0 : fget(e, rm, type));
        return 1;
    }
    if ((bits1510 & 0x7) == 0x4 && rn == 0) { /* FMOV immediate */
        fset(e, rd, type, vfp_imm8((i >> 13) & 0xFF, esz));
        return 1;
    }
    if ((bits1510 & 3) == 1) { /* FCCMP / FCCMPE */
        int cond = (i >> 12) & 15;
        if (cond_holds(e, cond)) fp_compare(e, fget(e, rn, type), fget(e, rm, type));
        else { int nzcv = (int)(i & 15); e->n = (nzcv >> 3) & 1; e->z = (nzcv >> 2) & 1; e->c = (nzcv >> 1) & 1; e->v = nzcv & 1; }
        return 1;
    }
    if ((bits1510 & 3) == 2) { /* 2-source */
        int opcode = (i >> 12) & 15;
        double a = fget(e, rn, type), b = fget(e, rm, type), r;
        switch (opcode) {
        case 0: r = a * b; break;
        case 1: r = a / b; break;
        case 2: r = a + b; break;
        case 3: r = a - b; break;
        case 4: r = fp_max(a, b); break;
        case 5: r = fp_min(a, b); break;
        case 6: r = fp_maxnm(a, b); break;
        case 7: r = fp_minnm(a, b); break;
        case 8: r = -(fp_round_to(a * b, esz)); break;
        default: return 0;
        }
        fset(e, rd, type, fp_round_to(r, esz));
        return 1;
    }
    if ((bits1510 & 3) == 3) { /* FCSEL */
        int cond = (i >> 12) & 15;
        int src = cond_holds(e, cond) ? rn : rm;
        uint8_t tmp[16]; memcpy(tmp, e->q[src].b, 16);
        memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, tmp, (size_t)esz);
        return 1;
    }
    return 0;
}

/* ================= vector helpers ================= */
static uint64_t sat_s(int64_t v, int bits) { int64_t hi = (int64_t)ones(bits - 1), lo = -hi - 1; return (uint64_t)(v > hi ? hi : v < lo ? lo : v) & ones(bits); }
static uint64_t sat_u(int64_t v, int bits) { if (v < 0) return 0; if ((uint64_t)v > ones(bits)) return ones(bits); return (uint64_t)v; }

/* integer three-same element op; returns 0 if not handled */
static int int3(int U, int op, int bits, uint64_t a, uint64_t b, uint64_t d, uint64_t *out) {
    int64_t sa = sext(a, bits), sb = sext(b, bits);
    uint64_t ua = a & ones(bits), ub = b & ones(bits), r;
    switch (op) {
    case 0x00: r = U ? (ua + ub) >> 1 : (uint64_t)((sa + sb) >> 1); break;               /* HADD */
    case 0x01: r = U ? sat_u((int64_t)(ua + ub), bits) : sat_s(sa + sb, bits); if (bits == 64) r = U ? (ua + ub < ua ? ~0ull : ua + ub) : (uint64_t)(sa + sb); break; /* QADD */
    case 0x02: r = U ? (ua + ub + 1) >> 1 : (uint64_t)((sa + sb + 1) >> 1); break;        /* RHADD */
    case 0x04: r = U ? (ua - ub) >> 1 : (uint64_t)((sa - sb) >> 1); break;               /* HSUB */
    case 0x05: r = U ? (ua < ub ? 0 : ua - ub) : sat_s(sa - sb, bits); break;            /* QSUB */
    case 0x06: r = (U ? ua > ub : sa > sb) ? ~0ull : 0; break;                           /* CMGT/CMHI */
    case 0x07: r = (U ? ua >= ub : sa >= sb) ? ~0ull : 0; break;                         /* CMGE/CMHS */
    case 0x08: { int sh = (int)(int8_t)(b & 0xFF);                                       /* SSHL/USHL */
        if (sh >= 0) r = sh >= bits ? 0 : ua << sh;
        else r = U ? (-sh >= bits ? 0 : ua >> -sh) : (uint64_t)(-sh >= bits ? (sa < 0 ? -1 : 0) : sa >> -sh);
        break; }
    case 0x0C: r = U ? (ua > ub ? ua : ub) : (uint64_t)(sa > sb ? sa : sb); break;       /* MAX */
    case 0x0D: r = U ? (ua < ub ? ua : ub) : (uint64_t)(sa < sb ? sa : sb); break;       /* MIN */
    case 0x0E: r = U ? (ua > ub ? ua - ub : ub - ua) : (uint64_t)(sa > sb ? sa - sb : sb - sa); break; /* ABD */
    case 0x0F: r = d + (U ? (ua > ub ? ua - ub : ub - ua) : (uint64_t)(sa > sb ? sa - sb : sb - sa)); break; /* ABA */
    case 0x10: r = U ? ua - ub : ua + ub; break;                                         /* ADD/SUB */
    case 0x11: r = U ? (ua == ub ? ~0ull : 0) : ((ua & ub) ? ~0ull : 0); break;          /* CMEQ/CMTST */
    case 0x12: r = U ? d - ua * ub : d + ua * ub; break;                                 /* MLA/MLS */
    case 0x13: if (U) return 0; r = ua * ub; break;                                      /* MUL */
    default: return 0;
    }
    *out = r & ones(bits);
    return 1;
}

static int fp3(int U, int a_bit, int op, int esz, double x, double y, double d, uint64_t *out) {
    double r; int cmp = -1;
    int key = (U << 6) | (a_bit << 5) | op;
    switch (key) {
    case 0x18: r = fp_maxnm(x, y); break;                 /* FMAXNM */
    case 0x19: r = fp_fma(x, y, d, esz); break;           /* FMLA */
    case 0x1A: r = x + y; break;                          /* FADD */
    case 0x1B: r = x * y; break;                          /* FMULX (approx) */
    case 0x1C: cmp = x == y; break;                       /* FCMEQ */
    case 0x1E: r = fp_max(x, y); break;                   /* FMAX */
    case 0x1F: r = 2.0 - x * y; break;                    /* FRECPS */
    case 0x38: r = fp_minnm(x, y); break;                 /* FMINNM */
    case 0x39: r = fp_fma(-x, y, d, esz); break;          /* FMLS */
    case 0x3A: r = x - y; break;                          /* FSUB */
    case 0x3E: r = fp_min(x, y); break;                   /* FMIN */
    case 0x3F: r = (3.0 - x * y) / 2.0; break;            /* FRSQRTS */
    case 0x5B: r = x * y; break;                          /* FMUL */
    case 0x5C: cmp = x >= y; break;                       /* FCMGE */
    case 0x5D: cmp = fabs(x) >= fabs(y); break;           /* FACGE */
    case 0x5F: r = x / y; break;                          /* FDIV */
    case 0x7A: r = fabs(x - y); break;                    /* FABD */
    case 0x7C: cmp = x > y; break;                        /* FCMGT */
    case 0x7D: cmp = fabs(x) > fabs(y); break;            /* FACGT */
    default: return 0;
    }
    if (cmp >= 0) { *out = cmp ? ones(esz * 8) : 0; return 1; }
    *out = fp_to_bits(fp_round_to(r, esz), esz);
    return 1;
}

/* ================= Advanced SIMD ================= */
static int exec_simd(mgr_emu *e, uint32_t i) {
    int rd = (int)(i & 31), rn = (int)((i >> 5) & 31), rm = (int)((i >> 16) & 31);
    int Q = (i >> 30) & 1, U = (i >> 29) & 1, size = (i >> 22) & 3;
    uint8_t r[16] = {0};
    int scalar = (i >> 28) & 1; /* 01x11110 scalar AdvSIMD vs 0Qx01110 vector */

    /* ---- three same (vector or scalar) ---- */
    if ((i & 0x9F200400) == 0x0E200400 || (i & 0xDF200400) == 0x5E200400) {
        int op = (i >> 11) & 0x1F;
        if (op >= 0x18) { /* FP */
            int sz = (i >> 22) & 1, a_bit = (i >> 23) & 1, esz = sz ? 8 : 4;
            int n = scalar ? 1 : (Q ? 16 : 8) / esz;
            if (U && !a_bit && (op == 0x1A || op == 0x18 || op == 0x1E) && !scalar) { /* FADDP / FMAXNMP / FMAXP */
                uint8_t cat[32]; memcpy(cat, e->q[rn].b, 16); memcpy(cat + (Q ? 16 : 8), e->q[rm].b, 16);
                for (int k = 0; k < n; k++) {
                    uint64_t xa = 0, xb = 0; memcpy(&xa, cat + 2 * k * esz, (size_t)esz); memcpy(&xb, cat + (2 * k + 1) * esz, (size_t)esz);
                    double fa = bits_to_fp(xa, esz), fb = bits_to_fp(xb, esz);
                    double v = op == 0x1A ? fa + fb : op == 0x18 ? fp_maxnm(fa, fb) : fp_max(fa, fb);
                    vset(r, k, esz, fp_to_bits(fp_round_to(v, esz), esz));
                }
                vcommit(e, rd, r, Q);
                return 1;
            }
            if (U && a_bit && (op == 0x18 || op == 0x1E) && !scalar) { /* FMINNMP / FMINP */
                uint8_t cat[32]; memcpy(cat, e->q[rn].b, 16); memcpy(cat + (Q ? 16 : 8), e->q[rm].b, 16);
                for (int k = 0; k < n; k++) {
                    uint64_t xa = 0, xb = 0; memcpy(&xa, cat + 2 * k * esz, (size_t)esz); memcpy(&xb, cat + (2 * k + 1) * esz, (size_t)esz);
                    double fa = bits_to_fp(xa, esz), fb = bits_to_fp(xb, esz);
                    vset(r, k, esz, fp_to_bits(op == 0x18 ? fp_minnm(fa, fb) : fp_min(fa, fb), esz));
                }
                vcommit(e, rd, r, Q);
                return 1;
            }
            for (int k = 0; k < n; k++) {
                uint64_t out;
                double x = bits_to_fp(vget(e, rn, k, esz), esz), y = bits_to_fp(vget(e, rm, k, esz), esz), d = bits_to_fp(vget(e, rd, k, esz), esz);
                if (!fp3(U, a_bit, op, esz, x, y, d, &out)) return 0;
                vset(r, k, esz, out);
            }
            if (scalar) { memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, r, (size_t)esz); } else vcommit(e, rd, r, Q);
            return 1;
        }
        if (op == 0x03) return 0; /* logical: handled by caller */
        int esz = 1 << size, bits = esz * 8;
        int n = scalar ? 1 : (Q ? 16 : 8) / esz;
        if (op == 0x17 || op == 0x14 || op == 0x15) { /* ADDP / SMAXP,UMAXP / SMINP,UMINP */
            if (scalar) return 0;
            uint8_t cat[32]; memcpy(cat, e->q[rn].b, 16); memcpy(cat + (Q ? 16 : 8), e->q[rm].b, 16);
            for (int k = 0; k < n; k++) {
                uint64_t a = 0, b = 0, v;
                memcpy(&a, cat + 2 * k * esz, (size_t)esz); memcpy(&b, cat + (2 * k + 1) * esz, (size_t)esz);
                if (op == 0x17) { if (U) return 0; v = a + b; }
                else if (op == 0x14) v = U ? (a > b ? a : b) : (sext(a, bits) > sext(b, bits) ? a : b);
                else v = U ? (a < b ? a : b) : (sext(a, bits) < sext(b, bits) ? a : b);
                vset(r, k, esz, v & ones(bits));
            }
            vcommit(e, rd, r, Q);
            return 1;
        }
        for (int k = 0; k < n; k++) {
            uint64_t out;
            if (!int3(U, op, bits, vget(e, rn, k, esz), vget(e, rm, k, esz), vget(e, rd, k, esz), &out)) return 0;
            vset(r, k, esz, out);
        }
        if (scalar) { memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, r, (size_t)esz); } else vcommit(e, rd, r, Q);
        return 1;
    }

    /* ---- two-register misc (vector or scalar) ---- */
    if ((i & 0x9F3E0C00) == 0x0E200800 || (i & 0xDF3E0C00) == 0x5E200800) {
        int op = (i >> 12) & 0x1F;
        int esz = 1 << size, bits = esz * 8;
        int n = scalar ? 1 : (Q ? 16 : 8) / esz;
        /* FP group: ops 0x0C..0x1F with size bit 1 meaning double or a-bit */
        int fpgrp = op >= 0x0C && !(op == 0x12 || op == 0x13 || op == 0x14);
        if (op == 0x00 && !U) { /* REV64 */
            for (int k = 0; k < n; k++) { int per = 8 / esz; int grp = k / per, pos = k % per; vset(r, grp * per + (per - 1 - pos), esz, vget(e, rn, k, esz)); }
            vcommit(e, rd, r, Q); return 1;
        }
        if (op == 0x00 && U) { /* REV32 */
            for (int k = 0; k < n; k++) { int per = 4 / esz; int grp = k / per, pos = k % per; vset(r, grp * per + (per - 1 - pos), esz, vget(e, rn, k, esz)); }
            vcommit(e, rd, r, Q); return 1;
        }
        if (op == 0x01 && !U) { /* REV16 */
            for (int k = 0; k < (Q ? 16 : 8); k += 2) { r[k] = e->q[rn].b[k + 1]; r[k + 1] = e->q[rn].b[k]; }
            vcommit(e, rd, r, Q); return 1;
        }
        if (op == 0x05) { /* CNT / NOT / RBIT */
            for (int k = 0; k < (Q ? 16 : 8); k++) {
                uint8_t b = e->q[rn].b[k];
                if (!U) r[k] = (uint8_t)__builtin_popcount(b);
                else if (size == 0) r[k] = (uint8_t)~b;
                else { uint8_t o = 0; for (int t = 0; t < 8; t++) if (b >> t & 1) o |= (uint8_t)(1 << (7 - t)); r[k] = o; }
            }
            vcommit(e, rd, r, Q); return 1;
        }
        if (op == 0x04) { /* CLS / CLZ */
            for (int k = 0; k < n; k++) {
                uint64_t a = vget(e, rn, k, esz); int c = 0;
                if (U) { for (int t = bits - 1; t >= 0 && !((a >> t) & 1); t--) c++; }
                else { int s = (int)((a >> (bits - 1)) & 1); for (int t = bits - 2; t >= 0 && (int)((a >> t) & 1) == s; t--) c++; }
                vset(r, k, esz, (uint64_t)c);
            }
            vcommit(e, rd, r, Q); return 1;
        }
        if (op == 0x02 || op == 0x06) { /* SADDLP/UADDLP, SADALP/UADALP */
            int nd = (Q ? 16 : 8) / (esz * 2);
            for (int k = 0; k < nd; k++) {
                uint64_t a = vget(e, rn, 2 * k, esz), b = vget(e, rn, 2 * k + 1, esz);
                uint64_t s = U ? a + b : (uint64_t)(sext(a, bits) + sext(b, bits));
                if (op == 0x06) s += vget(e, rd, k, esz * 2);
                vset(r, k, esz * 2, s & ones(bits * 2));
            }
            vcommit(e, rd, r, Q); return 1;
        }
        if (op >= 0x08 && op <= 0x0B) { /* CMGT/CMGE/CMEQ/CMLE/CMLT zero, ABS/NEG */
            for (int k = 0; k < n; k++) {
                int64_t a = sext(vget(e, rn, k, esz), bits); uint64_t v;
                if (op == 0x08) v = (U ? a >= 0 : a > 0) ? ~0ull : 0;
                else if (op == 0x09) v = (U ? a <= 0 : a == 0) ? ~0ull : 0;
                else if (op == 0x0A) { if (U) return 0; v = a < 0 ? ~0ull : 0; }
                else v = U ? (uint64_t)(-a) : (uint64_t)(a < 0 ? -a : a);
                vset(r, k, esz, v & ones(bits));
            }
            if (scalar) { memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, r, (size_t)esz); } else vcommit(e, rd, r, Q);
            return 1;
        }
        if (op == 0x12 || op == 0x14) { /* XTN/SQXTN/UQXTN/SQXTUN (op 0x12 U=0 XTN, U=1 SQXTUN; 0x14 SQXTN/UQXTN) */
            int nd = 8 / esz;
            for (int k = 0; k < nd; k++) {
                uint64_t a = vget(e, rn, k, esz * 2), v;
                if (op == 0x12 && !U) v = a & ones(bits);
                else if (op == 0x12) v = sat_u(sext(a, bits * 2), bits);
                else v = U ? (a > ones(bits) ? ones(bits) : a) : sat_s(sext(a, bits * 2), bits);
                vset(r, k, esz, v);
            }
            if (Q) { memcpy(e->q[rd].b + 8, r, 8); } else { memcpy(e->q[rd].b, r, 8); memset(e->q[rd].b + 8, 0, 8); }
            return 1;
        }
        if (op == 0x13 && U) { /* SHLL */
            int nd = 8 / esz;
            for (int k = 0; k < nd; k++) vset(r, k, esz * 2, vget(e, rn, k + (Q ? nd : 0), esz) << bits);
            memcpy(e->q[rd].b, r, 16); return 1;
        }
        if (fpgrp) {
            int sz = size & 1, a_bit = size >> 1, fesz = sz ? 8 : 4;
            int fn = scalar ? 1 : (Q ? 16 : 8) / fesz;
            int key = (U << 6) | (a_bit << 5) | op;
            if (op == 0x16 && !a_bit) { /* FCVTN/FCVTXN: narrow double->single or single->half */
                int dsz = sz ? 4 : 2, ssz = sz ? 8 : 4, nd = 8 / dsz;
                for (int k = 0; k < nd; k++) vset(r, k, dsz, fp_to_bits(bits_to_fp(vget(e, rn, k, ssz), ssz), dsz));
                if (Q) memcpy(e->q[rd].b + 8, r, 8); else { memcpy(e->q[rd].b, r, 8); memset(e->q[rd].b + 8, 0, 8); }
                return 1;
            }
            if (op == 0x17 && !a_bit) { /* FCVTL */
                int ssz = sz ? 4 : 2, dsz = sz ? 8 : 4, nd = 16 / dsz;
                for (int k = 0; k < nd; k++) vset(r, k, dsz, fp_to_bits(bits_to_fp(vget(e, rn, k + (Q ? nd : 0), ssz), ssz), dsz));
                memcpy(e->q[rd].b, r, 16); return 1;
            }
            for (int k = 0; k < fn; k++) {
                uint64_t raw = vget(e, rn, k, fesz);
                double x = bits_to_fp(raw, fesz);
                uint64_t out;
                switch (key) {
                case 0x18: case 0x19: case 0x38: case 0x39: case 0x58: case 0x59: case 0x79: { /* FRINT N,M,P,Z,A,X,I */
                    int mode = key == 0x18 ? 0 : key == 0x19 ? 2 : key == 0x38 ? 1 : key == 0x39 ? 3 : key == 0x58 ? 4 : 5;
                    out = fp_to_bits(fp_round_int(x, mode), fesz); break; }
                case 0x1A: case 0x1B: case 0x3A: case 0x3B: case 0x1C: case 0x5A: case 0x5B: case 0x7A: case 0x7B: case 0x5C: {
                    int mode = (key & 0x1F) == 0x1C ? 4 : (key == 0x1A || key == 0x5A) ? 0 : (key == 0x1B || key == 0x5B) ? 2 : (key == 0x3A || key == 0x7A) ? 1 : 3;
                    out = fp_to_int(x, mode, U, fesz * 8); break; }
                case 0x1D: case 0x5D: out = fp_to_bits(int_to_fp(raw, !U, fesz * 8, fesz), fesz); break; /* SCVTF/UCVTF */
                case 0x2C: out = x > 0 ? ones(fesz * 8) : 0; break;     /* FCMGT zero */
                case 0x2D: out = x == 0 ? ones(fesz * 8) : 0; break;    /* FCMEQ zero */
                case 0x2E: out = x < 0 ? ones(fesz * 8) : 0; break;     /* FCMLT zero */
                case 0x6C: out = x >= 0 ? ones(fesz * 8) : 0; break;    /* FCMGE zero */
                case 0x6D: out = x <= 0 ? ones(fesz * 8) : 0; break;    /* FCMLE zero */
                case 0x2F: out = raw & ~(1ull << (fesz * 8 - 1)); break;/* FABS */
                case 0x6F: out = raw ^ (1ull << (fesz * 8 - 1)); break; /* FNEG */
                case 0x7F: out = fp_to_bits(fp_round_to(sqrt(x), fesz), fesz); break; /* FSQRT */
                case 0x3D: out = fp_to_bits(fp_round_to(1.0 / x, fesz), fesz); break;  /* FRECPE (exact recip) */
                case 0x7D: out = fp_to_bits(fp_round_to(1.0 / sqrt(x), fesz), fesz); break; /* FRSQRTE */
                default: return 0;
                }
                vset(r, k, fesz, out);
            }
            if (scalar) { memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, r, (size_t)fesz); } else vcommit(e, rd, r, Q);
            return 1;
        }
        return 0;
    }

    /* ---- across lanes ---- */
    if ((i & 0x9F3E0C00) == 0x0E300800) {
        int op = (i >> 12) & 0x1F, esz = 1 << size, bits = esz * 8, n = (Q ? 16 : 8) / esz;
        if (op == 0x0C || op == 0x0F) { /* FMAXNMV/FMINNMV, FMAXV/FMINV (single only) */
            int mn = size >> 1;
            double acc = bits_to_fp(vget(e, rn, 0, 4), 4);
            for (int k = 1; k < (Q ? 4 : 2); k++) {
                double x = bits_to_fp(vget(e, rn, k, 4), 4);
                acc = op == 0x0C ? (mn ? fp_minnm(acc, x) : fp_maxnm(acc, x)) : (mn ? fp_min(acc, x) : fp_max(acc, x));
            }
            fset(e, rd, 0, acc);
            return 1;
        }
        uint64_t acc = vget(e, rn, 0, esz);
        int64_t sacc = sext(acc, bits);
        int64_t wide = U ? (int64_t)acc : sacc;
        for (int k = 1; k < n; k++) {
            uint64_t a = vget(e, rn, k, esz);
            int64_t sa = sext(a, bits);
            switch (op) {
            case 0x03: wide += U ? (int64_t)a : sa; break;                           /* SADDLV/UADDLV */
            case 0x0A: if (U) { if (a > acc) acc = a; } else if (sa > sacc) { sacc = sa; acc = a; } break; /* MAXV */
            case 0x1A: if (U) { if (a < acc) acc = a; } else if (sa < sacc) { sacc = sa; acc = a; } break; /* MINV */
            case 0x1B: acc += a; break;                                              /* ADDV */
            default: return 0;
            }
        }
        memset(e->q[rd].b, 0, 16);
        if (op == 0x03) { uint64_t w = (uint64_t)wide & ones(bits * 2); memcpy(e->q[rd].b, &w, (size_t)(esz * 2)); }
        else { acc &= ones(bits); memcpy(e->q[rd].b, &acc, (size_t)esz); }
        return 1;
    }

    /* ---- shift by immediate (vector and scalar) ---- */
    if (((i & 0x9F800400) == 0x0F000400 || (i & 0xDF800400) == 0x5F000400) && ((i >> 19) & 0xF)) {
        int immh = (i >> 19) & 0xF, immb = (i >> 16) & 7, op = (i >> 11) & 0x1F;
        int hs = 31 - __builtin_clz((unsigned)immh); /* 0..3 */
        int esz = 1 << hs, bits = esz * 8;
        int immhb = (immh << 3) | immb;
        int rsh = 2 * bits - immhb, lsh = immhb - bits;
        int n = scalar ? 1 : (Q ? 16 : 8) / esz;
        if (op == 0x14) { /* SSHLL/USHLL (SXTL/UXTL) */
            int nd = 8 / esz;
            for (int k = 0; k < nd; k++) {
                uint64_t a = vget(e, rn, k + (Q ? nd : 0), esz);
                uint64_t v = U ? a << lsh : (uint64_t)(sext(a, bits) << lsh);
                vset(r, k, esz * 2, v & ones(bits * 2));
            }
            memcpy(e->q[rd].b, r, 16); return 1;
        }
        if (op == 0x10 || op == 0x11 || op == 0x12 || op == 0x13) { /* SHRN/RSHRN/SQSHRN..., narrowing: hs is dest size */
            int dsz = esz, ssz = esz * 2, sbits = ssz * 8;
            int sh = 2 * bits - immhb; /* shift = (2*esize) - immh:immb with esize = dest */
            sh = bits * 2 - immhb; (void)sbits;
            int nd = 8 / dsz;
            for (int k = 0; k < nd; k++) {
                uint64_t a = vget(e, rn, k, ssz), v;
                if (op == 0x10 && !U) v = (a >> sh) & ones(bits);
                else if (op == 0x11 && !U) v = ((a + (1ull << (sh - 1))) >> sh) & ones(bits);
                else if (op == 0x12) v = U ? (( (a >> sh) > ones(bits)) ? ones(bits) : (a >> sh)) : sat_s(sext(a, ssz * 8) >> sh, bits);
                else if (op == 0x10 && U) v = sat_u(sext(a, ssz * 8) >> sh, bits); /* SQSHRUN */
                else return 0;
                vset(r, k, dsz, v);
            }
            if (Q) memcpy(e->q[rd].b + 8, r, 8); else { memcpy(e->q[rd].b, r, 8); memset(e->q[rd].b + 8, 0, 8); }
            return 1;
        }
        if (op == 0x1C || op == 0x1F) { /* SCVTF/UCVTF fixed, FCVTZS/FCVTZU fixed */
            if (esz < 4) return 0;
            double mul = ldexp(1.0, rsh);
            for (int k = 0; k < n; k++) {
                uint64_t a = vget(e, rn, k, esz), v;
                if (op == 0x1C) v = fp_to_bits(fp_round_to(int_to_fp(a, !U, bits, 8) / mul, esz), esz);
                else v = fp_to_int(bits_to_fp(a, esz) * mul, 3, U, bits);
                vset(r, k, esz, v);
            }
            if (scalar) { memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, r, (size_t)esz); } else vcommit(e, rd, r, Q);
            return 1;
        }
        for (int k = 0; k < n; k++) {
            uint64_t a = vget(e, rn, k, esz), d = vget(e, rd, k, esz), v;
            int64_t sa = sext(a, bits);
            switch (op) {
            case 0x00: v = U ? (rsh >= bits ? 0 : a >> rsh) : (uint64_t)(rsh >= bits ? (sa < 0 ? -1 : 0) : sa >> rsh); break; /* SSHR/USHR */
            case 0x02: v = d + (U ? (rsh >= bits ? 0 : a >> rsh) : (uint64_t)(rsh >= bits ? (sa < 0 ? -1 : 0) : sa >> rsh)); break; /* SSRA/USRA */
            case 0x04: { uint64_t rnd = 1ull << (rsh - 1); v = U ? (rsh >= bits ? 0 : (a >> rsh) + ((a >> (rsh - 1)) & 1))
                                                                 : (uint64_t)((sa >> (rsh >= bits ? bits - 1 : rsh)) + ((a >> (rsh - 1)) & 1)); (void)rnd; break; } /* SRSHR/URSHR */
            case 0x08: if (!U) return 0; { uint64_t m = rsh >= bits ? 0 : ones(bits) >> rsh; v = (d & ~m) | (rsh >= bits ? 0 : a >> rsh); } break; /* SRI */
            case 0x0A: v = U ? ((d & ones(lsh)) | (a << lsh)) : a << lsh; break; /* SHL / SLI */
            case 0x0C: if (!U) return 0; v = sat_u(sa << lsh, bits); break;     /* SQSHLU */
            case 0x0E: v = U ? ((a << lsh) >> lsh != a ? ones(bits) : a << lsh) : sat_s(sa * (int64_t)(1ull << lsh), bits); break; /* SQSHL/UQSHL */
            default: return 0;
            }
            vset(r, k, esz, v & ones(bits));
        }
        if (scalar) { memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, r, (size_t)esz); } else vcommit(e, rd, r, Q);
        return 1;
    }

    /* ---- three different (widening / narrowing) ---- */
    if ((i & 0x9F200C00) == 0x0E200000) {
        int op = (i >> 12) & 0xF, esz = 1 << size, bits = esz * 8, nd = 8 / esz;
        int half = Q ? nd : 0;
        if (op == 0x4 || op == 0x6) { /* ADDHN/SUBHN (+R) */
            for (int k = 0; k < nd; k++) {
                uint64_t a = vget(e, rn, k, esz * 2), b = vget(e, rm, k, esz * 2);
                uint64_t s = op == 4 ? a + b : a - b;
                if (U) s += 1ull << (bits - 1);
                vset(r, k, esz, (s >> bits) & ones(bits));
            }
            if (Q) memcpy(e->q[rd].b + 8, r, 8); else { memcpy(e->q[rd].b, r, 8); memset(e->q[rd].b + 8, 0, 8); }
            return 1;
        }
        for (int k = 0; k < nd; k++) {
            uint64_t a, b, d = vget(e, rd, k, esz * 2), v;
            int wide = (op == 1 || op == 3);
            a = wide ? vget(e, rn, k, esz * 2) : vget(e, rn, k + half, esz);
            b = vget(e, rm, k + half, esz);
            int64_t sa = wide ? (int64_t)a : sext(a, bits), sb = sext(b, bits);
            uint64_t ua = a, ub = b;
            switch (op) {
            case 0x0: case 0x1: v = U ? ua + ub : (uint64_t)(sa + sb); break;            /* ADDL/ADDW */
            case 0x2: case 0x3: v = U ? ua - ub : (uint64_t)(sa - sb); break;            /* SUBL/SUBW */
            case 0x5: v = d + (U ? (ua > ub ? ua - ub : ub - ua) : (uint64_t)(sa > sb ? sa - sb : sb - sa)); break; /* ABAL */
            case 0x7: v = U ? (ua > ub ? ua - ub : ub - ua) : (uint64_t)(sa > sb ? sa - sb : sb - sa); break;     /* ABDL */
            case 0x8: v = d + (U ? ua * ub : (uint64_t)(sa * sb)); break;               /* MLAL */
            case 0xA: v = d - (U ? ua * ub : (uint64_t)(sa * sb)); break;               /* MLSL */
            case 0xC: v = U ? ua * ub : (uint64_t)(sa * sb); break;                     /* MULL */
            case 0xE: if (U || size) return 0; { uint64_t p = 0; for (int t = 0; t < 8; t++) if (ub >> t & 1) p ^= ua << t; v = p; } break; /* PMULL.8 */
            default: return 0;
            }
            vset(r, k, esz * 2, v & ones(bits * 2));
        }
        memcpy(e->q[rd].b, r, 16);
        return 1;
    }

    /* ---- permutes: UZP/TRN/ZIP ---- */
    if ((i & 0xBF208C00) == 0x0E000800) {
        int op = (i >> 12) & 7, esz = 1 << size, n = (Q ? 16 : 8) / esz;
        int part = (op >> 2) & 1;
        for (int k = 0; k < n; k++) {
            uint64_t v;
            switch (op & 3) {
            case 1: { int idx = 2 * k + part; v = idx < n ? vget(e, rn, idx, esz) : vget(e, rm, idx - n, esz); break; } /* UZP */
            case 2: { int p = k / 2; v = (k & 1) ? vget(e, rm, 2 * p + part, esz) : vget(e, rn, 2 * p + part, esz); break; } /* TRN */
            case 3: { int base = part * n / 2; v = (k & 1) ? vget(e, rm, base + k / 2, esz) : vget(e, rn, base + k / 2, esz); break; } /* ZIP */
            default: return 0;
            }
            vset(r, k, esz, v);
        }
        vcommit(e, rd, r, Q);
        return 1;
    }

    /* ---- EXT ---- */
    if ((i & 0xBFE08400) == 0x2E000000) {
        int pos = (i >> 11) & 15, len = Q ? 16 : 8;
        uint8_t cat[32];
        memcpy(cat, e->q[rn].b, (size_t)len); memcpy(cat + len, e->q[rm].b, (size_t)len);
        memcpy(r, cat + pos, (size_t)len);
        vcommit(e, rd, r, Q);
        return 1;
    }

    /* ---- TBL / TBX ---- */
    if ((i & 0xBFE08C00) == 0x0E000000) {
        int len = ((i >> 13) & 3) + 1, tbx = (i >> 12) & 1;
        uint8_t table[64];
        for (int t = 0; t < len; t++) memcpy(table + t * 16, e->q[(rn + t) & 31].b, 16);
        for (int k = 0; k < (Q ? 16 : 8); k++) {
            uint8_t idx = e->q[rm].b[k];
            r[k] = idx < len * 16 ? table[idx] : (tbx ? e->q[rd].b[k] : 0);
        }
        vcommit(e, rd, r, Q);
        return 1;
    }

    /* ---- DUP (element), vector and scalar ---- */
    if ((i & 0xBFE0FC00) == 0x0E000400 || (i & 0xFFE0FC00) == 0x5E000400) {
        int imm5 = (i >> 16) & 31;
        int sz = __builtin_ctz((unsigned)imm5 | 0x20);
        if (sz > 3) return 0;
        int esz = 1 << sz, idx = imm5 >> (sz + 1);
        uint64_t v = vget(e, rn, idx, esz);
        if (scalar) { memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, &v, (size_t)esz); return 1; }
        for (int k = 0; k < (Q ? 16 : 8) / esz; k++) vset(r, k, esz, v);
        vcommit(e, rd, r, Q);
        return 1;
    }

    /* ---- vector x indexed element (vector and scalar) ---- */
    if ((i & 0x9F000400) == 0x0F000000 || (i & 0xDF000400) == 0x5F000000) {
        int op = (i >> 12) & 0xF, L = (i >> 21) & 1, M = (i >> 20) & 1, H = (i >> 11) & 1;
        int fp = (op == 0x1 || op == 0x5 || op == 0x9) && !(U && op != 0x9);
        if (fp) {
            int sz = size & 1, esz = sz ? 8 : 4;
            int idx = sz ? H : (H << 1) | L;
            int mreg = (int)((i >> 16) & 15) | (M << 4);
            double y = bits_to_fp(vget(e, mreg, idx, esz), esz);
            int n = scalar ? 1 : (Q ? 16 : 8) / esz;
            for (int k = 0; k < n; k++) {
                double x = bits_to_fp(vget(e, rn, k, esz), esz), d = bits_to_fp(vget(e, rd, k, esz), esz), v;
                if (op == 0x1) v = fp_fma(x, y, d, esz);
                else if (op == 0x5) v = fp_fma(-x, y, d, esz);
                else v = fp_round_to(x * y, esz);
                vset(r, k, esz, fp_to_bits(v, esz));
            }
            if (scalar) { memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, r, (size_t)esz); } else vcommit(e, rd, r, Q);
            return 1;
        }
        int esz = 1 << size, bits = esz * 8;
        int idx = size == 1 ? (H << 2) | (L << 1) | M : (H << 1) | L;
        int mreg = size == 1 ? (int)((i >> 16) & 15) : (int)((i >> 16) & 31);
        if (size != 1 && size != 2) return 0;
        uint64_t y = vget(e, mreg, idx, esz);
        if (op == 0x8 || op == 0x0 || op == 0x4) { /* MUL / MLA / MLS (same width) */
            if (scalar) return 0;
            int n = (Q ? 16 : 8) / esz;
            for (int k = 0; k < n; k++) {
                uint64_t a = vget(e, rn, k, esz), d = vget(e, rd, k, esz), v;
                if (op == 0x8 && !U) v = a * y;
                else if (op == 0x0 && U) v = d + a * y;
                else if (op == 0x4 && U) v = d - a * y;
                else return 0;
                vset(r, k, esz, v & ones(bits));
            }
            vcommit(e, rd, r, Q);
            return 1;
        }
        if (op == 0xA || op == 0x2 || op == 0x6) { /* SMULL/UMULL, SMLAL/UMLAL, SMLSL/UMLSL by element */
            int nd = 8 / esz;
            for (int k = 0; k < nd; k++) {
                uint64_t a = vget(e, rn, k + (Q ? nd : 0), esz), d = vget(e, rd, k, esz * 2);
                uint64_t p = U ? a * (y & ones(bits)) : (uint64_t)(sext(a, bits) * sext(y, bits));
                uint64_t v = op == 0xA ? p : op == 0x2 ? d + p : d - p;
                vset(r, k, esz * 2, v & ones(bits * 2));
            }
            memcpy(e->q[rd].b, r, 16);
            return 1;
        }
        return 0;
    }

    /* ---- scalar pairwise: ADDP Dd, Vn.2D ; FADDP/FMAXP... scalar ---- */
    if ((i & 0xDF3E0C00) == 0x5E300800) {
        int op = (i >> 12) & 0x1F;
        if (!U && op == 0x1B && size == 3) { uint64_t v = vget(e, rn, 0, 8) + vget(e, rn, 1, 8); memset(e->q[rd].b, 0, 16); memcpy(e->q[rd].b, &v, 8); return 1; }
        if (U) {
            int sz = size & 1, esz = sz ? 8 : 4, mn = size >> 1;
            double a = bits_to_fp(vget(e, rn, 0, esz), esz), b = bits_to_fp(vget(e, rn, 1, esz), esz), v;
            if (op == 0x0D && !mn) v = fp_round_to(a + b, esz);
            else if (op == 0x0C) v = mn ? fp_minnm(a, b) : fp_maxnm(a, b);
            else if (op == 0x0F) v = mn ? fp_min(a, b) : fp_max(a, b);
            else return 0;
            fset(e, rd, sz ? 1 : 0, v);
            return 1;
        }
        return 0;
    }

    /* ---- FMOV (vector, immediate) cmode 1111 ---- */
    if ((i & 0x9FF8FC00) == 0x0F00F400) {
        int op = (i >> 29) & 1;
        uint32_t imm8 = ((i >> 11) & 0xE0) | ((i >> 5) & 0x1F);
        int esz = op ? 8 : 4;
        if (op && !Q) return 0;
        uint64_t b = fp_to_bits(vfp_imm8(imm8, esz), esz);
        for (int k = 0; k < (Q ? 16 : 8) / esz; k++) vset(r, k, esz, b);
        vcommit(e, rd, r, Q);
        return 1;
    }
    return 0;
}

/* ---- AdvSIMD load/store multiple & single structures ---- */
static int exec_simd_ldst(mgr_emu *e, uint32_t i) {
    int Q = (i >> 30) & 1, L = (i >> 22) & 1, post = (i >> 23) & 1;
    int rn = (int)((i >> 5) & 31), rt = (int)(i & 31), rm = (int)((i >> 16) & 31);
    uint64_t addr = XSP(e, rn), total = 0;
    if ((i & 0xBFBF0000) == 0x0C000000 || (i & 0xBFA00000) == 0x0C800000) { /* multiple structures */
        if (!post && rm != 0) return 0;
        int opcode = (i >> 12) & 15, size = (i >> 10) & 3, esz = 1 << size;
        int nregs, selem;
        switch (opcode) {
        case 0x0: nregs = 1; selem = 4; break;  /* LD4/ST4 */
        case 0x2: nregs = 4; selem = 1; break;  /* LD1 x4 */
        case 0x4: nregs = 1; selem = 3; break;  /* LD3 */
        case 0x6: nregs = 3; selem = 1; break;
        case 0x7: nregs = 1; selem = 1; break;
        case 0x8: nregs = 1; selem = 2; break;  /* LD2 */
        case 0xA: nregs = 2; selem = 1; break;
        default: return 0;
        }
        int elems = (Q ? 16 : 8) / esz;
        uint64_t a = addr;
        for (int rr = 0; rr < nregs; rr++) {
            for (int k = 0; k < elems; k++) {
                for (int s = 0; s < selem; s++) {
                    int t = (rt + rr + s) & 31;
                    if (L) { uint64_t v = mrd(e, a, esz); memcpy(e->q[t].b + k * esz, &v, (size_t)esz); }
                    else mwr(e, a, esz, vget(e, t, k, esz));
                    a += (uint64_t)esz;
                }
            }
        }
        if (L && !Q) for (int s = 0; s < nregs * selem; s++) memset(e->q[(rt + s) & 31].b + 8, 0, 8);
        total = a - addr;
    } else if ((i & 0xBF9F0000) == 0x0D000000 || (i & 0xBF800000) == 0x0D800000) { /* single structure / replicate */
        if (!post && rm != 0) return 0;
        int R = (i >> 21) & 1, opcode = (i >> 13) & 7, S = (i >> 12) & 1, size = (i >> 10) & 3;
        int selem = (((opcode & 1) << 1) | R) + 1;
        int scale = opcode >> 1, idx;
        if (scale == 3) { /* LDnR replicate */
            if (!L) return 0;
            int esz = 1 << size;
            uint64_t a = addr;
            for (int s = 0; s < selem; s++) {
                uint64_t v = mrd(e, a, esz);
                uint8_t buf[16] = {0};
                for (int k = 0; k < (Q ? 16 : 8) / esz; k++) vset(buf, k, esz, v);
                memcpy(e->q[(rt + s) & 31].b, buf, 16);
                a += (uint64_t)esz;
            }
            total = a - addr;
        } else {
            int esz = 1 << scale;
            if (scale == 0) idx = (Q << 3) | (S << 2) | size;
            else if (scale == 1) idx = (Q << 2) | (S << 1) | (size >> 1);
            else if (size & 1) { esz = 8; idx = Q; }
            else idx = (Q << 1) | S;
            uint64_t a = addr;
            for (int s = 0; s < selem; s++) {
                int t = (rt + s) & 31;
                if (L) { uint64_t v = mrd(e, a, esz); memcpy(e->q[t].b + idx * esz, &v, (size_t)esz); }
                else mwr(e, a, esz, vget(e, t, idx, esz));
                a += (uint64_t)esz;
            }
            total = a - addr;
        }
    } else return 0;
    if (post) SETXSP(e, rn, addr + (rm == 31 ? total : X(e, rm)));
    return 1;
}
