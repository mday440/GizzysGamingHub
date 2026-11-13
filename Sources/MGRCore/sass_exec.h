/* A reference interpreter for Ampere shader code: runs one thread at a time (no warp
 * operations), so it is slow but simple; it is the ground truth for the Metal translator.
 * Operand rules (from the encoding in sass.h): three-operand ops read A, B, C; one- and
 * two-operand ops read their last operand from B in forms 1/4/5/6 and from C in 2/3/7. */
#ifndef MGR_SASS_EXEC_H
#define MGR_SASS_EXEC_H
#include "sass.h"
#include <math.h>
#include <string.h>

typedef struct sass_env sass_env;
struct sass_env {          /* what a shader can reach */
    void *user;
    uint32_t (*cbuf)(sass_env *, uint32_t idx, uint32_t off);
    uint32_t (*attr_in)(sass_env *, uint32_t addr);              /* VS: ALD input; PS: IPA (already / w) */
    void (*attr_out)(sass_env *, uint32_t addr, uint32_t v);      /* VS: AST */
    uint32_t (*sysreg)(sass_env *, uint32_t idx);
    /* texture: handle from cbuf or register; coords in c[]; writes up to 4 channels to out[] */
    void (*tex)(sass_env *, uint32_t op12, uint32_t handle, const uint32_t *c, int nc, uint32_t dim, uint32_t lod_mode, uint32_t out[4]);
    uint32_t (*mem_ld)(sass_env *, uint64_t addr);
    uint64_t unknown_ops;   /* first unimplemented opcode seen (for logs) */
    /* compute: global stores, image load/store (bindless handle), workgroup shared memory, barriers */
    void (*mem_st)(sass_env *, uint64_t addr, uint32_t v);
    void (*surf_st)(sass_env *, uint32_t handle, const uint32_t *coord, uint32_t dim, const uint32_t *data, int formatted, uint32_t mask_or_type);
    void (*surf_ld)(sass_env *, uint32_t handle, const uint32_t *coord, uint32_t dim, uint32_t *out, int formatted, uint32_t mask_or_type);
    uint32_t *shared; uint32_t shared_words; int bar_yield, warp_sync;
    int64_t code_delta;
    void (*trace)(sass_env *, void *t, uint32_t pc, const sass_ins *in);   /* called after each executed instruction */
    void *trace_t;   /* CALL.ABS / RET addresses: offset of this view of the program from the compiled one */
    uint32_t op_hist_unknown[64];   /* unimplemented opcodes seen: op12 | count<<16 (compute statistics) */
};

typedef struct { uint32_t r[256], ur[64]; uint8_t p[8], up[8]; int killed, exited, at_bar; uint32_t resume_pc;
    /* warp operations in compute: the thread stops at the instruction (at_sync) with its operands, the
       scheduler combines the warp's values and resumes it with the result (sync_done) */
    int at_sync, sync_done, sync_kind, sync_pres; uint32_t sync_pc, sync_val, sync_lane, sync_c, sync_mode, sync_res;
    uint32_t sync_in[10], sync_out[4];   /* HMMA / MOVM fragments */
    uint32_t local[1024]; } sass_thread;

static inline float sx_f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t sx_u(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static inline uint32_t sx_reg(sass_thread *t, uint32_t r) { return r == 255 ? 0 : t->r[r]; }
static inline uint32_t sx_ureg(sass_thread *t, uint32_t r) { return (r & 63) == 63 ? 0 : t->ur[r & 63]; }
static inline void sx_setr(sass_thread *t, uint32_t r, uint32_t v) { if (r != 255) t->r[r] = v; }
static inline void sx_setur(sass_thread *t, uint32_t r, uint32_t v) { if ((r & 63) != 63) t->ur[r & 63] = v; }
static inline int sx_pred(sass_thread *t, uint32_t p, int neg) { int v = p == 7 ? 1 : t->p[p]; return neg ? !v : v; }
static inline int sx_upred(sass_thread *t, uint32_t p, int neg) { int v = p == 7 ? 1 : t->up[p]; return neg ? !v : v; }
static inline void sx_setp(sass_thread *t, uint32_t p, int v) { if (p != 7) t->p[p] = (uint8_t)!!v; }
static inline void sx_setup(sass_thread *t, uint32_t p, int v) { if (p != 7) t->up[p] = (uint8_t)!!v; }

static uint32_t sx_src(sass_thread *t, sass_env *env, const sass_src *s, int is_float) {
    uint32_t v;
    switch (s->kind) {
    case SRC_REG: v = sx_reg(t, s->reg); break;
    case SRC_UREG: v = sx_ureg(t, s->reg); break;
    case SRC_IMM: v = s->imm; break;
    case SRC_CBUF: v = env->cbuf(env, s->ucb ? sx_ureg(t, s->reg) : s->cb_idx, s->cb_off); break;
    default: v = 0;
    }
    if (is_float) {
        if (s->abs) v &= 0x7FFFFFFFu;
        if (s->neg) v ^= 0x80000000u;
    } else if (s->neg) v = 0u - v;
    return v;
}

static float sx_half_to_f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 0x3FF;
    if (e == 0) { if (!m) return sx_f(s); float f = ldexpf((float)m, -24); return s ? -f : f; }
    if (e == 31) return sx_f(s | 0x7F800000u | (m << 13));
    return sx_f(s | ((e + 112) << 23) | (m << 13));
}
static uint16_t sx_f_to_half(float f) {
    uint32_t x = sx_u(f), s = (x >> 16) & 0x8000; int e = (int)((x >> 23) & 255) - 112; uint32_t m = x & 0x7FFFFF;
    if (((x >> 23) & 255) == 255) return (uint16_t)(s | 0x7C00 | (m ? 0x200 : 0));
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    if (e <= 0) { if (e < -10) return (uint16_t)s; m |= 0x800000; uint32_t sh = (uint32_t)(14 - e); return (uint16_t)(s | ((m + (1u << (sh - 1))) >> sh)); }
    uint32_t h = s | ((uint32_t)e << 10) | (m >> 13);
    if ((m & 0x1FFF) > 0x1000 || ((m & 0x1FFF) == 0x1000 && (h & 1))) h++;
    return (uint16_t)h;
}
/* .FTZ: denormal inputs and results become (signed) zero */
static float sx_ftz(float x) { return fpclassify(x) == FP_SUBNORMAL ? copysignf(0.0f, x) : x; }
/* min / max as the GPU does them: a NaN operand is ignored (both NaN: NaN) */
static float sx_fmin(float a, float b) { return isnan(a) ? b : isnan(b) ? a : (a < b ? a : (b < a ? b : (signbit(a) ? a : b))); }
static float sx_fmax(float a, float b) { return isnan(a) ? b : isnan(b) ? a : (a > b ? a : (b > a ? b : (signbit(a) ? b : a))); }
static float sx_round(float f, uint32_t mode) { return mode == 1 ? floorf(f) : mode == 2 ? ceilf(f) : mode == 3 ? truncf(f) : nearbyintf(f); }
static int sx_fcmp(float a, float b, uint32_t op) {
    int un = isnan(a) || isnan(b);
    switch (op) {
    case 0: return 0; case 15: return 1;
    case 1: return !un && a < b; case 2: return !un && a == b; case 3: return !un && a <= b;
    case 4: return !un && a > b; case 5: return !un && a != b; case 6: return !un && a >= b;
    case 7: return !un; case 8: return un;
    case 9: return un || a < b; case 10: return un || a == b; case 11: return un || a <= b;
    case 12: return un || a > b; case 13: return un || a != b; case 14: return un || a >= b;
    }
    return 0;
}
static int sx_icmp(uint32_t a, uint32_t b, uint32_t op, int sgn) {
    int lt = sgn ? (int32_t)a < (int32_t)b : a < b, eq = a == b;
    switch (op) { case 0: return 0; case 1: return lt; case 2: return eq; case 3: return lt || eq; case 4: return !lt && !eq; case 5: return !eq; case 6: return !lt; default: return 1; }
}
static void sx_unknown(sass_env *env, uint32_t op12) {
    if (!env->unknown_ops) env->unknown_ops = op12 | 0x10000;
    for (int i = 0; i < 64; i++) {
        if ((env->op_hist_unknown[i] & 0xFFFF) == op12 && env->op_hist_unknown[i]) { if ((env->op_hist_unknown[i] >> 16) < 0xFFFF) env->op_hist_unknown[i] += 0x10000; return; }
        if (!env->op_hist_unknown[i]) { env->op_hist_unknown[i] = op12 | 0x10000; return; }
    }
}
static uint32_t sx_atom(uint32_t op, int sgn, uint32_t old, uint32_t v) {
    switch (op) { case 0: return old + v; case 1: return sgn ? (uint32_t)((int32_t)old < (int32_t)v ? (int32_t)old : (int32_t)v) : (old < v ? old : v);
    case 2: return sgn ? (uint32_t)((int32_t)old > (int32_t)v ? (int32_t)old : (int32_t)v) : (old > v ? old : v);
    case 3: return old >= v ? 0 : old + 1; case 4: return (old == 0 || old > v) ? v : old - 1;
    case 5: return old & v; case 6: return old | v; case 7: return old ^ v; case 8: return v; default: return old; }
}
/* packed half2 operands: swizzle 0 = H1_H0, 1 = F32, 2 = H0_H0, 3 = H1_H1 */
static void sx_h2(uint32_t v, uint32_t swz, int neg, int abs_, float o[2]) {
    if (swz == 1) { o[0] = o[1] = sx_f(v); }
    else { float lo = sx_half_to_f((uint16_t)v), hi = sx_half_to_f((uint16_t)(v >> 16)); if (swz == 2) hi = lo; else if (swz == 3) lo = hi; o[0] = lo; o[1] = hi; }
    for (int k = 0; k < 2; k++) { if (abs_) o[k] = fabsf(o[k]); if (neg) o[k] = -o[k]; }
}
static uint32_t sx_raw(sass_thread *t, sass_env *env, const sass_src *s) { sass_src c = *s; c.neg = c.abs = 0; return sx_src(t, env, &c, 0); }
static int sx_combine(int v, int acc, uint32_t op) { return op == 0 ? v && acc : op == 1 ? v || acc : v ^ acc; }
static uint32_t sx_lop3(uint32_t a, uint32_t b, uint32_t c, uint32_t lut) {
    uint32_t r = 0;
    for (int i = 0; i < 8; i++) if ((lut >> i) & 1) r |= ((i & 4) ? a : ~a) & ((i & 2) ? b : ~b) & ((i & 1) ? c : ~c);
    return r;
}

/* Run from byte offset pc (code points at the first instruction, after the header).
   Returns the number of instructions executed. */
/* dec: optional pre-decoded instructions (one per 16 bytes of code), so the hot loop skips decoding */
static uint64_t sass_run_dec(sass_thread *t, sass_env *env, const uint32_t *code, const sass_ins *dec, uint32_t code_bytes, uint32_t pc);
static uint64_t sass_run(sass_thread *t, sass_env *env, const uint32_t *code, uint32_t code_bytes, uint32_t pc) { return sass_run_dec(t, env, code, 0, code_bytes, pc); }
static uint64_t sass_run_dec(sass_thread *t, sass_env *env, const uint32_t *code, const sass_ins *dec, uint32_t code_bytes, uint32_t pc) {
    uint64_t steps = 0;
    while (pc + 16 <= code_bytes && steps < 1000000) {
        const uint32_t *w = code + pc / 4;
        sass_ins in; if (dec) in = dec[pc / 16]; else sass_decode(w, &in);
        steps++;
        uint32_t next = pc + 16;
        if (!sx_pred(t, in.pred, in.pred_neg)) { pc = next; continue; }
        if (env->trace && env->trace_t == t && steps > 1) env->trace(env, t, 0xFFFFFFFFu, 0);   /* flush previous */
        if (env->trace && env->trace_t == t) env->trace(env, t, pc, &in);
        if (in.is_alu) {
            const sass_src *two = (in.form == 2 || in.form == 3 || in.form == 7) ? &in.c : &in.b;   /* last operand of 1/2-operand ops */
            int isf = 0;
            switch (in.op9) { case 0x021: case 0x020: case 0x023: case 0x009: case 0x00b: case 0x00a: case 0x108: case 0x008: case 0x104: case 0x105: case 0x107: case 0x03e: isf = 1; }
            /* integer negation modifiers exist only on IADD3 / IMAD / LEA; elsewhere those bits are other fields (SHF type, ...) */
            int ineg = 0; switch (in.op9) { case 0x010: case 0x024: case 0x025: case 0x027: case 0x011: case 0x090: case 0x0a4: case 0x0a5: case 0x091: ineg = 1; }
            uint32_t A, B, C, S;
            if (isf || ineg) { A = sx_src(t, env, &in.a, isf); B = sx_src(t, env, &in.b, isf); C = sx_src(t, env, &in.c, isf); S = sx_src(t, env, two, isf); }
            else { A = sx_raw(t, env, &in.a); B = sx_raw(t, env, &in.b); C = sx_raw(t, env, &in.c); S = sx_raw(t, env, two); }
            uint32_t d = 0; int wr = 1, sat = (int)sass_bits(w, 77, 78);
            uint32_t pa = (uint32_t)sass_bits(w, 87, 90); int pan = (int)sass_bits(w, 90, 91);
            int uni = in.uniform;
            #define PRED(p, n) (uni ? sx_upred(t, p, n) : sx_pred(t, p, n))
            #define SETP(p, v) do { if (uni) sx_setup(t, p, v); else sx_setp(t, p, v); } while (0)
            switch (in.op9) {
            case 0x021: { int fz = (int)sass_bits(w, 80, 81); float r = fz ? sx_ftz(sx_ftz(sx_f(A)) + sx_ftz(sx_f(S))) : sx_f(A) + sx_f(S); if (sat) r = fminf(fmaxf(r, 0), 1); d = sx_u(r); break; }       /* FADD */
            case 0x020: { int fz = (int)sass_bits(w, 80, 81); float r = fz ? sx_ftz(sx_ftz(sx_f(A)) * sx_ftz(sx_f(S))) : sx_f(A) * sx_f(S); if (sat) r = fminf(fmaxf(r, 0), 1); d = sx_u(r); break; }       /* FMUL */
            case 0x023: { int fz = (int)sass_bits(w, 80, 81); float r = fz ? sx_ftz(fmaf(sx_ftz(sx_f(A)), sx_ftz(sx_f(B)), sx_ftz(sx_f(C)))) : fmaf(sx_f(A), sx_f(B), sx_f(C)); if (sat) r = fminf(fmaxf(r, 0), 1); d = sx_u(r); break; } /* FFMA */
            case 0x009: { float x = sx_f(A), y = sx_f(S); if (sass_bits(w, 80, 81)) { x = sx_ftz(x); y = sx_ftz(y); } d = sx_u(sx_pred(t, pa, pan) ? sx_fmin(x, y) : sx_fmax(x, y)); break; } /* FMNMX */
            case 0x008: d = sx_pred(t, pa, pan) ? A : S; break;                                                    /* FSEL */
            case 0x00b: {                                                                                          /* FSETP */
                int fz = (int)sass_bits(w, 80, 81); int c = sx_fcmp(fz ? sx_ftz(sx_f(A)) : sx_f(A), fz ? sx_ftz(sx_f(S)) : sx_f(S), (uint32_t)sass_bits(w, 76, 80)), acc = sx_pred(t, pa, pan); uint32_t so = (uint32_t)sass_bits(w, 74, 76);
                sx_setp(t, (uint32_t)sass_bits(w, 81, 84), sx_combine(c, acc, so)); sx_setp(t, (uint32_t)sass_bits(w, 84, 87), sx_combine(!c, acc, so)); wr = 0; break;
            }
            case 0x00a: { int c = sx_fcmp(sx_f(A), sx_f(S), (uint32_t)sass_bits(w, 76, 80)); d = sass_bits(w, 63, 64) ? (c ? 0xFFFFFFFFu : 0) : (c ? 0x3F800000u : 0); break; } /* FSET */
            case 0x108: {                                                                                          /* MUFU */
                float x = sx_ftz(sx_f(S)), r;                                  /* MUFU flushes denormal inputs */
                switch (sass_bits(w, 74, 80)) { case 0: r = cosf(x); break; case 1: r = sinf(x); break; case 2: r = exp2f(x); break; case 3: r = log2f(x); break;
                case 4: r = 1.0f / x; break; case 5: r = 1.0f / sqrtf(x); break; case 8: r = sqrtf(x); break; case 9: r = tanhf(x); break; default: r = 0; }
                d = sx_u(r); break;
            }
            case 0x010: case 0x090: {                                                                              /* IADD3 */
                uint64_t s64 = (uint64_t)A + B + C;
                if (sass_bits(w, 74, 75)) s64 += (uint64_t)PRED((uint32_t)sass_bits(w, 87, 90), (int)sass_bits(w, 90, 91)) + PRED((uint32_t)sass_bits(w, 77, 80), (int)sass_bits(w, 80, 81));
                d = (uint32_t)s64;
                SETP((uint32_t)sass_bits(w, 81, 84), (s64 >> 32) & 1); break;
            }
            case 0x024: case 0x0a4: d = A * B + C; break;                                                          /* IMAD */
            case 0x004: { uint32_t m = S & 0x7F, v = A; for (int i = 0; i < 7; i++) if ((m >> i) & 1) sx_setp(t, (uint32_t)i, (v >> i) & 1); wr = 0; break; }   /* R2P */
            case 0x003: { uint32_t m = S & 0x7F, pr = 0; for (int i = 0; i < 7; i++) pr |= (uint32_t)t->p[i] << i; d = (A & ~m) | (pr & m); break; }          /* P2R */
            case 0x027: { int sg = (int)sass_bits(w, 73, 74);                                                        /* IMAD.HI */
                uint64_t p = sg ? (uint64_t)((int64_t)(int32_t)A * (int64_t)(int32_t)B) : (uint64_t)A * B; d = (uint32_t)(p >> 32) + C; break; }
            case 0x025: case 0x0a5: {                                                                              /* IMAD.WIDE */
                int sg = (int)sass_bits(w, 73, 74);
                uint64_t c64 = (uint64_t)C | ((uint64_t)(in.c.kind == SRC_REG ? sx_reg(t, in.c.reg == 255 ? 255 : in.c.reg + 1) : in.c.kind == SRC_UREG ? sx_ureg(t, in.c.reg + 1) : 0) << 32);
                uint64_t p = sg ? (uint64_t)((int64_t)(int32_t)A * (int64_t)(int32_t)B) : (uint64_t)A * B;
                uint64_t r = p + c64;
                if (uni) { sx_setur(t, in.dst, (uint32_t)r); sx_setur(t, in.dst + 1, (uint32_t)(r >> 32)); } else { sx_setr(t, in.dst, (uint32_t)r); if (in.dst != 255) sx_setr(t, in.dst + 1, (uint32_t)(r >> 32)); }
                wr = 0; break;
            }
            case 0x017: { int sg = (int)sass_bits(w, 73, 74), mn = sx_pred(t, pa, pan); int lt = sg ? (int32_t)A < (int32_t)S : A < S; d = (mn ? lt : !lt) ? A : S; break; } /* IMNMX */
            case 0x00c: case 0x08c: {                                                                              /* ISETP */
                int c = sx_icmp(A, S, (uint32_t)sass_bits(w, 76, 79), (int)sass_bits(w, 73, 74)), acc = PRED(pa, pan); uint32_t so = (uint32_t)sass_bits(w, 74, 76);
                SETP((uint32_t)sass_bits(w, 81, 84), sx_combine(c, acc, so)); SETP((uint32_t)sass_bits(w, 84, 87), sx_combine(!c, acc, so)); wr = 0; break;
            }
            case 0x012: case 0x092: d = sx_lop3(A, B, C, (uint32_t)sass_bits(w, 72, 80)); if (sass_bits(w, 81, 84) != 7) SETP((uint32_t)sass_bits(w, 81, 84), d != 0); break; /* LOP3 */
            case 0x019: case 0x099: {                                                                              /* SHF */
                uint32_t ty = (uint32_t)sass_bits(w, 73, 75), right = (uint32_t)sass_bits(w, 76, 77), hi = (uint32_t)sass_bits(w, 80, 81), wrap = (uint32_t)sass_bits(w, 75, 76);
                uint64_t v = ((uint64_t)C << 32) | A; uint32_t s = B; uint32_t lim = ty < 2 ? 64 : 32;
                s = wrap ? s & (lim - 1) : (s > lim ? lim : s);
                uint64_t r;
                if (right) r = (ty == 0 || ty == 2) ? (uint64_t)((int64_t)v >> (s > 63 ? 63 : s)) : (s > 63 ? 0 : v >> s);
                else r = s > 63 ? 0 : v << s;
                d = hi ? (uint32_t)(r >> 32) : (uint32_t)r; break;
            }
            case 0x011: case 0x091: {                                                                              /* LEA */
                uint32_t sh = (uint32_t)sass_bits(w, 75, 80); int hi = (int)sass_bits(w, 80, 81);
                uint64_t v = hi ? (((uint64_t)C << 32) | A) : A; v <<= sh;
                uint32_t part = hi ? (uint32_t)(v >> 32) : (uint32_t)v;
                if (sass_bits(w, 72, 73)) part = 0u - part;
                d = part + B; break;
            }
            case 0x002: case 0x082: d = S; break;
            case 0x030: case 0x031: case 0x032: case 0x033: case 0x034: case 0x040: {                       /* HADD2 HFMA2 HMUL2 HSET2 HSETP2 HMNMX2 */
                int midb = !(in.form == 2 || in.form == 3 || in.form == 7);          /* operand in bits 32..64: B in forms 1/4/5/6, else C */
                const sass_src *bs = (in.op9 == 0x031) ? &in.b : two, *cs = &in.c;
                int bmid = (bs == &in.b) ? midb : !midb, cmid = !midb;
                float a2[2], b2[2], c2[2] = {0, 0};
                sx_h2(sx_raw(t, env, &in.a), (uint32_t)sass_bits(w, 74, 76), (int)sass_bits(w, 72, 73), (int)sass_bits(w, 73, 74), a2);
                #define H2(src, ismid, o) do { int imm = (src)->kind == SRC_IMM; \
                    if (ismid) sx_h2(sx_raw(t, env, src), imm ? 0 : (uint32_t)sass_bits(w, 60, 62), imm ? 0 : (int)sass_bits(w, 63, 64), imm ? 0 : (int)sass_bits(w, 62, 63), o); \
                    else sx_h2(sx_raw(t, env, src), (uint32_t)sass_bits(w, 81, 83), (int)sass_bits(w, 84, 85), (int)sass_bits(w, 83, 84), o); } while (0)
                H2(bs, bmid, b2);
                if (in.op9 == 0x031) H2(cs, cmid, c2);
                #undef H2
                float r[2];
                if (in.op9 == 0x034 || in.op9 == 0x033) {
                    uint32_t cmp = (uint32_t)sass_bits(w, 76, 80), so = (uint32_t)sass_bits(w, 69, 71); int acc = sx_pred(t, pa, pan);
                    int c0 = sx_fcmp(a2[0], b2[0], cmp), c1 = sx_fcmp(a2[1], b2[1], cmp);
                    if (in.op9 == 0x034) {
                        if (sass_bits(w, 71, 72)) { int c = c0 && c1; sx_setp(t, (uint32_t)sass_bits(w, 81, 84), sx_combine(c, acc, so)); sx_setp(t, (uint32_t)sass_bits(w, 84, 87), sx_combine(!c, acc, so)); }
                        else { sx_setp(t, (uint32_t)sass_bits(w, 81, 84), sx_combine(c0, acc, so)); sx_setp(t, (uint32_t)sass_bits(w, 84, 87), sx_combine(c1, acc, so)); }
                        wr = 0; break;
                    }
                    c0 = sx_combine(c0, acc, so); c1 = sx_combine(c1, acc, so);
                    int bf = (int)sass_bits(w, 71, 72);
                    d = (c0 ? (bf ? 0x3C00u : 0xFFFFu) : 0) | ((c1 ? (bf ? 0x3C00u : 0xFFFFu) : 0) << 16); break;
                }
                for (int k = 0; k < 2; k++) {
                    switch (in.op9) { case 0x030: r[k] = a2[k] + b2[k]; break; case 0x032: r[k] = a2[k] * b2[k]; break; case 0x031: r[k] = a2[k] * b2[k] + c2[k]; break;
                    default: r[k] = sx_pred(t, pa, pan) ? sx_fmin(a2[k], b2[k]) : sx_fmax(a2[k], b2[k]); break; }
                    if (sat) r[k] = fminf(fmaxf(r[k], 0), 1);
                }
                if (in.op9 != 0x040 && sass_bits(w, 78, 79)) d = sx_u(r[0]);
                else d = (uint32_t)sx_f_to_half(r[0]) | ((uint32_t)sx_f_to_half(r[1]) << 16);
                break;
            }                                                                              /* MOV */
            case 0x016: case 0x096: {                                                                              /* PRMT */
                uint64_t v = ((uint64_t)C << 32) | A; d = 0;
                for (int i = 0; i < 4; i++) { uint32_t sel = (B >> (i * 4)) & 15; uint8_t byte = (uint8_t)(v >> ((sel & 7) * 8)); if (sel & 8) byte = (byte & 0x80) ? 0xFF : 0; d |= (uint32_t)byte << (i * 8); }
                break;
            }
            case 0x007: case 0x087: d = PRED(pa, pan) ? A : S; break;                                              /* SEL */
            case 0x013: d = (int32_t)S < 0 ? 0u - S : S; break;                                                     /* IABS */
            case 0x109: case 0x0bf: d = (uint32_t)__builtin_popcount(S); break;                                    /* POPC */
            case 0x100: case 0x0bd: { uint32_t x = sass_bits(w, 63, 64) ? ~S : S; if (sass_bits(w, 73, 74) && (int32_t)x < 0) x = ~x; d = x ? (uint32_t)(31 - __builtin_clz(x)) : 0xFFFFFFFFu; if (sass_bits(w, 74, 75) && x) d = 31 - d; break; } /* FLO */
            case 0x101: case 0x0be: { uint32_t x = S, r = 0; for (int i = 0; i < 32; i++) r |= ((x >> i) & 1) << (31 - i); d = r; break; } /* BREV */
            case 0x01b: case 0x09b: { uint32_t pos = A & 31, wd = B & 63; uint64_t m = wd >= 32 ? 0xFFFFFFFFull : ((1ull << wd) - 1); d = (uint32_t)(m << pos); break; } /* BMSK */
            case 0x01a: case 0x09a: { uint32_t nb = S & 63; if (nb == 0 || nb >= 32) d = A; else { uint32_t m = (1u << nb) - 1; d = A & m; if (sass_bits(w, 73, 74) && (d >> (nb - 1)) & 1) d |= ~m; } break; } /* SGXT */
            case 0x104: {                                                                                          /* F2F */
                uint32_t ss = (uint32_t)sass_bits(w, 84, 86), ds = (uint32_t)sass_bits(w, 75, 77);
                float f = ss == 1 ? sx_half_to_f((uint16_t)S) : sx_f(S);
                uint32_t rm = (uint32_t)sass_bits(w, 78, 80); if (ss == ds && rm) f = sx_round(f, rm);
                d = ds == 1 ? sx_f_to_half(f) : sx_u(f); break;
            }
            case 0x105: {                                                                                          /* F2I */
                uint32_t ss = (uint32_t)sass_bits(w, 84, 86); float f = ss == 1 ? sx_half_to_f((uint16_t)S) : sx_f(S);
                f = sx_round(f, (uint32_t)sass_bits(w, 78, 80)); int sg = (int)sass_bits(w, 72, 73);
                if (isnan(f)) d = 0; else if (sg) d = f >= 2147483647.0f ? 0x7FFFFFFF : f <= -2147483648.0f ? 0x80000000u : (uint32_t)(int32_t)f;
                else d = f >= 4294967295.0f ? 0xFFFFFFFFu : f <= 0 ? 0 : (uint32_t)f;
                break;
            }
            case 0x106: { int sg = (int)sass_bits(w, 74, 75); uint32_t ss = (uint32_t)sass_bits(w, 84, 86); uint32_t x = S; float f;
                if (ss == 0) f = sg ? (float)(int8_t)x : (float)(uint8_t)x; else if (ss == 1) f = sg ? (float)(int16_t)x : (float)(uint16_t)x; else f = sg ? (float)(int32_t)x : (float)x;
                d = sass_bits(w, 75, 77) == 1 ? sx_f_to_half(f) : sx_u(f); break; }                                 /* I2F */
            case 0x045: d = sass_bits(w, 74, 75) ? sx_u((float)(int32_t)S) : sx_u((float)S); break;               /* I2FP.F32 (Ampere) */
            case 0x107: { uint32_t ss = (uint32_t)sass_bits(w, 84, 86); float f = ss == 1 ? sx_half_to_f((uint16_t)S) : sx_f(S); f = sx_round(f, (uint32_t)sass_bits(w, 78, 80)); d = ss == 1 ? sx_f_to_half(f) : sx_u(f); break; } /* FRND */
            case 0x03e: d = (uint32_t)sx_f_to_half(sx_f(B)) | ((uint32_t)sx_f_to_half(sx_f(A)) << 16); break;     /* F2FP.PACK_AB */
            default: wr = 0; sx_unknown(env, in.op12); break;
            }
            #undef PRED
            #undef SETP
            if (wr) { if (uni) sx_setur(t, in.dst, d); else sx_setr(t, in.dst, d); }
            pc = next; continue;
        }
        switch (in.op12) {
        case 0xc82: sx_setur(t, in.dst, in.form == 6 ? sx_ureg(t, (uint32_t)sass_bits(w, 32, 38)) : (uint32_t)sass_bits(w, 32, 64)); break; /* UMOV */
        case 0xab9: { uint32_t idx = (uint32_t)sass_bits(w, 54, 59), off = (uint32_t)sass_bits(w, 38, 54), ty = (uint32_t)sass_bits(w, 73, 76);   /* ULDC */
            sx_setur(t, in.dst, env->cbuf(env, idx, off)); if (ty == 5) sx_setur(t, in.dst + 1, env->cbuf(env, idx, off + 4)); break; }
        case 0xb82: { uint32_t idx = (uint32_t)sass_bits(w, 54, 59), off = (uint32_t)sass_bits(w, 38, 54) + sx_reg(t, (uint32_t)sass_bits(w, 24, 32)), ty = (uint32_t)sass_bits(w, 73, 76); /* LDC */
            sx_setr(t, in.dst, env->cbuf(env, idx, off)); if (ty == 5 && in.dst != 255) sx_setr(t, in.dst + 1, env->cbuf(env, idx, off + 4)); break; }
        case 0x919: sx_setr(t, in.dst, env->sysreg(env, (uint32_t)sass_bits(w, 72, 80))); break;                  /* S2R */
        case 0x9c3: sx_setur(t, in.dst, env->sysreg(env, (uint32_t)sass_bits(w, 72, 80))); break;                 /* S2UR */
        case 0x805: { uint32_t idx = (uint32_t)sass_bits(w, 72, 80); uint32_t v = idx == 0xff ? 0 : env->sysreg(env, idx);   /* CS2R */
            sx_setr(t, in.dst, v); if (sass_bits(w, 80, 81) && in.dst != 255) sx_setr(t, in.dst + 1, idx == 0xff ? 0 : env->sysreg(env, idx + 1)); break; }
        case 0x2ca: case 0x3c2: sx_setur(t, in.dst, sx_reg(t, (uint32_t)sass_bits(w, 24, 32))); break;           /* R2UR */
        case 0x326: sx_setr(t, in.dst, env->attr_in(env, (uint32_t)sass_bits(w, 64, 72) * 4)); break;            /* IPA */
        case 0x321: { uint32_t n = (uint32_t)sass_bits(w, 74, 76) + 1, base = (uint32_t)sass_bits(w, 40, 50) + sx_reg(t, (uint32_t)sass_bits(w, 24, 32)); /* ALD */
            for (uint32_t i = 0; i < n; i++) if (in.dst != 255) sx_setr(t, in.dst + i, env->attr_in(env, base + i * 4)); break; }
        case 0x322: { uint32_t n = (uint32_t)sass_bits(w, 74, 76) + 1, base = (uint32_t)sass_bits(w, 40, 50) + sx_reg(t, (uint32_t)sass_bits(w, 24, 32)), r = (uint32_t)sass_bits(w, 32, 40); /* AST */
            for (uint32_t i = 0; i < n; i++) env->attr_out(env, base + i * 4, sx_reg(t, r == 255 ? 255 : r + i)); break; }
        case 0xb69: case 0x36a: sx_setr(t, in.dst, 0); if (in.dst < 254) sx_setr(t, in.dst + 1, 0); break;   /* TMML: LOD not modelled */
        case 0xabb: { uint32_t idx = (uint32_t)sass_bits(w, 54, 59), off = (uint32_t)sass_bits(w, 38, 54) + sx_ureg(t, (uint32_t)sass_bits(w, 24, 30)), ty = (uint32_t)sass_bits(w, 73, 76); /* LDC, uniform offset */
            sx_setr(t, in.dst, env->cbuf(env, idx, off)); if (ty == 5 && in.dst < 254) sx_setr(t, in.dst + 1, env->cbuf(env, idx, off + 4)); break; }
        case 0x526: sx_setr(t, in.dst, env->attr_in(env, (uint32_t)sass_bits(w, 64, 72) * 4)); break;              /* IPA (variant) */
        case 0x822: { float x = sx_f(sx_reg(t, (uint32_t)sass_bits(w, 24, 32))), y = sx_f(sx_reg(t, (uint32_t)sass_bits(w, 64, 72)));   /* FSWZADD (as quad lane 0) */
            uint32_t op = ((uint32_t)sass_bits(w, 32, 40) >> 6) & 3; sx_setr(t, in.dst, sx_u(op == 0 ? x + y : op == 1 ? x - y : op == 2 ? y - x : y)); break; }
        case 0xf60: case 0xb6c: case 0x36d:
        case 0xb60: case 0xb66: case 0xd61: case 0x361: case 0xd67: case 0x367: case 0xb6f: case 0x370: case 0xb63: case 0xd64: case 0x364: {   /* TEX / TLD / TXQ / TLD4 */
            uint32_t ra = (uint32_t)sass_bits(w, 24, 32), rb = (uint32_t)sass_bits(w, 32, 40), d2 = (uint32_t)sass_bits(w, 64, 72);
            uint32_t mask = (uint32_t)sass_bits(w, 72, 76), dim = (uint32_t)sass_bits(w, 61, 64), lodm = (uint32_t)sass_bits(w, 87, 90);
            uint32_t c[8] = {0}; int nc = 0;
            int is_txq = in.op12 == 0xb6f || in.op12 == 0x370, is_tld = in.op12 == 0xb66 || in.op12 == 0xd67 || in.op12 == 0x367;
            int is_tld4 = in.op12 == 0xb63 || in.op12 == 0xd64 || in.op12 == 0x364;
            int bound = in.op12 == 0xb60 || in.op12 == 0xb66 || in.op12 == 0xb6f || in.op12 == 0xb63 || in.op12 == 0xb6c || in.op12 == 0xf60;
            uint32_t handle = in.op12 == 0xf60 ? sx_ureg(t, (uint32_t)sass_bits(w, 40, 46)) : bound ? env->cbuf(env, (uint32_t)sass_bits(w, 54, 59), (uint32_t)sass_bits(w, 40, 54) * 4) : 0xFFFFFFFFu;
            if (in.op12 == 0xb6c || in.op12 == 0x36d) lodm = 0;               /* TXD: gradients ignored */
            if (is_txq) {                                                    /* TXQ: [handle if bindless], lod */
                dim = (uint32_t)sass_bits(w, 62, 64);
                for (int i = 0; i < 2 && ra != 255; i++) c[nc++] = sx_reg(t, ra + i);
            } else {
                /* operand layout (Mesa NAK nak_nir_lower_tex): first register group = [array index] + coordinates;
                   second = [bindless handle], [lod/bias], [offset], [depth reference], [sample];
                   .SCR (bit 60) with an empty second group splits the first one 1+1, 2+1 or 2+2 */
                int arr = dim == 4 || dim == 5 || dim == 7, ncoord = (dim == 0 || dim == 4) ? 1 : (dim == 1 || dim == 5) ? 2 : 3;
                int n0 = arr + ncoord;
                int has_lod = !is_tld4 && (lodm == 2 || lodm == 3 || lodm == 5);
                int aoffi = !is_tld4 && (int)sass_bits(w, 76, 77), dc = !is_tld && (int)sass_bits(w, 78, 79), ms = is_tld && (int)sass_bits(w, 78, 79);
                int n1 = (!bound) + has_lod + aoffi + dc + ms;
                int m0 = n0, m1 = 0;
                if (sass_bits(w, 60, 61) && n1 == 0) { if (n0 == 4) { m0 = 2; m1 = 2; } else if (n0 == 3) { m0 = 2; m1 = 1; } else if (n0 == 2) { m0 = 1; m1 = 1; } }
                for (int i = 0; i < m0; i++) c[nc++] = ra == 255 ? 0 : sx_reg(t, ra + (uint32_t)i);
                for (int i = 0; i < m1; i++) c[nc++] = rb == 255 ? 0 : sx_reg(t, rb + (uint32_t)i);
                int k1 = m1;                                                 /* second group, after any moved coordinates */
                if (!bound) handle = rb == 255 ? 0 : sx_reg(t, rb + (uint32_t)k1++);
                c[7] = has_lod && rb != 255 ? sx_reg(t, rb + (uint32_t)k1) : 0;   /* lod (unused by the sampler yet) */
                if (dc) { int kd = k1 + has_lod + aoffi; c[6] = rb != 255 ? sx_reg(t, rb + (uint32_t)kd) : 0; lodm |= 0x100; }   /* depth reference for shadow compares */
                if (is_tld4) lodm = (uint32_t)sass_bits(w, 87, 89) | (lodm & 0x100);   /* gather component */
            }
            uint32_t out[4] = {0, 0, 0, 0};
            env->tex(env, in.op12, handle, c, nc, dim, lodm, out);
            uint32_t vals[4]; int nv = 0; for (int i = 0; i < 4; i++) if ((mask >> i) & 1) vals[nv++] = out[i];
            if (!is_txq && sass_bits(w, 79, 80)) {                           /* .F16: results packed as halves, two per register */
                uint32_t p0 = 0, p1 = 0;
                for (int i = 0; i < nv && i < 2; i++) p0 |= (uint32_t)sx_f_to_half(sx_f(vals[i])) << (16 * i);
                for (int i = 2; i < nv; i++) p1 |= (uint32_t)sx_f_to_half(sx_f(vals[i])) << (16 * (i - 2));
                if (in.dst != 255) sx_setr(t, in.dst, p0);
                if (nv > 2) { if (d2 != 255) sx_setr(t, d2, p1); else if (in.dst != 255) sx_setr(t, in.dst + 1, p1); }
                break;
            }
            for (int i = 0; i < nv; i++) {
                if (i < 2) { if (in.dst != 255) sx_setr(t, in.dst + i, vals[i]); }
                else if (d2 != 255) sx_setr(t, d2 + i - 2, vals[i]);
                else if (in.dst != 255) sx_setr(t, in.dst + i, vals[i]);
            }
            break;
        }
        case 0x981: case 0x381: case 0x983: case 0x984: {                                                          /* LDG / LDL / LDS */
            uint32_t ra = (uint32_t)sass_bits(w, 24, 32), ty = (uint32_t)sass_bits(w, 73, 76);
            int32_t off = sass_sext(sass_bits(w, 40, 64), 24);
            uint64_t a = sx_reg(t, ra);
            if (in.op12 == 0x981 || in.op12 == 0x381) {
                int wide = in.op12 == 0x981 ? (int)sass_bits(w, 90, 91) : (int)sass_bits(w, 72, 73);
                if (wide && ra != 255) a |= (uint64_t)sx_reg(t, ra + 1) << 32;
                if (in.op12 == 0x981 && sass_bits(w, 91, 92)) { uint32_t ur = (uint32_t)sass_bits(w, 32, 38); a += sx_ureg(t, ur) | (sass_bits(w, 72, 73) ? (uint64_t)sx_ureg(t, ur + 1) << 32 : 0); }
            }
            if (in.op12 == 0x984 && env->shared) {                       /* LDS: address stride (78..80) and uniform base */
                static const uint32_t mul[4] = {1, 4, 8, 16}; a = (uint64_t)(uint32_t)a * mul[sass_bits(w, 78, 80)];
                if (sass_bits(w, 91, 92)) a += sx_ureg(t, (uint32_t)sass_bits(w, 32, 38));   /* uniform base only in .E mode (bit 91) */
            }
            a += (uint64_t)(int64_t)off;
            uint32_t n = ty == 5 ? 2 : ty == 6 ? 4 : 1;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t v;
                if (in.op12 == 0x984 && env->shared) { uint32_t li = (uint32_t)(a / 4) + i; v = li < env->shared_words ? env->shared[li] : 0; }
                else if (in.op12 == 0x983 || in.op12 == 0x984) { uint32_t li = (uint32_t)(a / 4) + i; v = li < 1024 ? t->local[li] : 0; }
                else v = env->mem_ld ? env->mem_ld(env, a + i * 4) : 0;
                if (ty < 4) { uint32_t sh = (uint32_t)(a & 3) * 8; v >>= sh; v = ty == 0 ? v & 0xFF : ty == 1 ? (uint32_t)(int8_t)v : ty == 2 ? v & 0xFFFF : (uint32_t)(int16_t)v; }
                if (in.dst != 255) sx_setr(t, in.dst + i, v);
            }
            break;
        }
        case 0x987: case 0x387: case 0x988: case 0x388: case 0x986: case 0x386: {                                  /* STL / STS / STG */
            uint32_t ra = (uint32_t)sass_bits(w, 24, 32), rd = (uint32_t)sass_bits(w, 32, 40), ty = (uint32_t)sass_bits(w, 73, 76);
            int sh = in.op12 == 0x988 || in.op12 == 0x388, gl = in.op12 == 0x986 || in.op12 == 0x386;
            uint64_t a = sx_reg(t, ra);
            if (gl) {
                int wide = in.op12 == 0x986 ? (int)sass_bits(w, 90, 91) : (int)sass_bits(w, 72, 73);
                if (wide && ra != 255) a |= (uint64_t)sx_reg(t, ra + 1) << 32;
                if (in.op12 == 0x986 && sass_bits(w, 91, 92)) { uint32_t ur = (uint32_t)sass_bits(w, 64, 70); a += sx_ureg(t, ur) | (sass_bits(w, 72, 73) ? (uint64_t)sx_ureg(t, ur + 1) << 32 : 0); }
            } else if (sh && env->shared) {
                static const uint32_t mul[4] = {1, 4, 8, 16}; a = (uint64_t)(uint32_t)a * mul[sass_bits(w, 78, 80)];
                if (in.op12 == 0x988 && sass_bits(w, 91, 92)) a += sx_ureg(t, (uint32_t)sass_bits(w, 64, 70));
            }
            a += (uint64_t)(int64_t)sass_sext(sass_bits(w, 40, 64), 24);
            uint32_t n = ty == 5 ? 2 : ty == 6 ? 4 : 1;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t v = sx_reg(t, rd == 255 ? 255 : rd + i), li = (uint32_t)(a / 4) + i;
                if (gl) {
                    if (!env->mem_st) continue;
                    if (ty < 4) { uint32_t old = env->mem_ld ? env->mem_ld(env, a & ~3ull) : 0, s8 = (uint32_t)(a & 3) * 8, m = (ty < 2 ? 0xFFu : 0xFFFFu) << s8;
                        env->mem_st(env, a & ~3ull, (old & ~m) | ((v << s8) & m)); }
                    else env->mem_st(env, a + i * 4, v);
                    continue;
                }
                uint32_t *mem = (sh && env->shared) ? env->shared : t->local, lim = (sh && env->shared) ? env->shared_words : 1024;
                if (li >= lim) continue;
                if (ty < 4) { uint32_t s8 = (uint32_t)(a & 3) * 8, m = (ty < 2 ? 0xFFu : 0xFFFFu) << s8; mem[li] = (mem[li] & ~m) | ((v << s8) & m); }
                else mem[li] = v;
            }
            break;
        }
        case 0x38c: case 0x98c: case 0x38d: {                                                                      /* ATOMS */
            uint32_t ra = (uint32_t)sass_bits(w, 24, 32), rb = (uint32_t)sass_bits(w, 32, 40), op = (uint32_t)sass_bits(w, 87, 91), ty = (uint32_t)sass_bits(w, 73, 76);
            static const uint32_t mul[4] = {1, 4, 8, 16};
            uint32_t a = sx_reg(t, ra) * mul[sass_bits(w, 78, 80)] + (uint32_t)sass_sext(sass_bits(w, 40, 64), 24);
            if (in.op12 == 0x98c) a += sx_ureg(t, (uint32_t)sass_bits(w, 64, 70));
            uint32_t li = a / 4, old = 0;
            if (env->shared && li < env->shared_words) {
                old = env->shared[li];
                if (in.op12 == 0x38d) { if (old == sx_reg(t, rb)) env->shared[li] = sx_reg(t, (uint32_t)sass_bits(w, 64, 72)); }
                else if (ty == 3 && op == 0) env->shared[li] = sx_u(sx_f(old) + sx_f(sx_reg(t, rb)));
                else env->shared[li] = sx_atom(op, ty == 1, old, sx_reg(t, rb));
            }
            sx_setr(t, in.dst, old); break;
        }
        case 0x3a8: case 0x9a8: case 0x98e: case 0x3a9: {                                                          /* ATOMG / RED */
            uint32_t ra = (uint32_t)sass_bits(w, 24, 32), rb = (uint32_t)sass_bits(w, 32, 40), ty = (uint32_t)sass_bits(w, 73, 76);
            uint32_t op = in.op12 == 0x98e ? (uint32_t)sass_bits(w, 87, 90) : (uint32_t)sass_bits(w, 87, 91);
            uint64_t a = sx_reg(t, ra);
            uint32_t wb = in.op12 == 0x9a8 ? 70 : in.op12 == 0x98e ? 90 : 72;           /* 64-bit address flag */
            if (sass_bits(w, wb, wb + 1) && ra != 255) a |= (uint64_t)sx_reg(t, ra + 1) << 32;
            if ((in.op12 == 0x9a8 || in.op12 == 0x98e) && sass_bits(w, 91, 92)) a += sx_ureg(t, (uint32_t)sass_bits(w, 64, 70));
            a += (uint64_t)(int64_t)sass_sext(sass_bits(w, 40, 64), 24);
            uint32_t old = env->mem_ld ? env->mem_ld(env, a) : 0, nv;
            if (in.op12 == 0x3a9) nv = old == sx_reg(t, rb) ? sx_reg(t, (uint32_t)sass_bits(w, 64, 72)) : old;
            else if (ty == 3 && op == 0) nv = sx_u(sx_f(old) + sx_f(sx_reg(t, rb)));
            else nv = sx_atom(op, ty == 1, old, sx_reg(t, rb));
            if (env->mem_st) env->mem_st(env, a, nv);
            if (in.op12 != 0x98e) sx_setr(t, in.dst, old);
            break;
        }
        case 0x998: case 0x99a: case 0x99c: case 0x99e: case 0xb97: case 0xb99: case 0xb9b: case 0xb9d: {          /* SULD / SUST (bindless, or handle in a constant buffer) */
            int bound = in.op12 >= 0xb00; uint32_t op = bound ? in.op12 - 0x1ff : in.op12;
            uint32_t rc = (uint32_t)sass_bits(w, 24, 32), rd = (uint32_t)sass_bits(w, 32, 40), rh = (uint32_t)sass_bits(w, 64, 72), dim = (uint32_t)sass_bits(w, 61, 64);
            int fmt = op == 0x998 || op == 0x99c;
            uint32_t mt = fmt ? (uint32_t)sass_bits(w, 72, 76) : (uint32_t)sass_bits(w, 73, 76);
            uint32_t co[4] = { sx_reg(t, rc), rc == 255 ? 0 : sx_reg(t, rc + 1), rc == 255 ? 0 : sx_reg(t, rc + 2), 0 };
            uint32_t h = bound ? env->cbuf(env, (uint32_t)sass_bits(w, 54, 59), (uint32_t)sass_bits(w, 40, 54) * 4) : sx_reg(t, rh);
            if (op >= 0x99c) {
                uint32_t d[4]; for (int i = 0; i < 4; i++) d[i] = rd == 255 ? 0 : sx_reg(t, rd + (uint32_t)i);
                if (env->surf_st) env->surf_st(env, h, co, dim, d, fmt, mt);
            } else {
                uint32_t o[4] = {0, 0, 0, 0};
                if (env->surf_ld) env->surf_ld(env, h, co, dim, o, fmt, mt);
                int nv = 0; uint32_t vals[4];
                if (fmt) { for (int i = 0; i < 4; i++) if ((mt >> i) & 1) vals[nv++] = o[i]; }
                else { nv = mt == 5 ? 2 : mt == 6 ? 4 : 1; for (int i = 0; i < nv; i++) vals[i] = o[i]; }
                for (int i = 0; i < nv; i++) sx_setr(t, in.dst == 255 ? 255 : in.dst + (uint32_t)i, vals[i]);
            }
            break;
        }
        case 0x81c: case 0x89c: {                                                                                  /* PLOP3 */
            int u = in.op12 == 0x89c;
            int p0 = u ? sx_upred(t, (uint32_t)sass_bits(w, 87, 90), (int)sass_bits(w, 90, 91)) : sx_pred(t, (uint32_t)sass_bits(w, 87, 90), (int)sass_bits(w, 90, 91));
            int p1 = u ? sx_upred(t, (uint32_t)sass_bits(w, 77, 80), (int)sass_bits(w, 80, 81)) : sx_pred(t, (uint32_t)sass_bits(w, 77, 80), (int)sass_bits(w, 80, 81));
            int p2 = u ? sx_upred(t, (uint32_t)sass_bits(w, 68, 71), (int)sass_bits(w, 71, 72)) : sx_pred(t, (uint32_t)sass_bits(w, 68, 71), (int)sass_bits(w, 71, 72));
            uint32_t lut0 = (uint32_t)(sass_bits(w, 64, 67) | sass_bits(w, 72, 77) << 3), lut1 = (uint32_t)sass_bits(w, 16, 24);
            int idx = (p0 << 2) | (p1 << 1) | p2;
            if (u) { sx_setup(t, (uint32_t)sass_bits(w, 81, 84), (lut0 >> idx) & 1); sx_setup(t, (uint32_t)sass_bits(w, 84, 87), (lut1 >> idx) & 1); }
            else { sx_setp(t, (uint32_t)sass_bits(w, 81, 84), (lut0 >> idx) & 1); sx_setp(t, (uint32_t)sass_bits(w, 84, 87), (lut1 >> idx) & 1); }
            break;
        }
        case 0x947: {                                                                                              /* BRA */
            if (sx_pred(t, (uint32_t)sass_bits(w, 87, 90), (int)sass_bits(w, 90, 91))) next = (uint32_t)((int64_t)next + 4 * sass_sext(sass_bits(w, 34, 82), 48));
            break;
        }
        case 0x547: if (sx_upred(t, (uint32_t)sass_bits(w, 24, 27), (int)sass_bits(w, 27, 28))) next = (uint32_t)((int64_t)next + 4 * sass_sext(sass_bits(w, 34, 82), 48)); break;
        /* CALL / RET: the 32-bit field is relative to the next instruction; RET adds the return address the
           code put in a register (which counts from the program's own base, so field + register lands right) */
        case 0x944: next = (uint32_t)((int64_t)next + (int32_t)w[1]); break;                                       /* CALL */
        case 0x950: { uint32_t r = (uint32_t)sass_bits(w, 24, 32); next = (uint32_t)((int64_t)next + (int32_t)w[1] + (int64_t)sx_reg(t, r)); break; }   /* RET */
        case 0x94d: t->exited = 1; return steps;                                                                   /* EXIT */
        case 0x95b: t->killed = 1; t->exited = 1; return steps;                                                    /* KILL */
        case 0xb1d: case 0x31d: if (env->bar_yield) { t->at_bar = 1; t->resume_pc = next; return steps; } break;   /* BAR: wait for the workgroup */
        case 0x949: { uint32_t r = (uint32_t)sass_bits(w, 24, 32); next = (uint32_t)((int64_t)next + 4 * sass_sext(sass_bits(w, 34, 82), 48) + (int64_t)(int32_t)sx_reg(t, r)); break; }   /* BRX (indirect) */
        case 0x948: case 0x942: case 0x918: case 0x945: case 0x941: case 0x148: case 0x992: case 0x98f: case 0x355: case 0x356: case 0x91a: break; /* no-ops here (0x91a DEPBAR) */
        case 0x389: case 0x589: case 0x989: case 0xf89: case 0x3c4: {                                                /* SHFL / REDUX */
            int redux = in.op12 == 0x3c4;
            if (env->warp_sync) {
                if (t->sync_done) {
                    t->sync_done = 0;
                    if (redux) sx_setur(t, in.dst, t->sync_res); else { sx_setr(t, in.dst, t->sync_res); sx_setp(t, (uint32_t)sass_bits(w, 81, 84), t->sync_pres); }
                    break;
                }
                t->sync_val = sx_reg(t, (uint32_t)sass_bits(w, 24, 32));
                if (redux) { t->sync_kind = 3; t->sync_mode = (uint32_t)sass_bits(w, 78, 81) | (uint32_t)sass_bits(w, 73, 74) << 4; }
                else {
                    t->sync_kind = 1; t->sync_mode = (uint32_t)sass_bits(w, 58, 60);
                    t->sync_lane = (in.op12 == 0x389 || in.op12 == 0x589) ? sx_reg(t, (uint32_t)sass_bits(w, 32, 40)) : (uint32_t)sass_bits(w, 53, 58);
                    t->sync_c = (in.op12 == 0x389 || in.op12 == 0x989) ? sx_reg(t, (uint32_t)sass_bits(w, 64, 72)) : (uint32_t)sass_bits(w, 40, 53);
                }
                t->at_sync = 1; t->sync_pc = pc; t->resume_pc = pc; return steps;
            }
            if (redux) sx_setur(t, in.dst, sx_reg(t, (uint32_t)sass_bits(w, 24, 32)));
            else { sx_setr(t, in.dst, sx_reg(t, (uint32_t)sass_bits(w, 24, 32))); sx_setp(t, (uint32_t)sass_bits(w, 81, 84), 1); }
            break;
        }
        case 0x23c: case 0x23a: {                                                                                  /* HMMA / MOVM (warp) */
            int hmma = in.op12 == 0x23c;
            uint32_t k16 = (uint32_t)sass_bits(w, 75, 76), f32 = hmma ? (uint32_t)sass_bits(w, 76, 77) : 0;
            uint32_t nd = hmma ? (f32 ? 4 : 2) : 1;
            if (env->warp_sync && t->sync_done) { t->sync_done = 0; for (uint32_t i = 0; i < nd; i++) sx_setr(t, in.dst == 255 ? 255 : in.dst + i, t->sync_out[i]); break; }
            uint32_t ra = (uint32_t)sass_bits(w, 24, 32), rb = (uint32_t)sass_bits(w, 32, 40), rc = (uint32_t)sass_bits(w, 64, 72);
            uint32_t na = hmma ? (k16 ? 4 : 2) : 1, nb = k16 ? 2 : 1, nc = f32 ? 4 : 2;
            for (uint32_t i = 0; i < 10; i++) t->sync_in[i] = 0;
            for (uint32_t i = 0; i < na; i++) t->sync_in[i] = ra == 255 ? 0 : sx_reg(t, ra + i);
            if (hmma) { for (uint32_t i = 0; i < nb; i++) t->sync_in[4 + i] = rb == 255 ? 0 : sx_reg(t, rb + i);
                        for (uint32_t i = 0; i < nc; i++) t->sync_in[6 + i] = rc == 255 ? 0 : sx_reg(t, rc + i); }
            if (!env->warp_sync) { if (!env->unknown_ops) env->unknown_ops = in.op12 | 0x10000; break; }
            t->sync_kind = hmma ? 4 : 5; t->sync_mode = k16 | f32 << 1; t->at_sync = 1; t->sync_pc = pc; t->resume_pc = pc; return steps;
        }
        case 0x806: case 0x886: if (env->warp_sync) {                                                               /* VOTE (warp) */
            if (t->sync_done) { t->sync_done = 0;
                if (in.op12 == 0x806) sx_setr(t, in.dst, t->sync_res); else sx_setur(t, in.dst, t->sync_res);
                sx_setp(t, (uint32_t)sass_bits(w, 81, 84), t->sync_pres); break; }
            t->sync_kind = 2; t->sync_val = (uint32_t)sx_pred(t, (uint32_t)sass_bits(w, 87, 90), (int)sass_bits(w, 90, 91)); t->sync_mode = (uint32_t)sass_bits(w, 72, 74);
            t->at_sync = 1; t->sync_pc = pc; t->resume_pc = pc; return steps;
        } /* fall through */
        { int v = sx_pred(t, (uint32_t)sass_bits(w, 87, 90), (int)sass_bits(w, 90, 91));  /* VOTE (one thread) */
            if (in.op12 == 0x806) sx_setr(t, in.dst, v ? 1 : 0); else sx_setur(t, in.dst, v ? 1 : 0);
            sx_setp(t, (uint32_t)sass_bits(w, 81, 84), v); break; }
        default: sx_unknown(env, in.op12); break;
        }
        pc = next;
    }
    return steps;
}
#endif
