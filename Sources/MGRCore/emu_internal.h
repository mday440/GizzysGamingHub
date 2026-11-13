/* Shared state between the CPU (cpu.c + cpu_simd.h) and the HLE kernel (kernel.c, ipc.c). */
#ifndef EMU_INTERNAL_H
#define EMU_INTERNAL_H

#include "mgr_internal.h"

#define MAX_REGIONS  1024
#define MEM_TLB_SIZE 4096
#define MAX_THREADS  256
#define MAX_HANDLES  2048
#define MAX_EVENTS   1024
#define MAX_SERVICES 8192
#define MAX_DOMAIN_OBJS 512

#define CODE_BASE   0x0000000008000000ull
#define TLS_BASE    0x0000000040000000ull
#define TLS_SIZE    0x20000ull            /* 0x200 per thread */
#define STACK_BASE  0x0000000050000000ull
#define HEAP_BASE   0x0000000080000000ull
#define HEAP_MAX    (16ull << 30)   /* Switch 2 heaps exceed 4 GiB; host memory is committed lazily */
#define ALIAS_BASE  0x0000001000000000ull
#define ALIAS_SIZE  (64ull << 30)
#define ASPACE_BASE 0x0000000008000000ull
#define ASPACE_SIZE (0x7FF8000000ull)
#define TICK_HZ     19200000ull
#define EMU_CORES   3                     /* emulated cores for scheduling fairness */
#define QUANTUM     20000u                /* instructions per time slice */


/* ---- Switch 2 GPU front-end (gpu_nx2.c) ---- */
#define NX2_MAX_MAPS  65536
#define NX2_MAX_MEM   65536
#define NX2_MAX_CH    16
#define NX2_QUEUE     65536
#define NX2_ENG_REGS  0x2000
typedef struct { uint64_t gva, cpu, size; } nx2_map;
typedef struct { uint32_t code[0x3000], wr, start[0x80], start_ptr; int loaded; } nx2_mme;
#define NX2_MAX_QMD 4096
typedef struct { uint64_t addr; int32_t deps, deferred; int loaded, scheduled, completed; uint32_t hist[8], nhist; uint32_t words[64]; } nx2_qmd;
typedef struct {
    uint32_t cls; int kind; uint32_t regs[NX2_ENG_REGS];
    int mme_pending; uint32_t mme_index, mme_nparams, mme_params[0x2000];   /* macro words being collected */
    uint16_t mme_start[0x2000];                    /* per collected word: macro index if it starts a call, else 0xFFFF */
    uint32_t mme_r[24], mme_carry, mme_method, mme_inc;   /* MME state kept between macro calls */
    uint32_t mme_consumed;                         /* words the last macro call used */
    int mme_waiting;                               /* the collected call needs more parameters */
    uint32_t mme_left[0x1000], mme_nleft;        /* DMA_READ_FIFOED words a macro left in the FIFO */
} nx2_engine;
typedef struct {
    uint32_t sp;                                   /* channel syncpoint (identifies the channel) */
    uint64_t queue[NX2_QUEUE]; uint32_t q_head, q_tail;
    uint32_t qinc[NX2_QUEUE], cur_inc;            /* syncpoint increments due when an entry is done */
    uint64_t pb_addr; uint32_t pb_words, pb_pos;   /* current pushbuffer segment */
    uint32_t pb_mode, pb_sub, pb_method, pb_remaining, pb_first;
    int stalled;
    uint64_t sem_addr; uint32_t sem_payload, sem_payload_hi;
    uint64_t i2m_dst; uint32_t i2m_len, i2m_pos;
    nx2_engine eng[8];
    uint32_t mme_dram[0xC00];                      /* macro data RAM (per channel) */
} nx2_channel;

typedef struct { uint64_t vpage; uint8_t *host; } mem_tlb;
typedef struct { uint64_t pc, lr, v, ins, a; int t; } watch_rec;

typedef struct {
    uint64_t base, size, map_size;
    uint8_t *mem;
    uint32_t state;   /* MemoryState for QueryMemory */
    uint32_t perm;    /* 1 R, 2 W, 4 X */
    char name[24];
    uint64_t hvf_ipa; int hvf_mapped;   /* native CPU backend: where it sits in the VM */
    int alias_of; uint64_t alias_off;   /* >= 0: shares another region's memory (MapMemory), not owned */
} region;

typedef struct { uint8_t b[16]; } vreg;

enum { TH_FREE = 0, TH_CREATED, TH_READY, TH_WAIT, TH_DEAD };
enum { WAIT_NONE = 0, WAIT_SLEEP, WAIT_SYNC, WAIT_MUTEX, WAIT_CONDVAR, WAIT_ADDRESS };

/* Saved CPU context + scheduling state of one guest thread. */
typedef struct {
    int state;
    uint32_t handle;
    uint64_t id;
    int priority;
    int core; uint64_t core_mask;
    uint64_t x[31], sp, pc;
    int n, z, c, v;
    vreg q[32];
    uint64_t tpidr, tpidrro, fpcr, fpsr;
    /* waiting */
    int wait_kind;
    uint64_t wake_tick;          /* UINT64_MAX = no timeout */
    uint32_t wait_handles[64];
    int wait_count;
    uint64_t wait_addr;          /* mutex / condvar key / address */
    uint64_t wait_mutex;         /* condvar: mutex to re-acquire */
    uint32_t wait_tag;           /* condvar/mutex: this thread's handle tag */
    int cancel_pending;          /* CancelSynchronization arrived while not waiting */
} kthread;

enum { KO_FREE = 0, KO_THREAD, KO_EVENT, KO_SESSION, KO_PROCESS, KO_OTHER, KO_TMEM };
#define SHMEM_HID  0x70     /* KO_OTHER idx: hid's shared memory (controller state) */
#define SHMEM_TIME 0x71     /* KO_OTHER idx: the time service's shared memory (clock contexts) */
#define MAX_TMEM 256
typedef struct { uint64_t addr, size; } ktmem;
typedef struct { int type; int idx; } khandle;

typedef struct { int used; int signaled; int sticky; } kevent;   /* sticky: clearing has no effect */

/* A service object (session target or domain object). */
typedef struct {
    int used;
    int iface;                  /* see ipc.c */
    char name[48];
    int is_domain;
    int domain_objs[MAX_DOMAIN_OBJS];        /* object id -> service index (0 = none) */
    int state;                  /* per-interface scratch (e.g. message queue position) */
    int event;                  /* associated event, -1 if none */
    uint64_t storage_size;
} kservice;

struct mgr_emu {
    mgr_game *game;
    region reg[MAX_REGIONS];
    int nreg;
    int last;
    /* live CPU state of the current thread */
    uint64_t x[31], sp, pc;
    int n, z, c, v;
    vreg q[32];
    uint64_t tpidr, tpidrro, fpcr, fpsr;
    uint64_t excl_addr; int excl_valid;
    /* kernel */
    kthread th[MAX_THREADS];
    int cur;                    /* current thread index */
    uint64_t next_thread_id;
    uint32_t quantum_left;
    khandle handles[MAX_HANDLES];
    kevent events[MAX_EVENTS];
    kservice svcs[MAX_SERVICES];
    ktmem tmem[MAX_TMEM];
    int ntmem;
    uint64_t nx2_syncpt_ro, nx2_syncpt_rw;   /* Switch 2 syncpoint pages (gsv 204) */
    uint32_t nx2_syncpt_count;
    int nx2_gpu_ev[32]; uint32_t nx2_gpu_devices, nx2_gpu_next_mem;
    uint64_t nx2_as_start, nx2_as_split, nx2_as_end; uint32_t nx2_as_big;
    uint32_t nx2_ch_syncpt[64], nx2_next_syncpt, nx2_sp_max[512], nx2_sp_cur[512]; int nx2_sp_real; int nx2_sp_ev[16], nx2_sp_nev;
    nx2_map nx2_maps[NX2_MAX_MAPS]; int nx2_nmaps, nx2_map_last;
    struct { uint64_t cpu, size; } nx2_mem[NX2_MAX_MEM];  /* nx2:gpu memory handles */
    nx2_channel nx2_ch[NX2_MAX_CH]; int nx2_nch;
    nx2_mme nx2_mme[NX2_MAX_CH];                     /* macro instruction RAM, one per channel */
    nx2_qmd nx2_qmd[NX2_MAX_QMD];
    uint64_t nx2_qmds, nx2_qmd_errors, nx2_dispatches_skipped;
    int nx2_vi_ev[16]; uint32_t nx2_vsync_mask; uint64_t nx2_next_vsync, nx2_vsyncs, nx2_presents;
    uint32_t nx2_next_native_mem, nx2_next_image;
    uint64_t nx2_nmem_addr[256];
    uint64_t nx2_blits2d;
    uint64_t nx2_last_draw_rt;                       /* colour target of the last software draw */                     /* native memory id -> CPU address (gsv 423) */
    struct { uint32_t pref[16]; uint32_t count, memory; uint64_t size; } nx2_img[256];
    uint64_t nx2_submits, nx2_entries, nx2_methods, nx2_sem_releases, nx2_copies, nx2_draws,
             nx2_macro_calls, nx2_macro_errors, nx2_macro_borrow, nx2_inline_qmd_words, nx2_macro_waits, nx2_macro_underflows, nx2_qmd1_seen, nx2_bad_headers, nx2_logged_submits, nx2_gpu_faults, nx2_block_linear_copies;
    uint64_t ticks;
    uint64_t system_resource_size, physmem_bytes;
    int alias_reg;
    uint64_t watch_lo, watch_hi; int watch_logged, watch_reads; uint8_t *watch_host; uint64_t watch_last; uint32_t last_svc;
    watch_rec watch_ring[512]; uint64_t watch_n;
    uint64_t sched_slot;
    int pref_thread;                           /* thread to run next (+1), 0 = none */
    int ipc_bulk_logged;
    uint32_t aud_voices, aud_sinks, aud_effects, aud_revision, aud_streams; uint64_t aud_updates;
    uint64_t svc_hist[256];
    uint64_t hid_shmem, hid_sampling, hid_logged_press;              /* hid shared memory (guest address) and input sample counter */
    int aud_event[4]; int aud_nevents; uint64_t aud_next_tick;   /* audio renderer events: signalled every 5 ms */
    mem_tlb tlb[MEM_TLB_SIZE];
    uint64_t heap_size;
    uint64_t program_id;
    /* status */
    mgr_emu_status status;
    uint64_t instructions, svc_calls;
    char message[256];
    char *log; size_t log_len, log_cap;
    uint64_t svc_log_budget;
    uint64_t ipc_log_budget;
    int insn_ok;
    void *hvf; int hvf_tried; void *native;  /* native CPU backend (cpu_hvf.c) and its thread; NULL = interpreter */
    uint64_t break_pc, break_after;          /* MGR_BREAK debug breakpoint (0 = none) */
};

/* memory & logging (cpu.c) */
void     elog(mgr_emu *e, const char *fmt, ...);
void     emu_stop(mgr_emu *e, mgr_emu_status st, const char *fmt, ...);
region  *add_region_reserve(mgr_emu *e, uint64_t base, uint64_t size, uint64_t reserve, uint32_t perm, uint32_t state, const char *name);
uint8_t *guest_mem_alloc(uint64_t size);
void     guest_mem_free(uint8_t *m, uint64_t size);
void     guest_mem_discard(uint8_t *m, uint64_t size);
region  *add_region(mgr_emu *e, uint64_t base, uint64_t size, uint32_t perm, uint32_t state, const char *name);
region  *add_region_alias(mgr_emu *e, uint64_t base, uint64_t size, region *src, uint64_t off, const char *name);
region  *find_region(mgr_emu *e, uint64_t addr);
uint8_t *mem_ptr_slow(mgr_emu *e, uint64_t addr, uint64_t len);
void     mem_tlb_flush(mgr_emu *e);
/* MGR_WATCH debugging: report writes by any path (CPU, services, GPU) into the watched range. */
void watch_report(mgr_emu *e, uint64_t addr, uint64_t len, const char *who);
void watch_dump(mgr_emu *e);
#define WATCH(e, addr, len, who) do { if ((e)->watch_hi && (addr) < (e)->watch_hi && (addr) + (len) > (e)->watch_lo) \
    watch_report((e), (addr), (len), (who)); } while (0)

/* Guest address -> host pointer. Fast path: a direct-mapped cache of whole 4 KiB pages. */
static inline uint8_t *mem_ptr(mgr_emu *e, uint64_t addr, uint64_t len) {
    uint64_t pg = addr & ~0xFFFull;
    mem_tlb *t = &e->tlb[(pg >> 12) & (MEM_TLB_SIZE - 1)];
    if (t->vpage == (pg | 1) && (addr & 0xFFF) + len <= 0x1000) return t->host + (addr & 0xFFF);
    return mem_ptr_slow(e, addr, len);
}
uint64_t mrd(mgr_emu *e, uint64_t a, int bytes);
void     mwr(mgr_emu *e, uint64_t a, int bytes, uint64_t v);
void     cpu_step(mgr_emu *e);
void     emu_backtrace(mgr_emu *e);
void     nx2_gpu_map(mgr_emu *e, uint64_t gva, uint64_t cpu, uint64_t size);
void     nx2_gpu_unmap(mgr_emu *e, uint64_t gva, uint64_t size);
uint64_t nx2_gpu_xlate(mgr_emu *e, uint64_t gva);
void     nx2_gpfifo_submit(mgr_emu *e, uint64_t list, uint32_t n, uint32_t sp);
void     nx2_gpfifo_submit_entries(mgr_emu *e, const uint64_t *entries, uint32_t n, uint32_t sp);
void     nx2_gpfifo_fence(mgr_emu *e, uint32_t sp, uint32_t inc);
void     nx2_soft_frame(mgr_emu *e, uint64_t presents);
void     kernel_address_changed(mgr_emu *e, uint64_t addr);
uint32_t nx2_syncpt_value(mgr_emu *e, uint32_t id);
void     nx2_gpu_poll(mgr_emu *e);
void     nx2_gpu_dump(mgr_emu *e);
void     ipc_dump_stats(mgr_emu *e);
void     hid_update(mgr_emu *e);
void     emu_backtrace_from(mgr_emu *e, uint64_t pc, uint64_t lr, uint64_t fp);   /* log a frame-pointer backtrace with module offsets */


/* kernel (kernel.c) */
void     do_svc(mgr_emu *e, uint32_t imm);
/* native CPU backend (cpu_hvf.c; Apple Silicon) */
int      hvf_available(void);
int      hvf_init(mgr_emu *e);
void     hvf_destroy(mgr_emu *e);
void     hvf_map_region(mgr_emu *e, region *r);
void     hvf_pull_full(mgr_emu *e);
void     hvf_mark_dirty(mgr_emu *e);
uint64_t hvf_run_slice(mgr_emu *e);
void     hvf_sleep_until(mgr_emu *e, uint64_t tick);
uint64_t hvf_now_ticks(mgr_emu *e);
uint32_t handle_new(mgr_emu *e, int type, int idx);
khandle *handle_get(mgr_emu *e, uint32_t h);
int      event_new(mgr_emu *e, int signaled);
void     event_signal(mgr_emu *e, int ev);
uint64_t cur_tls(mgr_emu *e);

/* IPC (ipc.c) */
int      service_new(mgr_emu *e, const char *name);
uint32_t ipc_send_sync_request(mgr_emu *e, uint32_t handle);
uint32_t ipc_connect_named_port(mgr_emu *e, const char *name, uint32_t *out_handle);

#endif
