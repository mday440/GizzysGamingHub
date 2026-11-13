/*
 * MikesGameRoom - native CPU backend (Apple Silicon, Hypervisor.framework).
 *
 * Switch and Switch 2 games are AArch64 programs, and so is an Apple Silicon Mac, so instead of
 * interpreting every instruction the game's code runs directly on the Mac's CPU inside a small
 * virtual machine:
 *
 *   - Guest memory is the emulator's own region memory: every region is mapped into the VM's
 *     physical (IPA) space with hv_vm_map, and guest virtual addresses are mapped onto it by
 *     stage-1 page tables we build ourselves (4 KiB granule, 39-bit addresses, like Horizon).
 *     Host pages are 16 KiB, but the VM's own page tables can use the Switch's 4 KiB pages.
 *   - The game runs at EL0. Our EL1 "kernel" is just an exception vector table whose every
 *     entry executes HVC: any exception (SVC, a trapped WFE, an undefined instruction, a fault)
 *     comes back to the host, which reads ESR/ELR/FAR and handles it exactly as the interpreter
 *     would (do_svc, scheduling, ...), then resumes the thread at EL0.
 *   - Preemption: a watchdog thread kicks the vCPU out every millisecond so guest threads are
 *     time-sliced like on the interpreter.
 *   - Anything the Mac's CPU refuses (an instruction or system register it doesn't have) is run
 *     by the interpreter for that one instruction.
 *
 * Set MGR_CPU=interp to use the interpreter instead (needed for MGR_WATCH / MGR_BREAK).
 */
#include "emu_internal.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <Hypervisor/Hypervisor.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PAGE4K      0x1000ull
#define BLOCK2M     0x200000ull
#define HOSTPAGE    0x4000ull                 /* 16 KiB: stage-2 mappings must use host pages */
#define PT_POOL     (256ull << 20)            /* page-table memory */
#define VEC_VA      0x1000ull                 /* where the EL1 vectors live in guest VA */
#define IPA_PT      0x0ull                    /* page tables at the bottom of the IPA space */
#define IPA_VEC     (PT_POOL)                 /* vectors page right after them */
#define IPA_FIRST   (PT_POOL + BLOCK2M)       /* regions after that */

/* Descriptor bits (stage 1, 4 KiB granule). */
#define D_VALID     1ull
#define D_TABLE     2ull                      /* table (L0-L2) / page (L3) */
#define D_AF        (1ull << 10)
#define D_SH_INNER  (3ull << 8)
#define D_AP_EL0    (1ull << 6)               /* EL0 accessible */
#define D_AP_RO     (1ull << 7)               /* read-only */
#define D_PXN       (1ull << 53)
#define D_UXN       (1ull << 54)
#define D_ADDR      0x0000FFFFFFFFF000ull

typedef struct {
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *exit;
    uint8_t *pt;                    /* page-table pool (host) */
    uint64_t pt_used;
    uint8_t *vec;                   /* vectors page (host, 16 KiB) */
    uint64_t next_ipa;
    uint32_t ipa_bits;
    int full_dirty;                 /* SIMD/FP/TLS registers must be pushed before the next run */
    int clrex_pending;              /* another thread ran: clear the exclusive monitor first */
    int vcpu_used;
    pthread_t watchdog;
    atomic_int quit;
    uint64_t t0_ns;
    uint64_t runs, exits_svc, exits_kick, exits_wfe, exits_interp;
} hvf_state;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* Emulated system tick (19.2 MHz) from the host clock. */
static uint64_t hvf_ticks(hvf_state *h) { return (uint64_t)((now_ns() - h->t0_ns) * 0.0192); }

/* ---------------- stage-1 page tables ---------------- */

static uint64_t pt_alloc(hvf_state *h) {
    if (h->pt_used + PAGE4K > PT_POOL) return 0;
    uint64_t off = h->pt_used;
    h->pt_used += PAGE4K;
    memset(h->pt + off, 0, PAGE4K);
    return off;                                   /* also the IPA (pool is at IPA 0) */
}

static uint64_t *pt_entry(hvf_state *h, uint64_t table_ipa, unsigned idx) {
    return (uint64_t *)(h->pt + table_ipa) + idx;
}

/* Walk to the table at `level` (2 or 3) covering va, creating tables as needed. */
static uint64_t pt_table_for(hvf_state *h, uint64_t va, int level) {
    uint64_t table = 0;                           /* L1 table is the first page of the pool */
    for (int l = 1; l < level; l++) {
        unsigned idx = (unsigned)((va >> (39 - 9 * l)) & 511);
        uint64_t *d = pt_entry(h, table, idx);
        if ((*d & 3) != 3) {                      /* missing, or a block that must be split */
            uint64_t old = *d;
            uint64_t n = pt_alloc(h);
            if (!n) return UINT64_MAX;
            if ((old & 3) == 1 && l == 2) {       /* 2 MiB block -> 512 pages with the same attributes */
                uint64_t base = old & 0x0000FFFFFFE00000ull, attrs = old & ~0x0000FFFFFFFFF003ull;
                for (unsigned k = 0; k < 512; k++)
                    *pt_entry(h, n, k) = (base + k * PAGE4K) | attrs | D_VALID | D_TABLE;
            }
            d = pt_entry(h, table, idx);
            *d = n | D_VALID | D_TABLE;
        }
        table = *d & D_ADDR;
    }
    return table;
}

static uint64_t attrs_for(uint32_t perm, int el1_only) {
    uint64_t a = D_AF | D_SH_INNER | D_PXN;       /* normal memory (MAIR index 0), never EL1-executable */
    if (el1_only) return D_AF | D_SH_INNER | D_UXN | D_AP_RO;
    a |= D_AP_EL0;
    if (!(perm & 2)) a |= D_AP_RO;
    if (!(perm & 4)) a |= D_UXN;
    return a;
}

/* Map [va, va+size) to [ipa, ...) with the given attributes, using 2 MiB blocks where possible. */
static int pt_map(hvf_state *h, uint64_t va, uint64_t ipa, uint64_t size, uint64_t attrs) {
    uint64_t end = va + size;
    while (va < end) {
        if (!(va & (BLOCK2M - 1)) && !(ipa & (BLOCK2M - 1)) && end - va >= BLOCK2M) {
            uint64_t t = pt_table_for(h, va, 2);
            if (t == UINT64_MAX) return -1;
            *pt_entry(h, t, (unsigned)((va >> 21) & 511)) = (ipa & D_ADDR) | attrs | D_VALID;   /* block */
            va += BLOCK2M; ipa += BLOCK2M;
            continue;
        }
        uint64_t t = pt_table_for(h, va, 3);
        if (t == UINT64_MAX) return -1;
        *pt_entry(h, t, (unsigned)((va >> 12) & 511)) = (ipa & D_ADDR) | attrs | D_VALID | D_TABLE;  /* page */
        va += PAGE4K; ipa += PAGE4K;
    }
    return 0;
}

/* ---------------- regions ---------------- */

/* Map one emulator region into the VM: host memory -> IPA (stage 2) and guest VA -> IPA. */
static void flush_tlb(hvf_state *h);

/* MGR_WATCH in native mode: make the watched 4 KiB page read-only so each write to it traps;
   the interpreter then runs that one store (which logs it) and the game carries on natively. */
static void watch_protect(mgr_emu *e, hvf_state *h, region *r) {
    if (!e->watch_hi || r->alias_of >= 0) return;
    uint64_t pg = e->watch_lo & ~(uint64_t)(PAGE4K - 1);
    if (pg < r->base || pg >= r->base + r->map_size) return;
    if (!(r->perm & 2) && r->perm) return;
    pt_map(h, pg, r->hvf_ipa + (pg - r->base), PAGE4K, attrs_for((r->perm ? r->perm : 3) & ~2u, 0));
    elog(e, "[hvf] watching %llx natively (page %llx is write-protected)\n", (unsigned long long)e->watch_lo, (unsigned long long)pg);
}

void hvf_map_region(mgr_emu *e, region *r) {
    hvf_state *h = e->hvf;
    if (!h || r->hvf_mapped) return;
    if (r->alias_of >= 0) {                       /* a second view of mapped memory: stage 1 only */
        region *src = &e->reg[r->alias_of];
        if (!src->hvf_mapped) hvf_map_region(e, src);
        if (pt_map(h, r->base, src->hvf_ipa + r->alias_off, (r->size + PAGE4K - 1) & ~(PAGE4K - 1), attrs_for(3, 0)) != 0)
            elog(e, "[hvf] page-table pool exhausted mapping %s\n", r->name);
        r->hvf_ipa = src->hvf_ipa + r->alias_off;
        r->hvf_mapped = 1;
        flush_tlb(h);                             /* it may replace earlier translations */
        return;
    }
    uint64_t host_size = (r->map_size + HOSTPAGE - 1) & ~(HOSTPAGE - 1);
    uint64_t ipa;
    if (!(r->base & (HOSTPAGE - 1)))              /* keep IPA = VA (mod 2 MiB) so big regions get 2 MiB blocks */
        ipa = ((h->next_ipa + BLOCK2M - 1) & ~(BLOCK2M - 1)) + (r->base & (BLOCK2M - 1));
    else
        ipa = (h->next_ipa + HOSTPAGE - 1) & ~(HOSTPAGE - 1);
    if (ipa + host_size > (1ull << h->ipa_bits)) {
        elog(e, "[hvf] out of VM address space mapping %s (%llx bytes)\n", r->name, (unsigned long long)host_size);
        return;
    }
    hv_return_t rc = hv_vm_map(r->mem, ipa, host_size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
    if (rc != HV_SUCCESS) {
        elog(e, "[hvf] hv_vm_map(%s) failed: %d\n", r->name, rc);
        return;
    }
    /* stage 1: the guest may use the whole reservation (e.g. the heap can grow into it) */
    if (pt_map(h, r->base, ipa, (r->map_size + PAGE4K - 1) & ~(PAGE4K - 1), attrs_for(r->perm ? r->perm : 3, 0)) != 0)
        elog(e, "[hvf] page-table pool exhausted mapping %s\n", r->name);
    r->hvf_ipa = ipa;
    r->hvf_mapped = 1;
    h->next_ipa = ipa + host_size;
    watch_protect(e, h, r);
    flush_tlb(h);
}

/* Run the TLB-invalidate stub in the VM (needed when a mapping changes after the game ran). */
static void clear_exclusive(hvf_state *h) {
    h->clrex_pending = 0;
    if (!h->vcpu_used) return;
    hv_vcpu_set_reg(h->vcpu, HV_REG_PC, VEC_VA + 0x880);
    hv_vcpu_set_reg(h->vcpu, HV_REG_CPSR, 0x3C5);   /* EL1h, interrupts masked */
    for (int k = 0; k < 8; k++) {
        if (hv_vcpu_run(h->vcpu) != HV_SUCCESS) return;
        if (h->exit->reason == HV_EXIT_REASON_EXCEPTION && (h->exit->exception.syndrome >> 26) == 0x16) return;
    }
}
static void flush_tlb(hvf_state *h) {
    if (!h->vcpu_used) return;                    /* nothing cached yet */
    hv_vcpu_set_reg(h->vcpu, HV_REG_PC, VEC_VA + 0x800);
    hv_vcpu_set_reg(h->vcpu, HV_REG_CPSR, 0x3C5);   /* EL1h, interrupts masked */
    for (int k = 0; k < 8; k++) {
        if (hv_vcpu_run(h->vcpu) != HV_SUCCESS) return;
        if (h->exit->reason == HV_EXIT_REASON_EXCEPTION && (h->exit->exception.syndrome >> 26) == 0x16) return;
        /* a watchdog kick: just run on */
    }
}

/* ---------------- registers ---------------- */

static void push_gprs(mgr_emu *e, hvf_state *h) {
    for (int i = 0; i < 31; i++) hv_vcpu_set_reg(h->vcpu, (hv_reg_t)(HV_REG_X0 + i), e->x[i]);
    hv_vcpu_set_reg(h->vcpu, HV_REG_PC, e->pc);
    uint64_t cpsr = ((uint64_t)e->n << 31) | ((uint64_t)e->z << 30) | ((uint64_t)e->c << 29) | ((uint64_t)e->v << 28);
    hv_vcpu_set_reg(h->vcpu, HV_REG_CPSR, cpsr);                 /* EL0t */
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_SP_EL0, e->sp);
}

static void push_full(mgr_emu *e, hvf_state *h) {
    for (int i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t v;
        memcpy(&v, e->q[i].b, 16);
        hv_vcpu_set_simd_fp_reg(h->vcpu, (hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0 + i), v);
    }
    hv_vcpu_set_reg(h->vcpu, HV_REG_FPCR, e->fpcr);
    hv_vcpu_set_reg(h->vcpu, HV_REG_FPSR, e->fpsr);
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_TPIDR_EL0, e->tpidr);
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_TPIDRRO_EL0, e->tpidrro);
    h->full_dirty = 0;
}

/* After an exception: the thread's EL0 state (pc = ELR_EL1, flags from SPSR_EL1). */
static void pull_gprs(mgr_emu *e, hvf_state *h, uint64_t *esr, uint64_t *far) {
    uint64_t v;
    for (int i = 0; i < 31; i++) { hv_vcpu_get_reg(h->vcpu, (hv_reg_t)(HV_REG_X0 + i), &v); e->x[i] = v; }
    hv_vcpu_get_sys_reg(h->vcpu, HV_SYS_REG_SP_EL0, &v); e->sp = v;
    hv_vcpu_get_sys_reg(h->vcpu, HV_SYS_REG_ELR_EL1, &v); e->pc = v;
    hv_vcpu_get_sys_reg(h->vcpu, HV_SYS_REG_SPSR_EL1, &v);
    e->n = (v >> 31) & 1; e->z = (v >> 30) & 1; e->c = (v >> 29) & 1; e->v = (v >> 28) & 1;
    if (esr) hv_vcpu_get_sys_reg(h->vcpu, HV_SYS_REG_ESR_EL1, esr);
    if (far) hv_vcpu_get_sys_reg(h->vcpu, HV_SYS_REG_FAR_EL1, far);
}

/* A kick (watchdog) stops the vCPU wherever it is, still at EL0: read PC/CPSR directly. */
static void pull_gprs_direct(mgr_emu *e, hvf_state *h) {
    uint64_t v;
    for (int i = 0; i < 31; i++) { hv_vcpu_get_reg(h->vcpu, (hv_reg_t)(HV_REG_X0 + i), &v); e->x[i] = v; }
    hv_vcpu_get_sys_reg(h->vcpu, HV_SYS_REG_SP_EL0, &v); e->sp = v;
    hv_vcpu_get_reg(h->vcpu, HV_REG_PC, &v); e->pc = v;
    hv_vcpu_get_reg(h->vcpu, HV_REG_CPSR, &v);
    e->n = (v >> 31) & 1; e->z = (v >> 30) & 1; e->c = (v >> 29) & 1; e->v = (v >> 28) & 1;
}

/* SIMD/FP and the thread pointer: needed when the thread is switched out or interpreted. */
void hvf_pull_full(mgr_emu *e) {
    hvf_state *h = e->hvf;
    if (!h || h->full_dirty) return;              /* the vCPU doesn't hold anything newer */
    uint64_t v;
    for (int i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t q;
        hv_vcpu_get_simd_fp_reg(h->vcpu, (hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0 + i), &q);
        memcpy(e->q[i].b, &q, 16);
    }
    hv_vcpu_get_reg(h->vcpu, HV_REG_FPCR, &v); e->fpcr = v;
    hv_vcpu_get_reg(h->vcpu, HV_REG_FPSR, &v); e->fpsr = v;
    hv_vcpu_get_sys_reg(h->vcpu, HV_SYS_REG_TPIDR_EL0, &v); e->tpidr = v;
}

/* The emulator changed the live thread (context switch, interpreter step): push it all next run. */
void hvf_mark_dirty(mgr_emu *e) { if (e->hvf) { ((hvf_state *)e->hvf)->full_dirty = 1; ((hvf_state *)e->hvf)->clrex_pending = 1; } }

/* ---------------- setup ---------------- */

static void *watchdog_main(void *arg) {
    hvf_state *h = arg;
    while (!atomic_load(&h->quit)) {
        usleep(1000);                             /* 1 ms time slices */
        hv_vcpus_exit(&h->vcpu, 1);
    }
    return NULL;
}

int hvf_available(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *m = getenv("MGR_CPU");
        cached = !(m && !strcmp(m, "interp"));
    }
    return cached;
}

/* Create the VM and vCPU; returns 0 on success (the caller falls back to the interpreter otherwise).
   Must run on the thread that will call mgr_emu_run (a vCPU belongs to its creating thread). */
int hvf_init(mgr_emu *e) {
    if (!hvf_available()) return -1;
    hvf_state *h = calloc(1, sizeof *h);
    if (!h) return -1;
    hv_return_t rc;
    uint32_t bits = 36;
    if (__builtin_available(macOS 13.0, *)) {
        uint32_t maxb = 0;
        hv_vm_config_t cfg = hv_vm_config_create();
        if (hv_vm_config_get_max_ipa_size(&maxb) == HV_SUCCESS && maxb) bits = maxb > 40 ? 40 : maxb;
        hv_vm_config_set_ipa_size(cfg, bits);
        rc = hv_vm_create(cfg);
    } else {
        rc = hv_vm_create(NULL);
    }
    if (rc != HV_SUCCESS) {
        elog(e, "[hvf] Hypervisor.framework unavailable (hv_vm_create: %d) - is the app signed with the "
                "com.apple.security.hypervisor entitlement? Using the interpreter.\n", rc);
        free(h);
        return -1;
    }
    h->ipa_bits = bits;
    h->pt = guest_mem_alloc(PT_POOL);
    h->vec = guest_mem_alloc(HOSTPAGE);
    if (!h->pt || !h->vec) { hv_vm_destroy(); free(h); return -1; }
    h->pt_used = 0;
    pt_alloc(h);                                  /* L1 table at IPA 0 */
    hv_vm_map(h->pt, IPA_PT, PT_POOL, HV_MEMORY_READ | HV_MEMORY_WRITE);
    /* EL1 vectors: 16 entries of 0x80 bytes; each does "hvc #entry" and loops. */
    for (uint32_t k = 0; k < 16; k++) {
        uint32_t *p = (uint32_t *)(h->vec + k * 0x80);
        p[0] = 0xD4000002u | (k << 5);            /* hvc #k */
        p[1] = 0x14000000u;                       /* b . */
    }
    {   /* at +0x880: clear the exclusive monitor (a kernel does this on every thread switch:
           otherwise a thread preempted between LDXR and STXR can still succeed after another
           thread changed the value, losing that update), then report back with hvc #0x21 */
        uint32_t *p = (uint32_t *)(h->vec + 0x880);
        p[0] = 0xD503305Fu;                       /* clrex */
        p[1] = 0xD4000422u;                       /* hvc #0x21 */
        p[2] = 0x14000000u;                       /* b . */
    }
    {   /* at +0x800: invalidate this VM's TLB, then report back with hvc #0x20 */
        uint32_t *p = (uint32_t *)(h->vec + 0x800);
        p[0] = 0xD508871Fu;                       /* tlbi vmalle1 */
        p[1] = 0xD5033F9Fu;                       /* dsb sy */
        p[2] = 0xD5033FDFu;                       /* isb */
        p[3] = 0xD4000402u;                       /* hvc #0x20 */
        p[4] = 0x14000000u;                       /* b . */
    }
    hv_vm_map(h->vec, IPA_VEC, HOSTPAGE, HV_MEMORY_READ | HV_MEMORY_EXEC);
    pt_map(h, VEC_VA, IPA_VEC, PAGE4K, D_AF | D_SH_INNER | D_UXN | D_AP_RO);   /* EL1 read + execute only */
    h->next_ipa = IPA_FIRST;

    rc = hv_vcpu_create(&h->vcpu, &h->exit, NULL);
    if (rc != HV_SUCCESS) { elog(e, "[hvf] hv_vcpu_create failed: %d\n", rc); hv_vm_destroy(); free(h); return -1; }
    hv_vcpu_set_trap_debug_exceptions(h->vcpu, false);

    /* EL1 environment for an EL0 program. */
    uint64_t sctlr = (1ull << 0)  /* M: MMU on */ | (1ull << 2) /* C */ | (1ull << 12) /* I */
                   | (1ull << 14) /* DZE: EL0 DC ZVA */ | (1ull << 15) /* UCT: EL0 CTR_EL0 */
                   | (1ull << 16) /* nTWI */ | (1ull << 26) /* UCI: EL0 cache maintenance */
                   | (1ull << 11) | (1ull << 20) | (1ull << 22) | (1ull << 23) | (1ull << 28) | (1ull << 29); /* RES1 */
    /* nTWE (bit 18) clear: a spinning thread's WFE comes back to us and we run another thread */
    uint64_t ips = bits > 36 ? 2 : 1;             /* 40-bit / 36-bit physical */
    uint64_t tcr = 25ull                          /* T0SZ: 39-bit VA */
                 | (1ull << 8) | (1ull << 10) | (3ull << 12)   /* IRGN0/ORGN0 WB, SH0 inner */
                 | (0ull << 14)                   /* TG0: 4 KiB */
                 | (1ull << 23)                   /* EPD1: no TTBR1 walks */
                 | (ips << 32);
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_MAIR_EL1, 0xFFull);          /* attr 0: normal WB */
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_TCR_EL1, tcr);
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_TTBR0_EL1, IPA_PT);
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_TTBR1_EL1, 0);
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_VBAR_EL1, VEC_VA);
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_CPACR_EL1, 3ull << 20);      /* FP/SIMD at EL0 and EL1 */
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_CNTKCTL_EL1, 3);             /* EL0 may read the counters */
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_SP_EL1, 0);
    hv_vcpu_set_sys_reg(h->vcpu, HV_SYS_REG_SCTLR_EL1, sctlr);

    e->hvf = h;
    h->t0_ns = now_ns();
    for (int i = 0; i < e->nreg; i++) hvf_map_region(e, &e->reg[i]);
    h->full_dirty = 1;
    atomic_store(&h->quit, 0);
    pthread_create(&h->watchdog, NULL, watchdog_main, h);
    elog(e, "[hvf] native CPU: game code runs on the Mac's CPU (Hypervisor.framework, %u-bit VM)\n", bits);
    return 0;
}

void hvf_destroy(mgr_emu *e) {
    hvf_state *h = e->hvf;
    if (!h) return;
    atomic_store(&h->quit, 1);
    pthread_join(h->watchdog, NULL);
    elog(e, "[hvf] %llu runs: %llu system calls, %llu preemptions, %llu WFE yields, %llu interpreted instructions\n",
         (unsigned long long)h->runs, (unsigned long long)h->exits_svc, (unsigned long long)h->exits_kick,
         (unsigned long long)h->exits_wfe, (unsigned long long)h->exits_interp);
    hv_vcpu_destroy(h->vcpu);
    hv_vm_destroy();
    guest_mem_free(h->pt, PT_POOL);
    guest_mem_free(h->vec, HOSTPAGE);
    free(h);
    e->hvf = NULL;
}

/* ---------------- running ---------------- */

uint64_t hvf_now_ticks(mgr_emu *e) { return e->hvf ? hvf_ticks(e->hvf) : e->ticks; }

/* The thread took an exception into our EL1 vectors (entry `vector`): handle it like the
   interpreter would. ESR_EL1/ELR_EL1/FAR_EL1 describe the EL0 event. */
static void handle_guest_exception(mgr_emu *e, hvf_state *h, uint32_t vector) {
    uint64_t esr = 0, far = 0;
    pull_gprs(e, h, &esr, &far);
    if (vector != 8) {                            /* only "synchronous, from AArch64 EL0" is expected */
        emu_stop(e, MGR_EMU_FAULT, "unexpected exception vector %u (ESR %llx, ELR %llx)", vector,
                 (unsigned long long)esr, (unsigned long long)e->pc);
        return;
    }
    uint32_t ec = (uint32_t)(esr >> 26);
    switch (ec) {
    case 0x15:                                    /* SVC: ELR is already past the instruction */
        h->exits_svc++;
        do_svc(e, (uint32_t)(esr & 0xFFFF));
        if (e->th[e->cur].state != TH_READY) e->quantum_left = 0;
        return;
    case 0x01:                                    /* WFI/WFE: a spin-wait. Let another thread run. */
        h->exits_wfe++;
        e->pc += 4;
        e->quantum_left = 0;
        return;
    case 0x00: case 0x18: case 0x07:              /* undefined / trapped system register / FP trap */
        h->exits_interp++;
        hvf_pull_full(e);
        cpu_step(e);                              /* the interpreter runs this one instruction */
        h->full_dirty = 1;
        return;
    case 0x24:
        if (e->watch_hi && (esr & 0x3C) == 0x0C && (esr & 0x40) &&
            (far & ~(uint64_t)(PAGE4K - 1)) == (e->watch_lo & ~(uint64_t)(PAGE4K - 1))) {
            h->exits_interp++;                    /* a write to the watched page: let the interpreter do it */
            hvf_pull_full(e);
            cpu_step(e);
            h->full_dirty = 1;
            return;
        }
        /* fall through */
    case 0x20:                                    /* data / instruction abort */
        emu_stop(e, MGR_EMU_FAULT, "%s at 0x%016llx (pc 0x%016llx)", ec == 0x24 ? "memory fault" : "instruction fetch fault",
                 (unsigned long long)far, (unsigned long long)e->pc);
        return;
    case 0x3C:                                    /* BRK */
        emu_stop(e, MGR_EMU_BREAK, "guest BRK #%u at %016llx", (unsigned)(esr & 0xFFFF), (unsigned long long)e->pc);
        return;
    default:
        emu_stop(e, MGR_EMU_FAULT, "unhandled exception class 0x%02x (ESR %llx) at pc %016llx address %llx", ec,
                 (unsigned long long)esr, (unsigned long long)e->pc, (unsigned long long)far);
        return;
    }
}

/* Run the current thread until something needs the emulator. Returns an estimate of the
   instructions executed (for the run budget): ~1 per 19.2 MHz tick. */
uint64_t hvf_run_slice(mgr_emu *e) {
    hvf_state *h = e->hvf;
    if (h->clrex_pending) {
        static int off = -1; if (off < 0) off = getenv("MGR_NO_CLREX") != NULL;
        if (!off) clear_exclusive(h); else h->clrex_pending = 0;
    }
    if (h->full_dirty) push_full(e, h);
    push_gprs(e, h);
    uint64_t t_before = hvf_ticks(h);
    hv_return_t rc = hv_vcpu_run(h->vcpu);
    h->runs++;
    h->vcpu_used = 1;
    uint64_t t_after = hvf_ticks(h);
    e->ticks = t_after;
    uint64_t spent = t_after > t_before ? (t_after - t_before) * 50 : 1;   /* ~1 GHz-ish instructions */
    if (rc != HV_SUCCESS) { emu_stop(e, MGR_EMU_FAULT, "hv_vcpu_run failed: %d", rc); return spent; }

    switch (h->exit->reason) {
    case HV_EXIT_REASON_CANCELED: {               /* watchdog: time slice over */
        uint64_t cpsr = 0, pc = 0;
        hv_vcpu_get_reg(h->vcpu, HV_REG_CPSR, &cpsr);
        h->exits_kick++;
        e->quantum_left = 0;
        if (cpsr & 0xF) {                         /* stopped in our vectors, before their HVC: same as the HVC */
            hv_vcpu_get_reg(h->vcpu, HV_REG_PC, &pc);
            handle_guest_exception(e, h, (uint32_t)((pc - VEC_VA) >> 7));
            return spent;
        }
        pull_gprs_direct(e, h);
        return spent;
    }
    case HV_EXIT_REASON_VTIMER_ACTIVATED:
        pull_gprs_direct(e, h);
        hv_vcpu_set_vtimer_mask(h->vcpu, false);
        return spent;
    case HV_EXIT_REASON_EXCEPTION:
        break;
    default:
        emu_stop(e, MGR_EMU_FAULT, "unexpected VM exit (reason %d)", (int)h->exit->reason);
        return spent;
    }

    uint64_t el2_esr = h->exit->exception.syndrome;
    uint32_t el2_ec = (uint32_t)(el2_esr >> 26);
    if (el2_ec == 0x01 || el2_ec == 0x18) {       /* trapped by the hypervisor itself, still at EL0 */
        pull_gprs_direct(e, h);
        if (el2_ec == 0x01) {                     /* WFI/WFE: a spin-wait, run another thread */
            h->exits_wfe++;
            e->pc += 4;
            e->quantum_left = 0;
        } else {                                  /* system register (e.g. the physical counter): interpret */
            h->exits_interp++;
            hvf_pull_full(e);
            cpu_step(e);
            h->full_dirty = 1;
        }
        return spent;
    }
    if (el2_ec != 0x16) {                         /* not our HVC: an EL1 problem (should not happen) */
        uint64_t pc; hv_vcpu_get_reg(h->vcpu, HV_REG_PC, &pc);
        emu_stop(e, MGR_EMU_FAULT, "VM exception at EL1 (ESR_EL2 %llx, pc %llx, address %llx)",
                 (unsigned long long)el2_esr, (unsigned long long)pc, (unsigned long long)h->exit->exception.virtual_address);
        return spent;
    }
    handle_guest_exception(e, h, (uint32_t)(el2_esr & 0xFFFF));
    return spent;
}

/* Nothing runnable until `tick`: wait in real time (the guest reads the real counter). */
void hvf_sleep_until(mgr_emu *e, uint64_t tick) {
    hvf_state *h = e->hvf;
    uint64_t now = hvf_ticks(h);
    if (tick > now) {
        uint64_t ns = (uint64_t)((tick - now) / 0.0192);
        if (ns > 100000000ull) ns = 100000000ull;     /* at most 100 ms per wait */
        struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
        nanosleep(&ts, NULL);
    }
    e->ticks = hvf_ticks(h);
}

#else  /* not Apple Silicon: interpreter only */

int hvf_available(void) { return 0; }
int hvf_init(mgr_emu *e) { (void)e; return -1; }
void hvf_destroy(mgr_emu *e) { (void)e; }
void hvf_map_region(mgr_emu *e, region *r) { (void)e; (void)r; }
void hvf_pull_full(mgr_emu *e) { (void)e; }
void hvf_mark_dirty(mgr_emu *e) { (void)e; }
uint64_t hvf_run_slice(mgr_emu *e) { (void)e; return 0; }
void hvf_sleep_until(mgr_emu *e, uint64_t tick) { e->ticks = tick; }
uint64_t hvf_now_ticks(mgr_emu *e) { return e->ticks; }

#endif
