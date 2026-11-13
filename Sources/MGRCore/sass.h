/* Decoding of NVIDIA Ampere (SM86/T239) shader instructions.
 *
 * Every instruction is 128 bits. The field layout comes from Mesa's NAK compiler
 * (src/nouveau/compiler/nak/sm70_encode.rs, MIT licence, Copyright (c) 2022 Collabora),
 * which encodes these instructions; this file reads them back. */
#ifndef MGR_SASS_H
#define MGR_SASS_H
#include <stdint.h>

static inline uint64_t sass_bits(const uint32_t w[4], unsigned lo, unsigned hi) {   /* [lo, hi) */
    uint64_t v = 0;
    for (unsigned b = lo; b < hi; b++) v |= (uint64_t)((w[b / 32] >> (b % 32)) & 1) << (b - lo);
    return v;
}

enum {   /* source kinds */
    SRC_NONE, SRC_REG, SRC_UREG, SRC_IMM, SRC_CBUF
};
typedef struct { uint8_t kind, neg, abs, ucb; uint32_t reg; uint32_t imm; uint32_t cb_idx, cb_off; } sass_src;

typedef struct {
    uint16_t op12;          /* full 12-bit opcode */
    uint16_t op9;           /* low 9 bits (ALU ops: the form is in bits 9..11) */
    uint8_t form;
    uint8_t pred, pred_neg; /* guard predicate: 7 = always */
    uint8_t dst;            /* GPR 0..254, 255 = RZ */
    uint8_t is_alu, uniform;
    sass_src a, b, c;       /* ALU sources 0, 1, 2 */
    const char *name;
} sass_ins;

/* Name of an opcode (12-bit, or 9-bit with an ALU form). */
static const char *sass_name(uint16_t op12, int *alu) {
    static const struct { uint16_t op; const char *n; int alu; } t[] = {
        {0x021, "FADD", 1}, {0x023, "FFMA", 1}, {0x020, "FMUL", 1}, {0x009, "FMNMX", 1}, {0x00b, "FSETP", 1},
        {0x00a, "FSET", 1}, {0x108, "MUFU", 1}, {0x029, "DADD", 1}, {0x02b, "DFMA", 1}, {0x028, "DMUL", 1},
        {0x02a, "DSETP", 1}, {0x030, "HADD2", 1}, {0x031, "HFMA2", 1}, {0x032, "HMUL2", 1}, {0x033, "HSET2", 1},
        {0x034, "HSETP2", 1}, {0x040, "HMNMX2", 1}, {0x01b, "BMSK", 1}, {0x101, "BREV", 1}, {0x100, "FLO", 1},
        {0x013, "IABS", 1}, {0x010, "IADD3", 1}, {0x026, "IDP4", 1}, {0x024, "IMAD", 1}, {0x025, "IMAD.WIDE", 1},
        {0x017, "IMNMX", 1}, {0x00c, "ISETP", 1}, {0x011, "LEA", 1}, {0x012, "LOP3", 1}, {0x109, "POPC", 1},
        {0x019, "SHF", 1}, {0x104, "F2F", 1}, {0x110, "F2F.64", 1}, {0x105, "F2I", 1}, {0x111, "F2I.64", 1},
        {0x106, "I2F", 1}, {0x112, "I2F.64", 1}, {0x107, "FRND", 1}, {0x113, "FRND.64", 1}, {0x002, "MOV", 1},
        {0x016, "PRMT", 1}, {0x007, "SEL", 1}, {0x01a, "SGXT", 1}, {0x03e, "F2FP", 1}, {0x124, "OUT", 1},
        {0x008, "FSEL", 1}, {0x045, "I2FP", 1}, {0x01c, "PLOP3", 1}, {0x082, "UMOV", 1}, {0x027, "IMAD.HI", 1}, {0x004, "R2P", 1}, {0x003, "P2R", 1},
        /* uniform datapath (also ALU-form) */
        {0x090, "UIADD3", 1}, {0x0a4, "UIMAD", 1}, {0x08c, "UISETP", 1}, {0x092, "ULOP3", 1}, {0x099, "USHF", 1},
        {0x091, "ULEA", 1}, {0x096, "UPRMT", 1}, {0x087, "USEL", 1}, {0x09a, "USGXT", 1}, {0x09b, "UBMSK", 1},
        {0x0be, "UBREV", 1}, {0x0bd, "UFLO", 1}, {0x0bf, "UPOPC", 1}, {0x0a5, "UIMAD.WIDE", 1},
        /* full 12-bit opcodes */
        {0xc82, "UMOV", 0}, {0x822, "FSWZADD", 0}, {0x326, "IPA", 0}, {0xb60, "TEX", 0}, {0xd61, "TEX.B", 0},
        {0x361, "TEX.B", 0}, {0xb66, "TLD", 0}, {0xd67, "TLD.B", 0}, {0x367, "TLD.B", 0}, {0xb63, "TLD4", 0},
        {0xd64, "TLD4.B", 0}, {0x364, "TLD4.B", 0}, {0xb69, "TMML", 0}, {0x36a, "TMML.B", 0}, {0xb6c, "TXD", 0},
        {0xd6d, "TXD.B", 0}, {0x36d, "TXD.B", 0}, {0xb6f, "TXQ", 0}, {0x370, "TXQ.B", 0}, {0x321, "ALD", 0},
        {0x322, "AST", 0}, {0x920, "AL2P", 0}, {0xab9, "ULDC", 0}, {0xb82, "LDC", 0}, {0x582, "LDC.B", 0},
        {0x9c3, "S2UR", 0}, {0x919, "S2R", 0}, {0x805, "CS2R", 0}, {0x947, "BRA", 0}, {0x547, "BRA.U", 0},
        {0x94d, "EXIT", 0}, {0x95b, "KILL", 0}, {0x981, "LDG", 0}, {0x381, "LDG", 0}, {0x983, "LDL", 0},
        {0x984, "LDS", 0}, {0x986, "STG", 0}, {0x386, "STG", 0}, {0x987, "STL", 0}, {0x387, "STL", 0},
        {0x988, "STS", 0}, {0x388, "STS", 0}, {0x89c, "UPLOP3", 0}, {0x81c, "PLOP3", 0}, {0x2ca, "R2UR", 0},
        {0x3c2, "R2UR", 0}, {0xb1d, "BAR", 0}, {0x31d, "BAR", 0}, {0x918, "NOP", 0}, {0x925, "PIXLD", 0},
        {0x99a, "SULD.B", 0}, {0x998, "SULD.P", 0}, {0x99e, "SUST.B", 0}, {0x99c, "SUST.P", 0}, {0x886, "UVOTE", 0},
        {0x806, "VOTE", 0}, {0x923, "ISBERD", 0}, {0x945, "BSSY", 0}, {0x941, "BSYNC", 0}, {0x942, "BREAK", 0},
        {0x148, "WARPSYNC", 0}, {0x355, "BCLEAR", 0}, {0x356, "BMOV", 0}, {0x992, "MEMBAR", 0}, {0x98f, "CCTL", 0},
        {0x389, "SHFL", 0}, {0x589, "SHFL", 0}, {0x989, "SHFL", 0}, {0xf89, "SHFL", 0}, {0x3c4, "REDUX", 0},
        {0x950, "RET", 0}, {0x943, "CALL", 0}, {0x944, "CALL.ABS", 0}, {0x94c, "BPT", 0}, {0x3a1, "MATCH", 0},
        {0x9a6, "ATOMG", 0}, {0x98e, "RED", 0}, {0x3a8, "ATOMS", 0}, {0x38c, "ATOMS", 0}, {0x83b, "LDSM", 0},
        {0x3ad, "LDTRAM", 0}, {0x23c, "HMMA", 0}, {0x237, "IMMA", 0}, {0x0, 0, 0}
    };
    for (int i = 0; t[i].n; i++) if (!t[i].alu && t[i].op == op12) { *alu = 0; return t[i].n; }
    for (int i = 0; t[i].n; i++) if (t[i].alu && t[i].op == (op12 & 0x1FF)) { *alu = 1; return t[i].n; }
    *alu = 0; return 0;
}

static inline void sass_decode(const uint32_t w[4], sass_ins *in) {
    in->op12 = (uint16_t)sass_bits(w, 0, 12); in->op9 = in->op12 & 0x1FF; in->form = (uint8_t)sass_bits(w, 9, 12);
    in->pred = (uint8_t)sass_bits(w, 12, 15); in->pred_neg = (uint8_t)sass_bits(w, 15, 16);
    in->dst = (uint8_t)sass_bits(w, 16, 24);
    int alu = 0; in->name = sass_name(in->op12, &alu); in->is_alu = (uint8_t)alu;
    in->uniform = alu && in->op9 >= 0x080 && in->op9 < 0x0c0;
    sass_src none = {0}; in->a = in->b = in->c = none;
    if (!alu) return;
    /* ALU forms: 1 r,r,r  2 r,imm(src2),r  3 r,cb(src2),r  4 r,imm,r  5 r,cb,r  6 r,ur,r  7 r,ur(src2),r */
    in->a.kind = SRC_REG; in->a.reg = (uint32_t)sass_bits(w, 24, 32); in->a.abs = (uint8_t)sass_bits(w, 73, 74); in->a.neg = (uint8_t)sass_bits(w, 72, 73);
    sass_src r64 = { SRC_REG, (uint8_t)sass_bits(w, 75, 76), (uint8_t)sass_bits(w, 74, 75), 0, (uint32_t)sass_bits(w, 64, 72), 0, 0, 0 };
    sass_src mid = {0};
    mid.neg = (uint8_t)sass_bits(w, 63, 64); mid.abs = (uint8_t)sass_bits(w, 62, 63);
    switch (in->form) {
    case 1: mid.kind = SRC_REG; mid.reg = (uint32_t)sass_bits(w, 32, 40); in->b = mid; in->c = r64; break;
    case 2: mid.kind = SRC_IMM; mid.imm = (uint32_t)sass_bits(w, 32, 64); mid.neg = mid.abs = 0; in->c = mid; in->b = r64; break;
    case 3: mid.kind = SRC_CBUF; mid.cb_off = (uint32_t)sass_bits(w, 38, 54); mid.cb_idx = (uint32_t)sass_bits(w, 54, 59);
            mid.ucb = (uint8_t)sass_bits(w, 91, 92); mid.reg = (uint32_t)sass_bits(w, 32, 38); in->c = mid; in->b = r64; break;
    case 4: mid.kind = SRC_IMM; mid.imm = (uint32_t)sass_bits(w, 32, 64); mid.neg = mid.abs = 0; in->b = mid; in->c = r64; break;
    case 5: mid.kind = SRC_CBUF; mid.cb_off = (uint32_t)sass_bits(w, 38, 54); mid.cb_idx = (uint32_t)sass_bits(w, 54, 59);
            mid.ucb = (uint8_t)sass_bits(w, 91, 92); mid.reg = (uint32_t)sass_bits(w, 32, 38); in->b = mid; in->c = r64; break;
    case 6: mid.kind = SRC_UREG; mid.reg = (uint32_t)sass_bits(w, 32, 38); in->b = mid; in->c = r64; break;
    case 7: mid.kind = SRC_UREG; mid.reg = (uint32_t)sass_bits(w, 32, 38); in->c = mid; in->b = r64; break;
    default: break;
    }
    if (in->uniform) {                       /* uniform ops: registers are UGPRs (6 bits) */
        if (in->a.kind == SRC_REG) { in->a.kind = SRC_UREG; in->a.reg &= 63; }
        if (in->b.kind == SRC_REG) { in->b.kind = SRC_UREG; in->b.reg &= 63; }
        if (in->c.kind == SRC_REG) { in->c.kind = SRC_UREG; in->c.reg &= 63; }
        in->dst &= 63;
    }
}

/* ---- text form (for logs) ---- */
#include <stdio.h>
static int sass_fmt_src(char *o, int n, const sass_src *s) {
    char t[48];
    switch (s->kind) {
    case SRC_REG:  if (s->reg == 255) snprintf(t, sizeof t, "RZ"); else snprintf(t, sizeof t, "R%u", s->reg); break;
    case SRC_UREG: if (s->reg == 63) snprintf(t, sizeof t, "URZ"); else snprintf(t, sizeof t, "UR%u", s->reg); break;
    case SRC_IMM:  snprintf(t, sizeof t, "0x%x", s->imm); break;
    case SRC_CBUF: if (s->ucb) snprintf(t, sizeof t, "c[UR%u][0x%x]", s->reg, s->cb_off); else snprintf(t, sizeof t, "c[0x%x][0x%x]", s->cb_idx, s->cb_off); break;
    default: return 0;
    }
    return snprintf(o, n, ", %s%s%s%s", s->neg ? "-" : "", s->abs ? "|" : "", t, s->abs ? "|" : "");
}
static int sass_sext(uint64_t v, int bits) { return (int)((int64_t)(v << (64 - bits)) >> (64 - bits)); }
/* One line: "@P0 FFMA R1, R2, c[0x0][0x10], R3" (pc = byte offset, for branch targets). */
static void sass_disasm(const uint32_t w[4], uint64_t pc, char *o, int n) {
    sass_ins in; sass_decode(w, &in);
    int k = 0;
    if (in.pred != 7) k += snprintf(o + k, n - k, "@%sP%u ", in.pred_neg ? "!" : "", in.pred);
    if (!in.name) { snprintf(o + k, n - k, "?? op %03x (%08x %08x %08x %08x)", in.op12, w[0], w[1], w[2], w[3]); return; }
    k += snprintf(o + k, n - k, "%s", in.name);
    if (in.is_alu) {
        if (in.op9 == 0x108) k += snprintf(o + k, n - k, ".%s", (const char *[]){"COS","SIN","EX2","LG2","RCP","RSQ","RCP64H","RSQ64H","SQRT","TANH","?","?","?","?","?","?"}[sass_bits(w, 74, 80) & 15]);
        if (in.op9 == 0x012 || in.op9 == 0x092) k += snprintf(o + k, n - k, ".LUT%02x", (unsigned)sass_bits(w, 72, 80));
        if (in.op9 == 0x00b || in.op9 == 0x00c || in.op9 == 0x08c) {
            static const char *f[] = {"F","LT","EQ","LE","GT","NE","GE","NUM","NAN","LTU","EQU","LEU","GTU","NEU","GEU","T"};
            static const char *ic[] = {"F","LT","EQ","LE","GT","NE","GE","T"};
            k += snprintf(o + k, n - k, ".%s P%u", in.op9 == 0x00b ? f[sass_bits(w, 76, 80)] : ic[sass_bits(w, 76, 79)], (unsigned)sass_bits(w, 81, 84));
        } else if (in.uniform) k += snprintf(o + k, n - k, " UR%u", in.dst);
        else if (in.dst == 255) k += snprintf(o + k, n - k, " RZ");
        else k += snprintf(o + k, n - k, " R%u", in.dst);
        k += sass_fmt_src(o + k, n - k, &in.a); k += sass_fmt_src(o + k, n - k, &in.b); k += sass_fmt_src(o + k, n - k, &in.c);
        if (in.op9 == 0x007 || in.op9 == 0x008 || in.op9 == 0x00b || in.op9 == 0x00c) k += snprintf(o + k, n - k, ", %sP%u", sass_bits(w, 90, 91) ? "!" : "", (unsigned)sass_bits(w, 87, 90));
        return;
    }
    switch (in.op12) {
    case 0x326: snprintf(o + k, n - k, " R%u, a[0x%x], R%u", in.dst, (unsigned)sass_bits(w, 64, 72) * 4, (unsigned)sass_bits(w, 32, 40)); break;
    case 0x321: snprintf(o + k, n - k, ".%u R%u, a[R%u+0x%x] vtx R%u", (unsigned)sass_bits(w, 74, 76) + 1, in.dst, (unsigned)sass_bits(w, 24, 32), (unsigned)sass_bits(w, 40, 50), (unsigned)sass_bits(w, 32, 40)); break;
    case 0x322: snprintf(o + k, n - k, ".%u a[R%u+0x%x], R%u", (unsigned)sass_bits(w, 74, 76) + 1, (unsigned)sass_bits(w, 24, 32), (unsigned)sass_bits(w, 40, 50), (unsigned)sass_bits(w, 32, 40)); break;
    case 0xb60: case 0xb66: case 0xb63: case 0xb69: case 0xb6c: case 0xb6f:
        snprintf(o + k, n - k, " R%u, R%u, R%u, R%u, tex[c%u:0x%x] dim %u mask %x lod %u", in.dst, (unsigned)sass_bits(w, 64, 72), (unsigned)sass_bits(w, 24, 32),
                 (unsigned)sass_bits(w, 32, 40), (unsigned)sass_bits(w, 54, 59), (unsigned)sass_bits(w, 40, 54) * 4, (unsigned)sass_bits(w, 61, 64),
                 (unsigned)sass_bits(w, 72, 76), (unsigned)sass_bits(w, 87, 90)); break;
    case 0xd61: case 0x361: case 0xd67: case 0x367:
        snprintf(o + k, n - k, " R%u, R%u, R%u, R%u (bindless) dim %u mask %x", in.dst, (unsigned)sass_bits(w, 64, 72), (unsigned)sass_bits(w, 24, 32),
                 (unsigned)sass_bits(w, 32, 40), (unsigned)sass_bits(w, 61, 64), (unsigned)sass_bits(w, 72, 76)); break;
    case 0xb82: snprintf(o + k, n - k, ".%u R%u, c[0x%x][R%u+0x%x]", (unsigned)sass_bits(w, 73, 76), in.dst, (unsigned)sass_bits(w, 54, 59), (unsigned)sass_bits(w, 24, 32), (unsigned)sass_bits(w, 38, 54)); break;
    case 0xab9: snprintf(o + k, n - k, ".%u UR%u, c[0x%x][0x%x]", (unsigned)sass_bits(w, 73, 76), in.dst & 63, (unsigned)sass_bits(w, 54, 59), (unsigned)sass_bits(w, 38, 54)); break;
    case 0x919: snprintf(o + k, n - k, " R%u, SR%u", in.dst, (unsigned)sass_bits(w, 72, 80)); break;
    case 0x9c3: snprintf(o + k, n - k, " UR%u, SR%u", in.dst & 63, (unsigned)sass_bits(w, 72, 80)); break;
    case 0x805: snprintf(o + k, n - k, " R%u, SR%u", in.dst, (unsigned)sass_bits(w, 72, 80)); break;
    case 0xc82: snprintf(o + k, n - k, " UR%u, %s0x%x", in.dst & 63, in.form == 6 ? "UR" : "", in.form == 6 ? (unsigned)sass_bits(w, 32, 38) : (unsigned)sass_bits(w, 32, 64)); break;
    case 0x947: snprintf(o + k, n - k, " 0x%llx%s", (unsigned long long)(pc + 16 + 4 * sass_sext(sass_bits(w, 34, 82), 48) ), sass_bits(w, 87, 90) != 7 ? " (cond)" : ""); break;
    default: snprintf(o + k, n - k, " R%u (%08x %08x %08x %08x)", in.dst, w[0], w[1], w[2], w[3]); break;
    }
}
#endif
