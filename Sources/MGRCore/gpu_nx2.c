/*
 * MikesGameRoom - Switch 2 GPU front-end (command processor).
 *
 * This is the first stage of the Ampere GPU emulation: it keeps the GPU address
 * space (what the graphics server maps through nx2:gpu), walks GPFIFO entries the
 * driver submits, decodes the pushbuffer method stream and runs the parts that the
 * CPU side observes directly:
 *   - host-class semaphores (acquire / release / reduction), which NVN uses to gate
 *     command buffers and to report completion,
 *   - 3D / compute report semaphores,
 *   - the copy engine's linear copies and semaphore releases,
 *   - inline-to-memory uploads.
 * Draws, shaders and textures are not executed yet; methods of those engines are
 * stored and counted so the work that remains is visible in the log.
 *
 * Method numbers follow NVIDIA's published class headers (open-gpu-doc: clc56f.h,
 * clc797.h, clc7c0.h, clc7b5.h). Independent implementation.
 */
#include "emu_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

/* ---------------- GPU address space ---------------- */

void nx2_gpu_map(mgr_emu *e, uint64_t gva, uint64_t cpu, uint64_t size) {
    if (!size) return;
    nx2_gpu_unmap(e, gva, size);
    if (e->nx2_nmaps >= NX2_MAX_MAPS) { elog(e, "[gpu] map table full\n"); return; }
    nx2_map *m = &e->nx2_maps[e->nx2_nmaps++];
    m->gva = gva; m->cpu = cpu; m->size = size;
}

void nx2_gpu_unmap(mgr_emu *e, uint64_t gva, uint64_t size) {
    uint64_t end = gva + size;
    for (int i = 0; i < e->nx2_nmaps; i++) {
        nx2_map *m = &e->nx2_maps[i];
        uint64_t mend = m->gva + m->size;
        if (mend <= gva || m->gva >= end) continue;
        if (m->gva >= gva && mend <= end) {                     /* fully covered: drop */
            e->nx2_maps[i--] = e->nx2_maps[--e->nx2_nmaps];
            continue;
        }
        if (m->gva < gva && mend > end) {                       /* hole in the middle: split */
            nx2_map tail = { end, m->cpu + (end - m->gva), mend - end };
            m->size = gva - m->gva;
            if (e->nx2_nmaps < NX2_MAX_MAPS) e->nx2_maps[e->nx2_nmaps++] = tail;
            continue;
        }
        if (m->gva < gva) m->size = gva - m->gva;               /* trim the end */
        else { uint64_t cut = end - m->gva; m->gva += cut; m->cpu += cut; m->size -= cut; }  /* trim start */
    }
}

/* GPU virtual address -> CPU (guest) address, 0 if unmapped. */
uint64_t nx2_gpu_xlate(mgr_emu *e, uint64_t gva) {
    int c = e->nx2_map_last;                                   /* pushbuffers read word by word: cache the hit */
    if (c < e->nx2_nmaps && gva - e->nx2_maps[c].gva < e->nx2_maps[c].size) return e->nx2_maps[c].cpu + (gva - e->nx2_maps[c].gva);
    for (int i = 0; i < e->nx2_nmaps; i++) {
        nx2_map *m = &e->nx2_maps[i];
        if (gva - m->gva < m->size) { e->nx2_map_last = i; return m->cpu + (gva - m->gva); }
    }
    return 0;
}

/* fault summary: (operation, page) -> count */
static struct { const char *op; uint64_t page; uint64_t n; } g_faults[48];
static void fault_note(const char *op, uint64_t gva) {
    for (int k = 0; k < 48; k++) {
        if (!g_faults[k].op) { g_faults[k].op = strdup(op); g_faults[k].page = gva >> 12; g_faults[k].n = 1; return; }
        if (g_faults[k].page == gva >> 12 && !strcmp(g_faults[k].op, op)) { g_faults[k].n++; return; }
    }
}
/* GPU memory with no CPU mapping (GPU-only and sparse ranges, which NVN uses for query reports):
   host pages created on first touch, so what the GPU writes there it can read back. */
#define GPU_ONLY_PAGES (1u << 20)
static struct { uint64_t page; uint8_t *mem; } g_gonly[GPU_ONLY_PAGES];
static uint64_t g_gonly_n;
static uint8_t *gpu_only_page(uint64_t gva, int create) {
    uint64_t pg = gva >> 12;
    uint32_t h = (uint32_t)(pg * 2654435761u) & (GPU_ONLY_PAGES - 1);
    for (uint32_t k = 0; k < 64; k++) {
        uint32_t i = (h + k) & (GPU_ONLY_PAGES - 1);
        if (g_gonly[i].mem && g_gonly[i].page == pg) return g_gonly[i].mem;
        if (!g_gonly[i].mem) {
            if (!create || g_gonly_n >= GPU_ONLY_PAGES / 2) return NULL;
            g_gonly[i].mem = calloc(1, 0x1000); g_gonly[i].page = pg; g_gonly_n++;
            return g_gonly[i].mem;
        }
    }
    return NULL;
}
static int gpu_only_ok(mgr_emu *e, uint64_t gva) {
    return gva >= 0x100000000ull && !(e->nx2_as_end && gva >= e->nx2_as_end - 0x1000 && gva < e->nx2_as_end);
}
static int gread(mgr_emu *e, uint64_t gva, void *dst, uint64_t n) {
    uint8_t *d = dst;
    while (n) {
        uint64_t cpu = nx2_gpu_xlate(e, gva);
        uint64_t chunk = 0x1000 - (gva & 0xFFF);
        if (chunk > n) chunk = n;
        uint8_t *p = cpu ? mem_ptr(e, cpu, chunk) : NULL;
        if (!p && !cpu && e->nx2_as_end && gva >= e->nx2_as_end - 0x1000 && gva < e->nx2_as_end) {
            /* syncpoint shim page: one 32-bit value per syncpoint, as far ahead as the CPU view */
            for (uint64_t k = 0; k < chunk; k++) {
                uint32_t id = (uint32_t)(((gva + k) & 0xFFF) >> 2), v = nx2_syncpt_value(e, id);
                d[k] = (uint8_t)(v >> (8 * ((gva + k) & 3)));
            }
            d += chunk; gva += chunk; n -= chunk; continue;
        }
        if (!p && !cpu && gpu_only_ok(e, gva)) { uint8_t *g = gpu_only_page(gva, 0); if (g) p = g + (gva & 0xFFF); else { memset(d, 0, chunk); d += chunk; gva += chunk; n -= chunk; continue; } }
        if (!p) { memset(d, 0, chunk); fault_note("read", gva); if (e->nx2_gpu_faults++ < 24) elog(e, "[gpu] fault: read of unmapped GPU address %llx\n", (unsigned long long)gva); }
        else memcpy(d, p, chunk);
        d += chunk; gva += chunk; n -= chunk;
    }
    return 0;
}

/* SET_RENDER_ENABLE_A/B/C (0x1550/0x1554/0x1558): draws and clears can depend on a report in memory
   (Zelda's renderer clears a copy of the pre-pass depth under a condition the console skips; drawn
   anyway, the opaque pass's EQUAL depth test fails everywhere). Modes: 0 never, 1 always,
   2 if the 64-bit value is non-zero, 3 if the two reports 16 bytes apart are equal, 4 if they differ. */
static uint64_t g_soft_cond_skipped;
static int nx2_render_enabled(mgr_emu *e, nx2_engine *en) {
    uint32_t mode = en->regs[0x1558 / 4] & 7;
    uint64_t a = ((uint64_t)(en->regs[0x1550 / 4] & 0xFF) << 32) | en->regs[0x1554 / 4];
    if (mode == 0 && !a) return 1;                         /* never set: rendering on */
    switch (mode) {
    case 0: return 0;
    case 1: return 1;
    case 2: { uint64_t v = 0; gread(e, a, &v, 8); return v != 0; }
    case 3: case 4: { uint64_t x = 0, y = 0; gread(e, a, &x, 8); gread(e, a + 16, &y, 8); return mode == 3 ? x == y : x != y; }
    default: return 1;
    }
}

static const char *g_op = "?";
static int g_in_macro = -1;           /* macro index being run, -1 outside macros */   /* which GPU operation is writing (for fault logs) */
/* MGR_SEMTRACE=<gpu address>: count GPU writes to one address by operation */
static uint64_t g_trace = 1;
static const char *g_trace_op[16]; static uint64_t g_trace_n[16];
static uint64_t g_qmd_addr;
/* MGR_PAGETRACE=<gpu page>: log GPU writes into one page and host acquires on it */
static uint64_t g_ptrace = 1; static int g_ptrace_n;
static int ptrace_hit(uint64_t a) {
    if (g_ptrace == 1) { const char *t = getenv("MGR_PAGETRACE"); g_ptrace = t ? strtoull(t, NULL, 16) >> 12 : 0; }
    return g_ptrace && (a >> 12) == g_ptrace && (a & 0xFFF) >= 4 && (a & 0xFFF) < 0x300 && g_ptrace_n < 6000;
}
static void gwrite(mgr_emu *e, uint64_t gva, const void *src, uint64_t n) {
    const uint8_t *s = src;
    if (ptrace_hit(gva)) { g_ptrace_n++; elog(e, "[ptrace] write %llx n %llu = %08x by %s, QMD %llx\n", (unsigned long long)gva, (unsigned long long)n,
                                              n >= 4 ? rd32(s) : 0, g_op, (unsigned long long)g_qmd_addr); }
    {   /* MGR_RANGETRACE=addr,len: count GPU writes into a range by operation */
        static uint64_t lo = 1, hi; static const char *ops[16]; static uint64_t cnt[16];
        if (lo == 1) { const char *t = getenv("MGR_RANGETRACE"); lo = 0; if (t) { char *x; lo = strtoull(t, &x, 16); hi = lo + (*x == ',' ? strtoull(x + 1, NULL, 0) : 0x1000); } }
        if (lo && gva < hi && gva + n > lo) {
            int k = 0; while (k < 15 && ops[k] && strcmp(ops[k], g_op)) k++;
            if (!ops[k]) { ops[k] = strdup(g_op); elog(e, "[range] first write into %llx.. by %s at %llx\n", (unsigned long long)lo, g_op, (unsigned long long)gva); }
            if (!(++cnt[k] & (cnt[k] - 1))) elog(e, "[range] %llu writes by %s\n", (unsigned long long)cnt[k], ops[k]);
        }
    }
    if (g_trace == 1) { const char *t = getenv("MGR_SEMTRACE"); g_trace = t ? strtoull(t, NULL, 16) : 0; }
    if (g_trace && gva <= g_trace && g_trace < gva + n) {
        int k = 0;
        while (k < 15 && g_trace_op[k] && strcmp(g_trace_op[k], g_op)) k++;
        if (!g_trace_op[k]) g_trace_op[k] = strdup(g_op);
        if (g_trace_n[k]++ < 6) elog(e, "[trace] %s writes %llx (n %llu) = %08x, QMD %llx\n", g_op, (unsigned long long)g_trace,
                                     (unsigned long long)n, n >= 4 ? rd32(s + (g_trace - gva)) : 0, (unsigned long long)g_qmd_addr);
        if (g_qmd_addr && g_trace_n[k] <= 6) {
            uint32_t q[64]; gread(e, g_qmd_addr, q, sizeof q);
            elog(e, "[trace]   QMD words 24-31: %08x %08x %08x %08x %08x %08x %08x %08x; 52-55: %08x %08x %08x %08x\n",
                 q[24], q[25], q[26], q[27], q[28], q[29], q[30], q[31], q[52], q[53], q[54], q[55]);
        }
    }
    while (n) {
        uint64_t cpu = nx2_gpu_xlate(e, gva);
        uint64_t chunk = 0x1000 - (gva & 0xFFF);
        if (chunk > n) chunk = n;
        uint8_t *p = cpu ? mem_ptr(e, cpu, chunk) : NULL;
        if (cpu) WATCH(e, cpu, chunk, g_op);
        if (!p && !cpu && gpu_only_ok(e, gva)) { uint8_t *g = gpu_only_page(gva, 1); if (g) p = g + (gva & 0xFFF); }
        if (p) memcpy(p, s, chunk);
        else if (fault_note(g_op, gva), e->nx2_gpu_faults++ < 24) elog(e, "[gpu] fault: %s write to unmapped GPU address %llx\n", g_op, (unsigned long long)gva);
        s += chunk; gva += chunk; n -= chunk;
    }
}

static uint32_t gread32(mgr_emu *e, uint64_t gva) { uint8_t b[4]; gread(e, gva, b, 4); return rd32(b); }
static uint64_t gread64(mgr_emu *e, uint64_t gva) { uint8_t b[8]; gread(e, gva, b, 8); return rd64(b); }
static void gwrite32(mgr_emu *e, uint64_t gva, uint32_t v) { uint8_t b[4]; wr32(b, v); gwrite(e, gva, b, 4); }
static void gwrite64(mgr_emu *e, uint64_t gva, uint64_t v) { uint8_t b[8]; wr64(b, v); gwrite(e, gva, b, 8); }

static int gpu_log_on(void) { static int v = -1; if (v < 0) v = getenv("MGR_GPU_LOG") != NULL; return v; }
#define GLOG(...) do { if (gpu_log_on()) elog(e, __VA_ARGS__); } while (0)

static uint64_t gpu_timestamp(mgr_emu *e) { return e->ticks * 625 / 12; }   /* 19.2 MHz ticks -> ns */

/* ---------------- classes ---------------- */

enum { CLS_HOST = 0, CLS_3D, CLS_COMPUTE, CLS_COPY, CLS_I2M, CLS_2D, CLS_OTHER };

static int class_kind(uint32_t cls) {
    switch (cls & 0xFF) {
    case 0x97: return CLS_3D;
    case 0xC0: return CLS_COMPUTE;
    case 0xB5: return CLS_COPY;
    case 0x40: return CLS_I2M;
    case 0x2D: return CLS_2D;
    case 0x6F: return CLS_HOST;
    default:   return CLS_OTHER;
    }
}

/* ---------------- semaphores ---------------- */

static uint32_t reduce32(uint32_t op, uint32_t old, uint32_t v, int is_signed) {
    switch (op) {
    case 0: return is_signed ? (uint32_t)((int32_t)old < (int32_t)v ? (int32_t)v : (int32_t)old) : (old < v ? v : old); /* MAX */
    case 1: return is_signed ? (uint32_t)((int32_t)old > (int32_t)v ? (int32_t)v : (int32_t)old) : (old > v ? v : old); /* MIN */
    case 2: return old ^ v;
    case 3: return old & v;
    case 4: return old | v;
    case 5: return old + v;
    case 6: return old >= v ? 0 : old + 1;   /* INC (wrap) */
    case 7: return (old == 0 || old > v) ? v : old - 1;   /* DEC (wrap) */
    default: return v;
    }
}

/* Returns 1 if an acquire is satisfied. */
static int sem_acquire_ok(mgr_emu *e, uint64_t addr, uint32_t op, uint64_t payload, int is64) {
    uint64_t cur = is64 ? gread64(e, addr) : gread32(e, addr);
    if (!is64) payload = (uint32_t)payload;
    {   /* MGR_ACQTRACE=lo-hi: log acquires on a GPU address range (first time per address and value change) */
        static int init; static uint64_t lo, hi; static uint64_t last_a[32], last_c[32]; static int nlog;
        if (!init) { init = 1; const char *t = getenv("MGR_ACQTRACE"); if (t) { lo = strtoull(t, (char **)&t, 16); if (*t == '-') hi = strtoull(t + 1, NULL, 16); } }
        if (addr >= lo && addr < hi && nlog < 400) {
            uint32_t k = (uint32_t)((addr - lo) / 4) & 31;
            if (last_a[k] != addr + 1 || last_c[k] != cur) {
                last_a[k] = addr + 1; last_c[k] = cur; nlog++;
                elog(e, "[acq] %llx op %u payload %llx cur %llx%s (%s)\n", (unsigned long long)addr, op, (unsigned long long)payload,
                     (unsigned long long)cur, is64 ? " 64-bit" : "", g_op);
            }
        }
    }
    switch (op) {
    case 0:                                                          /* equal */
        /* Channels here run one after another, so a counter another channel bumps can already be
           past the value an "equal" acquire waits for: treat having passed it as reached. */
        return is64 ? (int64_t)(cur - payload) >= 0 : (int32_t)((uint32_t)cur - (uint32_t)payload) >= 0;
    case 2: return cur >= payload;                                   /* strict >= */
    case 3: return is64 ? (int64_t)(cur - payload) >= 0 : (int32_t)((uint32_t)cur - (uint32_t)payload) >= 0; /* circular >= */
    case 4: return (cur & payload) != 0;                             /* AND */
    case 5: return (cur & payload) == 0;                             /* NOR */
    default: return 1;
    }
}

/* Host class semaphore methods. Returns 0 to stall the channel (unmet acquire). */
static int host_method(mgr_emu *e, nx2_channel *ch, uint32_t m, uint32_t v) {
    switch (m) {
    case 0x10 / 4: ch->sem_addr = (ch->sem_addr & 0xFFFFFFFFull) | ((uint64_t)(v & 0xFF) << 32); return 1; /* SEMAPHOREA */
    case 0x14 / 4: ch->sem_addr = (ch->sem_addr & ~0xFFFFFFFFull) | (v & ~3u); return 1;                 /* SEMAPHOREB */
    case 0x18 / 4: ch->sem_payload = v; return 1;                                                         /* SEMAPHOREC */
    case 0x1c / 4: {                                                                                      /* SEMAPHORED */
        uint32_t op = v & 0x1F;
        if (ptrace_hit(ch->sem_addr)) {
            static uint64_t la; static uint32_t lp;
            if (la != ch->sem_addr || lp != ch->sem_payload) {
                la = ch->sem_addr; lp = ch->sem_payload; g_ptrace_n++;
                elog(e, "[ptrace] ch %u SEMAPHORED %x at %llx payload %x (now %x)\n", ch->sp, v, (unsigned long long)ch->sem_addr, ch->sem_payload, gread32(e, ch->sem_addr));
            }
        }
        if (op == 1 || op == 4 || op == 8) {
            uint32_t cur = gread32(e, ch->sem_addr);
            int ok = op == 1 ? (int32_t)(cur - ch->sem_payload) >= 0 : op == 4 ? (int32_t)(cur - ch->sem_payload) >= 0 : (cur & ch->sem_payload) != 0;
            return ok;
        }
        if (op == 2) {
            g_op = "host SEMAPHORED";
            GLOG("[gpu] host SEMAPHORED release %llx = %x\n", (unsigned long long)ch->sem_addr, ch->sem_payload);
            if (v & (1u << 24)) gwrite32(e, ch->sem_addr, ch->sem_payload);
            else { gwrite32(e, ch->sem_addr, ch->sem_payload); gwrite32(e, ch->sem_addr + 4, 0); gwrite64(e, ch->sem_addr + 8, gpu_timestamp(e)); }
            e->nx2_sem_releases++;
        } else if (op == 16) {
            uint32_t old = gread32(e, ch->sem_addr);
            gwrite32(e, ch->sem_addr, reduce32((v >> 27) & 0xF, old, ch->sem_payload, !(v >> 31)));
            e->nx2_sem_releases++;
        }
        return 1;
    }
    case 0x5c / 4: ch->sem_addr = (ch->sem_addr & ~0xFFFFFFFFull) | (v & ~3u); return 1;                 /* SEM_ADDR_LO */
    case 0x60 / 4: ch->sem_addr = (ch->sem_addr & 0xFFFFFFFFull) | ((uint64_t)(v & 0x1FFFF) << 32); return 1; /* SEM_ADDR_HI */
    case 0x64 / 4: ch->sem_payload = v; return 1;                                                         /* SEM_PAYLOAD_LO */
    case 0x68 / 4: ch->sem_payload_hi = v; return 1;                                                      /* SEM_PAYLOAD_HI */
    case 0x6c / 4: {                                                                                      /* SEM_EXECUTE */
        uint32_t op = v & 7;
        int is64 = (v >> 24) & 1;
        uint64_t payload = ch->sem_payload | ((uint64_t)ch->sem_payload_hi << 32);
        g_op = "host SEM_EXECUTE";
        if (op == 1) {                                                                     /* RELEASE */
            if (is64) gwrite64(e, ch->sem_addr, payload); else gwrite32(e, ch->sem_addr, (uint32_t)payload);
            if ((v >> 25) & 1) gwrite64(e, ch->sem_addr + 8, gpu_timestamp(e));
            e->nx2_sem_releases++;
            GLOG("[gpu] host SEM_EXECUTE release %llx = %llx\n", (unsigned long long)ch->sem_addr, (unsigned long long)payload);
            return 1;
        }
        if (op == 6) {                                                                     /* REDUCTION */
            uint32_t old = gread32(e, ch->sem_addr);
            gwrite32(e, ch->sem_addr, reduce32((v >> 27) & 0xF, old, (uint32_t)payload, !(v >> 31)));
            e->nx2_sem_releases++;
            return 1;
        }
        return sem_acquire_ok(e, ch->sem_addr, op, payload, is64);
    }
    default: return 1;     /* NOP, wait-for-idle, syncpoint ops (syncpoints read far ahead), etc. */
    }
}

/* 3D / compute: report semaphore (SET_REPORT_SEMAPHORE_A..D at 0x1b00) and inline uploads. */
static void report_semaphore(mgr_emu *e, uint32_t *r) {
    uint64_t addr = ((uint64_t)(r[0x1b00 / 4] & 0xFF) << 32) | r[0x1b04 / 4];
    uint32_t payload = r[0x1b08 / 4], d = r[0x1b0c / 4];
    uint32_t op = d & 3;
    GLOG("[gpu] report semaphore op %u %llx = %x (d=%x)\n", op, (unsigned long long)addr, payload, d);
    if (op == 0) {                                     /* RELEASE */
        if ((d >> 28) & 1) gwrite32(e, addr, payload); /* one word */
        else { gwrite32(e, addr, payload); gwrite32(e, addr + 4, 0); gwrite64(e, addr + 8, gpu_timestamp(e)); }
        e->nx2_sem_releases++;
    } else if (op == 2) {                              /* REPORT_ONLY: counters read as zero, timestamp */
        gwrite64(e, addr, 0); gwrite64(e, addr + 8, gpu_timestamp(e));
        e->nx2_sem_releases++;
    }
}

/* Inline-to-memory (same register block in 3D, compute and the I2M class). */
static void i2m_launch(nx2_channel *ch, nx2_engine *en) {
    uint32_t *r = en->regs;
    ch->i2m_dst = ((uint64_t)(r[0x188 / 4] & 0xFF) << 32) | r[0x18c / 4];
    ch->i2m_len = r[0x180 / 4] * (r[0x184 / 4] ? r[0x184 / 4] : 1);
    ch->i2m_pos = 0;
}

static void i2m_data(mgr_emu *e, nx2_channel *ch, uint32_t v) {
    if (ch->i2m_pos >= ch->i2m_len) return;
    uint8_t b[4]; wr32(b, v);
    uint32_t n = ch->i2m_len - ch->i2m_pos; if (n > 4) n = 4;
    gwrite(e, ch->i2m_dst + ch->i2m_pos, b, n);
    ch->i2m_pos += n;
}

/* Copy engine: pitch-linear copies and its semaphore. */
#include "gpu_image.h"
static int soft_frame_active(void);
static void soft_blit2d(mgr_emu *e, const uint32_t *r);
/* Block-linear copy (captured frames only, where the result is looked at): SET_SRC/DST_BLOCK_SIZE,
   WIDTH, HEIGHT and the origins (clc7b5.h). len is in bytes. */
static void copy_bl(mgr_emu *e, uint32_t *r, uint64_t src, uint64_t dst, int src_pitch, int dst_pitch, uint32_t len, uint32_t lines, uint32_t bpe) {
    uint32_t sbh = 1u << ((r[0x728 / 4] >> 4) & 7), dbh = 1u << ((r[0x70c / 4] >> 4) & 7);
    uint32_t swb = r[0x72c / 4] * bpe, dwb = r[0x710 / 4] * bpe;
    uint32_t spb = (swb + 63) & ~63u, dpb = (dwb + 63) & ~63u;
    uint32_t sox = r[0x744 / 4] * bpe, soy = r[0x748 / 4], dox = r[0x74c / 4] * bpe, doy = r[0x750 / 4];
    uint32_t spitch = r[0x410 / 4], dpitch = r[0x414 / 4];
    uint8_t buf[16];
    for (uint32_t y = 0; y < lines; y++)
        for (uint32_t x = 0; x < len; ) {
            uint32_t n = 16 - ((x + (src_pitch ? 0 : sox)) & 15); uint32_t n2 = 16 - ((x + (dst_pitch ? 0 : dox)) & 15);
            if (n2 < n) n = n2; if (n > len - x) n = len - x;
            uint64_t sa = src_pitch ? src + (uint64_t)y * spitch + x : src + bl_offset(sox + x, soy + y, spb, sbh);
            uint64_t da = dst_pitch ? dst + (uint64_t)y * dpitch + x : dst + bl_offset(dox + x, doy + y, dpb, dbh);
            gread(e, sa, buf, n); gwrite(e, da, buf, n);
            x += n;
        }
}
static void copy_launch(mgr_emu *e, uint32_t *r, uint32_t v) {
    uint32_t xfer = v & 3;                        /* DATA_TRANSFER_TYPE: 0 none, 1 pipelined, 2 non-pipelined */
    uint32_t sem = (v >> 3) & 3;                  /* SEMAPHORE_TYPE: 1 one word, 2 four words */
    int src_pitch = (v >> 7) & 1, dst_pitch = (v >> 8) & 1;   /* MEMORY_LAYOUT: 1 pitch */
    int multi = (v >> 9) & 1;                     /* MULTI_LINE_ENABLE */
    int remap = (v >> 10) & 1;
    if (xfer) {
        uint64_t src = ((uint64_t)r[0x400 / 4] << 32) | r[0x404 / 4];
        uint64_t dst = ((uint64_t)r[0x408 / 4] << 32) | r[0x40c / 4];
        uint32_t len = r[0x418 / 4], lines = multi ? r[0x41c / 4] : 1;
        uint32_t spitch = r[0x410 / 4], dpitch = r[0x414 / 4];
        if (remap) {
            /* remapped component layout: treat as plain byte copy of len * component size */
            uint32_t rc = r[0x708 / 4];
            uint32_t comp = ((rc >> 16) & 3) + 1, ncomp = ((rc >> 20) & 3) + 1;
            len *= comp * ncomp;
            if ((rc & 7) >= 4 && ((rc & 7) == 4 || (rc & 7) == 5)) {     /* const A/B fill */
                uint32_t cv = (rc & 7) == 4 ? r[0x700 / 4] : r[0x704 / 4];
                for (uint32_t y = 0; y < lines; y++)
                    for (uint32_t x = 0; x + 4 <= len; x += 4) gwrite32(e, dst + (uint64_t)y * dpitch + x, cv);
                goto sem;
            }
        }
        if (!src_pitch || !dst_pitch) {
            e->nx2_block_linear_copies++;
            if (soft_frame_active()) {
                uint32_t bpe = 1; if (remap) { uint32_t rc = r[0x708 / 4]; bpe = (((rc >> 16) & 3) + 1) * (((rc >> 20) & 3) + 1); len = r[0x418 / 4] * bpe; }
                copy_bl(e, r, src, dst, src_pitch, dst_pitch, len, lines, bpe);
                { static int nl; if (nl++ < 20) elog(e, "[soft]   block-linear copy %llx -> %llx: %u x %u bytes\n", (unsigned long long)src, (unsigned long long)dst, len, lines); }
                e->nx2_copies++; goto sem;
            }
        }
        if (len && len <= (64u << 20)) {
            uint8_t *tmp = malloc(len);
            if (tmp) {
                for (uint32_t y = 0; y < lines; y++) {
                    gread(e, src + (uint64_t)y * spitch, tmp, len);
                    gwrite(e, dst + (uint64_t)y * dpitch, tmp, len);
                }
                free(tmp);
            }
        }
        e->nx2_copies++;
    }
sem:
    if (sem) {
        uint64_t a = ((uint64_t)(r[0x240 / 4] & 0x1FFFF) << 32) | r[0x244 / 4];
        uint32_t payload = r[0x248 / 4];
        g_op = "copy-engine semaphore";
        if (sem == 1) gwrite32(e, a, payload);
        else { gwrite32(e, a, payload); gwrite32(e, a + 4, 0); gwrite64(e, a + 8, gpu_timestamp(e)); }
        e->nx2_sem_releases++;
    }
}

#include "gpu_mme.h"
#include "sass.h"
#include "sass_exec.h"

/* ---------------- compute: QMD 3.0 scheduling and releases ----------------
   NVN2 orders work between queues with compute descriptors (QMDs): each QMD has a dependency
   count, PCAS signals (compute methods 0xad = QMD address >> 8, 0xb0 = action) schedule it (3),
   load it (2) or change its count (7 = -n, 12 = +n, n = bits 14-23 + 1). When it can run, its
   release slots (words 24, 28 and 52: address, flags | address high, payload) are written, and
   an optional dependent QMD (words 15/16) is signalled. Shaders are not run yet: a QMD with a
   grid still completes, since its releases are what other queues and the CPU wait on. */
static nx2_qmd *qmd_state(mgr_emu *e, uint64_t addr) {
    uint32_t h = (uint32_t)((addr >> 8) * 2654435761u) & (NX2_MAX_QMD - 1);
    for (uint32_t k = 0; k < NX2_MAX_QMD; k++) {
        nx2_qmd *q = &e->nx2_qmd[(h + k) & (NX2_MAX_QMD - 1)];
        if (q->addr == addr) return q;
        if (!q->addr) { memset(q, 0, sizeof *q); q->addr = addr; return q; }
    }
    /* full: forget finished entries */
    for (uint32_t k = 0; k < NX2_MAX_QMD; k++)
        if (e->nx2_qmd[k].completed && !e->nx2_qmd[k].deferred) e->nx2_qmd[k].addr = 0;
    return NULL;
}

static void qmd_release(mgr_emu *e, const uint32_t *q) {
    static const int slots[3] = { 24, 28, 52 };
    for (int i = 0; i < 3; i++) {
        uint32_t fl = q[slots[i] + 1];
        if (!(fl & (1u << 23))) continue;
        uint64_t a = ((uint64_t)(fl & 0xFF) << 32) | q[slots[i]];
        uint32_t p = q[slots[i] + 2], old = gread32(e, a);
        g_op = "QMD release";
        /* QMD 3.0 release: word +1 bits 20-22 reduction op, 24-25 format (signed), 26 reduction
           enable, 30-31 structure size (0 four words, 1 one word, 2 two words) */
        uint32_t v = p;
        if (fl & (1u << 26)) {
            int sg = ((fl >> 24) & 3) == 1;
            switch ((fl >> 20) & 7) {
            case 0: v = old + p; break;
            case 1: v = sg ? ((int32_t)old < (int32_t)p ? old : p) : (old < p ? old : p); break;
            case 2: v = sg ? ((int32_t)old > (int32_t)p ? old : p) : (old > p ? old : p); break;
            case 3: v = old >= p ? 0 : old + 1; break;
            case 4: v = (old == 0 || old > p) ? p : old - 1; break;
            case 5: v = old & p; break;
            case 6: v = old | p; break;
            default: v = old ^ p; break;
            }
        }
        gwrite32(e, a, v);
        uint32_t size = (fl >> 30) & 3;
        if (size == 2) gwrite32(e, a + 4, q[slots[i] + 3]);                       /* two words: 64-bit payload */
        else if (size == 0) { gwrite32(e, a + 4, 0); gwrite64(e, a + 8, gpu_timestamp(e)); }   /* four words */
        e->nx2_sem_releases++;
        GLOG("[gpu] QMD release %llx = %x (flags %x)\n", (unsigned long long)a, v, fl);
    }
}

static struct { uint64_t addr, draws, clears; uint32_t w, h, fmt; } g_rt[32];
/* MGR_SHADER_DUMP=N: disassemble the shaders of the first N distinct programs that draws use
   (each stage's program address: SET_PIPELINE_PROGRAM_ADDRESS_A/B at 0x2014/0x2018 + stage*0x40). */
static void shader_dump_draw(mgr_emu *e, nx2_channel *ch, nx2_engine *en) {
    static int limit = -1; static uint64_t seen[256]; static int nseen;
    if (limit < 0) limit = getenv("MGR_SHADER_DUMP") ? atoi(getenv("MGR_SHADER_DUMP")) : 0;
    if (nseen >= limit) return;
    uint64_t rt = ((uint64_t)(en->regs[0x800 / 4] & 0xFF) << 32) | en->regs[0x804 / 4];
    for (int j = 0; j < 6; j++) {
        uint32_t sh = en->regs[(0x2000 + j * 64) / 4];
        if (!(sh & 1)) continue;
        uint64_t a = ((uint64_t)(en->regs[(0x2014 + j * 64) / 4] & 0xFF) << 32) | en->regs[(0x2018 + j * 64) / 4];
        int k; for (k = 0; k < nseen && seen[k] != a; k++) {}
        if (k < nseen || nseen >= limit || nseen >= 256) continue;
        seen[nseen++] = a;
        uint32_t hdr[32]; gread(e, a, hdr, sizeof hdr);
        elog(e, "[shader] ch %u stage %d type %u program %llx (render target %llx), registers %u; header:", ch->sp, j, (sh >> 4) & 15,
             (unsigned long long)a, (unsigned long long)rt, en->regs[(0x200c + j * 64) / 4]);
        for (int i = 0; i < 32; i++) elog(e, " %08x", hdr[i]);
        elog(e, "\n");
        for (uint64_t pc = 0x80; pc < 0x80 + 0x4000; pc += 16) {
            uint32_t w[4]; gread(e, a + pc, w, 16);
            char line[200]; sass_disasm(w, pc - 0x80, line, sizeof line);
            elog(e, "  %04llx: %s\n", (unsigned long long)(pc - 0x80), line);
            if ((w[0] & 0xFFF) == 0x947 && (sass_bits(w, 34, 82) == ((1ull << 48) - 4) || sass_bits(w, 34, 82) == ((1ull << 48) - 16))) break;   /* BRA to itself: end */
            if (pc > 0x80 + 0x3000) break;
        }
    }
}
static uint32_t g_relwatch = 0x80085000; static uint64_t g_rel_loaded, g_rel_done_loaded, g_rel_lost;
static uint64_t g_dropped[128], g_inline_from_macro; static uint32_t g_last_macro;
static void soft_compute(mgr_emu *e, nx2_engine *en, const uint32_t *q);
static uint32_t g_pcas_ch; static uint64_t g_inline_by_ch[16];
static void pcas_signal(mgr_emu *e, nx2_engine *en, uint32_t m, uint32_t arg) {
    if (m == 0xae || (m == 0xaf && !(arg & 2)) || (m == 0xb0 && (arg & 15) <= 1)) return;
    struct { uint64_t addr; uint32_t action, count; } pend[64];
    int n = 1;
    pend[0].addr = (uint64_t)en->regs[0xad] << 8; pend[0].action = arg & 15; pend[0].count = ((arg >> 14) & 0x3FF) + 1;
    for (int i = 0; i < n; i++) {
        nx2_qmd *st = qmd_state(e, pend[i].addr);
        if (!st) { elog(e, "[gpu] QMD table full\n"); return; }
        uint32_t act = pend[i].action;
        if (act == 3 || act == 2) {
            if (act == 3 || !st->loaded) {
                uint32_t q[64]; gread(e, st->addr, q, sizeof q);
                if (st->loaded && !st->completed) {                 /* reloaded while the last use was still waiting */
                    static int nre; static uint64_t total;
                    total++;
                    if (nre++ < 8 || !(total & (total - 1)))
                        elog(e, "[gpu] QMD %llx reloaded (action %u) while still waiting: deps %d, scheduled %d (%llu so far)\n",
                             (unsigned long long)st->addr, act, st->deps, st->scheduled, (unsigned long long)total);
                }
                if ((q[18] & 0xFF) != 0x30) { if (e->nx2_qmd_errors++ < 4) elog(e, "[gpu] QMD %llx rejected: version byte %x (word 18 = %x)\n", (unsigned long long)st->addr, q[18] & 0xFF, q[18]); GLOG("[gpu] QMD %llx: version %x\n", (unsigned long long)st->addr, q[18] & 0xFF); continue; }
                memcpy(st->words, q, sizeof q);                         /* the scheduler works from its own copy */
                if (q[24] == g_relwatch && ((q[25] >> 23) & 1)) g_rel_loaded++;
                st->deps = (int32_t)((q[4] >> 14) & 0xFFFF) + st->deferred;
                st->deferred = 0; st->loaded = 1; st->completed = 0;
            }
            st->scheduled = 1;
        } else if (act == 7 || act == 12) {
            int32_t d = act == 7 ? -(int32_t)pend[i].count : (int32_t)pend[i].count;
            if (!st->loaded || st->completed) { st->deferred += d; st->hist[st->nhist++ & 7] = (i ? 0x80000000u : 0) | ((g_pcas_ch & 7) << 28) | (act << 24) | 0x400000u | ((uint32_t)st->completed << 23) | ((uint32_t)(st->deferred & 0x7FF) << 12); continue; }
            st->deps += d;
        } else continue;
        st->hist[st->nhist++ & 7] = (i ? 0x80000000u : 0) | ((g_pcas_ch & 7) << 28) | (act << 24) | ((uint32_t)st->completed << 23) | ((uint32_t)(st->deferred & 0x7FF) << 12) | (uint32_t)(st->deps & 0xFFF);
        if (!st->loaded || !st->scheduled || st->completed || st->deps > 0) continue;
        /* The QMD is read when it launches: NVN fills in parts after scheduling it. (A copy taken
           at load time, MGR_QMD_SNAPSHOT=1, stops Zelda early.) */
        uint32_t q[64];
        static int snap = -1; if (snap < 0) snap = getenv("MGR_QMD_SNAPSHOT") != NULL;
        if (!snap) gread(e, st->addr, q, sizeof q); else memcpy(q, st->words, sizeof q);
        st->completed = 1;
        st->hist[st->nhist++ & 7] = 0x7F000000u;
        e->nx2_qmds++;
        if (q[12] && (q[13] & 0xFFFF) && (q[14] & 0xFFFF)) e->nx2_dispatches_skipped++;
        g_qmd_addr = st->addr;
        if (g_relwatch && st->words[24] == g_relwatch && ((st->words[25] >> 23) & 1)) {
            g_rel_done_loaded++;
            if (!(q[24] == g_relwatch && ((q[25] >> 23) & 1)) && g_rel_lost++ < 6)
                elog(e, "[gpu] QMD %llx was loaded releasing %x but at launch releases %x (flags %x)\n", (unsigned long long)st->addr,
                     g_relwatch, q[24], q[25]);
        }
        soft_compute(e, en, q);                                   /* captured frames: run it in software */
        qmd_release(e, q);
        g_qmd_addr = 0;
        if ((q[16] >> 5) & 1) { if (e->nx2_qmd1_seen++ < 3) elog(e, "[gpu] QMD %llx uses dependent QMD 1 (action %u) - not handled\n", (unsigned long long)st->addr, (q[16] >> 6) & 7); }
        if ((q[16] & 1) && n < 64) {
            uint32_t a2 = (q[16] >> 1) & 7;
            pend[n].addr = (uint64_t)q[15] << 8;
            pend[n].action = a2 == 4 ? 7 : a2 == 3 ? 3 : 2;
            pend[n].count = 1;
            n++;
        }
    }
}


/* One method write to a subchannel. Returns 0 to stall (unmet acquire). */
/* An engine method (also what macros emit). */
/* Macro RAM is part of each channel's context: Zelda's compute channels load their own macro set,
   which replaced the graphics channel's when there was one shared RAM. A channel that never
   loaded any uses the first one loaded. MGR_MME_SHARED=1: one RAM for all (old behaviour). */
static nx2_mme *mme_for(mgr_emu *e, nx2_channel *ch, int writing) {
    static int shared = -1; if (shared < 0) shared = getenv("MGR_MME_SHARED") != NULL;
    if (shared) return &e->nx2_mme[0];
    int i = (int)(ch - e->nx2_ch);
    if (i < 0 || i >= NX2_MAX_CH) i = 0;
    if (writing || e->nx2_mme[i].loaded) { e->nx2_mme[i].loaded = 1; return &e->nx2_mme[i]; }
    for (int k = 0; k < NX2_MAX_CH; k++) if (e->nx2_mme[k].loaded) return &e->nx2_mme[k];
    return &e->nx2_mme[i];
}
static uint32_t g_hist[2][NX2_ENG_REGS];      /* MGR_GPU_HIST: method counts for 3D (0) and compute (1) */
#include "gpu_soft.h"
static void engine_method(mgr_emu *e, nx2_channel *ch, nx2_engine *en, uint32_t m, uint32_t v) {
    if (m < NX2_ENG_REGS) en->regs[m] = v;
    {   /* MGR_MME_TRACE=<macro index>: log what the first calls of one macro emit */
        static int tidx = -2; static int tn;
        if (tidx == -2) { const char *t = getenv("MGR_MME_TRACE"); tidx = t ? atoi(t) : -1; }
        if (tidx >= 0 && g_in_macro == tidx && tn < 300) { tn++; elog(e, "[mme %d] ch %u cls %x m %x = %x\n", tidx, ch->sp, en->cls, m * 4, v); }
    }
    if (m < NX2_ENG_REGS && (en->kind == CLS_3D || en->kind == CLS_COMPUTE)) g_hist[en->kind == CLS_COMPUTE][m]++;
    if (gpu_log_on() && (en->kind == CLS_COMPUTE || (m >= 0x6c0 && m < 0x6c4) || (m >= 0x80 && m < 0xC0)))
        elog(e, "[gpu] ch %u cls %x m %x = %x\n", ch->sp, en->cls, m * 4, v);
    e->nx2_methods++;
    switch (en->kind) {
    case CLS_3D: case CLS_COMPUTE: case CLS_I2M:
        if (m == 0x1b0 / 4) i2m_launch(ch, en);
        else if (m == 0x1b4 / 4) i2m_data(e, ch, v);
        else if (en->kind != CLS_I2M && m == 0x1b0c / 4) {
            static char opname[64];
            snprintf(opname, sizeof opname, "class %x report semaphore (d=%x%s%d)", en->cls, v,
                     g_in_macro >= 0 ? ", from macro " : "", g_in_macro);
            g_op = opname;
            if (!en->regs[0x1b04 / 4] && !en->regs[0x1b00 / 4]) {          /* a release with no address set: show where */
                static int nz;
                if (nz++ < 4) {
                    char line[600]; int n = 0;
                    uint32_t from = ch->pb_pos > 24 ? ch->pb_pos - 24 : 0;
                    for (uint32_t k = from; k < ch->pb_pos + 4 && k < ch->pb_words && n < 560; k++)
                        n += snprintf(line + n, sizeof line - n, "%s%08x", k == ch->pb_pos ? " >" : " ", gread32(e, ch->pb_addr + (uint64_t)k * 4));
                    elog(e, "[gpu] ch %u cls %x report semaphore with address 0 (d=%x, macro %d): pushbuffer %llx pos %u:%s\n", ch->sp, en->cls, v,
                         g_in_macro, (unsigned long long)ch->pb_addr, ch->pb_pos, line);
                }
            }
            report_semaphore(e, en->regs);
        }
        else if (m >= 0x110 / 4 && m <= 0x120 / 4) {                            /* macro upload (LOAD_MME_*) */
            /* 0x114 instruction pointer, 0x118 instruction, 0x11c start-address pointer, 0x120 start
               address. One macro RAM serves the 3D and compute engines of every channel (NVN2 loads
               the same set from each channel and calls compute macros on channels that never loaded them). */
            int k = (int)m - 0x45;
            nx2_mme *mm = mme_for(e, ch, 1);
            {
                static uint64_t nup; static int nlog;
                if ((k == 0 || k == 2) && nup++ > 400 && nlog < 40) {
                    nlog++;
                    elog(e, "[gpu] ch %u cls %x macro upload: %s %u (%llu so far)\n", ch->sp, en->cls, k ? "start-table pointer" : "code pointer", v, (unsigned long long)nup);
                }
            }
            if (k == 0) mm->wr = v * 3;
            else if (k == 1) { if (mm->wr < MME_CODE_WORDS) mm->code[mm->wr++] = v; }
            else if (k == 2) mm->start_ptr = v;
            else if (k == 3) { if (mm->start_ptr < 0x80) mm->start[mm->start_ptr] = v; mm->start_ptr++; }
        }
        if (en->kind == CLS_COMPUTE && (m == 0xae || m == 0xaf || m == 0xb0)) { g_pcas_ch = ch->sp; pcas_signal(e, en, m, v); }
        /* Inline QMDs (Ampere+ compute): SET_INLINE_QMD_ADDRESS_A/B (0x318/0x31c, address >> 8),
           then LOAD_INLINE_QMD_DATA(0..63) at 0x320+: the words go to memory at that address,
           where the QMD scheduler (PCAS) reads them like any other QMD. */
        if (en->kind == CLS_COMPUTE && m >= 0x320 / 4 && m < 0x320 / 4 + 64) {
            uint64_t qa = (((uint64_t)en->regs[0x318 / 4] << 32) | en->regs[0x31c / 4]) << 8;
            g_op = "inline QMD";
            if (qa) gwrite32(e, qa + (uint64_t)(m - 0x320 / 4) * 4, v);
            {   /* inline QMD bookkeeping: each word should arrive once, in order, before word 63 */
                static uint32_t next[16]; static uint64_t bad; uint32_t c = ch->sp & 15, w = m - 0x320 / 4;
                if (w != next[c] && bad++ < 12)
                    elog(e, "[gpu] ch %u inline QMD at %llx: word %u arrived, expected %u (macro %d)\n", ch->sp, (unsigned long long)qa, w, next[c], g_in_macro);
                next[c] = w == 63 ? 0 : w + 1;
            }
            if (m == 0x320 / 4 + 63 && qa) {                                   /* last word: the QMD is scheduled */
                uint32_t keep = en->regs[0xad];
                en->regs[0xad] = (uint32_t)(qa >> 8);
                g_pcas_ch = ch->sp; g_inline_by_ch[ch->sp & 15]++; if (g_in_macro >= 0) g_inline_from_macro++; pcas_signal(e, en, 0xb0, 3);
                en->regs[0xad] = keep;
                if (e->nx2_inline_qmd_words++ < 3) elog(e, "[gpu] inline QMD at %llx scheduled\n", (unsigned long long)qa);
            }
        }
        /* SET_FALCON04 (firmware call 4): the firmware updates privileged GPU registers and then
           sets MME shadow scratch 0 to 1, which the calling macro polls. */
        if (m == 0x8c4) en->regs[0xd00] = 1;
        if (en->kind == CLS_3D) {                                    /* constant buffers, draws (software path) */
            if (m >= 0x2410 / 4 && m < 0x2410 / 4 + 5 * 8 && !((m - 0x2410 / 4) & 7)) {      /* BIND_GROUP_CONSTANT_BUFFER(j) */
                int j = (int)(m - 0x2410 / 4) / 8, slot = (int)((v >> 4) & 31), c = (int)(ch - e->nx2_ch);
                if (slot < 18 && c >= 0 && c < NX2_MAX_CH) {
                    soft_cb *b = &g_cbs[c][j][slot];
                    if (v & 1) { b->addr = ((uint64_t)(en->regs[0x2384 / 4] & 0xFF) << 32) | en->regs[0x2388 / 4]; b->size = en->regs[0x2380 / 4] & 0x1FFFF; }
                    else b->addr = 0;
                }
            }
            if (m >= 0x2390 / 4 && m < 0x2390 / 4 + 16) {                                     /* LOAD_CONSTANT_BUFFER(i) */
                uint64_t a = ((uint64_t)(en->regs[0x2384 / 4] & 0xFF) << 32) | en->regs[0x2388 / 4];
                g_op = "LOAD_CONSTANT_BUFFER";
                if (a) gwrite32(e, a + en->regs[0x238c / 4], v);
                en->regs[0x238c / 4] += 4;
            }
            if (g_soft_frame && (m == 0xd78 / 4 || m == 0x17e0 / 4 || m == 0x274 / 4 || m == 0x26c / 4 || (m >= 0x17e4 / 4 && m <= 0x17f8 / 4) || m == 0x19d0 / 4)
                && !nx2_render_enabled(e, en)) { g_soft_cond_skipped++; }
            else if (g_soft_frame) {
                if (m == 0xd78 / 4) soft_draw(e, ch, en, en->regs[0x1618 / 4] & 0xFFFF, en->regs[0xd74 / 4], v, 0);
                else if (m == 0x17e0 / 4) soft_draw(e, ch, en, en->regs[0x1618 / 4] & 0xFFFF, en->regs[0x17dc / 4], v, 1);
                else if (m == 0x274 / 4) soft_draw(e, ch, en, en->regs[0x260 / 4] & 15, en->regs[0x270 / 4], v, 0);
                else if (m == 0x26c / 4) soft_draw(e, ch, en, en->regs[0x260 / 4] & 15, en->regs[0x268 / 4], v, 1);
                else if (m >= 0x17e4 / 4 && m <= 0x17f8 / 4) {
                    uint32_t k = m - 0x17e4 / 4, sz = (k % 3 == 0) ? 2 : (k % 3 == 1) ? 1 : 0;
                    en->regs[0x17d8 / 4] = (en->regs[0x17d8 / 4] & ~3u) | sz; soft_draw(e, ch, en, v >> 28, v & 0xFFFF, (v >> 16) & 0xFFF, 1);
                }
                if (m == 0x19d0 / 4) soft_clear(e, en, v);
            }
        }
        if (en->kind == CLS_3D && m == 0x19d0 / 4) {               /* CLEAR_SURFACE: count per target */
            uint32_t rti = (v >> 6) & 0xF;
            uint64_t rt = ((uint64_t)(en->regs[(0x800 + rti * 0x40) / 4] & 0xFF) << 32) | en->regs[(0x804 + rti * 0x40) / 4];
            int k = 0; while (k < 31 && g_rt[k].addr && g_rt[k].addr != rt) k++;
            if (!g_rt[k].addr) { g_rt[k].addr = rt; g_rt[k].w = en->regs[(0x808 + rti * 0x40) / 4]; g_rt[k].h = en->regs[(0x80c + rti * 0x40) / 4]; g_rt[k].fmt = en->regs[(0x810 + rti * 0x40) / 4]; }
            g_rt[k].clears++;
        }
        if (en->kind == CLS_3D && (m == 0x1618 / 4 || m == 0x1540 / 4 || m == 0x1544 / 4 || m == 0x5f8 / 4))
            {   e->nx2_draws++;
                shader_dump_draw(e, ch, en);
                /* render target statistics: which colour target 0 the draws go to */
                uint64_t rt = ((uint64_t)(en->regs[0x800 / 4] & 0xFF) << 32) | en->regs[0x804 / 4];
                int k = 0; while (k < 31 && g_rt[k].addr && g_rt[k].addr != rt) k++;
                if (!g_rt[k].addr) { g_rt[k].addr = rt; g_rt[k].w = en->regs[0x808 / 4]; g_rt[k].h = en->regs[0x80c / 4]; g_rt[k].fmt = en->regs[0x810 / 4]; }
                g_rt[k].draws++;
            }
        break;
    case CLS_COPY:
        if (m == 0x300 / 4) copy_launch(e, en->regs, v);
        break;
    case CLS_2D:
        if (m == 0x8dc / 4) { e->nx2_blits2d++; soft_blit2d(e, en->regs); }
        break;
    default:
        break;
    }
}

/* Run the macro collected on a subchannel, if any. */
/* Run the macro words collected since the last non-macro method. Like the hardware's MME
   FIFO, a running macro takes words in order: usually its own parameters, but if it needs
   more it reads on into the words of the calls that follow (which then do not start). */
static int macro_run_one(mgr_emu *e, nx2_channel *ch, nx2_engine *en, uint32_t sub, uint32_t first, int may_wait);
/* force: run even a call still waiting for parameters (it then fails). */
/* may_wait: at the end of the submitted pushbuffers, a call that needs more parameters than have
   arrived waits for the next submission (the hardware MME FIFO waits for more data); anywhere
   else it runs, reading missing parameters as zero. */
static void macro_flush_ex(mgr_emu *e, nx2_channel *ch, uint32_t sub, int may_wait) {
    nx2_engine *en = &ch->eng[sub & 7];
    if (!en->mme_pending) return;
    en->mme_pending = 0; en->mme_waiting = 0;
    static int nowait = -1;
    if (nowait < 0) nowait = getenv("MGR_MME_NOWAIT") != NULL;
    if (nowait) may_wait = 0;
    uint32_t k = 0;
    while (k < en->mme_nparams) {
        if (en->mme_start[k] == 0xFFFF) {                   /* leftover parameters: dropped */
            g_dropped[g_last_macro & 127]++;
            k++; continue;
        }
        g_last_macro = en->mme_start[k];
        uint32_t before = k;
        if (macro_run_one(e, ch, en, sub, k, may_wait)) {
            uint32_t n = en->mme_nparams - k;
            memmove(en->mme_params, en->mme_params + k, n * sizeof en->mme_params[0]);
            memmove(en->mme_start, en->mme_start + k, n * sizeof en->mme_start[0]);
            en->mme_nparams = n; en->mme_pending = 1; en->mme_waiting = 1;
            if (e->nx2_macro_waits++ < 6) elog(e, "[gpu] ch %u macro %u waits for more parameters (%u so far)\n", ch->sp, en->mme_start[0], n);
            return;
        }
        k = before + (en->mme_consumed ? en->mme_consumed : 1);
    }
}
static void macro_flush(mgr_emu *e, nx2_channel *ch, uint32_t sub) {
    macro_flush_ex(e, ch, sub, 0);
}
static int macro_run_one(mgr_emu *e, nx2_channel *ch, nx2_engine *en, uint32_t sub, uint32_t first, int may_wait) {
    uint32_t idx = en->mme_start[first];
    static mme_ctx ctx;                                     /* large (parameter area): not on the stack */
    memset(&ctx, 0, offsetof(mme_ctx, extra));
    ctx.nextra = 0; ctx.dma_pos = 0; ctx.dma_pending = 0;
    /* Words an earlier macro fetched into the parameter FIFO (DMA_READ_FIFOED with no barrier
       before it ended) are read first: NVN2 has a macro fetch saved state that the following
       pass-through macros emit. MGR_MME_NOCARRY=1: drop them (old behaviour). */
    static int nocarry = -1; if (nocarry < 0) nocarry = getenv("MGR_MME_NOCARRY") != NULL;
    if (!nocarry && en->mme_nleft) { memcpy(ctx.extra, en->mme_left, en->mme_nleft * 4); ctx.nextra = en->mme_nleft; }
    nx2_mme *mm = mme_for(e, ch, 0);
    ctx.code = mm->code; ctx.dram = ch->mme_dram;
    ctx.fifo = en->mme_params + first; ctx.nfifo = en->mme_nparams - first;
    ctx.e = e; ctx.ch = ch; ctx.en = en;
    e->nx2_macro_calls++;
    static uint32_t dram_before[0xC00];
    if (getenv("MGR_MME_DUMP")) memcpy(dram_before, ch->mme_dram, sizeof dram_before);
    ctx.buffering = may_wait; ctx.nbuf = 0; ctx.dram_saved = 0;
    uint32_t start_method = en->mme_method, start_inc = en->mme_inc;
    static uint32_t last_idx[8]; uint32_t prev_idx = last_idx[sub & 7]; last_idx[sub & 7] = idx;
    g_in_macro = (int)idx;
    const char *err = mme_run(&ctx, mm->start[idx & 0x7F]);
    g_in_macro = -1;
    {
        static int tidx = -2, tn;
        if (tidx == -2) { const char *t = getenv("MGR_MME_TRACE"); tidx = t ? atoi(t) : -1; }
        if ((int)idx == tidx && (tn == 0 || tn == 39)) {
            char fn[512]; snprintf(fn, sizeof fn, "%s/mme_%s.bin", getenv("MGR_DUMP_DIR") ? getenv("MGR_DUMP_DIR") : "/tmp/mgr_dump", tn ? "late" : "early");
            FILE *f = fopen(fn, "wb");
            if (f) { fwrite(mm->code, 4, MME_CODE_WORDS, f); fwrite(mm->start, 4, 0x80, f); fclose(f); }
        }
        if ((int)idx == tidx && tn++ < 40) {
            char pb[120] = ""; int o = 0;
            for (uint32_t k = 0; k < ctx.nfifo && k < 8; k++) o += snprintf(pb + o, sizeof pb - o, " %x", ctx.fifo[k]);
            elog(e, "[mme %d] call on ch %u cls %x start %u: params%s; consumed %u, %u emits buffered, err %s; r0-r5 after %x %x %x %x %x %x\n", tidx, ch->sp, en->cls,
                 mm->start[idx & 0x7F], pb, ctx.consumed, ctx.nbuf, err ? err : "none", en->mme_r[0], en->mme_r[1], en->mme_r[2], en->mme_r[3], en->mme_r[4], en->mme_r[5]);
        }
    }
    if (err == MME_UNDERFLOW && may_wait && ctx.buffering) {          /* undo, and wait for more words */
        if (ctx.dram_saved) memcpy(ch->mme_dram, ctx.dram_backup, sizeof ctx.dram_backup);
        e->nx2_macro_calls--;
        return 1;
    }
    if (ctx.buffering) {                                              /* apply the collected emits */
        g_in_macro = (int)idx;
        for (uint32_t i = 0; i < ctx.nbuf; i++) engine_method(e, ch, en, ctx.buf[i][0], ctx.buf[i][1]);
        g_in_macro = -1;
    }
    {   /* what is left in the FIFO from memory reads stays there for the next call */
        uint32_t n = 0;
        if (ctx.dma_pending) {                              /* requested, no barrier yet: the read still completes */
            uint64_t src = ((uint64_t)(en->regs[0x154] & 0x1FFFFFF) << 32) | en->regs[0x155];
            uint32_t left = ctx.dma_pos < ctx.nextra ? ctx.nextra - ctx.dma_pos : 0;
            memmove(en->mme_left, ctx.extra + ctx.dma_pos, left * 4); n = left;
            for (uint32_t i = 0; i < ctx.dma_pending && n < 0x1000; i++) en->mme_left[n++] = gread32(e, src + i * 4ull);
        } else if (ctx.dma_pos < ctx.nextra) {
            n = ctx.nextra - ctx.dma_pos; memmove(en->mme_left, ctx.extra + ctx.dma_pos, n * 4);
        }
        en->mme_nleft = n;
    }
    {   /* recent calls, shown with the first underflows */
        static struct { uint32_t ch, idx, nfifo, consumed, nextra, dma_pos, left, under; } rec[48]; static uint32_t nrec;
        rec[nrec % 48].ch = ch->sp; rec[nrec % 48].idx = idx; rec[nrec % 48].nfifo = ctx.nfifo; rec[nrec % 48].consumed = ctx.consumed;
        rec[nrec % 48].nextra = ctx.nextra; rec[nrec % 48].dma_pos = ctx.dma_pos; rec[nrec % 48].left = en->mme_nleft; rec[nrec % 48].under = ctx.underflows;
        nrec++;
        static int shown;
        if (ctx.underflows > 4 && shown++ < 2) {
            elog(e, "[gpu] recent macro calls (channel, macro, words available, read, FIFO-memory words, of them read, left after, underflow):\n");
            for (uint32_t k = nrec > 48 ? nrec - 48 : 0; k < nrec; k++)
                if (rec[k % 48].ch == ch->sp)
                    elog(e, "  ch %u m%u avail %u read %u mem %u/%u left %u under %u\n", rec[k % 48].ch, rec[k % 48].idx, rec[k % 48].nfifo,
                         rec[k % 48].consumed, rec[k % 48].dma_pos, rec[k % 48].nextra, rec[k % 48].left, rec[k % 48].under);
        }
    }
    uint32_t own = 1;                                       /* words that belonged to this call */
    while (first + own < en->mme_nparams && en->mme_start[first + own] == 0xFFFF) own++;
    en->mme_consumed = ctx.consumed > own ? (ctx.consumed > ctx.nfifo ? ctx.nfifo : ctx.consumed) : own;
    if (ctx.consumed > own && e->nx2_macro_borrow++ < 4)
        elog(e, "[gpu] macro %u read %u words past its own %u (into the following calls)\n", idx, ctx.consumed - own, own);
    if (!err && ctx.underflows) {
        e->nx2_macro_underflows++;
        if (e->nx2_macro_underflows <= 6) {
            elog(e, "[gpu] macro %u read %u words past its parameters (as zero); it had %u (ch %u sub %u)\n", idx, ctx.underflows, ctx.nfifo, ch->sp, sub);
            if (e->nx2_macro_underflows <= 3) {
                char line[700]; int n = 0;
                uint32_t from = ch->pb_pos > 40 ? ch->pb_pos - 40 : 0;
                for (uint32_t k = from; k < ch->pb_pos + 6 && k < ch->pb_words && n < 660; k++)
                    n += snprintf(line + n, sizeof line - n, "%s%08x", k == ch->pb_pos ? " >" : " ", gread32(e, ch->pb_addr + (uint64_t)k * 4));
                elog(e, "[gpu]   pushbuffer %llx pos %u/%u:%s\n", (unsigned long long)ch->pb_addr, ch->pb_pos, ch->pb_words, line);
            }
        }
    }
    if (err) {
        uint32_t nparams = ctx.nfifo;
        e->nx2_macro_errors++;
        const char *dump = getenv("MGR_MME_DUMP");               /* file: code RAM, start table, call */
        if (dump && e->nx2_macro_errors == 1) {
            FILE *f = fopen(dump, "wb");
            if (f) {
                fwrite(mm->code, 4, MME_CODE_WORDS, f); fwrite(mm->start, 4, 0x80, f);
                fwrite(&idx, 4, 1, f); fwrite(&nparams, 4, 1, f);
                fwrite((en->mme_params + first), 4, nparams, f);
                fwrite(dram_before, 4, 0xC00, f);
                fclose(f);
            }
        }
        if (e->nx2_macro_errors <= 8) {
            elog(e, "[gpu] %s macro %u (start %u, %u params) failed: %s [method at start %x inc %u, previous macro %u]\n",
                 en->kind == CLS_COMPUTE ? "compute" : "3D", idx, mm->start[idx & 0x7F], nparams, err, start_method * 4, start_inc, prev_idx);
            if (e->nx2_macro_errors <= 3) {                           /* the pushbuffer around the call */
                char line[400]; int n = 0;
                uint32_t from = ch->pb_pos > 8 ? ch->pb_pos - 8 : 0;
                for (uint32_t k = from; k < ch->pb_pos + 12 && k < ch->pb_words && n < 380; k++)
                    n += snprintf(line + n, sizeof line - n, "%s%08x", k == ch->pb_pos ? " >" : " ", gread32(e, ch->pb_addr + (uint64_t)k * 4));
                elog(e, "[gpu]   pushbuffer %llx pos %u sub %u:%s\n", (unsigned long long)ch->pb_addr, ch->pb_pos, sub, line);
                elog(e, "[gpu]   params:");
                for (uint32_t k = 0; k < nparams && k < 8; k++) elog(e, " %08x", (en->mme_params + first)[k]);
                elog(e, "\n");
            }
        }
    }
    return 0;
}

static void macro_flush_all(mgr_emu *e, nx2_channel *ch) { for (uint32_t s = 0; s < 8; s++) macro_flush(e, ch, s); }

/* One method write to a subchannel. Returns 0 to stall (unmet acquire). */
static int method(mgr_emu *e, nx2_channel *ch, uint32_t sub, uint32_t m, uint32_t v) {
    nx2_engine *en = &ch->eng[sub & 7];
    if (gpu_log_on() && (m == 0x44 || m == 0x46 || m == 0x47 || m == 0x6c3))
        elog(e, "[gpu] ch %u sub %u cls %x method %x = %x\n", ch->sp, sub, en->cls, m * 4, v);
    if (m >= 0xE00 && (en->kind == CLS_3D || en->kind == CLS_COMPUTE)) {   /* macro call / parameter */
        uint32_t idx = (m - 0xE00) >> 1;
        /* a new call starts at an even method written as the first word of a method run; later
           words of the same run (non-increasing writes) and odd methods are more parameters */
        int starts = !en->mme_pending || en->mme_index != idx || (!(m & 1) && ch->pb_first);
        if (en->mme_nparams >= MME_MAX_PARAMS) macro_flush_ex(e, ch, sub, 0);
        if (!en->mme_pending) { en->mme_pending = 1; en->mme_nparams = 0; starts = 1; }
        if (starts) en->mme_index = idx;
        en->mme_start[en->mme_nparams] = starts ? (uint16_t)idx : 0xFFFF;
        en->mme_params[en->mme_nparams++] = v;
        if (en->mme_waiting) macro_flush_ex(e, ch, sub, 1);           /* a waiting call may have enough now */
        return 1;
    }
    if (m >= 0xE00 && m < NX2_ENG_REGS) {
        static int n_lost;
        if (n_lost++ < 6) elog(e, "[gpu] ch %u: macro method %x on subchannel %u, which has class %x (kind %d): not run\n", ch->sp, m * 4, sub, en->cls, en->kind);
    }
    macro_flush(e, ch, sub);
    if (m == 0) { en->cls = v & 0xFFFF; en->kind = class_kind(en->cls); return 1; }  /* SET_OBJECT */
    if (m < 0x40) { macro_flush_all(e, ch); return host_method(e, ch, m, v); }
    engine_method(e, ch, en, m, v);
    return 1;
}

/* ---------------- pushbuffer decoding ---------------- */

/* Run words of one pushbuffer segment from ch->pb_pos. Returns 0 if stalled on an acquire. */
static int run_segment(mgr_emu *e, nx2_channel *ch) {
    while (ch->pb_pos < ch->pb_words) {
        uint64_t wa = ch->pb_addr + (uint64_t)ch->pb_pos * 4;
        uint32_t w = gread32(e, wa);
        if (ch->pb_remaining) {                                  /* data word of the current method */
            if (!method(e, ch, ch->pb_sub, ch->pb_method, w)) return 0;
            ch->pb_pos++;
            ch->pb_remaining--;
            if (ch->pb_mode == 1) ch->pb_method++;               /* increasing */
            else if (ch->pb_mode == 5) { ch->pb_method++; ch->pb_mode = 3; }   /* increase once, then non-increasing */
            ch->pb_first = 0;
            continue;
        }
        uint32_t mode = w >> 29, sub = (w >> 13) & 7, meth = w & 0x1FFF, arg = (w >> 16) & 0x1FFF;
        if (mode == 4) {                                         /* immediate */
            ch->pb_first = 1;
            if (!method(e, ch, sub, meth, arg)) return 0;
            ch->pb_pos++;
            continue;
        }
        if (mode == 1 || mode == 3 || mode == 5) {
            ch->pb_mode = mode; ch->pb_sub = sub; ch->pb_method = meth; ch->pb_remaining = arg;
            ch->pb_first = 1;
            ch->pb_pos++;
            continue;
        }
        if (mode == 0 && w == 0) { ch->pb_pos++; continue; }    /* padding */
        e->nx2_bad_headers++;
        ch->pb_pos++;
    }
    return 1;
}

/* ---------------- syncpoints ----------------
   Each channel's syncpoint advances when the work of a submission has been run, and the CPU
   (syncpoint RO page) and GPU (shim page) see the value. Before this they read "far ahead", so
   the game reused command memory a stalled channel had not run yet (MGR_SP_FAR=1: old way). */
static int sp_real(mgr_emu *e) {
    if (!e->nx2_sp_real) e->nx2_sp_real = getenv("MGR_SP_FAR") ? -1 : 1;
    return e->nx2_sp_real > 0;
}
uint32_t nx2_syncpt_value(mgr_emu *e, uint32_t id) {
    /* only GPU channel syncpoints count real work; any other reads as far ahead */
    return sp_real(e) && id >= 1 && id <= e->nx2_next_syncpt ? e->nx2_sp_cur[id & 511] : 0x7FFFFFFFu;
}
static void sp_advance(mgr_emu *e, uint32_t id, uint32_t inc) {
    if (!inc) return;
    id &= 511;
    e->nx2_sp_cur[id] += inc;
    if (sp_real(e) && e->nx2_syncpt_ro && id < e->nx2_syncpt_count) {
        mwr(e, e->nx2_syncpt_ro + 0x40 + id * 8ull, 8, e->nx2_sp_cur[id]);
        kernel_address_changed(e, e->nx2_syncpt_ro + 0x40 + id * 8ull);
        /* changed-syncpoint bits (per syncpoint, and a summary bit per 64) */
        uint64_t bits = e->nx2_syncpt_ro + 0x1100 + (id / 64) * 8ull, sum = e->nx2_syncpt_ro + 0x1040;
        mwr(e, bits, 8, mrd(e, bits, 8) | (1ull << (id % 64)));
        mwr(e, sum, 8, mrd(e, sum, 8) | (1ull << (id / 64)));
    }
    if (sp_real(e)) for (int k = 0; k < e->nx2_sp_nev; k++) event_signal(e, e->nx2_sp_ev[k]);
}
/* Run queued GPFIFO entries of a channel until it is empty or stalls. */
static int channel_run(mgr_emu *e, nx2_channel *ch) {
    int progressed = 0;
    for (;;) {
        if (ch->pb_pos >= ch->pb_words) {
            if (ch->cur_inc) { sp_advance(e, ch->sp, ch->cur_inc); ch->cur_inc = 0; }
            if (ch->q_head == ch->q_tail) { for (uint32_t s2 = 0; s2 < 8; s2++) macro_flush_ex(e, ch, s2, 1); break; }
            uint64_t ent = ch->queue[ch->q_head % NX2_QUEUE];
            ch->cur_inc = ch->qinc[ch->q_head % NX2_QUEUE];
            {   /* experiment: MGR_MME_SEGREGS=1 clears macro registers at each pushbuffer entry */
                static int seg = -1; if (seg < 0) seg = getenv("MGR_MME_SEGREGS") != NULL;
                if (seg) for (int k = 0; k < 8; k++) memset(ch->eng[k].mme_r, 0, sizeof ch->eng[k].mme_r);
            }
            ch->q_head++;
            ch->pb_addr = ent & 0xFFFFFFFFFCull;
            ch->pb_words = (uint32_t)((ent >> 42) & 0x1FFFFF);
            ch->pb_pos = 0;
            ch->pb_remaining = 0;
            e->nx2_entries++;
            progressed = 1;
            continue;
        }
        uint32_t before = ch->pb_pos;
        if (!run_segment(e, ch)) { ch->stalled = 1; if (ch->pb_pos != before) progressed = 1; break; }
        ch->stalled = 0;
        progressed = 1;
    }
    return progressed;
}

static nx2_channel *channel_for(mgr_emu *e, uint32_t sp) {
    for (int i = 0; i < e->nx2_nch; i++) if (e->nx2_ch[i].sp == sp) return &e->nx2_ch[i];
    if (e->nx2_nch >= NX2_MAX_CH) return NULL;
    nx2_channel *ch = &e->nx2_ch[e->nx2_nch++];
    memset(ch, 0, sizeof *ch);
    ch->sp = sp;
    return ch;
}

void nx2_gpfifo_submit_entries(mgr_emu *e, const uint64_t *entries, uint32_t n, uint32_t sp) {
    nx2_channel *ch = channel_for(e, sp);
    if (!ch) return;
    for (uint32_t i = 0; i < n; i++) {
        if (ch->q_tail - ch->q_head >= NX2_QUEUE) { elog(e, "[gpu] channel %u queue full\n", sp); break; }
        ch->qinc[ch->q_tail % NX2_QUEUE] = 0;
        ch->queue[ch->q_tail++ % NX2_QUEUE] = entries[i];
    }
    if (e->nx2_submits < 8)
        for (uint32_t i = 0; i < n && i < 4; i++)
            elog(e, "[gpu] ch %u entry %llx: va %llx, %u words\n", sp, (unsigned long long)entries[i],
                 (unsigned long long)(entries[i] & 0xFFFFFFFFFCull), (unsigned)((entries[i] >> 42) & 0x1FFFFF));
    e->nx2_submits++;
    nx2_gpu_poll(e);
}

/* The syncpoint increment of a submission: due when its last entry has run. Call after
   queuing the entries (but this runs them, so queue first with nx2_gpfifo_submit_entries). */
void nx2_gpfifo_fence(mgr_emu *e, uint32_t sp, uint32_t inc) {
    nx2_channel *ch = channel_for(e, sp);
    if (!ch || !inc) return;
    if (ch->q_head != ch->q_tail) ch->qinc[(ch->q_tail - 1) % NX2_QUEUE] += inc;   /* last queued entry */
    else if (ch->pb_pos < ch->pb_words) ch->cur_inc += inc;                       /* the running one */
    else sp_advance(e, sp, inc);                                                  /* all done already */
}
void nx2_gpfifo_submit(mgr_emu *e, uint64_t list, uint32_t n, uint32_t sp) {
    if (!list || !n) return;
    if (n > 65536) { static int nt; if (nt++ < 8) elog(e, "[nv] SUBMIT_GPFIFO: only 65536 of %u entries taken\n", n); n = 65536; }
    uint64_t *ents = malloc(n * 8ull);
    if (!ents) return;
    for (uint32_t i = 0; i < n; i++) ents[i] = mrd(e, list + i * 8ull, 8);
    nx2_gpfifo_submit_entries(e, ents, n, sp);
    free(ents);
}

/* Give stalled channels another chance (called on submits and from CPU waits). */
/* Called at each present: the next frame is rendered in software when it will be captured. */
void nx2_soft_frame(mgr_emu *e, uint64_t presents) {
    static int on = -1, every = 0;
    if (on < 0) { on = getenv("MGR_SOFT_RENDER") != NULL; every = getenv("MGR_FRAME_DUMP") ? atoi(getenv("MGR_FRAME_DUMP")) : 0; }
    if (g_soft_frame) {
        elog(e, "[soft] frame rendered in %.1f s: %llu draws, %llu pixels shaded, depth tests %llu (%llu failed), blended writes %llu, first unsupported op %llx\n",
             now_seconds() - g_soft_t0, (unsigned long long)g_soft_draws, (unsigned long long)g_soft_pixels, (unsigned long long)g_soft_ztests,
             (unsigned long long)g_soft_zfail, (unsigned long long)g_soft_blends, (unsigned long long)g_soft_unk_op);
        { static const char *why[16] = {"", "HDR void-extent", "reserved block mode", "reserved block mode (low bits)", "weight range", "weight grid too big", "weight bits", "4 partitions + dual plane", "too many colour values", "colour bits", "colour range", "HDR endpoint mode"};
          for (int k = 0; k < 16; k++) if (astc_err_count[k]) elog(e, "[soft]   ASTC blocks decoded as error colour (%s): %llu texels\n", why[k] ? why[k] : "?", astc_err_count[k]);
          memset(astc_err_count, 0, sizeof astc_err_count); }
        for (int k = 0; k < 128; k++) if (g_soft_texfmt[k]) elog(e, "[soft]   undecoded texture format %02x: %llu texels\n", k, (unsigned long long)g_soft_texfmt[k]);
        elog(e, "[soft]   compute: %llu dispatches run (%llu threads), %llu skipped\n", (unsigned long long)g_soft_cs, (unsigned long long)g_soft_cs_threads, (unsigned long long)g_soft_cs_skipped);
        for (int k = 0; k < 64 && g_soft_cs_unk[k]; k++) elog(e, "[soft]   compute: unsupported op %03x x%u\n", g_soft_cs_unk[k] & 0xFFF, g_soft_cs_unk[k] >> 16);
        g_soft_cs = g_soft_cs_threads = g_soft_cs_skipped = 0; memset(g_soft_cs_unk, 0, sizeof g_soft_cs_unk);
        for (int k = 0; k < g_nsstat; k++) { soft_stat *x = &g_sstat[k]; if (x->draws < 3 && x->w * x->h < 500000) continue;
            elog(e, "[soft]   rt %llx z %llx %ux%u fmt %x mrt %u: ztest %u func %u write %u omap %u cw %u blend %u zout %u: %llu draws, %llu tests %llu failed, %llu shaded, %llu killed, %llu black, max %g, z %g..%g vs stored %g..%g\n",
                 (unsigned long long)x->rtaddr, (unsigned long long)x->zaddr, x->w, x->h, (x->key >> 12) & 0xFF, x->key >> 20, x->key & 1, (x->key >> 1) & 7, (x->key >> 4) & 1, (x->key >> 5) & 1, (x->key >> 6) & 1, (x->key >> 7) & 1, (x->key >> 8) & 1,
                 (unsigned long long)x->draws, (unsigned long long)x->tests, (unsigned long long)x->fails, (unsigned long long)x->shaded, (unsigned long long)x->killed, (unsigned long long)x->black, x->maxc, x->zmin, x->zmax, x->dmin, x->dmax); }
        g_nsstat = 0; g_nzwritten = 0;
        if (g_soft_cond_skipped) elog(e, "[soft]   %llu draws/clears skipped by conditional rendering\n", (unsigned long long)g_soft_cond_skipped);
        g_soft_cond_skipped = 0;
        if (g_soft_zequal_fallback) elog(e, "[soft]   %llu EQUAL-depth draws had no pre-pass in their depth buffer: drawn as LEQUAL\n", (unsigned long long)g_soft_zequal_fallback);
        g_soft_zequal_fallback = 0;
        for (int k = 0; k < 128; k++) if (g_soft_sustfmt[k]) elog(e, "[soft]   image stores in unhandled format %02x: %llu\n", k, (unsigned long long)g_soft_sustfmt[k]);
        memset(g_soft_sustfmt, 0, sizeof g_soft_sustfmt);
        memset(g_soft_texfmt, 0, sizeof g_soft_texfmt); g_soft_ztests = g_soft_zfail = g_soft_blends = 0; g_soft_unk_op = 0;
        g_soft_draws = g_soft_pixels = 0;
        /* save every colour target drawn in the frame (tone-mapped for float formats) */
        const char *dir = getenv("MGR_DUMP_DIR") ? getenv("MGR_DUMP_DIR") : "/tmp/mgr_dump/frames";
        for (int k = 0; k < g_soft_nrts; k++) {
            char path[600]; snprintf(path, sizeof path, "%s/soft_%06llu_rt%d_%llx_%ux%u_fmt%x.png", dir, (unsigned long long)presents, k,
                                     (unsigned long long)g_soft_rts[k].addr, g_soft_rts[k].w, g_soft_rts[k].h, g_soft_rts[k].fmt);
            if (soft_save_rt_png(e, &g_soft_rts[k], path)) elog(e, "[soft] saved %s (%u draws)\n", path, g_soft_rts[k].draws);
        }
        g_soft_nrts = 0;
    }
    static int nframes = -1; if (nframes < 0) nframes = getenv("MGR_SOFT_FRAMES") ? atoi(getenv("MGR_SOFT_FRAMES")) : 1;   /* consecutive frames (temporal effects) */
    if (nframes < 1) nframes = 1;
    g_soft_frame = on && every > 0 && (presents + 1) % (uint64_t)every < (uint64_t)nframes && presents + 1 >= (uint64_t)every;
    if (g_soft_frame) {
        g_soft_t0 = now_seconds(); g_soft_budget_hit = 0; g_soft_t0_id = presents + 1;
        if (getenv("MGR_SOFT_BUDGET")) g_soft_budget = atof(getenv("MGR_SOFT_BUDGET"));
        elog(e, "[soft] rendering frame %llu in software (budget %.0f s)\n", (unsigned long long)presents + 1, g_soft_budget);
    }
}
void nx2_gpu_dump(mgr_emu *e) {
    for (int k = 0; k < 32 && g_rt[k].addr; k++)
        elog(e, "[gpu] colour target %llx %ux%u format %x: %llu draws, %llu clears\n", (unsigned long long)g_rt[k].addr,
             g_rt[k].w, g_rt[k].h, g_rt[k].fmt, (unsigned long long)g_rt[k].draws, (unsigned long long)g_rt[k].clears);
    elog(e, "[gpu] inline QMDs sent from macros: %llu\n", (unsigned long long)g_inline_from_macro);
    for (int k = 0; k < 128; k++) if (g_dropped[k]) elog(e, "[gpu] macro %d left %llu parameter words unread (dropped)\n", k, (unsigned long long)g_dropped[k]);
    elog(e, "[gpu] QMDs releasing %x: %llu loaded, %llu launched, %llu lost their release before launch\n", g_relwatch,
         (unsigned long long)g_rel_loaded, (unsigned long long)g_rel_done_loaded, (unsigned long long)g_rel_lost);
    if (g_gonly_n) elog(e, "[gpu] %llu GPU-only pages in use\n", (unsigned long long)g_gonly_n);
    for (int k = 0; k < 16; k++) if (g_inline_by_ch[k]) elog(e, "[gpu] channel %d scheduled %llu inline QMDs\n", k, (unsigned long long)g_inline_by_ch[k]);
    for (int k = 0; k < 48 && g_faults[k].op; k++)
        elog(e, "[gpu] faults: %llu x %s at page %llx000\n", (unsigned long long)g_faults[k].n, g_faults[k].op, (unsigned long long)g_faults[k].page);
    for (int k = 0; k < 16 && g_trace_op[k]; k++)
        elog(e, "[trace] %llx written %llu times by %s\n", (unsigned long long)g_trace, (unsigned long long)g_trace_n[k], g_trace_op[k]);
    {   /* QMDs that were scheduled but never ran: what they are waiting for */
        int waiting = 0, unsched = 0, shown = 0;
        for (uint32_t k = 0; k < NX2_MAX_QMD; k++) {
            nx2_qmd *q = &e->nx2_qmd[k];
            if (!q->addr || q->completed) continue;
            if (q->scheduled) waiting++; else unsched++;
            if (shown++ < 6) {
                uint32_t w[64]; gread(e, q->addr, w, sizeof w);
                elog(e, "[gpu] QMD %llx: loaded %d scheduled %d deps %d deferred %d; releases %08x:%08x=%x (%08x) %08x:%08x=%x (%08x) %08x:%08x=%x (%08x); dependent %08x (w16 %x)\n",
                     (unsigned long long)q->addr, q->loaded, q->scheduled, q->deps, q->deferred,
                     w[25] & 0xFF, w[24], w[26], w[25], w[29] & 0xFF, w[28], w[30], w[29], w[53] & 0xFF, w[52], w[54], w[53], w[15], w[16]);
                for (int b = 4; b >= 0; b--) {           /* this QMD and the ones before it in memory */
                    uint64_t a = q->addr - (uint64_t)b * 0x100;
                    nx2_qmd *p2 = NULL;
                    for (uint32_t j = 0; j < NX2_MAX_QMD; j++) if (e->nx2_qmd[j].addr == a) { p2 = &e->nx2_qmd[j]; break; }
                    uint32_t x[64]; gread(e, a, x, sizeof x);
                    char hb[160] = ""; int o = 0;
                    if (p2) for (uint32_t h = p2->nhist > 8 ? p2->nhist - 8 : 0; h < p2->nhist; h++) o += snprintf(hb + o, sizeof hb - o, " %08x", p2->hist[h & 7]);
                    elog(e, "[gpu]   %llx: w4 %08x w15 %08x w16 %08x w18 %08x state %s%s%s deps %d def %d hist(src|act|cmp|def|deps):%s\n",
                         (unsigned long long)a, x[4], x[15], x[16], x[18], p2 ? "known" : "unknown", p2 && p2->completed ? " completed" : "",
                         p2 && p2->scheduled ? " sched" : "", p2 ? p2->deps : 0, p2 ? p2->deferred : 0, hb);
                }
            }
        }
        elog(e, "[gpu] QMDs not completed: %d scheduled (waiting on dependencies), %d never scheduled; %llu completed, %llu rejected (bad version), %llu inline\n",
             waiting, unsched, (unsigned long long)e->nx2_qmds, (unsigned long long)e->nx2_qmd_errors, (unsigned long long)e->nx2_inline_qmd_words);
    }
    if (getenv("MGR_GPU_HIST"))
        for (int k = 0; k < 2; k++) {
            elog(e, "[gpu] %s methods written (byte offset: count):\n", k ? "compute" : "3D");
            for (uint32_t m = 0; m < NX2_ENG_REGS; m++)
                if (g_hist[k][m]) elog(e, "  %04x: %u\n", m * 4, g_hist[k][m]);
        }
    for (int i = 0; i < e->nx2_nch; i++) {
        nx2_channel *ch = &e->nx2_ch[i];
        elog(e, "[gpu] channel %u: %s, queue %u/%u, segment %llx pos %u/%u, semaphore %llx payload %x (now %x)\n", ch->sp,
             ch->stalled ? "STALLED" : "idle", ch->q_head, ch->q_tail, (unsigned long long)ch->pb_addr, ch->pb_pos, ch->pb_words,
             (unsigned long long)ch->sem_addr, ch->sem_payload, ch->sem_addr ? gread32(e, ch->sem_addr) : 0);
        elog(e, "[gpu]   subchannel classes: %x %x %x %x %x %x %x %x\n", ch->eng[0].cls, ch->eng[1].cls, ch->eng[2].cls, ch->eng[3].cls,
             ch->eng[4].cls, ch->eng[5].cls, ch->eng[6].cls, ch->eng[7].cls);
        if (ch->stalled) {
            uint32_t from = ch->pb_pos > 40 ? ch->pb_pos - 40 : 0, to = ch->pb_pos + 24 < ch->pb_words ? ch->pb_pos + 24 : ch->pb_words;
            char b[800] = ""; int o = 0;
            for (uint32_t j = from; j < to && o < 760; j++) o += snprintf(b + o, sizeof b - o, "%s%08x", j == ch->pb_pos ? " >" : " ", gread32(e, ch->pb_addr + j * 4ull));
            elog(e, "[gpu]   segment words:%s\n", b);
        }
    }
}

void nx2_gpu_poll(mgr_emu *e) {
    for (int round = 0; round < 8; round++) {
        int any = 0;
        for (int i = 0; i < e->nx2_nch; i++) any |= channel_run(e, &e->nx2_ch[i]);
        if (!any) break;
    }
    if (e->nx2_submits && (e->nx2_submits & (e->nx2_submits - 1)) == 0 && e->nx2_logged_submits != e->nx2_submits && (e->nx2_logged_submits = e->nx2_submits))
        elog(e, "[gpu] %llu submits, %llu entries, %llu methods, %llu sem releases, %llu copies, %llu draws, "
             "%llu macro calls (%llu failed), %llu QMDs (%llu dispatches skipped), %llu bad headers, %llu faults\n",
             (unsigned long long)e->nx2_submits, (unsigned long long)e->nx2_entries, (unsigned long long)e->nx2_methods,
             (unsigned long long)e->nx2_sem_releases, (unsigned long long)e->nx2_copies, (unsigned long long)e->nx2_draws,
             (unsigned long long)e->nx2_macro_calls, (unsigned long long)e->nx2_macro_errors, (unsigned long long)e->nx2_qmds, (unsigned long long)e->nx2_dispatches_skipped, (unsigned long long)e->nx2_bad_headers, (unsigned long long)e->nx2_gpu_faults);
}
