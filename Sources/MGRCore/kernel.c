#include <time.h>
#include <pthread.h>
#include <stdlib.h>
/*
 * Horizon HLE kernel: handles, threads + cooperative scheduler, events,
 * mutexes / condition variables / address arbiters, memory SVCs, and the
 * public mgr_emu_* API.
 *
 * Time model: one tick per executed instruction (CNTVCT = TICK_HZ nominal);
 * when every thread is asleep the clock jumps to the earliest wake-up.
 */
#include "emu_internal.h"

#define RES_OK            0u
#define RES_TIMEOUT       0xEA01u
#define RES_INVALID_HANDLE 0xE401u
#define RES_CANCELLED     0xEC01u
#define RES_OUT_OF_RANGE  0xF401u
#define RES_INVALID_STATE 0xFA01u
#define RES_NOT_IMPL      0xF201u
#define CUR_PROCESS 0xFFFF8001u
#define CUR_THREAD  0xFFFF8000u
#define HANDLE_BASE 0xE000u
#define WAITERS_BIT 0x40000000u

/* ---------------- handles / events ---------------- */
uint32_t handle_new(mgr_emu *e, int type, int idx) {
    for (int i = 1; i < MAX_HANDLES; i++)
        if (e->handles[i].type == KO_FREE) {
            e->handles[i].type = type; e->handles[i].idx = idx;
            if (getenv("MGR_HANDLE_LOG")) elog(e, "[h] %x = type %d idx %d\n", HANDLE_BASE + (uint32_t)i, type, idx);
            return HANDLE_BASE + (uint32_t)i;
        }
    return 0;
}

khandle *handle_get(mgr_emu *e, uint32_t h) {
    static khandle cur_thread, cur_process;
    if (h == CUR_THREAD) { cur_thread.type = KO_THREAD; cur_thread.idx = e->cur; return &cur_thread; }
    if (h == CUR_PROCESS) { cur_process.type = KO_PROCESS; cur_process.idx = 0; return &cur_process; }
    if (h < HANDLE_BASE || h >= HANDLE_BASE + MAX_HANDLES) return NULL;
    khandle *k = &e->handles[h - HANDLE_BASE];
    return k->type == KO_FREE ? NULL : k;
}

int event_new(mgr_emu *e, int signaled) {
    for (int i = 0; i < MAX_EVENTS; i++)
        if (!e->events[i].used) { e->events[i].used = 1; e->events[i].signaled = signaled; e->events[i].sticky = 0; return i; }
    return 0;
}

static int handle_signaled(mgr_emu *e, uint32_t h) {
    khandle *k = handle_get(e, h);
    if (!k) return 0;
    switch (k->type) {
    case KO_EVENT: return e->events[k->idx].signaled;
    case KO_THREAD: return e->th[k->idx].state == TH_DEAD;
    case KO_SESSION: return 0;
    default: return 0;
    }
}

static void wake(mgr_emu *e, int t, uint32_t res, int has_index, int64_t index);

void event_signal(mgr_emu *e, int ev) {
    e->events[ev].signaled = 1;
    for (int t = 0; t < MAX_THREADS; t++) {
        kthread *th = &e->th[t];
        if (th->state != TH_WAIT || th->wait_kind != WAIT_SYNC) continue;
        for (int k = 0; k < th->wait_count; k++)
            if (handle_signaled(e, th->wait_handles[k])) { wake(e, t, RES_OK, 1, k); break; }
    }
}

/* The time service's shared memory, as the SDK's nn::time::detail::SharedMemoryClient reads it.
   Each record is a pair of entries selected by a counter (seqlock). The steady clock (0xD0/0xD8,
   0x38-byte entries) is tick-based: time = ns(tick) + clamp(mult*(tick-base) >> shift + offset).
   A zero clock-source id means "not set up", so every clock read fails without one. */
static void time_shmem_fill(mgr_emu *e, uint64_t base) {
    const uint64_t id0 = 0x4D696B6573474D52ull, id1 = 0x54696D65436C6B31ull;   /* clock source id */
    int64_t unix_now = (int64_t)time(NULL);
    for (int k = 0; k < 2; k++) {
        uint64_t st = base + 0xD8 + (uint64_t)k * 0x38;              /* steady clock: no correction */
        for (int o = 0; o < 0x28; o += 8) mwr(e, st + o, 8, 0);
        mwr(e, st + 0x28, 8, id0); mwr(e, st + 0x30, 8, id1);
        uint64_t old = base + 0x08 + (uint64_t)k * 0x18;             /* older layout: offset + id */
        mwr(e, old, 8, 0); mwr(e, old + 8, 8, id0); mwr(e, old + 16, 8, id1);
        for (int c = 0; c < 2; c++) {                                 /* user (0x40) and network (0x88) system clocks */
            uint64_t sc = base + (c ? 0x88 : 0x40) + (uint64_t)k * 0x20;
            mwr(e, sc, 8, (uint64_t)unix_now); mwr(e, sc + 8, 8, 0);
            mwr(e, sc + 16, 8, id0); mwr(e, sc + 24, 8, id1);
        }
    }
    mwr(e, base + 0x00, 4, 0); mwr(e, base + 0x38, 4, 0); mwr(e, base + 0x80, 4, 0);
    mwr(e, base + 0xC8, 4, 0); mwr(e, base + 0xCC, 2, 0x0101);       /* automatic correction on */
    mwr(e, base + 0xD0, 4, 0);
    elog(e, "[time] shared memory at %llx: clocks set up (unix time %lld)\n", (unsigned long long)base, (long long)unix_now);
}

/* hid's shared memory as nn::hid::detail::NpadSharedMemoryAccessor reads it: per-controller
   internal states at 0x9A00 + 0x5000*i, and the "npad condition" at 0x3E200 (+8 joy hold type,
   +12 initialized). With the condition unset, GetNpadJoyHoldType fails and the game aborts. */
static void hid_shmem_fill(mgr_emu *e, uint64_t base) {
    mwr(e, base + 0x3E200 + 8, 4, 1);           /* NpadJoyHoldType_Horizontal (the SDK default) */
    mwr(e, base + 0x3E200 + 12, 1, 1);          /* initialized */
    /* Player 1 (internal state 0 at 0x9A00) is a connected Pro Controller ("full key"):
       style set +0, device type +0x4188, and the full-key state ring at +0x28 (header: buffer
       count +0x30, tail +0x38, count +0x40; 17 entries of 0x30 from +0x48, each a sampling
       number then the state: sampling, buttons, two sticks, attributes). hid_update adds a
       sample every frame. */
    uint64_t st = base + 0x9A00;
    mwr(e, st + 0x00, 4, 1);                    /* NpadStyleFullKey */
    mwr(e, st + 0x04, 4, 0);                    /* joy assignment: dual */
    mwr(e, st + 0x08, 4, 0); mwr(e, st + 0x0C, 4, 0xFF323232); mwr(e, st + 0x10, 4, 0xFFE6E6E6);   /* colours */
    mwr(e, st + 0x4188, 4, 1);                  /* device type: full key */
    mwr(e, st + 0x30, 8, 17);
    e->hid_shmem = base;
    elog(e, "[hid] shared memory at %llx: npad condition set up, player 1 = Pro Controller\n", (unsigned long long)base);
}

/* One input sample for player 1 (no buttons yet). Called every vsync. */
void hid_update(mgr_emu *e) {
    if (!e->hid_shmem) return;
    uint64_t st = e->hid_shmem + 0x9A00;
    uint64_t n = ++e->hid_sampling;
    uint64_t tail = n % 17, count = n < 17 ? n : 17;
    uint64_t ent = st + 0x48 + tail * 0x30;
    mwr(e, ent + 0x00, 8, n * 2);               /* storage sequence number: even = stable (odd means being written) */
    mwr(e, ent + 0x08, 8, n);                   /* state: sampling number */
    /* MGR_PRESS="A@60,PLUS@75": scripted presses (button@seconds of game time, held 1/4 s),
       for testing until the app passes real controllers through. */
    uint64_t buttons = 0;
    const char *ps = getenv("MGR_PRESS");
    if (ps) {
        static const struct { const char *n; int bit; } bn[] = { {"A",0},{"B",1},{"X",2},{"Y",3},{"L",6},{"R",7},{"ZL",8},
            {"ZR",9},{"PLUS",10},{"MINUS",11},{"LEFT",12},{"UP",13},{"RIGHT",14},{"DOWN",15} };
        double now = (double)e->nx2_vsyncs / 60.0;
        for (const char *p = ps; *p; ) {
            char name[8] = {0}; int k = 0;
            while (*p && *p != '@' && k < 7) name[k++] = *p++;
            if (*p != '@') break;
            double at = strtod(p + 1, (char **)&p);
            if (now >= at && now < at + 0.25)
                for (unsigned b = 0; b < sizeof bn / sizeof bn[0]; b++) if (!strcmp(bn[b].n, name)) buttons |= 1ull << bn[b].bit;
            while (*p == ',' || *p == ' ') p++;
        }
        if (buttons && !(e->hid_logged_press++ % 15)) elog(e, "[hid] pressing %llx at %.1f s\n", (unsigned long long)buttons, now);
    }
    mwr(e, ent + 0x10, 8, buttons);             /* buttons */
    mwr(e, ent + 0x18, 8, 0);                   /* left stick */
    mwr(e, ent + 0x20, 8, 0);                   /* right stick */
    mwr(e, ent + 0x28, 4, 1 | 2);               /* attributes: connected, wired */
    mwr(e, st + 0x38, 8, tail);
    mwr(e, st + 0x40, 8, count);
}

uint64_t cur_tls(mgr_emu *e) { return e->tpidrro; }

/* ---------------- threads ---------------- */
static void save_ctx(mgr_emu *e, kthread *t) {
    memcpy(t->x, e->x, sizeof t->x); t->sp = e->sp; t->pc = e->pc;
    t->n = e->n; t->z = e->z; t->c = e->c; t->v = e->v;
    memcpy(t->q, e->q, sizeof t->q);
    t->tpidr = e->tpidr; t->tpidrro = e->tpidrro; t->fpcr = e->fpcr; t->fpsr = e->fpsr;
}
static void load_ctx(mgr_emu *e, const kthread *t) {
    memcpy(e->x, t->x, sizeof e->x); e->sp = t->sp; e->pc = t->pc;
    e->n = t->n; e->z = t->z; e->c = t->c; e->v = t->v;
    memcpy(e->q, t->q, sizeof e->q);
    e->tpidr = t->tpidr; e->tpidrro = t->tpidrro; e->fpcr = t->fpcr; e->fpsr = t->fpsr;
    e->excl_valid = 0;
}

/* Register write that works whether the thread is live (current) or saved. */
static uint64_t *treg(mgr_emu *e, int t, int r) { return t == e->cur ? &e->x[r] : &e->th[t].x[r]; }

static void wake(mgr_emu *e, int t, uint32_t res, int has_index, int64_t index) {
    kthread *th = &e->th[t];
    th->state = TH_READY;
    th->wait_kind = WAIT_NONE;
    *treg(e, t, 0) = res;
    if (has_index) *treg(e, t, 1) = (uint64_t)index;
    /* On the console a woken thread starts at once on another core. Emulate that by running it
       next: otherwise the waker keeps going and, for example, a main thread that posted a job
       ends up running the job itself (with its own, different, current heap). */
    if (t != e->cur && res == RES_OK) { e->pref_thread = t + 1; e->quantum_left = 0; }
}

static void block(mgr_emu *e, int kind, uint64_t timeout_ns) {
    kthread *th = &e->th[e->cur];
    th->state = TH_WAIT;
    th->wait_kind = kind;
    if ((int64_t)timeout_ns < 0) th->wake_tick = UINT64_MAX;
    else th->wake_tick = e->ticks + (uint64_t)(((unsigned __int128)timeout_ns * TICK_HZ + 999999999u) / 1000000000u) + 1;
    e->quantum_left = 0;
}

static int thread_new(mgr_emu *e, uint64_t entry, uint64_t arg, uint64_t stack_top, int prio) {
    int t = -1;
    for (int k = 0; k < MAX_THREADS && t < 0; k++) if (e->th[k].state == TH_FREE) t = k;
    for (int k = 0; k < MAX_THREADS && t < 0; k++) if (e->th[k].state == TH_DEAD) t = k;   /* reuse exited threads' slots */
    if (t >= 0) {
        if (find_region(e, TLS_BASE + (uint64_t)t * 0x200)) { uint8_t *tl = mem_ptr(e, TLS_BASE + (uint64_t)t * 0x200, 0x200); if (tl) memset(tl, 0, 0x200); }
        kthread *th = &e->th[t];
        memset(th, 0, sizeof *th);
        th->state = TH_CREATED;
        th->pc = entry; th->x[0] = arg; th->sp = stack_top & ~15ull;
        th->priority = prio;
        th->id = e->next_thread_id++;
        th->tpidrro = TLS_BASE + (uint64_t)t * 0x200;
        th->handle = handle_new(e, KO_THREAD, t);
        /* newer SDKs read the current thread's handle from TLS+0x110 (mutex owner tag) */
        if (find_region(e, th->tpidrro)) mwr(e, th->tpidrro + 0x110, 4, th->handle);
        return t;
    }
    return -1;
}

/* Pick the next runnable thread (round robin, higher priority = lower number first). */
static int schedule(mgr_emu *e) {
    /* A GPU channel stalled on a semaphore acquire waits for memory the CPU (or another
       channel) writes. Games often spin on GPU progress in plain memory without any system
       call, so retry stalled channels at every scheduling point too. */
    for (int i = 0; i < e->nx2_nch; i++) if (e->nx2_ch[i].stalled) { nx2_gpu_poll(e); break; }
    /* audio renderer: the DSP finishes a frame every 5 ms and signals the renderer's event */
    if (e->aud_nevents && e->ticks >= e->aud_next_tick) {
        for (int k = 0; k < e->aud_nevents; k++) event_signal(e, e->aud_event[k]);
        e->aud_next_tick = e->ticks + TICK_HZ / 200;
    }
    /* display vsync (Switch 2 vi2 native events), 60 Hz in emulated time */
    if (e->nx2_vsync_mask && e->ticks >= e->nx2_next_vsync) {
        for (int k = 0; k < 16; k++)
            if ((e->nx2_vsync_mask >> k) & 1 && e->nx2_vi_ev[k]) event_signal(e, e->nx2_vi_ev[k] - 1);
        e->nx2_vsyncs++;
        hid_update(e);
        if (e->nx2_vsyncs % 600 == 0)                               /* every 10 s of game time */
            elog(e, "[status] %llu s: %llu frames presented, %llu svcs, %llu GPU submits, %llu draws\n",
                 (unsigned long long)(e->nx2_vsyncs / 60), (unsigned long long)e->nx2_presents, (unsigned long long)e->svc_calls,
                 (unsigned long long)e->nx2_submits, (unsigned long long)e->nx2_draws);
        e->nx2_next_vsync += TICK_HZ / 60;
        if (e->nx2_next_vsync <= e->ticks) e->nx2_next_vsync = e->ticks + TICK_HZ / 60;
    }
    /* wake timed-out sleepers */
    for (int t = 0; t < MAX_THREADS; t++) {
        kthread *th = &e->th[t];
        if (th->state == TH_WAIT && th->wake_tick != UINT64_MAX && e->ticks >= th->wake_tick) {
            /* (a timed-out condition-variable wait returns without the mutex: the SDK takes it again) */
            wake(e, t, th->wait_kind == WAIT_SLEEP ? RES_OK : RES_TIMEOUT, 0, 0);
        }
    }
    int best = -1, best_prio = 1 << 30;
    /* The console runs a game's threads on several cores, so a busy high-priority thread
       does not starve the others. With one host thread we approximate that: every EMU_CORES-th
       slice goes to the highest-priority ready thread, the others round-robin over all ready
       threads regardless of priority (the "other cores"); a thread just woken by another runs next. */
    e->sched_slot++;
    int fair = (e->sched_slot % EMU_CORES) != 0;
    if (e->pref_thread) {
        int p = e->pref_thread - 1;
        e->pref_thread = 0;
        if (e->th[p].state == TH_READY) { best = p; goto picked; }
    }
    for (int k = 1; k <= MAX_THREADS; k++) {
        int t = (e->cur + k) % MAX_THREADS;
        if (e->th[t].state != TH_READY) continue;
        if (fair) { best = t; break; }
        if (e->th[t].priority < best_prio) { best = t; best_prio = e->th[t].priority; }
    }
picked:
    if (best < 0) {
        /* nobody runnable: fast-forward time to the next timed wake-up */
        uint64_t next = UINT64_MAX;
        for (int t = 0; t < MAX_THREADS; t++)
            if (e->th[t].state == TH_WAIT && e->th[t].wake_tick < next) next = e->th[t].wake_tick;
        if (e->nx2_vsync_mask && e->nx2_next_vsync < next) next = e->nx2_next_vsync;
        if (e->aud_nevents && e->aud_next_tick < next) next = e->aud_next_tick;
        if (next == UINT64_MAX) return -1;
        if (e->hvf) hvf_sleep_until(e, next);     /* native CPU: the guest reads the real clock, so wait */
        else e->ticks = next;
        return schedule(e);
    }
    if (best != e->cur) {
        if (e->hvf) hvf_pull_full(e);             /* native CPU: fetch SIMD/TLS state before saving */
        if (e->th[e->cur].state != TH_FREE) save_ctx(e, &e->th[e->cur]);
        e->cur = best;
        load_ctx(e, &e->th[best]);
        if (e->hvf) hvf_mark_dirty(e);
    }
    e->quantum_left = QUANTUM;
    return best;
}

/* ---------------- mutex / condvar / address arbiter ---------------- */
static int thread_by_handle(mgr_emu *e, uint32_t h) {
    khandle *k = handle_get(e, h);
    return (k && k->type == KO_THREAD) ? k->idx : -1;
}

static void mutex_release(mgr_emu *e, uint64_t addr) {
    int next = -1, waiters = 0;
    for (int t = 0; t < MAX_THREADS; t++)
        if (e->th[t].state == TH_WAIT && e->th[t].wait_kind == WAIT_MUTEX && e->th[t].wait_addr == addr) {
            if (next < 0) next = t; else waiters++;
        }
    if (next < 0) { mwr(e, addr, 4, 0); return; }
    mwr(e, addr, 4, e->th[next].wait_tag | (waiters ? WAITERS_BIT : 0));
    wake(e, next, RES_OK, 0, 0);
}

static void svc_arbitrate_lock(mgr_emu *e) {
    uint32_t owner = (uint32_t)e->x[0], tag = (uint32_t)e->x[2];
    uint64_t addr = e->x[1];
    uint32_t v = (uint32_t)mrd(e, addr, 4);
    if ((v & ~WAITERS_BIT) != owner) { e->x[0] = RES_OK; return; }  /* already released */
    mwr(e, addr, 4, v | WAITERS_BIT);
    kthread *th = &e->th[e->cur];
    th->wait_addr = addr; th->wait_tag = tag;
    e->x[0] = RES_OK;
    block(e, WAIT_MUTEX, (uint64_t)-1);
}

static void svc_wait_condvar(mgr_emu *e) {
    uint64_t mutex = e->x[0], key = e->x[1], timeout = e->x[3];
    uint32_t tag = (uint32_t)e->x[2];
    mutex_release(e, mutex);
    mwr(e, key, 4, 1);
    kthread *th = &e->th[e->cur];
    th->wait_addr = key; th->wait_mutex = mutex; th->wait_tag = tag;
    e->x[0] = RES_OK;
    block(e, WAIT_CONDVAR, timeout);
}

static void svc_signal_condvar(mgr_emu *e) {
    uint64_t key = e->x[0];
    int64_t count = (int32_t)e->x[1];
    int woke = 0, remaining = 0;
    for (int t = 0; t < MAX_THREADS; t++) {
        kthread *th = &e->th[t];
        if (th->state != TH_WAIT || th->wait_kind != WAIT_CONDVAR || th->wait_addr != key) continue;
        if (count > 0 && woke >= count) { remaining++; continue; }
        woke++;
        /* try to re-acquire the mutex for the woken thread */
        uint32_t mv = (uint32_t)mrd(e, th->wait_mutex, 4);
        if (mv == 0) { mwr(e, th->wait_mutex, 4, th->wait_tag); wake(e, t, RES_OK, 0, 0); }
        else {
            mwr(e, th->wait_mutex, 4, mv | WAITERS_BIT);
            th->wait_kind = WAIT_MUTEX;
            th->wait_addr = th->wait_mutex;
            th->wake_tick = UINT64_MAX;
            *treg(e, t, 0) = RES_OK;
        }
    }
    if (!remaining) mwr(e, key, 4, 0);
    e->x[0] = RES_OK;
}

static void svc_wait_for_address(mgr_emu *e) {
    uint64_t addr = e->x[0], timeout = e->x[3];
    int type = (int)e->x[1];
    int32_t value = (int32_t)e->x[2];
    int32_t cur = (int32_t)mrd(e, addr, 4);
    int wait;
    if (type == 0) wait = cur < value;
    else if (type == 1) { wait = cur < value; if (wait) mwr(e, addr, 4, (uint32_t)(cur - 1)); }
    else wait = cur == value;
    if (!wait) { e->x[0] = RES_INVALID_STATE; return; }
    if (timeout == 0) { e->x[0] = RES_TIMEOUT; return; }
    e->th[e->cur].wait_addr = addr;
    e->x[0] = RES_OK;
    block(e, WAIT_ADDRESS, timeout);
}

/* Memory a thread may wait on (WaitForAddress) changed outside the CPU: let waiters recheck. */
void kernel_address_changed(mgr_emu *e, uint64_t addr) {
    for (int t = 0; t < MAX_THREADS; t++)
        if (e->th[t].state == TH_WAIT && e->th[t].wait_kind == WAIT_ADDRESS && (e->th[t].wait_addr & ~7ull) == (addr & ~7ull))
            wake(e, t, RES_OK, 0, 0);
}

static void svc_signal_to_address(mgr_emu *e) {
    uint64_t addr = e->x[0];
    int type = (int)e->x[1];
    int32_t value = (int32_t)e->x[2];
    int32_t count = (int32_t)e->x[3];
    int waiting = 0;
    for (int t = 0; t < MAX_THREADS; t++)
        if (e->th[t].state == TH_WAIT && e->th[t].wait_kind == WAIT_ADDRESS && e->th[t].wait_addr == addr) waiting++;
    if (type == 1 || type == 2) {
        int32_t cur = (int32_t)mrd(e, addr, 4);
        if (cur != value) { e->x[0] = RES_INVALID_STATE; return; }
        /* type 2 (signal and modify by waiting count): no waiters +1; waking all (count <= 0) -2;
           waking every waiter -1; leaving some waiting: unchanged (Horizon 7.0.0+ rules) */
        int32_t nv = type == 1 ? value + 1 : (waiting == 0 ? value + 1 : count <= 0 ? value - 2 : waiting <= count ? value - 1 : value);
        mwr(e, addr, 4, (uint32_t)nv);
    }
    int woke = 0;
    for (int t = 0; t < MAX_THREADS && (count <= 0 || woke < count); t++)
        if (e->th[t].state == TH_WAIT && e->th[t].wait_kind == WAIT_ADDRESS && e->th[t].wait_addr == addr) {
            wake(e, t, RES_OK, 0, 0); woke++;
        }
    e->x[0] = RES_OK;
}

/* ---------------- SVC dispatch ---------------- */
static const char *svc_name(uint32_t n) {
    switch (n) {
    case 0x01: return "SetHeapSize"; case 0x02: return "SetMemoryPermission"; case 0x03: return "SetMemoryAttribute";
    case 0x04: return "MapMemory"; case 0x05: return "UnmapMemory"; case 0x06: return "QueryMemory";
    case 0x07: return "ExitProcess"; case 0x08: return "CreateThread"; case 0x09: return "StartThread";
    case 0x0A: return "ExitThread"; case 0x0B: return "SleepThread"; case 0x0C: return "GetThreadPriority";
    case 0x0D: return "SetThreadPriority"; case 0x0E: return "GetThreadCoreMask"; case 0x0F: return "SetThreadCoreMask";
    case 0x10: return "GetCurrentProcessorNumber"; case 0x11: return "SignalEvent"; case 0x12: return "ClearEvent";
    case 0x13: return "MapSharedMemory"; case 0x14: return "UnmapSharedMemory"; case 0x15: return "CreateTransferMemory";
    case 0x16: return "CloseHandle"; case 0x17: return "ResetSignal"; case 0x18: return "WaitSynchronization";
    case 0x19: return "CancelSynchronization"; case 0x1A: return "ArbitrateLock"; case 0x1B: return "ArbitrateUnlock";
    case 0x1C: return "WaitProcessWideKeyAtomic"; case 0x1D: return "SignalProcessWideKey"; case 0x1E: return "GetSystemTick";
    case 0x1F: return "ConnectToNamedPort"; case 0x21: return "SendSyncRequest"; case 0x22: return "SendSyncRequestWithUserBuffer";
    case 0x24: return "GetProcessId"; case 0x25: return "GetThreadId"; case 0x26: return "Break";
    case 0x27: return "OutputDebugString"; case 0x28: return "ReturnFromException"; case 0x29: return "GetInfo";
    case 0x2C: return "MapPhysicalMemory"; case 0x2D: return "UnmapPhysicalMemory"; case 0x32: return "SetThreadActivity";
    case 0x33: return "GetThreadContext3"; case 0x34: return "WaitForAddress"; case 0x35: return "SignalToAddress";
    case 0x36: return "SynchronizePreemptionState"; case 0x45: return "CreateEvent"; case 0x80: return "SetGpuMemoryAttribute";
    default: return "?";
    }
}

void do_svc(mgr_emu *e, uint32_t n) {
    e->svc_calls++;
    e->svc_hist[n & 0xFF]++;
    e->last_svc = n;
    uint64_t *x = e->x;
    /* synchronization and clock calls are the bulk while threads work: not logged unless asked */
    int sync_call = n == 0x1A || n == 0x1B || n == 0x1C || n == 0x1D || n == 0x34 || n == 0x35 || n == 0x0B || n == 0x18 || n == 0x10;
    if (e->svc_log_budget && n != 0x27 && n != 0x1E && (!sync_call || getenv("MGR_SVC_LOG_ALL"))) {
        e->svc_log_budget--;
        elog(e, "[svc] t%d 0x%02X %-24s x0=%llx x1=%llx x2=%llx x3=%llx\n", e->cur, n, svc_name(n),
             (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3]);
    }
    switch (n) {
    case 0x01: { /* SetHeapSize */
        uint64_t size = x[1];
        if (size > HEAP_MAX) { x[0] = 0xD401; return; }
        region *h = find_region(e, HEAP_BASE);
        if (!h && size) {
            h = add_region_reserve(e, HEAP_BASE, size, HEAP_MAX, 3, 5, "heap");   /* reserve the whole heap range once */
            if (!h) { x[0] = 0xD001; return; }
        } else if (h && size > h->map_size) { x[0] = 0xD001; return; }
        else if (h && size) {
            if (size < h->size) memset(h->mem + size, 0, h->size - size);       /* shrunk pages read zero if regrown */
            h->size = size;
            mem_tlb_flush(e);
        }
        e->heap_size = size;
        x[0] = RES_OK; x[1] = HEAP_BASE;
        return;
    }
    case 0x02: case 0x03: case 0x05: case 0x14: x[0] = RES_OK; return;
    case 0x04: { /* MapMemory(dst, src, size): dst becomes a second view of src's memory */
        region *src = find_region(e, x[1]);
        if (!src) { x[0] = 0xD401; return; }
        uint64_t off = x[1] - src->base;
        if (src->alias_of >= 0) { off += src->alias_off; src = &e->reg[src->alias_of]; }   /* view of a view */
        region *dst = add_region_alias(e, x[0], x[2], src, off, "stack-alias");
        x[0] = dst ? RES_OK : 0xD401;
        return;
    }
    case 0x06: { /* QueryMemory */
        uint64_t info = x[0], addr = x[2];
        region *r = find_region(e, addr);
        uint64_t base, size; uint32_t state = 0, perm = 0;
        if (r) { base = r->base; size = r->size; state = r->state; perm = r->perm; }
        else {
            uint64_t lo = 0, hi = ASPACE_BASE + ASPACE_SIZE;
            for (int k = 0; k < e->nreg; k++) {
                uint64_t end = e->reg[k].base + e->reg[k].size;
                if (end <= addr && end > lo) lo = end;
                if (e->reg[k].base > addr && e->reg[k].base < hi) hi = e->reg[k].base;
            }
            base = lo; size = hi - lo;
        }
        mwr(e, info, 8, base); mwr(e, info + 8, 8, size); mwr(e, info + 16, 4, state);
        mwr(e, info + 20, 4, 0); mwr(e, info + 24, 4, perm); mwr(e, info + 28, 4, 0);
        mwr(e, info + 32, 4, 0); mwr(e, info + 36, 4, 0);
        x[0] = RES_OK; x[1] = 0;
        return;
    }
    case 0x07: emu_stop(e, MGR_EMU_EXITED, "guest called svcExitProcess"); return;
    case 0x08: { /* CreateThread(entry x1, arg x2, stack x3, prio x4, core x5) */
        int t = thread_new(e, x[1], x[2], x[3], (int)x[4]);
        if (t < 0) { x[0] = 0xD601; return; }
        {   /* ideal core: -2 = the process default (core 0) */
            int core = (int)(int32_t)x[5];
            e->th[t].core = core >= 0 && core < 6 ? core : 0;
            e->th[t].core_mask = 1ull << e->th[t].core;
        }
        elog(e, "[kern] thread %d created: entry %llx prio %d core %d\n", t, (unsigned long long)x[1], (int)x[4], e->th[t].core);
        x[0] = RES_OK; x[1] = e->th[t].handle;
        return;
    }
    case 0x09: {
        int t = thread_by_handle(e, (uint32_t)x[0]);
        if (t < 0) { x[0] = RES_INVALID_HANDLE; return; }
        if (e->th[t].state == TH_CREATED) e->th[t].state = TH_READY;
        x[0] = RES_OK;
        return;
    }
    case 0x0A: { /* ExitThread */
        e->th[e->cur].state = TH_DEAD;
        e->quantum_left = 0;
        for (int t = 0; t < MAX_THREADS; t++) {   /* wake joiners */
            kthread *th = &e->th[t];
            if (th->state != TH_WAIT || th->wait_kind != WAIT_SYNC) continue;
            for (int k = 0; k < th->wait_count; k++)
                if (handle_signaled(e, th->wait_handles[k])) { wake(e, t, RES_OK, 1, k); break; }
        }
        return;
    }
    case 0x0B: { /* SleepThread */
        int64_t ns = (int64_t)x[0];
        x[0] = RES_OK;
        if (ns <= 0) { e->quantum_left = 0; return; }  /* yield */
        block(e, WAIT_SLEEP, (uint64_t)ns);
        return;
    }
    case 0x0C: { int t = thread_by_handle(e, (uint32_t)x[1]); x[0] = RES_OK; x[1] = (uint64_t)(t >= 0 ? e->th[t].priority : 44); return; }
    case 0x0D: { int t = thread_by_handle(e, (uint32_t)x[0]); if (t >= 0) e->th[t].priority = (int)x[1]; x[0] = RES_OK; return; }
    /* Cores: games pin worker threads to cores and pick per-core job queues by asking which core
       they run on, so each thread reports the core it was assigned (we run them all on one). */
    case 0x0E: {   /* GetThreadCoreMask(handle) -> ideal core, affinity mask */
        int t = thread_by_handle(e, (uint32_t)x[2]);
        if (t < 0 && (uint32_t)x[2] == 0xFFFF8000u) t = e->cur;
        x[0] = RES_OK; x[1] = t >= 0 ? (uint64_t)e->th[t].core : 0; x[2] = t >= 0 && e->th[t].core_mask ? e->th[t].core_mask : 1;
        return;
    }
    case 0x0F: {   /* SetThreadCoreMask(handle, ideal core, mask): -1 keeps the ideal core, -3 = process default */
        int t = thread_by_handle(e, (uint32_t)x[0]);
        if (t < 0 && (uint32_t)x[0] == 0xFFFF8000u) t = e->cur;
        if (t >= 0) {
            int core = (int)(int32_t)x[1];
            if (core == -3) core = 0;
            if (core >= 0 && core < 6) e->th[t].core = core;
            if (x[2]) e->th[t].core_mask = x[2];
            /* A thread only runs on cores in its mask: when the ideal core is kept (-1) but is not
               in the new mask, it moves to the highest core of the mask (as Horizon does). Games
               keep per-core work buffers indexed by GetCurrentProcessorNumber, so reporting core 0
               for workers pinned to other cores made them share (and corrupt) one buffer. */
            uint64_t m = e->th[t].core_mask & 0x3F;
            if (m && !((m >> e->th[t].core) & 1)) { int c = 5; while (c > 0 && !((m >> c) & 1)) c--; e->th[t].core = c; }
        }
        x[0] = RES_OK; return;
    }
    case 0x10: x[0] = (uint64_t)e->th[e->cur].core; return;   /* GetCurrentProcessorNumber */
    case 0x11: case 0x12: case 0x17: {
        khandle *k = handle_get(e, (uint32_t)x[0]);
        if (!k || k->type != KO_EVENT) { x[0] = n == 0x17 ? RES_OK : RES_INVALID_HANDLE; return; }
        if (n == 0x11) event_signal(e, k->idx); else if (!e->events[k->idx].sticky) e->events[k->idx].signaled = 0;
        x[0] = RES_OK;
        return;
    }
    case 0x13: { /* MapSharedMemory(handle, addr, size, perm): back it with zeroed guest memory */
        region *r = find_region(e, x[1]);
        if (!r) r = add_region(e, x[1], (x[2] + 0xFFF) & ~0xFFFull, (uint32_t)x[3] & 7, 0x8, "shared");
        khandle *k = handle_get(e, (uint32_t)x[0]);
        if (r && k && k->type == KO_OTHER && k->idx == SHMEM_TIME && x[2] >= 0x1000) time_shmem_fill(e, x[1]);
        if (r && k && k->type == KO_OTHER && k->idx == SHMEM_HID && x[2] >= 0x3E210) hid_shmem_fill(e, x[1]);
        x[0] = r ? RES_OK : 0xD401;
        return;
    }
    case 0x15: { /* CreateTransferMemory(addr x1, size x2, perm x3): remember the range for services */
        int t = e->ntmem < MAX_TMEM ? e->ntmem++ : MAX_TMEM - 1;
        e->tmem[t].addr = x[1]; e->tmem[t].size = x[2];
        x[0] = RES_OK; x[1] = handle_new(e, KO_TMEM, t);
        return;
    }
    case 0x16: {
        uint32_t h = (uint32_t)x[0];
        if (h >= HANDLE_BASE && h < HANDLE_BASE + MAX_HANDLES) {
            khandle *k = &e->handles[h - HANDLE_BASE];
            if (k->type == KO_THREAD && e->th[k->idx].state == TH_DEAD) e->th[k->idx].state = TH_FREE;
            k->type = KO_FREE;
        }
        x[0] = RES_OK;
        return;
    }
    case 0x18: { /* WaitSynchronization(handles x1, count x2, timeout x3) */
        int count = (int)x[2];
        if (count < 0 || count > 64) { x[0] = RES_OUT_OF_RANGE; return; }
        kthread *th = &e->th[e->cur];
        th->wait_count = count;
        if (e->nx2_nch) nx2_gpu_poll(e);      /* the CPU waiting is a good moment to let stalled GPU channels run */
        for (int k = 0; k < count; k++) {
            th->wait_handles[k] = (uint32_t)mrd(e, x[1] + (uint64_t)k * 4, 4);
            if (handle_signaled(e, th->wait_handles[k])) { x[0] = RES_OK; x[1] = (uint64_t)k; return; }
        }
        if (x[3] == 0) { x[0] = RES_TIMEOUT; return; }
        if (th->cancel_pending) { th->cancel_pending = 0; x[0] = RES_CANCELLED; return; }   /* (Horizon's order) */
        x[0] = RES_OK;
        block(e, WAIT_SYNC, x[3]);
        return;
    }
    case 0x19: {
        int t = thread_by_handle(e, (uint32_t)x[0]);
        /* As on Horizon: a thread that is not waiting yet keeps the request, and its next
           WaitSynchronization returns Cancelled at once (nn::os MultiWait wakes waiters this way
           when a user-mode object is signalled; dropping it loses the wake-up). */
        if (t >= 0 && e->th[t].state == TH_WAIT && e->th[t].wait_kind == WAIT_SYNC) wake(e, t, RES_CANCELLED, 0, 0);
        else if (t >= 0) e->th[t].cancel_pending = 1;
        x[0] = RES_OK;
        return;
    }
    case 0x1A: svc_arbitrate_lock(e); return;
    case 0x1B: mutex_release(e, x[0]); x[0] = RES_OK; return;
    case 0x1C: svc_wait_condvar(e); return;
    case 0x1D: svc_signal_condvar(e); return;
    case 0x1E: x[0] = e->ticks; return;
    case 0x1F: { /* ConnectToNamedPort */
        char name[16] = {0};
        for (int k = 0; k < 12; k++) { uint64_t c = mrd(e, x[1] + (uint64_t)k, 1); if (!c) break; name[k] = (char)c; }
        uint32_t h = 0;
        x[0] = ipc_connect_named_port(e, name, &h);
        x[1] = h;
        return;
    }
    case 0x21: case 0x22: x[0] = ipc_send_sync_request(e, (uint32_t)(n == 0x21 ? x[0] : x[2])); return;
    case 0x24: x[0] = RES_OK; x[1] = 0x51; return;
    case 0x25: { int t = thread_by_handle(e, (uint32_t)x[1]); x[0] = RES_OK; x[1] = t >= 0 ? e->th[t].id : 0; return; }
    case 0x26:
        if (x[0] & 0x80000000ull) { x[0] = RES_OK; return; }  /* notification only */
        if (x[2] == 4 && find_region(e, x[1])) {
            uint32_t rc = (uint32_t)mrd(e, x[1], 4);
            emu_stop(e, MGR_EMU_BREAK, "guest aborted with result 0x%x (module %u, description %u) at pc %llx, lr %llx",
                     rc, rc & 0x1FF, (rc >> 9) & 0x1FFF, (unsigned long long)e->pc, (unsigned long long)e->x[30]);
        } else
            emu_stop(e, MGR_EMU_BREAK, "guest called svcBreak(reason 0x%llx, info %llx)", (unsigned long long)x[0], (unsigned long long)x[1]);
        return;
    case 0x27: {
        uint64_t len = x[1] > 4096 ? 4096 : x[1];
        uint8_t *p = len ? mem_ptr(e, x[0], len) : NULL;
        if (p) {
            char *s = malloc(len + 1);
            if (s) { memcpy(s, p, len); s[len] = 0; elog(e, "[guest] %s%s", s, s[len - 1] == '\n' ? "" : "\n"); free(s); }
        }
        x[0] = RES_OK;
        return;
    }
    case 0x29: { /* GetInfo(id0 x1, handle x2, id1 x3) */
        uint64_t id0 = x[1], v = 0;
        region *h = find_region(e, HEAP_BASE);
        switch (id0) {
        case 0: v = 0x3F; break;                  /* core mask: cores 0-5 */
        case 1: v = 0xFFFFFFFFFFFFFFFFull; break;
        case 2: v = ALIAS_BASE; break;
        case 3: v = ALIAS_SIZE; break;
        case 4: v = HEAP_BASE; break;
        case 5: v = HEAP_MAX; break;
        case 6: case 21:                          /* total memory: Switch 2 titles are told ~9.5 GiB, Switch 1 ~3.2 GiB */
            v = (mgr_info(e->game)->platform == MGR_PLATFORM_SWITCH2) ? 0x264000000ull : (3ull << 30) + (200ull << 20); break;
        case 7: case 22: v = (h ? h->size : 0) + (16u << 20); break;
        case 8: v = 0; break;
        case 11: v = 0x0123456789ABCDEFull ^ (x[3] * 0x9E3779B97F4A7C15ull); break;
        case 12: v = ASPACE_BASE; break;
        case 13: v = ASPACE_SIZE; break;
        case 14: v = STACK_BASE; break;
        case 15: v = 0x10000000; break;
        case 16: v = e->system_resource_size; break;          /* personal system resource (enables VAMM / MapPhysicalMemory) */
        case 17: v = e->system_resource_size ? 0x10000 + e->physmem_bytes / 0x200 : 0; break;
        case 18: v = e->program_id; break;
        case 20:                                  /* UserExceptionContextAddress: a page the SDK keeps exception state in */
            if (!find_region(e, 0x3F000000ull)) add_region(e, 0x3F000000ull, 0x1000, 3, 4, "exception-context");
            v = 0x3F000000ull; break;
        case 0xF0000002: v = 0; break;
        case 23: v = 1; break;                    /* IsApplication */
        case 24: v = 0x60; break;                 /* FreeThreadCount */
        case 25: v = e->ticks; break;             /* ThreadTickCount */
        case 26: v = 1; break;                    /* IsSvcPermitted */
        case 27: case 28: v = 0; break;           /* IoRegionHint, AliasRegionExtraSize */
        case 33: v = 0x1000; break;               /* memory mapping granularity (page size), Switch 2 SDK */
        default:                                  /* newer (Switch 2) info types: answer 0 and log */
            elog(e, "[kern] GetInfo id %llu (sub %llu) unknown, returning 0\n", (unsigned long long)id0, (unsigned long long)x[3]);
            v = 0; break;
        }
        x[0] = RES_OK; x[1] = v;
        return;
    }
    case 0x2C: {   /* MapPhysicalMemory(addr, size): back pages in the alias region */
        if (!x[1] || (x[0] | x[1]) & 0xFFF) { x[0] = 0xCC01; return; }
        if (x[0] >= ALIAS_BASE && x[0] + x[1] <= ALIAS_BASE + ALIAS_SIZE) {
            /* The whole alias region is reserved once (host pages are committed on first touch),
               so mapping is bookkeeping; this also covers overlapping and growing requests. */
            if (!e->alias_reg) {
                region *r = add_region_reserve(e, ALIAS_BASE, ALIAS_SIZE, ALIAS_SIZE, 3, 6, "physmem");
                if (!r) { x[0] = 0xD401; return; }
                e->alias_reg = 1;
            }
            e->physmem_bytes += x[1];
            if (e->watch_hi && e->watch_lo >= x[0] && e->watch_lo < x[0] + x[1])
                elog(e, "[watch] t%d MapPhysicalMemory %llx +%llx covers the watched address\n", e->cur,
                     (unsigned long long)x[0], (unsigned long long)x[1]);
            x[0] = RES_OK; return;
        }
        {   static uint64_t nmap; nmap++;
            if (nmap <= 40 || !(nmap & (nmap - 1)))
                elog(e, "[kern] t%d MapPhysicalMemory %llx +%llx (call %llu)\n", e->cur, (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)nmap); }
        if (find_region(e, x[0])) { x[0] = RES_OK; return; }
        region *r = add_region(e, x[0], x[1], 3, 6, "physmem");
        if (r) e->physmem_bytes += x[1];
        x[0] = r ? RES_OK : 0xD401;
        return;
    }
    case 0x2D: {   /* UnmapPhysicalMemory: release the host pages (they read as zero if mapped again) */
        {   static uint64_t nun; nun++;
            if (nun <= 40 || !(nun & (nun - 1)))
                elog(e, "[kern] t%d UnmapPhysicalMemory %llx +%llx (call %llu)\n", e->cur, (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)nun); }
        if (e->watch_hi && e->watch_lo >= x[0] && e->watch_lo < x[0] + x[1])
            elog(e, "[watch] t%d UnmapPhysicalMemory %llx +%llx covers the watched address\n", e->cur,
                 (unsigned long long)x[0], (unsigned long long)x[1]);
        if (e->alias_reg && x[0] >= ALIAS_BASE && x[0] + x[1] <= ALIAS_BASE + ALIAS_SIZE && !(x[0] & 0xFFF)) {
            uint8_t *p = mem_ptr(e, x[0], 1);
            WATCH(e, x[0], x[1], "UnmapPhysicalMemory");
            if (p && e->hvf) memset(p, 0, x[1] & ~0xFFFull);   /* the VM maps these pages: zero, don't replace */
            else if (p) guest_mem_discard(p, x[1] & ~0xFFFull);
            if (e->physmem_bytes >= x[1]) e->physmem_bytes -= x[1];
        }
        x[0] = RES_OK; return;
    }
    case 0x32: x[0] = RES_OK; return;
    case 0x34: svc_wait_for_address(e); return;
    case 0x35: svc_signal_to_address(e); return;
    case 0x36: x[0] = RES_OK; return;
    case 0x80:   /* Switch 2 SetGpuMemoryAttribute(addr, size, mask, attr): no GPU MMU to update yet */
        if (e->ipc_log_budget) elog(e, "[kern] SetGpuMemoryAttribute %llx +%llx mask %llx attr %llx\n",
            (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3]);
        x[0] = RES_OK; return;
    case 0x45: { int ev = event_new(e, 0); x[0] = RES_OK; x[1] = handle_new(e, KO_EVENT, ev); x[2] = handle_new(e, KO_EVENT, ev); return; }
    default:
        elog(e, "[kern] svc 0x%02X (%s) not emulated, returning error\n", n, svc_name(n));
        x[0] = RES_NOT_IMPL;
        return;
    }
}

/* ---------------- public API ---------------- */
/* Map a module like the real loader: .text R-X (state Code), .rodata R-- (Code),
   .data+.bss RW- (CodeData), each as its own memory region. */
static int map_module(mgr_emu *e, uint64_t base, const mgr_module *m, const char *name) {
    struct { uint64_t off, end; uint32_t perm, state; const char *suffix; } seg[3] = {
        { m->text_off, m->ro_off,   5, 3, ".text" },
        { m->ro_off,   m->data_off, 1, 3, ".rodata" },
        { m->data_off, m->image_size, 3, 4, ".data" },
    };
    for (int k = 0; k < 3; k++) {
        uint64_t sz = seg[k].end > seg[k].off ? seg[k].end - seg[k].off : 0;
        sz = (sz + 0xFFF) & ~0xFFFull;
        if (!sz) continue;
        char rn[24];
        snprintf(rn, sizeof rn, "%.14s%s", name, seg[k].suffix);
        region *rg = add_region(e, base + seg[k].off, sz, seg[k].perm, seg[k].state, rn);
        if (!rg) return -1;
        uint64_t n = seg[k].off + sz <= m->image_size ? sz : m->image_size - seg[k].off;
        memcpy(rg->mem, m->image + seg[k].off, n);
    }
    return 0;
}

static const char *module_order[] = { "rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
                                      "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk" };

mgr_emu *mgr_emu_create(mgr_game *g, char *err, size_t errlen) {
    const mgr_game_info *in = mgr_info(g);
    if (!in->exefs_count) { mgr_seterr(err, errlen, in->note[0] ? in->note : "game has no program (ExeFS) to run"); return NULL; }
    mgr_emu *e = calloc(1, sizeof *e);
    if (!e) { mgr_seterr(err, errlen, "out of memory"); return NULL; }
    e->game = g;
    e->svc_log_budget = getenv("MGR_SVC_LOG_ALL") ? (uint64_t)-1 : 3000;
    if (getenv("MGR_WATCH")) {
        e->watch_lo = strtoull(getenv("MGR_WATCH"), NULL, 16);
        e->watch_hi = e->watch_lo + (getenv("MGR_WATCH_LEN") ? strtoull(getenv("MGR_WATCH_LEN"), NULL, 0) : 8);
    }
    e->ipc_log_budget = getenv("MGR_IPC_LOG") ? atoi(getenv("MGR_IPC_LOG")) : 3000;
    e->program_id = in->title_id;
    e->next_thread_id = 1;
    { const char *bp = getenv("MGR_BREAK"); if (bp) e->break_pc = strtoull(bp, NULL, 16); }
    { const char *ba = getenv("MGR_BREAK_AFTER"); if (ba) e->break_after = strtoull(ba, NULL, 10); }   /* instruction count */
    elog(e, "MikesGameRoom core %s - AArch64 interpreter + HLE kernel\n", mgr_version());
    elog(e, "Title %016llX (%s)\n", (unsigned long long)in->title_id, in->name);

    uint32_t stack_size = 0x100000;
    int main_prio = 44;
    uint8_t *buf; size_t len;
    if (mgr_exefs_read(g, "main.npdm", &buf, &len) == 0) {
        mgr_npdm np;
        if (npdm_parse(buf, len, &np) == 0) {
            elog(e, "NPDM: \"%s\" program %016llX, %s, stack 0x%x, priority %d, ACID 0x%x bytes%s\n", np.name,
                 (unsigned long long)np.program_id, np.is_64bit ? "64-bit" : "32-bit", np.main_stack_size,
                 np.main_priority, np.acid_size, in->platform == MGR_PLATFORM_SWITCH2 ? " (Switch 2)" : "");
            if (np.main_stack_size >= 0x4000 && np.main_stack_size <= (64u << 20)) stack_size = (np.main_stack_size + 0xFFF) & ~0xFFFu;
            if (np.program_id) e->program_id = np.program_id;
            if (np.main_priority) main_prio = np.main_priority;
            e->system_resource_size = np.system_resource_size;
            if (!np.is_64bit) { free(buf); mgr_seterr(err, errlen, "32-bit programs are not supported"); mgr_emu_destroy(e); return NULL; }
        }
        free(buf);
    }

    uint64_t base = CODE_BASE, entry = 0;
    int loaded = 0;
    if (in->format == MGR_FMT_NRO || in->format == MGR_FMT_NSO) {
        mgr_module m; char merr[128];
        int r = in->format == MGR_FMT_NRO ? nro_load(g->raw, g->raw_len, &m, merr, sizeof merr)
                                          : nso_load(g->raw, g->raw_len, &m, merr, sizeof merr);
        if (r) { mgr_seterr(err, errlen, "%s", merr); mgr_emu_destroy(e); return NULL; }
        if (map_module(e, base, &m, "main")) { module_free(&m); mgr_seterr(err, errlen, "out of memory"); mgr_emu_destroy(e); return NULL; }
        elog(e, "Loaded %-8s @ %016llx size 0x%zx\n", in->format == MGR_FMT_NRO ? "nro" : "nso", (unsigned long long)base, m.image_size);
        entry = base + m.text_off;
        module_free(&m);
        loaded = 1;
    } else {
        for (size_t k = 0; k < sizeof module_order / sizeof *module_order; k++) {
            if (mgr_exefs_read(g, module_order[k], &buf, &len) != 0) continue;
            mgr_module m; char merr[128];
            int r = nso_load(buf, len, &m, merr, sizeof merr);
            free(buf);
            if (r) { elog(e, "skip %s: %s\n", module_order[k], merr); continue; }
            if (map_module(e, base, &m, module_order[k])) { module_free(&m); continue; }
            char id[17]; for (int b = 0; b < 8; b++) snprintf(id + b * 2, 3, "%02X", m.module_id[b]);
            elog(e, "Loaded %-8s @ %016llx size 0x%08zx  module %s...\n", module_order[k], (unsigned long long)base, m.image_size, id);
            if (!entry) entry = base + m.text_off;
            base += (m.image_size + 0x1FFFFF) & ~0x1FFFFFull;
            module_free(&m);
            loaded++;
        }
    }
    if (!loaded) { mgr_seterr(err, errlen, "no loadable program modules in ExeFS"); mgr_emu_destroy(e); return NULL; }

    region *tls = add_region(e, TLS_BASE, TLS_SIZE, 3, 0xC, "tls");
    region *stk = add_region(e, STACK_BASE, stack_size, 3, 0xB, "stack");
    if (!tls || !stk) { mgr_seterr(err, errlen, "out of memory"); mgr_emu_destroy(e); return NULL; }

    int t = thread_new(e, entry, 0, STACK_BASE + stack_size, main_prio);
    e->th[t].x[1] = e->th[t].handle;    /* main thread handle in x1 */
    mwr(e, e->th[t].tpidrro + 0x110, 4, e->th[t].handle);
    e->th[t].state = TH_READY;
    e->cur = t;
    load_ctx(e, &e->th[t]);
    e->quantum_left = QUANTUM;
    e->status = MGR_EMU_READY;
    elog(e, "Entry point %016llx, stack top %016llx\n", (unsigned long long)entry, (unsigned long long)e->sp);
    return e;
}

static void native_stop(mgr_emu *e);

void mgr_emu_destroy(mgr_emu *e) {
    if (!e) return;
    native_stop(e);
    for (int i = 0; i < e->nreg; i++) if (e->reg[i].alias_of < 0) guest_mem_free(e->reg[i].mem, e->reg[i].map_size);
    free(e->log);
    free(e);
}

static mgr_emu_status run_native(mgr_emu *e, uint64_t max) {
    uint64_t start = e->instructions, t_start = hvf_now_ticks(e);
    while (e->instructions - start < max && e->status == MGR_EMU_RUNNING) {
        e->ticks = hvf_now_ticks(e);
        if (e->ticks - t_start > TICK_HZ / 2) break;   /* return at least twice a second, even when the game idles */
        if (e->th[e->cur].state != TH_READY || e->quantum_left == 0) {
            if (schedule(e) < 0) {
                int alive = 0;
                for (int t = 0; t < MAX_THREADS; t++) if (e->th[t].state == TH_WAIT) alive++;
                if (!alive) emu_stop(e, MGR_EMU_EXITED, "all threads exited");
                else emu_stop(e, MGR_EMU_BLOCKED, "all %d guest threads are waiting on events nothing will signal yet "
                              "(usually a service that is not emulated)", alive);
                break;
            }
        }
        e->instructions += hvf_run_slice(e);
    }
    return e->status;
}

/* The native CPU runs on its own thread: a Hypervisor vCPU only works on the thread that created
   it, and callers (the app's dispatch queue) may move between threads. mgr_emu_run hands each
   request to that thread and waits, so callers see the same synchronous API. */
typedef struct {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int ready, init_ok, quit;
    uint64_t request;                 /* instructions to run; 0 = idle */
    int busy;
} native_runner;

static void *native_thread(void *arg) {
    mgr_emu *e = arg;
    native_runner *nr = e->native;
    int ok = hvf_init(e) == 0;
    pthread_mutex_lock(&nr->mu);
    nr->init_ok = ok; nr->ready = 1;
    pthread_cond_broadcast(&nr->cv);
    while (ok && !nr->quit) {
        while (!nr->busy && !nr->quit) pthread_cond_wait(&nr->cv, &nr->mu);
        if (nr->quit) break;
        uint64_t n = nr->request;
        pthread_mutex_unlock(&nr->mu);
        run_native(e, n);
        pthread_mutex_lock(&nr->mu);
        nr->busy = 0;
        pthread_cond_broadcast(&nr->cv);
    }
    if (ok) hvf_destroy(e);           /* the VM goes away on the thread that owns it */
    pthread_mutex_unlock(&nr->mu);
    return NULL;
}

static void native_stop(mgr_emu *e) {
    native_runner *nr = e->native;
    if (!nr) return;
    pthread_mutex_lock(&nr->mu);
    nr->quit = 1;
    pthread_cond_broadcast(&nr->cv);
    pthread_mutex_unlock(&nr->mu);
    pthread_join(nr->thread, NULL);
    pthread_mutex_destroy(&nr->mu);
    pthread_cond_destroy(&nr->cv);
    free(nr);
    e->native = NULL;
}

mgr_emu_status mgr_emu_run(mgr_emu *e, uint64_t max) {
    if (e->status == MGR_EMU_READY) e->status = MGR_EMU_RUNNING;
    /* Native CPU when available (Apple Silicon). MGR_BREAK needs the interpreter; MGR_WATCH works natively. */
    if (!e->hvf_tried) {
        e->hvf_tried = 1;
        if (hvf_available() && !e->break_pc && !getenv("MGR_WATCH_INTERP")) {
            native_runner *nr = calloc(1, sizeof *nr);
            if (nr) {
                pthread_mutex_init(&nr->mu, NULL);
                pthread_cond_init(&nr->cv, NULL);
                e->native = nr;
                pthread_attr_t at;
                pthread_attr_init(&at);
                pthread_attr_setstacksize(&at, 8u << 20);
                if (pthread_create(&nr->thread, &at, native_thread, e) != 0) { free(nr); e->native = NULL; }
                pthread_attr_destroy(&at);
                if (e->native) {
                    pthread_mutex_lock(&nr->mu);
                    while (!nr->ready) pthread_cond_wait(&nr->cv, &nr->mu);
                    int ok = nr->init_ok;
                    pthread_mutex_unlock(&nr->mu);
                    if (!ok) native_stop(e);
                }
            }
        }
    }
    if (e->native) {
        native_runner *nr = e->native;
        pthread_mutex_lock(&nr->mu);
        nr->request = max; nr->busy = 1;
        pthread_cond_broadcast(&nr->cv);
        while (nr->busy) pthread_cond_wait(&nr->cv, &nr->mu);
        pthread_mutex_unlock(&nr->mu);
        return e->status;
    }
    for (uint64_t k = 0; k < max && e->status == MGR_EMU_RUNNING; k++) {
        if (e->th[e->cur].state != TH_READY || e->quantum_left == 0) {
            if (schedule(e) < 0) {
                int alive = 0;
                for (int t = 0; t < MAX_THREADS; t++) if (e->th[t].state == TH_WAIT) alive++;
                if (!alive) emu_stop(e, MGR_EMU_EXITED, "all threads exited");
                else emu_stop(e, MGR_EMU_BLOCKED, "all %d guest threads are waiting on events nothing will signal yet "
                              "(usually a service that is not emulated)", alive);
                break;
            }
        }
        cpu_step(e);
        if (e->quantum_left) e->quantum_left--;
        if (e->watch_host) {                               /* MGR_WATCH: catch changes no logged write explains */
            uint64_t now; memcpy(&now, e->watch_host, 8);
            if (now != e->watch_last) {
                elog(e, "[watch] value changed %llx -> %llx with no logged write: thread %d, pc %llx, last svc 0x%02x\n",
                     (unsigned long long)e->watch_last, (unsigned long long)now, e->cur, (unsigned long long)e->pc, e->last_svc);
                e->watch_last = now;
            }
        }
    }
    return e->status;
}

void mgr_emu_dump_threads(mgr_emu *e) {
    if (!e) return;
    watch_dump(e);
    nx2_gpu_dump(e);
    {                                                              /* where the time goes: busiest SVCs and IPC commands */
        char line[600]; int n = 0;
        for (int k = 0; k < 256; k++) if (e->svc_hist[k] > e->svc_calls / 200) n += snprintf(line + n, sizeof line - n, " %02x:%llu", k, (unsigned long long)e->svc_hist[k]);
        elog(e, "[stats] svcs%s\n", line);
        ipc_dump_stats(e);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        kthread *th = &e->th[t];
        if (th->state == TH_FREE) continue;
        elog(e, "[thread %d] state %d prio %d pc %llx%s\n", t, th->state, th->priority,
             (unsigned long long)(t == e->cur ? e->pc : th->pc), t == e->cur ? " (current)" : "");
        if (th->state == TH_WAIT && th->wait_kind == WAIT_SYNC) {      /* what is it waiting for? */
            for (int k = 0; k < th->wait_count && k < 8; k++) {
                khandle *kh = handle_get(e, th->wait_handles[k]);
                if (!kh) { elog(e, "    waits on %x (bad handle)\n", th->wait_handles[k]); continue; }
                const char *what = kh->type == KO_EVENT ? "event" : kh->type == KO_THREAD ? "thread" :
                                   kh->type == KO_SESSION ? e->svcs[kh->idx].name : "object";
                int sig = kh->type == KO_EVENT ? e->events[kh->idx].signaled : -1;
                const char *owner = "";
                if (kh->type == KO_EVENT)
                    for (int s2 = 1; s2 < MAX_SERVICES; s2++)
                        if (e->svcs[s2].used && e->svcs[s2].event == kh->idx) { owner = e->svcs[s2].name; break; }
                elog(e, "    waits on %x: %s %d%s%s (signaled %d)\n", th->wait_handles[k], what, kh->idx,
                     *owner ? " of " : "", owner, sig);
            }
        } else if (th->state == TH_WAIT)
            elog(e, "    wait kind %d addr %llx\n", th->wait_kind, (unsigned long long)th->wait_addr);
        if (t != e->cur && (t == 0 || getenv("MGR_ALL_BACKTRACES"))) emu_backtrace_from(e, th->pc, th->x[30], th->x[29]);
    }
    for (int r = 0; r < 31; r += 4) {                              /* the current thread's registers */
        char line[200]; int n = 0;
        for (int k = r; k < r + 4 && k < 31; k++) n += snprintf(line + n, sizeof line - n, " x%-2d=%016llx", k, (unsigned long long)e->x[k]);
        elog(e, " %s\n", line);
    }
    for (int k = 19; k < 29; k++) {                                /* what the callee-saved pointers point at */
        region *rg = find_region(e, e->x[k]);
        if (rg && e->x[k] + 8 <= rg->base + rg->size)
            elog(e, "  [x%d] = %08llx (%s+0x%llx)\n", k, (unsigned long long)(mrd(e, e->x[k], 4)), rg->name,
                 (unsigned long long)(e->x[k] - rg->base));
    }
    emu_backtrace(e);
}

int mgr_emu_read_mem(mgr_emu *e, uint64_t addr, void *buf, size_t n) {
    region *r = find_region(e, addr);
    if (!r || addr + n > r->base + r->size) return -1;
    memcpy(buf, r->mem + (addr - r->base), n);
    return 0;
}

void mgr_emu_get_state(const mgr_emu *e, mgr_emu_state *o) {
    memset(o, 0, sizeof *o);
    o->status = e->status;
    o->pc = e->pc;
    o->instructions = e->instructions;
    o->svc_calls = e->svc_calls;
    memcpy(o->x, e->x, sizeof o->x);
    o->sp = e->sp;
    mgr_strlcpy(o->message, e->message, sizeof o->message);
}

size_t mgr_emu_take_log(mgr_emu *e, char *buf, size_t buflen) {
    if (!buflen || !e->log_len) return 0;
    size_t n = e->log_len < buflen - 1 ? e->log_len : buflen - 1;
    memcpy(buf, e->log, n);
    buf[n] = 0;
    memmove(e->log, e->log + n, e->log_len - n);
    e->log_len -= n;
    return n;
}
