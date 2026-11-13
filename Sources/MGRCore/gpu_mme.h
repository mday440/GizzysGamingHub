/*
 * MikesGameRoom - Turing/Ampere "MME64" macro interpreter (included by gpu_nx2.c).
 *
 * NVN2 drives most 3D and compute state through macros: small programs uploaded to the
 * engine's macro instruction RAM and started by writing methods 0xE00+ (even method =
 * start with the first parameter, odd = more parameters). Each 96-bit instruction runs
 * two ALU lanes and two method outputs. Instruction semantics follow Mesa's TU104 MME
 * documentation and simulator (MIT); this is an independent implementation in C.
 *
 * Fields (bit offsets in the 96-bit instruction, word 2 of a triple is bits 0-31):
 *   0 end, 1-4 predicate mode, 5-9 predicate register,
 *   lane 0: 10-14 op, 15-19 dst, 20-24 srcA, 25-29 srcB, 30-45 imm
 *   lane 1: 46-50 op, 51-55 dst, 56-60 srcA, 61-65 srcB, 66-81 imm
 *   out 0: 82-84 method select, 85-88 emit select; out 1: 89-91 method, 92-95 emit
 * Registers 0-23 are general; 24 zero, 25 own imm (sign-extended), 26 other lane's imm,
 * 27 imm0:imm1, 28/29 the next words of the parameter FIFO (loaded when used).
 */

#define MME_CODE_WORDS 0x3000
#define MME_MAX_PARAMS 0x2000

typedef struct {
    const uint32_t *code;       /* instruction RAM (3 words per instruction) */
    uint32_t *dram;             /* data RAM (0xC00 words) */
    const uint32_t *fifo; uint32_t nfifo, consumed;
    uint32_t underflows;                         /* loads past the end of the parameters (read as 0) */
    uint32_t extra[0x1000]; uint32_t nextra, dma_pos, dma_pending;   /* DMA_READ_FIFOED words, read position, requested count */
    /* buffered mode: emits are collected (and the data RAM backed up before its first change)
       so a call that runs out of parameters can be undone and retried once more words arrive */
    int buffering; uint32_t nbuf; uint32_t buf[0x4000][2];
    int dram_saved; uint32_t dram_backup[0xC00];
    /* callbacks */
    mgr_emu *e; nx2_channel *ch; nx2_engine *en;
} mme_ctx;

static void engine_method(mgr_emu *e, nx2_channel *ch, nx2_engine *en, uint32_t m, uint32_t v);

static inline uint32_t mme_field(const uint32_t b[3], unsigned low, unsigned count) {
    uint64_t v = b[low / 32];
    if (low / 32 < 2) v |= (uint64_t)b[low / 32 + 1] << 32;
    return (uint32_t)((v >> (low % 32)) & ((1ull << count) - 1));
}

/* Parameter loads. After MME_DMA_READ_FIFOED + the EXTENDED barrier the loaded memory words
   come first (Mesa's TU104 simulator replaces the parameter stream with them); then any call
   parameters still unread. */
static int mme_pop(mme_ctx *c, uint32_t *out) {
    if (c->dma_pos < c->nextra) { *out = c->extra[c->dma_pos++]; return 1; }
    if (c->consumed < c->nfifo) { *out = c->fifo[c->consumed++]; return 1; }
    return 0;
}
static void mme_barrier(mme_ctx *c) {             /* EXTENDED 0x1000, 1: DMA_READ_FIFOED data becomes readable */
    if (!c->dma_pending) return;
    uint64_t src = ((uint64_t)(c->en->regs[0x154] & 0x1FFFFFF) << 32) | c->en->regs[0x155];
    uint32_t n = c->dma_pending > 0x1000 ? 0x1000 : c->dma_pending;
    for (uint32_t i = 0; i < n; i++) c->extra[i] = gread32(c->e, src + i * 4ull);
    c->nextra = n; c->dma_pos = 0; c->dma_pending = 0;
}

/* Turing MME memory reads triggered through methods it emits:
   0x154/0x155 SET_MME_MEM_ADDRESS_A/B, 0x156 data RAM address, 0x157 DMA_READ (into the data
   RAM), 0x158 DMA_READ_FIFOED (onto the parameter FIFO). */
static void mme_save_dram(mme_ctx *c) {
    if (c->buffering && !c->dram_saved) { memcpy(c->dram_backup, c->dram, sizeof c->dram_backup); c->dram_saved = 1; }
}
static void mme_emit(mme_ctx *c, uint32_t m, uint32_t v) {
    if (c->buffering && c->nbuf < 0x4000) {
        if (m < NX2_ENG_REGS) c->en->regs[m] = v;          /* later emits (DMA reads) read these at once */
        if (m == 0x8c4) c->en->regs[0xd00] = 1;             /* firmware call: the macro polls its completion */
        c->buf[c->nbuf][0] = m; c->buf[c->nbuf][1] = v; c->nbuf++;
    } else {
        if (c->buffering) {                                /* buffer full: apply what we have and go direct */
            for (uint32_t i = 0; i < c->nbuf; i++) engine_method(c->e, c->ch, c->en, c->buf[i][0], c->buf[i][1]);
            c->nbuf = 0; c->buffering = 0;
        }
        engine_method(c->e, c->ch, c->en, m, v);
    }
    if (m == 0x157 || m == 0x15c) mme_save_dram(c);
    if (m == 0x159) {                                      /* MME_DMA_WRITE(n): data RAM -> memory */
        uint64_t dst = ((uint64_t)(c->en->regs[0x154] & 0x1FFFFFF) << 32) | c->en->regs[0x155];
        uint32_t ram = c->en->regs[0x156], n = v > 0xC00 ? 0xC00 : v;
        for (uint32_t i = 0; i < n && ram + i < 0xC00; i++) gwrite32(c->e, dst + i * 4ull, c->dram[ram + i]);
    }
    if (m == 0x15c) {                                      /* MME_DMA_SYNC(v): DMA done, write v to data RAM */
        uint32_t ram = c->en->regs[0x156];
        if (ram < 0xC00) c->dram[ram] = v;                 /* (our DMA is synchronous) */
    }
    if (m == 0x157 || m == 0x158) {
        uint64_t src = ((uint64_t)(c->en->regs[0x154] & 0x1FFFFFF) << 32) | c->en->regs[0x155];
        uint32_t n = v > 0x1000 ? 0x1000 : v, ram = c->en->regs[0x156];
        if (m == 0x158) c->dma_pending = n;                /* read at the next barrier */
        else for (uint32_t i = 0; i < n; i++) if (ram + i < 0xC00) c->dram[ram + i] = gread32(c->e, src + i * 4ull);
    }
}

/* Run the macro starting at instruction `pc`. Returns 0 on success, else an error string id. */
static const char *mme_run_core(mme_ctx *c, uint32_t pc, uint32_t *r, uint32_t *carry_io, uint32_t *method_io, uint32_t *inc_io);
static const char MME_UNDERFLOW[] = "parameter FIFO underflow";
static const char *mme_run(mme_ctx *c, uint32_t pc) {
    /* The output method address and its increment carry over too: NVN2 has macros that enter
       the tail of another one and emit straight away to the method that one selected. */
    /* Registers keep their values from one call to the next (NVN2 macros read registers an
       earlier call set; zeroing them made several macros misread their parameters).
       MGR_MME_ZEROREGS=1: start every call from zero (old behaviour). */
    static int keep = -1; if (keep < 0) keep = getenv("MGR_MME_ZEROREGS") == NULL;
    if (keep) return mme_run_core(c, pc, c->en->mme_r, &c->en->mme_carry, &c->en->mme_method, &c->en->mme_inc);
    uint32_t r[24] = {0}, carry = 0;
    return mme_run_core(c, pc, r, &carry, &c->en->mme_method, &c->en->mme_inc);
}
static const char *mme_run_core(mme_ctx *c, uint32_t pc, uint32_t *r, uint32_t *carry_io, uint32_t *method_io, uint32_t *inc_io) {
    uint32_t carry = *carry_io, method = *method_io, inc = *inc_io;
    #define MME_RET(x) do { *carry_io = carry; *method_io = method; *inc_io = inc; return (x); } while (0)
    uint32_t loop_count = 0, loop_start = 0, loop_end = 0;
    int end_next = 0;
    static const char pm[16][5] = { "UUUU", "TTTT", "FFFF", "TTUU", "FFUU", "TFUU", "TUUU", "FUUU",
                                    "UUTT", "UUTF", "UUTU", "UUFT", "UUFF", "UUFU", "UUUT", "UUUF" };
    for (uint32_t steps = 0; steps < 1000000; steps++) {
        if ((uint64_t)pc * 3 + 2 >= MME_CODE_WORDS) MME_RET("pc outside instruction RAM");
        const uint32_t b[3] = { c->code[pc * 3 + 2], c->code[pc * 3 + 1], c->code[pc * 3] };
        uint32_t imm[2] = { mme_field(b, 30, 16), mme_field(b, 66, 16) };
        uint32_t op[2]  = { mme_field(b, 10, 5), mme_field(b, 46, 5) };
        uint32_t dst[2] = { mme_field(b, 15, 5), mme_field(b, 51, 5) };
        uint32_t sa[2]  = { mme_field(b, 20, 5), mme_field(b, 56, 5) };
        uint32_t sb[2]  = { mme_field(b, 25, 5), mme_field(b, 61, 5) };
        uint32_t om[2]  = { mme_field(b, 82, 3), mme_field(b, 89, 3) };
        uint32_t oe[2]  = { mme_field(b, 85, 4), mme_field(b, 92, 4) };
        uint32_t preg = mme_field(b, 5, 5), pmode = mme_field(b, 1, 4);
        int is_end = mme_field(b, 0, 1);
#ifdef MME_TRACE
        if (steps < 80) printf("pc %u r0=%x r1=%x r2=%x r3=%x\n", pc, r[0], r[1], r[2], r[3]);
#endif

        /* parameter FIFO loads: a word is popped for slot 28 / 29 when anything uses it */
        uint32_t load[2] = {0, 0};
        for (int i = 0; i < 2; i++) {
            uint32_t reg = 28 + i, out = 3 + i;
            int used = preg == reg || sa[0] == reg || sa[1] == reg || sb[0] == reg || sb[1] == reg ||
                       om[0] == out || om[1] == out || oe[0] == out || oe[1] == out;
            if (used && !mme_pop(c, &load[i])) {
                /* Out of parameters. The hardware would stall for more; NVN2 calls a few macros
                   with fewer words than their code reads, and stopping there skips the rest of
                   the macro (often a semaphore release at its end). Read zeros and carry on. */
                if (c->buffering) MME_RET(MME_UNDERFLOW);
                load[i] = 0; c->underflows++;
            }
        }
        #define GET(reg, lane) ((reg) < 24 ? r[reg] : (reg) == 24 ? 0u : \
            (reg) == 25 ? (uint32_t)(int32_t)(int16_t)imm[lane] : (reg) == 26 ? (uint32_t)(int32_t)(int16_t)imm[1 - (lane)] : \
            (reg) == 27 ? (imm[0] << 16 | imm[1]) : (reg) == 28 ? load[0] : load[1])
        uint32_t mask = 15;
        if (pmode) {
            int t = GET(preg, 0) != 0;
            mask = 0;
            for (int i = 0; i < 4; i++) if (pm[pmode][i] != (t ? 'T' : 'F')) mask |= 1u << i;   /* as observed with NVN2: a non-zero register disables 'T' slots */
        }
        uint32_t xs[2] = {0}, ys[2] = {0}, alu[2] = {0};
        for (int l = 0; l < 2; l++) if (mask & (1u << l)) { xs[l] = GET(sa[l], l); ys[l] = GET(sb[l], l); }
        #undef GET
        uint32_t next = pc + 1;
        int cancel_end = 0, stopped = 0;
        for (int l = 0; l < 2; l++) {
            if (!(mask & (1u << l))) continue;
            uint32_t x = xs[l], y = ys[l], im = imm[l], v = 0;
            int cond;
            {
                uint32_t cmp = op[l];
                if (cmp == 17 || cmp == 25) cond = (int32_t)x < (int32_t)y;
                else if (cmp == 18 || cmp == 26) cond = x < y;
                else if (cmp == 19 || cmp == 27) cond = (int32_t)x <= (int32_t)y;
                else if (cmp == 20 || cmp == 28) cond = x <= y;
                else cond = x == y;
            }
            switch (op[l]) {
            case 0: v = x + y; carry = v < x; break;
            case 1: v = x + y + carry; break;
            case 2: v = x - y; carry = v > x; break;
            case 3: v = x - y - carry; break;
            case 4: { int64_t p = (int64_t)(int32_t)x * (int64_t)(int32_t)y; v = (uint32_t)p; carry = (uint32_t)((uint64_t)p >> 32); break; }
            case 5: v = carry; break;
            case 6: { uint64_t p = (uint64_t)x * y; v = (uint32_t)p; carry = (uint32_t)(p >> 32); break; }
            case 7: if (x != 0x1000) MME_RET("unsupported extended op"); mme_barrier(c); break;     /* load barrier */
            case 8: v = x ? (uint32_t)__builtin_clz(x) : 32; break;
            case 9: v = x << (y & 31); break;
            case 10: v = x >> (y & 31); break;
            case 11: v = (uint32_t)((int32_t)x >> (y & 31)); break;
            case 12: v = x & y; break;
            case 13: v = ~(x & y); break;
            case 14: v = x | y; break;
            case 15: v = x ^ y; break;
            case 16: {                                                        /* MERGE */
                uint32_t d = (im >> 10) & 63, n = (im >> 5) & 31, s = im & 31;
                if (d >= 32) MME_RET("bad merge");
                uint32_t bm = n >= 32 ? ~0u : (1u << n) - 1;
                v = (x & ~(bm << d)) | (((y >> s) & bm) << d);
                break;
            }
            case 17: case 18: case 19: case 20: case 21: v = cond ? ~0u : 0; break;
            case 22: { uint32_t a = (x + y) & 0xFFFF; v = a < NX2_ENG_REGS ? c->en->regs[a] : 0; break; }  /* STATE read */
            case 23:                                                          /* LOOP */
                if (loop_count || im < 2) MME_RET("bad loop");
                /* Mesa's simulator runs a count of 0 once, but NVN2's macros expect it to skip the body
                   (running it reads past their parameters). */
                if (x == 0) { next = pc + im; break; }
                loop_count = x - 1; loop_start = pc; loop_end = pc + im - 1;
                break;
            case 24: next = pc + (im & 0x7FFF); break;                       /* JAL */
            case 25: case 26: case 27: case 28: case 29:                      /* branches */
                if (cond == ((im & 0x8000) != 0)) {
                    int32_t off = (int32_t)(im << 19) >> 19;
                    if (off == -4096) stopped = 1;
                    else { next = (uint32_t)((int32_t)pc + off); cancel_end = (im & 0x4000) != 0; }
                }
                break;
            case 30: if (x >= 0xC00) MME_RET("data RAM read out of bounds"); v = c->dram[x]; break;
            case 31: if (x >= 0xC00) MME_RET("data RAM write out of bounds"); mme_save_dram(c); c->dram[x] = y; break;
            }
            alu[l] = v;
            if (dst[l] < 24) r[dst[l]] = v;
        }
        for (int l = 0; l < 2; l++) {
            if (!(mask & (4u << l))) continue;
            #define OUT(sel) ((sel) == 1 ? alu[0] : (sel) == 2 ? alu[1] : (sel) == 3 ? load[0] : (sel) == 4 ? load[1] : \
                (sel) == 5 ? imm[0] : (sel) == 6 ? imm[1] : (sel) == 8 ? imm[0] >> 12 : (sel) == 9 ? imm[1] >> 12 : \
                (imm[0] << 16 | imm[1]))
            if (om[l]) { uint32_t a = OUT(om[l]); method = a & 0xFFF; inc = (a >> 12) & 15; }
            if (oe[l]) { mme_emit(c, method, OUT(oe[l])); method += inc; }
            #undef OUT
        }
        if (end_next || stopped) MME_RET(NULL);
        end_next = is_end && !cancel_end;
        if (loop_count && pc == loop_end) { loop_count--; next = loop_start + 1; }
        pc = next;
    }
    MME_RET("instruction limit");
    #undef MME_RET
}
