#include <unistd.h>
#include <sys/mman.h>
/*
 * AArch64 interpreter + Horizon (Switch OS) HLE kernel stub.
 *
 * Coverage: the A64 integer instruction set used by compiled game/SDK code
 * (data processing, branches, all load/store addressing modes, exclusives,
 * ARMv8.1 LSE atomics, system registers, DC ZVA) plus the SIMD/FP subset
 * used for register moves and memcpy/memset. Floating-point arithmetic and
 * most vector math are not implemented yet: the CPU stops with
 * MGR_EMU_UNIMPLEMENTED and reports the instruction, so progress is visible.
 *
 * OS services: memory/thread/sync SVCs are emulated for a single thread.
 * IPC (SendSyncRequest to sm:, fs:, hid:, vi: ...) is not emulated yet, so
 * retail games stop with MGR_EMU_BLOCKED at their first service request.
 */
#include "emu_internal.h"

/* ------------------------------------------------------------------ */
void elog(mgr_emu *e, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof buf) n = sizeof buf - 1;
    if (e->log_len + (size_t)n + 1 > e->log_cap) {
        if (e->log_len > (4u << 20)) return; /* cap the log */
        size_t nc = e->log_cap ? e->log_cap * 2 : 8192;
        while (nc < e->log_len + (size_t)n + 1) nc *= 2;
        char *nl = realloc(e->log, nc);
        if (!nl) return;
        e->log = nl; e->log_cap = nc;
    }
    memcpy(e->log + e->log_len, buf, (size_t)n);
    e->log_len += (size_t)n;
    e->log[e->log_len] = 0;
}

void emu_stop(mgr_emu *e, mgr_emu_status st, const char *fmt, ...) {
    if (e->status != MGR_EMU_RUNNING && e->status != MGR_EMU_READY) return;
    e->status = st;
    va_list ap; va_start(ap, fmt);
    vsnprintf(e->message, sizeof e->message, fmt, ap);
    va_end(ap);
    elog(e, "[stop] %s (thread %d)\n", e->message, e->cur);
    if (st == MGR_EMU_EXITED) return;
    watch_dump(e);
    for (int r = 0; r < 31; r += 4) {
        char line[160]; int o = 0;
        for (int k = r; k < r + 4 && k < 31; k++) o += snprintf(line + o, sizeof line - o, " x%-2d=%016llx", k, (unsigned long long)e->x[k]);
        elog(e, " %s\n", line);
    }
    elog(e, "  sp=%016llx tpidrro=%016llx\n", (unsigned long long)e->sp, (unsigned long long)e->tpidrro);
    emu_backtrace(e);
    if (getenv("MGR_FAULT_DUMP")) {                    /* MGR_FAULT_DUMP=<register>: memory around it */
        const char *spec = getenv("MGR_FAULT_DUMP");
        for (;;) {
        int r = atoi(spec);
        uint64_t a = r >= 0 && r < 31 ? e->x[r] : e->sp;
        elog(e, "  memory around x%d:\n", r);
        for (uint64_t p = (a & ~7ull) - 0x400; p < (a & ~7ull) + 0x100; p += 0x20) {
            char line[200]; int o = snprintf(line, sizeof line, "  %010llx:", (unsigned long long)p);
            for (int k = 0; k < 4; k++) {
                uint64_t pg = (p + k * 8) & ~0xFFFull; int ok = 0;
                for (int i = 0; i < e->nreg; i++) if (pg >= e->reg[i].base && pg - e->reg[i].base < e->reg[i].size) ok = 1;
                uint64_t v = 0; if (ok) { uint8_t *m = e->reg[0].mem; (void)m; memcpy(&v, mem_ptr(e, p + k * 8, 8), 8); }
                o += snprintf(line + o, sizeof line - o, " %016llx", (unsigned long long)v);
            }
            elog(e, "%s\n", line);
        }
        spec = strchr(spec, ','); if (!spec) break; spec++;
        }
    }
}

void emu_backtrace(mgr_emu *e) { emu_backtrace_from(e, e->pc, e->x[30], e->x[29]); }

void emu_backtrace_from(mgr_emu *e, uint64_t pc, uint64_t lr0, uint64_t fp) {
    /* backtrace via the frame-pointer chain, with module-relative addresses */
    uint64_t pcs[17];
    int n = 0;
    pcs[n++] = pc;
    pcs[n++] = lr0;
    for (int k = 0; k < 15 && fp; k++) {
        region *r = find_region(e, fp);
        if (!r || fp + 16 > r->base + r->size) break;
        uint64_t next = mrd(e, fp, 8), lr = mrd(e, fp + 8, 8);
        if (!lr) break;
        pcs[n++] = lr;
        if (next <= fp) break;
        fp = next;
    }
    for (int k = 0; k < n; k++) {
        region *r = find_region(e, pcs[k]);
        if (r) elog(e, "  #%d %016llx  %s+0x%llx\n", k, (unsigned long long)pcs[k], r->name,
                    (unsigned long long)(pcs[k] - r->base));
        else elog(e, "  #%d %016llx\n", k, (unsigned long long)pcs[k]);
    }
}

/* Guest memory is reserved with mmap: pages are zero-filled and only take host RAM once
   touched, so a game asking for a multi-gigabyte heap costs what it actually uses. */
uint8_t *guest_mem_alloc(uint64_t size) {
    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return m == MAP_FAILED ? NULL : m;
}
void guest_mem_free(uint8_t *m, uint64_t size) { if (m && size) munmap(m, size); }
/* Give pages back to the host and make them read as zero (fresh anonymous mapping in place). */
void guest_mem_discard(uint8_t *m, uint64_t size) {
    /* Host pages can be larger than the guest's 4 KiB (16 KiB on Apple Silicon): only whole host
       pages inside the range are handed back; the partial pages at either end are just zeroed,
       so neighbouring guest pages keep their data. */
    if (!m || !size) return;
    static uintptr_t hp;
    if (!hp) { long v = sysconf(_SC_PAGESIZE); hp = v > 0 ? (uintptr_t)v : 4096; }
    uintptr_t a = (uintptr_t)m, end = a + size;
    uintptr_t lo = (a + hp - 1) & ~(hp - 1), hi = end & ~(hp - 1);
    if (lo >= hi) { memset(m, 0, size); return; }
    if (lo > a) memset(m, 0, lo - a);
    if (end > hi) memset((uint8_t *)hi, 0, end - hi);
    mmap((void *)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
}

region *add_region_reserve(mgr_emu *e, uint64_t base, uint64_t size, uint64_t reserve, uint32_t perm, uint32_t state, const char *name) {
    if (e->nreg >= MAX_REGIONS || size == 0) return NULL;
    if (reserve < size) reserve = size;
    uint8_t *m = guest_mem_alloc(reserve);
    if (!m) return NULL;
    region *r = &e->reg[e->nreg++];
    r->base = base; r->size = size; r->mem = m; r->perm = perm; r->state = state; r->map_size = reserve;
    r->hvf_mapped = 0; r->alias_of = -1; r->alias_off = 0;
    mgr_strlcpy(r->name, name, sizeof r->name);
    mem_tlb_flush(e);                             /* a newer region may now cover cached pages */
    if (e->hvf) hvf_map_region(e, r);
    return r;
}

/* A second view of existing memory (svcMapMemory: thread stacks are mapped this way). */
region *add_region_alias(mgr_emu *e, uint64_t base, uint64_t size, region *src, uint64_t off, const char *name) {
    if (e->nreg >= MAX_REGIONS || !size || off + size > src->map_size) return NULL;
    int si = (int)(src - e->reg);
    region *r = &e->reg[e->nreg++];
    memset(r, 0, sizeof *r);
    r->base = base; r->size = size; r->map_size = size; r->mem = src->mem + off;
    r->perm = 3; r->state = 6; r->alias_of = si; r->alias_off = off;
    mgr_strlcpy(r->name, name, sizeof r->name);
    mem_tlb_flush(e);
    if (e->hvf) hvf_map_region(e, r);
    return r;
}

region *add_region(mgr_emu *e, uint64_t base, uint64_t size, uint32_t perm, uint32_t state, const char *name) {
    if (e->nreg >= MAX_REGIONS || size == 0) return NULL;
    uint8_t *m = guest_mem_alloc(size);
    if (!m) return NULL;
    region *r = &e->reg[e->nreg++];
    r->base = base; r->size = size; r->mem = m; r->perm = perm; r->state = state; r->map_size = size;
    r->hvf_mapped = 0; r->alias_of = -1; r->alias_off = 0;
    mgr_strlcpy(r->name, name, sizeof r->name);
    mem_tlb_flush(e);                             /* a newer region may now cover cached pages */
    if (e->hvf) hvf_map_region(e, r);
    return r;
}

void mem_tlb_flush(mgr_emu *e) { memset(e->tlb, 0, sizeof e->tlb); }

uint8_t *mem_ptr_slow(mgr_emu *e, uint64_t addr, uint64_t len) {
    region *r;
    for (int i = e->nreg - 1; i >= 0; i--) {         /* newest first: later mappings cover earlier ones */
        r = &e->reg[i];
        if (addr >= r->base && addr - r->base < r->size && addr - r->base + len <= r->size) {
            e->last = i;
            uint64_t pg = addr & ~0xFFFull;
            if (pg >= r->base && pg + 0x1000 <= r->base + r->size) {
                mem_tlb *t = &e->tlb[(pg >> 12) & (MEM_TLB_SIZE - 1)];
                t->vpage = pg | 1; t->host = r->mem + (pg - r->base);
            }
            return r->mem + (addr - r->base);
        }
    }
    emu_stop(e, MGR_EMU_FAULT, "memory fault at 0x%016llx (pc 0x%016llx)", (unsigned long long)addr, (unsigned long long)e->pc);
    return NULL;
}

uint64_t mrd(mgr_emu *e, uint64_t a, int bytes) {
    uint8_t *p = mem_ptr(e, a, (uint64_t)bytes);
    if (!p) return 0;
    uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; i--) v = (v << 8) | p[i];
    if (e->watch_hi && a < e->watch_hi && a + bytes > e->watch_lo && e->watch_reads < 40) {
        e->watch_reads++;
        elog(e, "[watch] t%d pc %llx reads %d bytes %llx at %llx (host %p)\n", e->cur, (unsigned long long)e->pc,
             bytes, (unsigned long long)v, (unsigned long long)a, (void *)p);
    }
    return v;
}
void watch_dump(mgr_emu *e) {
    if (e->watch_hi) {
        uint64_t n = e->watch_n, first = n > 512 ? n - 512 : 0;
        elog(e, "[watch] last %llu of %llu CPU writes to %llx:\n", (unsigned long long)(n - first), (unsigned long long)n,
             (unsigned long long)e->watch_lo);
        for (uint64_t k = first; k < n; k++) {
            watch_rec *w = &e->watch_ring[k % 512];
            elog(e, "  t%d pc %llx lr %llx value %llx at %llx, instruction %llu\n", w->t, (unsigned long long)w->pc,
                 (unsigned long long)w->lr, (unsigned long long)w->v, (unsigned long long)w->a, (unsigned long long)w->ins);
        }
    }
}

void watch_report(mgr_emu *e, uint64_t addr, uint64_t len, const char *who) {
    if (e->watch_logged >= 400) return;
    e->watch_logged++;
    elog(e, "[watch] t%d pc %llx %s writes %llu bytes at %llx\n", e->cur, (unsigned long long)e->pc, who,
         (unsigned long long)len, (unsigned long long)addr);
}

void mwr(mgr_emu *e, uint64_t a, int bytes, uint64_t v) {
    if (e->watch_hi && a < e->watch_hi && a + bytes > e->watch_lo) {   /* MGR_WATCH=addr: keep the last 64 writes */
        watch_rec *w = &e->watch_ring[e->watch_n++ % 512]; w->a = a;
        w->pc = e->pc; w->lr = e->x[30]; w->v = v; w->t = e->cur; w->ins = e->instructions;
        if (e->watch_logged < 400) {
            e->watch_logged++;
            elog(e, "[watch] t%d pc %llx lr %llx writes %d bytes %llx at %llx\n", e->cur, (unsigned long long)e->pc,
                 (unsigned long long)e->x[30], bytes, (unsigned long long)v, (unsigned long long)a);
        }
    }
    uint8_t *p = mem_ptr(e, a, (uint64_t)bytes);
    if (!p) return;
    for (int i = 0; i < bytes; i++) { p[i] = (uint8_t)v; v >>= 8; }
    if (e->watch_hi && a < e->watch_hi && a + bytes > e->watch_lo) {
        e->watch_host = mem_ptr(e, e->watch_lo, 8);
        if (e->watch_host) memcpy(&e->watch_last, e->watch_host, 8);
    }
}

/* ------------------------------------------------------------------ */
static inline uint64_t X(mgr_emu *e, int r) { return r == 31 ? 0 : e->x[r]; }
static inline uint64_t XSP(mgr_emu *e, int r) { return r == 31 ? e->sp : e->x[r]; }
static inline void SETX(mgr_emu *e, int r, uint64_t v) { if (r != 31) e->x[r] = v; }
static inline void SETXSP(mgr_emu *e, int r, uint64_t v) { if (r == 31) e->sp = v; else e->x[r] = v; }
static inline uint64_t W(uint64_t v, int sf) { return sf ? v : (v & 0xFFFFFFFFull); }
static inline uint64_t ones(int n) { return n >= 64 ? ~0ull : ((1ull << n) - 1); }
static inline int64_t sext(uint64_t v, int bits) { uint64_t m = 1ull << (bits - 1); v &= ones(bits); return (int64_t)((v ^ m) - m); }

static int cond_holds(mgr_emu *e, int cond) {
    int r;
    switch (cond >> 1) {
    case 0: r = e->z; break;
    case 1: r = e->c; break;
    case 2: r = e->n; break;
    case 3: r = e->v; break;
    case 4: r = e->c && !e->z; break;
    case 5: r = e->n == e->v; break;
    case 6: r = !e->z && e->n == e->v; break;
    default: r = 1;
    }
    if ((cond & 1) && cond != 15) r = !r;
    return r;
}

static uint64_t add_c(mgr_emu *e, uint64_t a, uint64_t b, int carry, int sf, int setf) {
    uint64_t r;
    if (sf) {
        r = a + b + (uint64_t)carry;
        if (setf) {
            e->n = (int)(r >> 63); e->z = r == 0;
            e->c = (r < a) || (carry && r == a);
            e->v = (int)((~(a ^ b) & (a ^ r)) >> 63);
        }
    } else {
        a &= 0xFFFFFFFF; b &= 0xFFFFFFFF;
        uint64_t full = a + b + (uint64_t)carry;
        r = full & 0xFFFFFFFF;
        if (setf) {
            e->n = (int)(r >> 31) & 1; e->z = r == 0;
            e->c = (int)(full >> 32) & 1;
            e->v = (int)(((~(a ^ b) & (a ^ r)) >> 31) & 1);
        }
    }
    return r;
}

static void set_nz(mgr_emu *e, uint64_t r, int sf) {
    if (sf) { e->n = (int)(r >> 63); e->z = r == 0; }
    else { e->n = (int)(r >> 31) & 1; e->z = (r & 0xFFFFFFFF) == 0; }
    e->c = 0; e->v = 0;
}

static uint64_t shift_reg(uint64_t v, int type, int amt, int sf) {
    int w = sf ? 64 : 32;
    v = W(v, sf);
    amt &= w - 1;
    if (!amt) return v;
    switch (type) {
    case 0: return W(v << amt, sf);
    case 1: return v >> amt;
    case 2: return W((uint64_t)(sext(v, w) >> amt), sf);
    default: return W((v >> amt) | (v << (w - amt)), sf);
    }
}

static uint64_t extend_reg(uint64_t v, int option, int shift) {
    switch (option) {
    case 0: v &= 0xFF; break;
    case 1: v &= 0xFFFF; break;
    case 2: v &= 0xFFFFFFFF; break;
    case 3: break;
    case 4: v = (uint64_t)sext(v, 8); break;
    case 5: v = (uint64_t)sext(v, 16); break;
    case 6: v = (uint64_t)sext(v, 32); break;
    default: break;
    }
    return v << shift;
}

static int decode_bitmask(int N, int imms, int immr, int sf, uint64_t *out) {
    int combined = (N << 6) | (~imms & 0x3F);
    int len = -1;
    for (int i = 6; i >= 0; i--) if (combined & (1 << i)) { len = i; break; }
    if (len < 1) return -1;
    int levels = (1 << len) - 1;
    int s = imms & levels, r = immr & levels;
    if (s == levels) return -1;
    int esize = 1 << len;
    uint64_t welem = ones(s + 1);
    if (r) welem = ((welem >> r) | (welem << (esize - r))) & ones(esize);
    uint64_t res = 0;
    for (int i = 0; i < 64; i += esize) res |= welem << i;
    *out = W(res, sf);
    return 0;
}


/* ------------------------------------------------------------------ */
static void unimpl(mgr_emu *e, uint32_t insn) {
    e->insn_ok = 0;
    emu_stop(e, MGR_EMU_UNIMPLEMENTED, "unimplemented instruction %08x at 0x%016llx", insn, (unsigned long long)e->pc);
}


/* ---- data processing (immediate) ---- */
static void dp_imm(mgr_emu *e, uint32_t i) {
    int sf = (int)(i >> 31), rd = (int)(i & 31), rn = (int)((i >> 5) & 31);
    uint32_t op = (i >> 23) & 7;
    switch (op) {
    case 0: case 1: { /* ADR / ADRP */
        int64_t imm = sext(((i >> 5) & 0x7FFFF) << 2 | ((i >> 29) & 3), 21);
        if (i >> 31) SETX(e, rd, (e->pc & ~0xFFFull) + (uint64_t)(imm << 12));
        else SETX(e, rd, e->pc + (uint64_t)imm);
        return;
    }
    case 2: { /* add/sub immediate */
        int opsub = (i >> 30) & 1, S = (i >> 29) & 1;
        uint64_t imm = (i >> 10) & 0xFFF;
        if ((i >> 22) & 1) imm <<= 12;
        uint64_t a = XSP(e, rn);
        uint64_t r = opsub ? add_c(e, a, W(~imm, sf), 1, sf, S) : add_c(e, a, imm, 0, sf, S);
        if (S) SETX(e, rd, r); else SETXSP(e, rd, r);
        return;
    }
    case 4: { /* logical immediate */
        uint64_t imm;
        if (decode_bitmask((i >> 22) & 1, (i >> 10) & 0x3F, (i >> 16) & 0x3F, sf, &imm)) { unimpl(e, i); return; }
        uint64_t a = X(e, rn), r;
        int opc = (i >> 29) & 3;
        switch (opc) {
        case 0: r = a & imm; break;
        case 1: r = a | imm; break;
        case 2: r = a ^ imm; break;
        default: r = a & imm; set_nz(e, W(r, sf), sf); SETX(e, rd, W(r, sf)); return;
        }
        SETXSP(e, rd, W(r, sf));
        return;
    }
    case 5: { /* move wide */
        int opc = (i >> 29) & 3, hw = (i >> 21) & 3;
        uint64_t imm = (uint64_t)((i >> 5) & 0xFFFF) << (hw * 16);
        if (opc == 0) SETX(e, rd, W(~imm, sf));
        else if (opc == 2) SETX(e, rd, imm);
        else if (opc == 3) SETX(e, rd, W((X(e, rd) & ~(0xFFFFull << (hw * 16))) | imm, sf));
        else unimpl(e, i);
        return;
    }
    case 6: { /* bitfield */
        int opc = (i >> 29) & 3, immr = (i >> 16) & 0x3F, imms = (i >> 10) & 0x3F;
        int w = sf ? 64 : 32;
        uint64_t src = W(X(e, rn), sf), r;
        if (opc == 2) { /* UBFM */
            if (imms >= immr) r = (src >> immr) & ones(imms - immr + 1);
            else r = W((src & ones(imms + 1)) << (w - immr), sf);
        } else if (opc == 0) { /* SBFM */
            if (imms >= immr) r = W((uint64_t)sext(src >> immr, imms - immr + 1), sf);
            else r = W((uint64_t)sext(src & ones(imms + 1), imms + 1) << (w - immr), sf);
        } else if (opc == 1) { /* BFM */
            uint64_t d = W(X(e, rd), sf);
            if (imms >= immr) {
                int width = imms - immr + 1;
                r = (d & ~ones(width)) | ((src >> immr) & ones(width));
            } else {
                int width = imms + 1, pos = w - immr;
                uint64_t m = ones(width) << pos;
                r = W((d & ~m) | ((src & ones(width)) << pos), sf);
            }
        } else { unimpl(e, i); return; }
        SETX(e, rd, r);
        return;
    }
    case 7: { /* EXTR */
        int rm = (i >> 16) & 31, lsb = (i >> 10) & 0x3F, w = sf ? 64 : 32;
        uint64_t hi = W(X(e, rn), sf), lo = W(X(e, rm), sf);
        uint64_t r = lsb ? W((lo >> lsb) | (hi << (w - lsb)), sf) : lo;
        SETX(e, rd, r);
        return;
    }
    default: unimpl(e, i);
    }
}

/* ---- system registers ---- */
static uint64_t *sysreg(mgr_emu *e, uint32_t key, int *ro) {
    *ro = 0;
    switch (key) {
    case 0xDE82: return &e->tpidr;              /* TPIDR_EL0 3,3,13,0,2 */
    case 0xDE83: *ro = 1; return &e->tpidrro;   /* TPIDRRO_EL0 */
    case 0xDA20: return &e->fpcr;               /* FPCR 3,3,4,4,0 */
    case 0xDA21: return &e->fpsr;               /* FPSR */
    default: return NULL;
    }
}

static void branch_sys(mgr_emu *e, uint32_t i, int *branched) {
    /* unconditional immediate */
    if ((i & 0x7C000000) == 0x14000000) {
        int64_t off = sext(i & 0x3FFFFFF, 26) << 2;
        if (i >> 31) e->x[30] = e->pc + 4;
        e->pc += (uint64_t)off; *branched = 1;
        return;
    }
    if ((i & 0x7E000000) == 0x34000000) { /* CBZ/CBNZ */
        int sf = (int)(i >> 31), nz = (i >> 24) & 1;
        uint64_t v = W(X(e, (int)(i & 31)), sf);
        if ((v == 0) != nz) { e->pc += (uint64_t)(sext((i >> 5) & 0x7FFFF, 19) << 2); *branched = 1; }
        return;
    }
    if ((i & 0x7E000000) == 0x36000000) { /* TBZ/TBNZ */
        int bit = (int)(((i >> 31) << 5) | ((i >> 19) & 31)), nz = (i >> 24) & 1;
        int set = (int)((X(e, (int)(i & 31)) >> bit) & 1);
        if (set == nz) { e->pc += (uint64_t)(sext((i >> 5) & 0x3FFF, 14) << 2); *branched = 1; }
        return;
    }
    if ((i & 0xFF000010) == 0x54000000) { /* B.cond */
        if (cond_holds(e, (int)(i & 15))) { e->pc += (uint64_t)(sext((i >> 5) & 0x7FFFF, 19) << 2); *branched = 1; }
        return;
    }
    if ((i & 0xFFE0001F) == 0xD4000001) { do_svc(e, (i >> 5) & 0xFFFF); return; }
    if ((i & 0xFFE0001F) == 0xD4200000) { emu_stop(e, MGR_EMU_BREAK, "BRK #%u at 0x%016llx", (i >> 5) & 0xFFFF, (unsigned long long)e->pc); return; }
    if ((i & 0xFE1FFC1F) == 0xD61F0000 || (i & 0xFE1FF000) == 0xD61F0000) { /* BR/BLR/RET (+PAC variants treated plainly) */
        int opc = (i >> 21) & 0xF, rn = (int)((i >> 5) & 31);
        uint64_t target = X(e, rn);
        if (opc == 0 || opc == 8) { e->pc = target; *branched = 1; return; }               /* BR / BRAA */
        if (opc == 1 || opc == 9) { e->x[30] = e->pc + 4; e->pc = target; *branched = 1; return; } /* BLR */
        if (opc == 2) { int op3 = (i >> 10) & 0x3F; e->pc = (rn == 31 && (op3 == 2 || op3 == 3)) ? e->x[30] : target; *branched = 1; return; } /* RET / RETAA */
        unimpl(e, i); return;
    }
    if ((i & 0xFFC00000) == 0xD5000000) { /* system */
        int L = (i >> 21) & 1, op0 = (i >> 19) & 3, rt = (int)(i & 31);
        uint32_t crn = (i >> 12) & 15, crm = (i >> 8) & 15, op1 = (i >> 16) & 7, op2 = (i >> 5) & 7;
        if (op0 == 0 && crn == 2 && !L) return;           /* hints: NOP, YIELD, PACIASP, BTI, ... */
        if (op0 == 0 && crn == 3 && !L) { e->excl_valid = 0; return; } /* barriers, CLREX */
        if (op0 == 0 && crn == 4 && !L) return;           /* MSR (immediate) PSTATE */
        if (op0 == 1 && !L) {                              /* SYS: DC/IC/AT */
            if (op1 == 3 && crn == 7 && crm == 4 && op2 == 1) { /* DC ZVA */
                uint64_t a = X(e, rt) & ~63ull;
                uint8_t *p = mem_ptr(e, a, 64);
                WATCH(e, a, 64, "DC ZVA");
                if (p) memset(p, 0, 64);
            }
            return;
        }
        if (op0 >= 2) {
            uint32_t key = (op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
            int ro;
            uint64_t *r = sysreg(e, key, &ro);
            if (L) {
                uint64_t v;
                if (r) v = *r;
                else if (key == 0xDF02 || key == 0xDF01) v = e->ticks; /* CNTVCT/CNTPCT_EL0 */
                else if (key == 0xDF00) v = 19200000;                          /* CNTFRQ_EL0 */
                else if (key == 0xD807) v = 4;                                 /* DCZID_EL0: 64-byte ZVA */
                else if (key == 0xD801) v = 0x8444C004;                        /* CTR_EL0 */
                else if (key == 0xDA10) v = ((uint64_t)e->n << 31) | ((uint64_t)e->z << 30) | ((uint64_t)e->c << 29) | ((uint64_t)e->v << 28); /* NZCV */
                else if (key == 0xC000 || key == 0xC005 || key == 0xC006) v = key == 0xC005 ? 0x80000000ull : 0x411FD070ull; /* MIDR/MPIDR */
                else { v = 0; elog(e, "[cpu] MRS of unknown sysreg %04x at %016llx (read as 0)\n", key, (unsigned long long)e->pc); }
                SETX(e, rt, v);
            } else {
                uint64_t v = X(e, rt);
                if (key == 0xDA10) { e->n = (int)(v >> 31) & 1; e->z = (int)(v >> 30) & 1; e->c = (int)(v >> 29) & 1; e->v = (int)(v >> 28) & 1; }
                else if (r && !ro) *r = v;
            }
            return;
        }
        unimpl(e, i);
        return;
    }
    unimpl(e, i);
}

/* ---- loads & stores ---- */
static void ld_st_simd(mgr_emu *e, int vt, uint64_t addr, int bytes, int load) {
    WATCH(e, addr, (uint64_t)bytes, "atomic");
    uint8_t *p = mem_ptr(e, addr, (uint64_t)bytes);
    if (!p) return;
    if (load) { memset(e->q[vt].b, 0, 16); memcpy(e->q[vt].b, p, (size_t)bytes); }
    else memcpy(p, e->q[vt].b, (size_t)bytes);
}

static void ld_st_gp(mgr_emu *e, int rt, uint64_t addr, int size, int opc) {
    int bytes = 1 << size;
    if (opc == 0) { mwr(e, addr, bytes, X(e, rt)); return; }
    if (opc == 1) { SETX(e, rt, mrd(e, addr, bytes)); return; }
    if (size == 3) return;                      /* PRFM */
    if (size == 2 && opc == 3) { unimpl(e, 0); return; }
    uint64_t v = (uint64_t)sext(mrd(e, addr, bytes), bytes * 8);
    SETX(e, rt, opc == 2 ? v : (v & 0xFFFFFFFF));
}

static uint64_t atomic_op(int opc, int bytes, uint64_t old, uint64_t val) {
    int bits = bytes * 8;
    switch (opc) {
    case 0: return old + val;
    case 1: return old & ~val;
    case 2: return old ^ val;
    case 3: return old | val;
    case 4: return sext(old, bits) > sext(val, bits) ? old : val;
    case 5: return sext(old, bits) < sext(val, bits) ? old : val;
    case 6: return (old & ones(bits)) > (val & ones(bits)) ? old : val;
    default: return (old & ones(bits)) < (val & ones(bits)) ? old : val;
    }
}

static int exec_simd_ldst(mgr_emu *e, uint32_t i);
static void loadstore(mgr_emu *e, uint32_t i) {
    int rt = (int)(i & 31), rn = (int)((i >> 5) & 31), V = (i >> 26) & 1;

    if ((i & 0xBE000000) == 0x0C000000) { /* AdvSIMD structure loads/stores (LD1..LD4, ST1..ST4, LDnR) */
        if (!exec_simd_ldst(e, i)) unimpl(e, i);
        return;
    }

    /* exclusives / acquire-release / CAS: size 001000 ... */
    if ((i & 0x3F000000) == 0x08000000) {
        int size = (int)(i >> 30), o2 = (i >> 23) & 1, L = (i >> 22) & 1, o1 = (i >> 21) & 1;
        int rs = (int)((i >> 16) & 31), rt2 = (int)((i >> 10) & 31);
        int bytes = 1 << size;
        uint64_t a = XSP(e, rn);
        if (o2 && o1 && rt2 == 31) { /* CAS */
            uint64_t old = mrd(e, a, bytes);
            if (old == (X(e, rs) & ones(bytes * 8))) mwr(e, a, bytes, X(e, rt));
            SETX(e, rs, old);
            return;
        }
        if (o2) { /* LDAR/STLR (and LDLAR...) */
            if (L) SETX(e, rt, mrd(e, a, bytes)); else mwr(e, a, bytes, X(e, rt));
            return;
        }
        if (!o1) {
            if (L) { SETX(e, rt, mrd(e, a, bytes)); e->excl_addr = a; e->excl_valid = 1; }
            else {
                int ok = e->excl_valid && e->excl_addr == a;
                if (ok) mwr(e, a, bytes, X(e, rt));
                SETX(e, rs, ok ? 0 : 1);
                e->excl_valid = 0;
            }
            return;
        }
        /* LDXP/STXP */
        int pb = size == 3 ? 8 : 4;
        if (L) { SETX(e, rt, mrd(e, a, pb)); SETX(e, rt2, mrd(e, a + (uint64_t)pb, pb)); e->excl_addr = a; e->excl_valid = 1; }
        else {
            int ok = e->excl_valid && e->excl_addr == a;
            if (ok) { mwr(e, a, pb, X(e, rt)); mwr(e, a + (uint64_t)pb, pb, X(e, rt2)); }
            SETX(e, rs, ok ? 0 : 1);
            e->excl_valid = 0;
        }
        return;
    }

    /* LDAPUR / STLUR family (ARMv8.4 RCpc, unscaled offset): size 011001 opc 0 imm9 00 Rn Rt */
    if ((i & 0x3F200C00) == 0x19000000) {
        int size = (int)(i >> 30), opc = (i >> 22) & 3, bytes = 1 << size;
        uint64_t a = XSP(e, rn) + (uint64_t)sext((i >> 12) & 0x1FF, 9);
        if (opc == 0) mwr(e, a, bytes, X(e, rt));
        else if (opc == 1) SETX(e, rt, mrd(e, a, bytes));
        else {
            uint64_t v = (uint64_t)sext(mrd(e, a, bytes), bytes * 8);
            SETX(e, rt, opc == 2 ? v : (v & 0xFFFFFFFF));
        }
        return;
    }

    /* load literal */
    if ((i & 0x3B000000) == 0x18000000) {
        int opc = (int)(i >> 30);
        uint64_t a = e->pc + (uint64_t)(sext((i >> 5) & 0x7FFFF, 19) << 2);
        if (V) { ld_st_simd(e, rt, a, 4 << opc, 1); return; }
        if (opc == 0) SETX(e, rt, mrd(e, a, 4));
        else if (opc == 1) SETX(e, rt, mrd(e, a, 8));
        else if (opc == 2) SETX(e, rt, (uint64_t)sext(mrd(e, a, 4), 32));
        return; /* PRFM literal */
    }

    /* load/store pair */
    if ((i & 0x3A000000) == 0x28000000) {
        int opc = (int)(i >> 30), idx = (i >> 23) & 3, L = (i >> 22) & 1;
        int rt2 = (int)((i >> 10) & 31);
        int scale = V ? 2 + opc : 2 + (opc >> 1);
        int bytes = 1 << scale;
        int64_t off = sext((i >> 15) & 0x7F, 7) * bytes;
        uint64_t base = XSP(e, rn);
        uint64_t a = (idx == 1) ? base : base + (uint64_t)off;
        if (V) { ld_st_simd(e, rt, a, bytes, L); ld_st_simd(e, rt2, a + (uint64_t)bytes, bytes, L); }
        else if (L) {
            uint64_t v1 = mrd(e, a, bytes), v2 = mrd(e, a + (uint64_t)bytes, bytes);
            if (opc == 1) { v1 = (uint64_t)sext(v1, 32); v2 = (uint64_t)sext(v2, 32); }
            SETX(e, rt, v1); SETX(e, rt2, v2);
        } else { mwr(e, a, bytes, X(e, rt)); mwr(e, a + (uint64_t)bytes, bytes, X(e, rt2)); }
        if (idx == 1 || idx == 3) SETXSP(e, rn, base + (uint64_t)off);
        return;
    }

    /* load/store register */
    if ((i & 0x3A000000) == 0x38000000) {
        int size = (int)(i >> 30), opc = (i >> 22) & 3;
        int bytes, simd_load = 0;
        if (V) {
            bytes = (opc & 2) ? 16 : (1 << size);
            simd_load = opc & 1;
            if ((opc & 2) && size != 0) { unimpl(e, i); return; }
        } else bytes = 1 << size;
        int scale = V && (opc & 2) ? 4 : size;
        uint64_t base = XSP(e, rn), a;
        int writeback = 0; uint64_t wbval = 0;
        if ((i >> 24) & 1) { /* unsigned offset */
            a = base + (((i >> 10) & 0xFFF) << scale);
        } else if ((i >> 21) & 1) {
            int kind = (i >> 10) & 3;
            if (kind == 2) { /* register offset */
                int rm = (int)((i >> 16) & 31), option = (i >> 13) & 7, S = (i >> 12) & 1;
                a = base + extend_reg(X(e, rm), option, S ? scale : 0);
            } else if (kind == 0 && !V) { /* LSE atomics */
                int o3 = (i >> 15) & 1, aop = (i >> 12) & 7, rs = (int)((i >> 16) & 31);
                uint64_t addr = base;
                uint64_t old = mrd(e, addr, bytes);
                uint64_t val = X(e, rs) & ones(bytes * 8);
                uint64_t nw = o3 ? val : atomic_op(aop, bytes, old, val);
                if (o3 && aop == 4 && rs == 31) { SETX(e, rt, mrd(e, addr, bytes)); return; } /* LDAPR/LDAPRB/LDAPRH (RCpc) */
                if (o3 && aop != 0) { unimpl(e, i); return; }
                mwr(e, addr, bytes, nw);
                SETX(e, rt, old);
                return;
            } else { unimpl(e, i); return; }
        } else {
            int64_t imm9 = sext((i >> 12) & 0x1FF, 9);
            int kind = (i >> 10) & 3;
            if (kind == 0 || kind == 2) a = base + (uint64_t)imm9;      /* unscaled / unprivileged */
            else if (kind == 1) { a = base; writeback = 1; wbval = base + (uint64_t)imm9; } /* post */
            else { a = base + (uint64_t)imm9; writeback = 1; wbval = a; }                 /* pre */
        }
        if (V) ld_st_simd(e, rt, a, bytes, simd_load);
        else ld_st_gp(e, rt, a, size, opc);
        if (writeback) SETXSP(e, rn, wbval);
        return;
    }
    unimpl(e, i);
}

/* ---- data processing (register) ---- */
static void dp_reg(mgr_emu *e, uint32_t i) {
    int sf = (int)(i >> 31), rd_ = (int)(i & 31), rn = (int)((i >> 5) & 31), rm = (int)((i >> 16) & 31);
    if ((i & 0x1F000000) == 0x0A000000) { /* logical shifted */
        int opc = (i >> 29) & 3, N = (i >> 21) & 1;
        uint64_t b = shift_reg(X(e, rm), (i >> 22) & 3, (i >> 10) & 0x3F, sf);
        if (N) b = W(~b, sf);
        uint64_t a = W(X(e, rn), sf), r;
        switch (opc) {
        case 0: r = a & b; break;
        case 1: r = a | b; break;
        case 2: r = a ^ b; break;
        default: r = a & b; set_nz(e, r, sf); break;
        }
        SETX(e, rd_, W(r, sf));
        return;
    }
    if ((i & 0x1F200000) == 0x0B000000) { /* add/sub shifted */
        int op = (i >> 30) & 1, S = (i >> 29) & 1;
        uint64_t b = shift_reg(X(e, rm), (i >> 22) & 3, (i >> 10) & 0x3F, sf);
        uint64_t a = X(e, rn);
        uint64_t r = op ? add_c(e, a, W(~b, sf), 1, sf, S) : add_c(e, a, b, 0, sf, S);
        SETX(e, rd_, r);
        return;
    }
    if ((i & 0x1F200000) == 0x0B200000) { /* add/sub extended */
        int op = (i >> 30) & 1, S = (i >> 29) & 1;
        uint64_t b = W(extend_reg(X(e, rm), (i >> 13) & 7, (i >> 10) & 7), sf);
        uint64_t a = XSP(e, rn);
        uint64_t r = op ? add_c(e, a, W(~b, sf), 1, sf, S) : add_c(e, a, b, 0, sf, S);
        if (S) SETX(e, rd_, r); else SETXSP(e, rd_, r);
        return;
    }
    if ((i & 0x1FE00000) == 0x1A000000) { /* ADC/SBC */
        int op = (i >> 30) & 1, S = (i >> 29) & 1;
        uint64_t b = X(e, rm);
        if (op) b = W(~b, sf);
        SETX(e, rd_, add_c(e, X(e, rn), W(b, sf), e->c, sf, S));
        return;
    }
    if ((i & 0x1FE00000) == 0x1A400000) { /* CCMP/CCMN */
        int op = (i >> 30) & 1, cond = (i >> 12) & 15, isimm = (i >> 11) & 1;
        if (cond_holds(e, cond)) {
            uint64_t b = isimm ? (uint64_t)rm : X(e, rm);
            if (op) add_c(e, X(e, rn), W(~b, sf), 1, sf, 1);
            else add_c(e, X(e, rn), b, 0, sf, 1);
        } else {
            int nzcv = (int)(i & 15);
            e->n = (nzcv >> 3) & 1; e->z = (nzcv >> 2) & 1; e->c = (nzcv >> 1) & 1; e->v = nzcv & 1;
        }
        return;
    }
    if ((i & 0x1FE00000) == 0x1A800000) { /* conditional select */
        int op = (i >> 30) & 1, op2 = (i >> 10) & 3, cond = (i >> 12) & 15;
        uint64_t r;
        if (cond_holds(e, cond)) r = X(e, rn);
        else {
            r = X(e, rm);
            if (!op && op2 == 1) r = r + 1;
            else if (op && op2 == 0) r = ~r;
            else if (op && op2 == 1) r = (uint64_t)(-(int64_t)r);
        }
        SETX(e, rd_, W(r, sf));
        return;
    }
    if ((i & 0x5FE00000) == 0x1AC00000) { /* 2-source */
        int opc = (i >> 10) & 0x3F;
        uint64_t a = W(X(e, rn), sf), b = W(X(e, rm), sf), r;
        switch (opc) {
        case 2: r = b ? a / b : 0; break;
        case 3:
            if (!b) r = 0;
            else if (sf) r = (sext(a, 64) == INT64_MIN && sext(b, 64) == -1) ? a : (uint64_t)(sext(a, 64) / sext(b, 64));
            else r = (sext(a, 32) == INT32_MIN && sext(b, 32) == -1) ? a : (uint64_t)(sext(a, 32) / sext(b, 32));
            break;
        case 8: case 9: case 10: case 11: r = shift_reg(a, opc - 8, (int)(b & (sf ? 63 : 31)), sf); break;
        case 12: {   /* PACGA Xd, Xn, Xm|SP: generic authentication code in bits 63:32. Keys are not
                        modelled, so any deterministic mix works: code compares two PACGA results. */
            uint64_t m = rm == 31 ? e->sp : X(e, rm), h = X(e, rn) * 0x9E3779B97F4A7C15ull ^ (m + 0x632BE59BD9B4E019ull);
            h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
            SETX(e, rd_, h << 32);
            return;
        }
        case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: { /* CRC32{B,H,W,X}, CRC32C{B,H,W,X} */
            uint32_t poly = (opc & 4) ? 0x82F63B78u : 0xEDB88320u;
            int bytes = 1 << (opc & 3);
            uint32_t crc = (uint32_t)X(e, rn);
            uint64_t data = X(e, rm);
            for (int k = 0; k < bytes; k++) {
                crc ^= (uint32_t)((data >> (8 * k)) & 0xFF);
                for (int t = 0; t < 8; t++) crc = (crc >> 1) ^ (poly & (0u - (crc & 1)));
            }
            SETX(e, rd_, crc);
            return;
        }
        default: unimpl(e, i); return;
        }
        SETX(e, rd_, W(r, sf));
        return;
    }
    if ((i & 0xFFFF0000) == 0xDAC10000) { /* pointer authentication: PAC*, AUT*, XPAC* (opcode2 = 00001).
                                             Pointers are never signed here, so these leave values unchanged. */
        return;
    }
    if ((i & 0x5FE00000) == 0x5AC00000) { /* 1-source */
        int opc = (i >> 10) & 0x3F;
        uint64_t a = W(X(e, rn), sf), r = 0;
        int w = sf ? 64 : 32;
        switch (opc) {
        case 0: for (int k = 0; k < w; k++) if (a >> k & 1) r |= 1ull << (w - 1 - k); break;
        case 1: for (int k = 0; k < w; k += 16) r |= ((a >> k & 0xFF) << (k + 8)) | ((a >> (k + 8) & 0xFF) << k); break;
        case 2:
            if (!sf) { r = __builtin_bswap32((uint32_t)a); }
            else { r = ((uint64_t)__builtin_bswap32((uint32_t)a)) | ((uint64_t)__builtin_bswap32((uint32_t)(a >> 32)) << 32); }
            break;
        case 3: r = __builtin_bswap64(a); break;
        case 4: r = a ? (uint64_t)(sf ? __builtin_clzll(a) : __builtin_clz((uint32_t)a)) : (uint64_t)w; break;
        case 5: { uint64_t s = (a >> (w - 1)) & 1; int cnt = 0; for (int k = w - 2; k >= 0 && ((a >> k) & 1) == s; k--) cnt++; r = (uint64_t)cnt; break; }
        default: unimpl(e, i); return;
        }
        SETX(e, rd_, W(r, sf));
        return;
    }
    if ((i & 0x1F000000) == 0x1B000000) { /* 3-source */
        int op31 = (i >> 21) & 7, o0 = (i >> 15) & 1, ra = (int)((i >> 10) & 31);
        uint64_t a = X(e, rn), b = X(e, rm), acc = X(e, ra), r;
        switch (op31) {
        case 0: r = o0 ? acc - a * b : acc + a * b; r = W(r, sf); break;
        case 1: { int64_t p = sext(a, 32) * sext(b, 32); r = o0 ? acc - (uint64_t)p : acc + (uint64_t)p; break; }
        case 5: { uint64_t p = (a & 0xFFFFFFFF) * (b & 0xFFFFFFFF); r = o0 ? acc - p : acc + p; break; }
        case 2: r = (uint64_t)(((__int128)(int64_t)a * (__int128)(int64_t)b) >> 64); break;
        case 6: r = (uint64_t)(((unsigned __int128)a * (unsigned __int128)b) >> 64); break;
        default: unimpl(e, i); return;
        }
        SETX(e, rd_, r);
        return;
    }
    unimpl(e, i);
}

#include "cpu_simd.h"

/* ---- SIMD & FP: register moves, then cpu_simd.h ---- */
static uint64_t simd_expand_imm(int op, int cmode, uint64_t imm8) {
    uint64_t r = 0;
    switch (cmode >> 1) {
    case 0: r = imm8; r |= r << 32; break;
    case 1: r = imm8 << 8; r |= r << 32; break;
    case 2: r = imm8 << 16; r |= r << 32; break;
    case 3: r = imm8 << 24; r |= r << 32; break;
    case 4: r = imm8; r |= r << 16; r |= r << 32; break;
    case 5: r = imm8 << 8; r |= r << 16; r |= r << 32; break;
    case 6:
        r = (cmode & 1) ? ((imm8 << 16) | 0xFFFF) : ((imm8 << 8) | 0xFF);
        r |= r << 32; break;
    default:
        if (!(cmode & 1) && !op) { r = imm8; r |= r << 8; r |= r << 16; r |= r << 32; }
        else if (!(cmode & 1) && op) { for (int k = 0; k < 8; k++) if (imm8 >> k & 1) r |= 0xFFull << (k * 8); }
        else r = 0; /* FMOV vector immediate: not supported here */
    }
    return r;
}

static void simd_fp(mgr_emu *e, uint32_t i) {
    int rd_ = (int)(i & 31), rn = (int)((i >> 5) & 31), rm = (int)((i >> 16) & 31), Q = (i >> 30) & 1;
    /* AdvSIMD modified immediate: MOVI/MVNI/ORR/BIC */
    if ((i & 0x9FF80400) == 0x0F000400) {
        int op = (i >> 29) & 1, cmode = (i >> 12) & 15;
        uint64_t imm8 = ((i >> 11) & 0xE0) | ((i >> 5) & 0x1F);
        if (cmode == 15) { if (!exec_simd(e, i)) unimpl(e, i); return; }
        uint64_t imm = simd_expand_imm(op, cmode, imm8);
        uint64_t lo, hi;
        memcpy(&lo, e->q[rd_].b, 8); memcpy(&hi, e->q[rd_].b + 8, 8);
        int is_orr_bic = (cmode < 12 && (cmode & 1));
        if (!is_orr_bic) {
            uint64_t v = (op && cmode != 14) ? ~imm : imm;
            lo = v; hi = Q ? v : 0;
        } else if (!op) { lo |= imm; if (Q) hi |= imm; else hi = 0; }
        else { lo &= ~imm; if (Q) hi &= ~imm; else hi = 0; }
        memcpy(e->q[rd_].b, &lo, 8); memcpy(e->q[rd_].b + 8, &hi, 8);
        return;
    }
    /* AdvSIMD three-same logical: AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF */
    if ((i & 0x9F20FC00) == 0x0E201C00) {
        int U = (i >> 29) & 1, size = (i >> 22) & 3;
        uint8_t r[16];
        const uint8_t *a = e->q[rn].b, *b = e->q[rm].b, *d = e->q[rd_].b;
        for (int k = 0; k < 16; k++) {
            uint8_t x;
            if (!U) x = size == 0 ? a[k] & b[k] : size == 1 ? a[k] & ~b[k] : size == 2 ? a[k] | b[k] : a[k] | ~b[k];
            else x = size == 0 ? a[k] ^ b[k]
                   : size == 1 ? (uint8_t)((d[k] & a[k]) | (~d[k] & b[k]))
                   : size == 2 ? (uint8_t)((a[k] & b[k]) | (d[k] & ~b[k]))
                   : (uint8_t)((a[k] & ~b[k]) | (d[k] & b[k]));
            r[k] = x;
        }
        if (!Q) memset(r + 8, 0, 8);
        memcpy(e->q[rd_].b, r, 16);
        return;
    }
    /* DUP (general), INS (general), UMOV/SMOV */
    if ((i & 0x9FE08400) == 0x0E000400) {
        int imm5 = (i >> 16) & 31, op = (i >> 29) & 1, imm4 = (i >> 11) & 15;
        int sz = __builtin_ctz((unsigned)imm5 | 0x20);
        if (sz > 3) { unimpl(e, i); return; }
        int esz = 1 << sz, idx = imm5 >> (sz + 1);
        if (!op && imm4 == 1) {         /* DUP Vd.T, Rn */
            uint64_t v = X(e, rn);
            uint8_t r[16] = {0};
            for (int k = 0; k < (Q ? 16 : 8); k += esz) memcpy(r + k, &v, (size_t)esz);
            memcpy(e->q[rd_].b, r, 16);
            return;
        }
        if (op == 0 && Q && imm4 == 3) { /* INS Vd.T[idx], Rn */
            uint64_t v = X(e, rn);
            memcpy(e->q[rd_].b + idx * esz, &v, (size_t)esz);
            return;
        }
        if (!op && (imm4 == 7 || imm4 == 5)) { /* UMOV / SMOV */
            uint64_t v = 0;
            memcpy(&v, e->q[rn].b + idx * esz, (size_t)esz);
            if (imm4 == 5) { v = (uint64_t)sext(v, esz * 8); if (!Q) v &= 0xFFFFFFFF; }
            SETX(e, rd_, v);
            return;
        }
        if (op && Q) {                   /* INS Vd.T[i1], Vn.T[i2] */
            int idx2 = imm4 >> sz;
            memcpy(e->q[rd_].b + idx * esz, e->q[rn].b + idx2 * esz, (size_t)esz);
            return;
        }
        if (!exec_simd(e, i)) unimpl(e, i);
        return;
    }
    /* FMOV between general and FP registers */
    if ((i & 0x7F20FC00) == 0x1E200000) {
        int sf = (int)(i >> 31), type = (i >> 22) & 3, rmode = (i >> 19) & 3, opcode = (i >> 16) & 7;
        if (rmode == 0 && opcode == 6 && ((!sf && type == 0) || (sf && type == 1))) { /* FMOV Rd, Vn */
            uint64_t v = 0; memcpy(&v, e->q[rn].b, sf ? 8 : 4); SETX(e, rd_, v); return;
        }
        if (rmode == 0 && opcode == 7 && ((!sf && type == 0) || (sf && type == 1))) { /* FMOV Vd, Rn */
            uint64_t v = X(e, rn); memset(e->q[rd_].b, 0, 16); memcpy(e->q[rd_].b, &v, sf ? 8 : 4); return;
        }
        if (sf && type == 2 && rmode == 1 && opcode == 6) { uint64_t v; memcpy(&v, e->q[rn].b + 8, 8); SETX(e, rd_, v); return; }
        if (sf && type == 2 && rmode == 1 && opcode == 7) { uint64_t v = X(e, rn); memcpy(e->q[rd_].b + 8, &v, 8); return; }
    }
    if (exec_fp_scalar(e, i) || exec_simd(e, i)) return;
    unimpl(e, i);
}

/* ------------------------------------------------------------------ */
void cpu_step(mgr_emu *e) {
    if (e->break_pc && e->pc == e->break_pc && e->instructions >= e->break_after) {
        emu_stop(e, MGR_EMU_BREAK, "breakpoint at %016llx (thread %d)", (unsigned long long)e->pc, e->cur);
        return;
    }
    uint8_t *p = mem_ptr(e, e->pc, 4);
    if (!p) return;
    uint32_t i = rd32(p);
    int branched = 0;
    e->insn_ok = 1;
    uint32_t op0 = (i >> 25) & 0xF;
    if ((op0 & 0xE) == 0x8) dp_imm(e, i);
    else if ((op0 & 0xE) == 0xA) branch_sys(e, i, &branched);
    else if ((op0 & 0x5) == 0x4) loadstore(e, i);
    else if ((op0 & 0x7) == 0x5) dp_reg(e, i);
    else if ((op0 & 0x7) == 0x7) simd_fp(e, i);
    else unimpl(e, i);
    if (e->status != MGR_EMU_RUNNING) return;
    e->instructions++;
    e->ticks++;
    if (!branched) e->pc += 4;
}

region *find_region(mgr_emu *e, uint64_t addr) {
    for (int i = e->nreg - 1; i >= 0; i--)
        if (addr >= e->reg[i].base && addr - e->reg[i].base < e->reg[i].size) return &e->reg[i];
    return NULL;
}
