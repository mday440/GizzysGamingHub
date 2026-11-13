/*
 * HIPC / CMIF IPC: parses SendSyncRequest messages from the thread's TLS,
 * supports domains (object ids), control requests, copy/move handles, and
 * dispatches to HLE service objects.
 *
 * Services with real behaviour: sm:, applet (OE/AE) proxy tree, fsp-srv
 * (DataStorage over the game's RomFS), set, time, hid, vi, nvdrv, acc,
 * pctl, audio. Every other service is a "generic" object that answers any
 * command with success and zeroed output, and logs it, so unknown calls are
 * visible instead of fatal.
 */
#include "emu_internal.h"

#define SFCI 0x49434653u
#define SFCO 0x4F434653u
#define RES_NO_MESSAGE   0x680u          /* am: no message */
#define RES_NOT_FOUND    0x202u          /* fs: path not found */
#define MAX_BUF 8

typedef struct { uint64_t addr, size; } ipc_buf;

typedef struct {
    mgr_emu *e;
    int session;            /* service index the handle refers to */
    int target;             /* service index the request is for (domain object or session) */
    int domain;             /* request arrived through a domain */
    uint32_t cmd;
    const uint8_t *args; size_t args_len;
    uint32_t nv_cur_fd;
    ipc_buf a[MAX_BUF], b[MAX_BUF], x[MAX_BUF], c[MAX_BUF];
    int na, nb, nx, nc;
    uint32_t in_objs[8]; int n_in_objs;
    uint32_t in_copy[8]; int n_in_copy;
    /* response */
    uint8_t out[0xC0]; size_t out_len;
    uint32_t copy_h[8]; int n_copy;
    uint32_t move_h[8]; int n_move;
    uint32_t out_objs[8]; int n_out_objs;
} ipc_ctx;

/* ---------------- service objects ---------------- */
int service_new(mgr_emu *e, const char *name) {
    for (int i = 1; i < MAX_SERVICES; i++)
        if (!e->svcs[i].used) {
            memset(&e->svcs[i], 0, sizeof e->svcs[i]);
            e->svcs[i].used = 1;
            e->svcs[i].event = -1;
            mgr_strlcpy(e->svcs[i].name, name, sizeof e->svcs[i].name);
            return i;
        }
    elog(e, "[ipc] out of service objects creating %s\n", name);
    return 0;
}

static void out_bytes(ipc_ctx *c, const void *p, size_t n) {
    if (c->out_len + n > sizeof c->out) return;
    memcpy(c->out + c->out_len, p, n);
    c->out_len += n;
}
static void out_u8(ipc_ctx *c, uint8_t v) { out_bytes(c, &v, 1); }
static void out_u32(ipc_ctx *c, uint32_t v) { while (c->out_len & 3) out_u8(c, 0); uint8_t b[4]; wr32(b, v); out_bytes(c, b, 4); }
static void out_u64(ipc_ctx *c, uint64_t v) { while (c->out_len & 7) out_u8(c, 0); uint8_t b[8]; wr64(b, v); out_bytes(c, b, 8); }
static uint64_t arg64(ipc_ctx *c, size_t off) { return off + 8 <= c->args_len ? rd64(c->args + off) : 0; }
static __attribute__((unused)) uint32_t arg32(ipc_ctx *c, size_t off) { return off + 4 <= c->args_len ? rd32(c->args + off) : 0; }

/* Return a new interface object: an object id in a domain, otherwise a moved session handle.
   out_object_idx also returns the new object's service index (for per-object state). */
static int out_object_idx(ipc_ctx *c, const char *name) {
    int s = service_new(c->e, name);
    if (c->domain || c->e->svcs[c->session].is_domain) {
        kservice *root = &c->e->svcs[c->session];
        for (int id = 1; id < MAX_DOMAIN_OBJS; id++)
            if (!root->domain_objs[id]) { root->domain_objs[id] = s; c->out_objs[c->n_out_objs++] = (uint32_t)id; return s; }
        elog(c->e, "[ipc] %s: domain full, can't add %s\n", root->name, name);
        return s;
    }
    c->move_h[c->n_move++] = handle_new(c->e, KO_SESSION, s);
    return s;
}
static void out_object(ipc_ctx *c, const char *name) { (void)out_object_idx(c, name); }

static void out_event(ipc_ctx *c, int signaled) {
    kservice *t = &c->e->svcs[c->target];
    if (t->event < 0) t->event = event_new(c->e, signaled);
    c->copy_h[c->n_copy++] = handle_new(c->e, KO_EVENT, t->event);
}

static void write_guest(ipc_ctx *c, const ipc_buf *b, const void *data, size_t n) {
    if (!b->addr || !n) return;
    if (n > b->size) n = (size_t)b->size;
    WATCH(c->e, b->addr, n, "service reply buffer");
    uint8_t *p = mem_ptr(c->e, b->addr, n);
    if (p) memcpy(p, data, n);
}
static ipc_buf *out_buf(ipc_ctx *c) {   /* B buffer, else C (receive list); auto-select buffers leave B empty */
    if (c->nb && c->b[0].addr) return &c->b[0];
    if (c->nc && c->c[0].addr) return &c->c[0];
    return c->nb ? &c->b[0] : NULL;
}

/* ---------------- RomFS data storage ---------------- */
int mgr_romfs_image_read(mgr_game *g, uint64_t off, void *buf, size_t n);
uint64_t mgr_romfs_image_size(mgr_game *g);


#include "ipc_nx2.h"
#include "fs_host.h"

/* ---------------- dispatch ---------------- */
#define IS(n) (strcmp(name, n) == 0)

/* Per (service name, command) call counts, for the end-of-run summary. */
typedef struct { char name[48]; uint32_t cmd; uint64_t n; } ipc_stat;
static ipc_stat g_ipc_stats[512]; static int g_nstats;
static void ipc_count(const char *name, uint32_t cmd) {
    for (int i = 0; i < g_nstats; i++) if (g_ipc_stats[i].cmd == cmd && !strcmp(g_ipc_stats[i].name, name)) { g_ipc_stats[i].n++; return; }
    if (g_nstats < 512) { mgr_strlcpy(g_ipc_stats[g_nstats].name, name, 48); g_ipc_stats[g_nstats].cmd = cmd; g_ipc_stats[g_nstats++].n = 1; }
}
void ipc_dump_stats(mgr_emu *e) {
    for (int round = 0; round < 15; round++) {                  /* top 15 */
        int best = -1;
        for (int i = 0; i < g_nstats; i++) if (g_ipc_stats[i].n && (best < 0 || g_ipc_stats[i].n > g_ipc_stats[best].n)) best = i;
        if (best < 0) break;
        elog(e, "[stats] ipc %s cmd %u: %llu\n", g_ipc_stats[best].name, g_ipc_stats[best].cmd, (unsigned long long)g_ipc_stats[best].n);
        g_ipc_stats[best].n = 0;
    }
}
static uint32_t dispatch_inner(ipc_ctx *c);
/* MGR_IPCSCAN=<hex address>: log requests whose message or input buffers hold a 32-bit value in the
   same 256-byte block (finds which service call hands an address to the system) */
static void ipc_scan(ipc_ctx *c) {
    static int init; static uint32_t blk; static int nlog;
    if (!init) { init = 1; const char *t = getenv("MGR_IPCSCAN"); if (t) blk = (uint32_t)(strtoull(t, NULL, 16) >> 8); }
    if (!blk || nlog >= 40) return;
    mgr_emu *e = c->e;
    struct { uint64_t a, n; } r[1 + 2 * MAX_BUF]; int nr = 0;
    r[nr].a = cur_tls(e); r[nr++].n = 0x100;
    for (int k = 0; k < c->na; k++) { r[nr].a = c->a[k].addr; r[nr++].n = c->a[k].size; }
    for (int k = 0; k < c->nx; k++) { r[nr].a = c->x[k].addr; r[nr++].n = c->x[k].size; }
    for (int i = 0; i < nr; i++) {
        uint64_t n = r[i].n > 0x1000 ? 0x1000 : r[i].n;
        uint8_t *p = n ? mem_ptr(e, r[i].a, n) : NULL;
        if (!p) continue;
        for (uint64_t o = 0; o + 4 <= n; o += 4)
            if (rd32(p + o) >> 8 == blk) {
                nlog++;
                elog(e, "[ipcscan] %s cmd %u: %s %d +%llx = %08x\n", e->svcs[c->target].name, c->cmd, i ? "buffer" : "message", i,
                     (unsigned long long)o, rd32(p + o));
                return;
            }
    }
}
static uint32_t dispatch(ipc_ctx *c) { ipc_count(c->e->svcs[c->target].name, c->cmd); ipc_scan(c); return dispatch_inner(c); }
static uint32_t dispatch_inner(ipc_ctx *c) {
    mgr_emu *e = c->e;
    kservice *t = &e->svcs[c->target];
    const char *name = t->name;
    uint32_t cmd = c->cmd;

    { uint32_t r; if (nx2_dispatch(c, &r)) return r; }
    if (IS("nx2:nvsrv")) { nvsrv_dispatch(c); return 0; }   /* NVIDIA driver (nvdrv protocol) */

    if (IS("sm:")) {
        if (cmd == 1) { /* GetService */
            char sn[9] = {0};
            uint64_t raw = arg64(c, 0);
            memcpy(sn, &raw, 8);
            elog(e, "[ipc] sm:GetService(\"%s\")\n", sn);
            out_object(c, sn);
            return 0;
        }
        if (cmd == 2) { c->move_h[c->n_move++] = handle_new(e, KO_OTHER, 0); return 0; }
        return 0;
    }
    if (IS("appletOE") || IS("appletAE")) { out_object(c, "IApplicationProxy"); return 0; }
    if (IS("IApplicationProxy") || IS("ISystemAppletProxy")) {
        static const char *kids[] = { "ICommonStateGetter", "ISelfController", "IWindowController", "IAudioController",
                                      "IDisplayController", 0, 0, 0, 0, 0, "IProcessWindingController", "ILibraryAppletCreator" };
        if (cmd < 12 && kids[cmd]) { out_object(c, kids[cmd]); return 0; }
        if (cmd == 20) { out_object(c, "IApplicationFunctions"); return 0; }
        if (cmd == 21) { out_object(c, "IGlobalStateController"); return 0; }
        if (cmd == 22) { out_object(c, "IApplicationCreator"); return 0; }
        if (cmd == 1000) { out_object(c, "IDebugFunctions"); return 0; }
        /* newer firmware added more child-interface getters: hand back a generic object */
        { char nm[48]; snprintf(nm, sizeof nm, "IAppletChild%u", cmd); out_object(c, nm); }
        return 0;
    }
    if (IS("ICommonStateGetter")) {
        switch (cmd) {
        case 0: out_event(c, 1); return 0;                       /* GetEventHandle */
        case 1:                                                   /* ReceiveMessage */
            if (t->state == 0) { t->state = 1; out_u32(c, 15); return 0; } /* FocusStateChanged */
            if (t->event >= 0) e->events[t->event].signaled = 0;
            return RES_NO_MESSAGE;
        case 5: out_u8(c, 0); return 0;                          /* GetOperationMode: handheld */
        case 6: out_u32(c, 0); return 0;                         /* GetPerformanceMode */
        case 8: out_u8(c, 0); return 0;                          /* GetBootMode */
        case 9: out_u8(c, 1); return 0;                          /* GetCurrentFocusState: InFocus */
        case 60: out_u32(c, 1280); out_u32(c, 720); return 0;    /* GetDefaultDisplayResolution */
        case 61: out_event(c, 0); return 0;
        default: return 0;
        }
    }
    if (IS("IAppletChild60")) {
        /* Switch 2-era applet interface (IApplicationProxy cmd 60). cmd 10 returns an
           enum the SDK requires to be 10/20/30; 10 maps to the first (default) state. */
        if (cmd == 10) { out_u32(c, 10); return 0; }
        for (int k = 0; k < 8; k++) out_u64(c, 0);
        return 0;
    }
    if (IS("ISelfController")) {
        if (cmd == 40) { out_u64(c, 1); return 0; }              /* CreateManagedDisplayLayer */
        if (cmd == 91) { out_event(c, 1); return 0; }            /* GetAccumulatedSuspendedTickChangedEvent (signaled) */
        if (cmd == 2000) {                                        /* Switch 2: graphics system (gsv) session, as a copy handle */
            int sv = service_new(e, "gsv");
            c->copy_h[c->n_copy++] = handle_new(e, KO_SESSION, sv);
            return 0;
        }
        if (cmd == 19 || cmd == 70) return 0;
        if (cmd == 9) {                                           /* GetLibraryAppletLaunchableEvent: always launchable */
            c->copy_h[c->n_copy++] = handle_new(e, KO_EVENT, event_new(e, 1));
            return 0;
        }
        if (cmd == 0 || cmd == 1 || cmd == 66) { if (cmd == 1) out_event(c, 0); return 0; }
        return 0;
    }
    if (IS("IStorage(launch)")) { if (cmd == 0) out_object(c, "IStorageAccessor(launch)"); return 0; }
    if (IS("IStorageAccessor(launch)")) {
        /* launch parameters for PreselectedUser: magic, account-selected flag, user id (0x88 bytes) */
        static const uint8_t uid[16] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE };
        uint8_t data[0x88] = {0};
        wr32(data, 0xC79497CAu); wr32(data + 4, 1); memcpy(data + 8, uid, 16);
        if (cmd == 0) { out_u64(c, sizeof data); return 0; }                    /* GetSize */
        if (cmd == 11) {                                                         /* Read(offset) -> B buffer */
            uint64_t off = arg64(c, 0);
            ipc_buf *b = out_buf(c);
            if (b && off < sizeof data) write_guest(c, b, data + off, sizeof data - (size_t)off);
            return 0;
        }
        return 0;
    }
    if (IS("IWindowController")) {
        if (cmd == 1) { out_u64(c, 0x51); return 0; }            /* GetAppletResourceUserId */
        return 0;
    }
    if (IS("IApplicationFunctions")) {
        switch (cmd) {
        case 1: {                                                 /* PopLaunchParameter(kind) */
            uint64_t kind = arg64(c, 0) & 0xFFFFFFFF;
            if (kind == 2) { out_object(c, "IStorage(launch)"); return 0; }   /* PreselectedUser */
            return 0x480;                                         /* am NotAvailable */
        }
        case 20: out_u64(c, 0); return 0;                         /* EnsureSaveData */
        case 21: out_u64(c, 0x53552D6E65ull); return 0;           /* GetDesiredLanguage "en-US" */
        case 22: return 0;
        case 40: out_u8(c, 1); return 0;                          /* NotifyRunning */
        case 50: out_u64(c, 0); out_u64(c, 0); return 0;          /* GetPseudoDeviceId */
        case 66: case 67: return 0;
        case 100: case 101: case 102: case 103: return 0;
        case 130: case 131: out_event(c, 0); return 0;
        case 210: out_event(c, 0); return 0;                        /* GetUnknownEvent (20.0.0+) */
        default: return 0;
        }
    }
    if (IS("fsp-srv")) {
        switch (cmd) {
        case 1: return 0;                                          /* SetCurrentProcess */
        case 18: out_object(c, "IFileSystem(sd)"); return 0;
        case 51: case 52: case 53: out_object(c, "IFileSystem(save)"); return 0;
        case 200: case 202: {                                      /* OpenDataStorageByCurrentProcess / ByDataId */
            uint64_t sz = mgr_romfs_image_size(e->game);
            if (!sz) { elog(e, "[fs] game has no RomFS image\n"); return RES_NOT_FOUND; }
            out_object(c, "IStorage(romfs)");
            elog(e, "[fs] DataStorage opened: RomFS image %llu bytes\n", (unsigned long long)sz);
            return 0;
        }
        case 203: out_object(c, "IStorage(patch)"); return 0;
        case 1003: case 1004: case 1006: return 0;
        case 1005: out_u32(c, 0); return 0;                        /* GetGlobalAccessLogMode */
        default: return 0;
        }
    }
    if (IS("IStorage(romfs)")) {
        if (cmd == 0) {                                            /* Read(offset, size) -> B buffer */
            uint64_t off = arg64(c, 0), size = arg64(c, 8);
            ipc_buf *b = out_buf(c);
            if (!b || size > b->size) size = b ? b->size : 0;
            uint8_t *p = size ? mem_ptr(e, b->addr, size) : NULL;
            if (size) WATCH(e, b->addr, size, "RomFS read");
            if (size && (!p || mgr_romfs_image_read(e->game, off, p, (size_t)size) != 0)) {
                elog(e, "[fs] RomFS read failed at %llx+%llx\n", (unsigned long long)off, (unsigned long long)size);
                return 0x3E802u;
            }
            return 0;
        }
        if (cmd == 4) { out_u64(c, mgr_romfs_image_size(e->game)); return 0; }  /* GetSize */
        return 0;
    }
    { uint32_t fr; if (fs_host_dispatch(c, name, cmd, &fr)) return fr; }   /* save data / SD card: host folders */
    if (IS("IDirectory")) { if (cmd == 0 || cmd == 1) out_u64(c, 0); return 0; }
    if (IS("set") || IS("set:sys")) {
        switch (cmd) {
        case 0: out_u64(c, 0x53552D6E65ull); return 0;             /* GetLanguageCode */
        case 1: case 5: case 9: {                                  /* available language codes -> buffer */
            static const uint64_t langs[] = { 0x53552D6E65ull, 0x616A, 0x7266 };
            ipc_buf *b = out_buf(c);
            if (b) write_guest(c, b, langs, sizeof langs);
            out_u32(c, 3);
            return 0;
        }
        case 3: out_u32(c, 3); return 0;
        case 4: out_u32(c, 1); return 0;                           /* GetRegionCode: USA */
        default: return 0;
        }
    }
    if (IS("time:u") || IS("time:a") || IS("time:s")) {
        static const char *kids[] = { "ISystemClock(user)", "ISystemClock(network)", "ISteadyClock", "ITimeZoneService", "ISystemClock(local)" };
        if (cmd < 5) { out_object(c, kids[cmd]); return 0; }
        if (cmd == 20) { c->copy_h[c->n_copy++] = handle_new(e, KO_OTHER, SHMEM_TIME); return 0; } /* GetSharedMemoryNativeHandle */
        return 0;
    }
    if (!strncmp(name, "ISystemClock", 12)) { if (cmd == 0) { out_u64(c, 1793000000ull); return 0; } return 0; }
    if (IS("ISteadyClock")) {
        if (cmd == 0) { uint8_t z[24] = {0}; wr64(z, (uint64_t)((double)e->ticks * 1e9 / (double)TICK_HZ)); out_bytes(c, z, 24); return 0; }
        return 0;
    }
    if (IS("hid")) {
        if (cmd == 0) { out_object(c, "IAppletResource"); return 0; }
        if (cmd == 66 || cmd == 70 || cmd == 72) return 0;
        if (cmd == 120 || cmd == 122) return 0;
        if (cmd == 203) { out_object(c, "IActiveVibrationDeviceList"); return 0; }
        return 0;
    }
    if (IS("IAppletResource")) {
        if (cmd == 0) {
            c->copy_h[c->n_copy++] = handle_new(e, KO_OTHER, SHMEM_HID);   /* filled in when the game maps it */
            return 0;
        }
        return 0;
    }
    if (IS("vi:m") || IS("vi:u") || IS("vi:s")) { out_object(c, "IApplicationDisplayService"); return 0; }
    if (IS("IApplicationDisplayService")) {
        switch (cmd) {
        case 100: out_object(c, "IHOSBinderDriver"); return 0;
        case 101: out_object(c, "ISystemDisplayService"); return 0;
        case 102: out_object(c, "IManagerDisplayService"); return 0;
        case 103: out_object(c, "IHOSBinderDriver"); return 0;
        case 1010: out_u64(c, 1); return 0;                         /* OpenDisplay */
        case 1020: return 0;
        case 2020: out_u64(c, 0x28); return 0;                      /* OpenLayer -> native window size */
        case 2030: out_u64(c, 2); out_u64(c, 0x28); return 0;       /* CreateStrayLayer */
        case 2101: return 0;
        case 5202: out_event(c, 1); return 0;                       /* GetDisplayVsyncEvent */
        default: return 0;
        }
    }
    if (IS("IHOSBinderDriver")) { if (cmd == 2) { out_event(c, 1); } return 0; }
    if (IS("nvdrv") || IS("nvdrv:a") || IS("nvdrv:s") || IS("nvdrv:t")) {
        switch (cmd) {
        case 0: out_u32(c, (uint32_t)(t->state++ + 1)); out_u32(c, 0); return 0;  /* Open -> fd, error */
        case 1: out_u32(c, 0); return 0;                           /* Ioctl -> error code */
        case 2: out_u32(c, 0); return 0;                           /* Close */
        case 3: out_u32(c, 0); return 0;                           /* Initialize */
        case 4: out_event(c, 0); out_u32(c, 0); return 0;          /* QueryEvent */
        default: out_u32(c, 0); return 0;
        }
    }
    if (!strncmp(name, "acc:", 4)) {
        switch (cmd) {
        case 0: out_u32(c, 1); return 0;                           /* GetUserCount */
        case 1: out_u8(c, 1); return 0;                            /* GetUserExistence */
        case 2: case 3: {                                          /* ListAllUsers / ListOpenUsers */
            uint8_t uid[16] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE };
            ipc_buf *b = out_buf(c);
            if (b) write_guest(c, b, uid, 16);
            return 0;
        }
        case 4: {                                                  /* GetLastOpenedUser */
            out_u64(c, 0xEFCDAB8967452301ull); out_u64(c, 0xFEDCBA9876543210ull); return 0;
        }
        case 5: out_object(c, "IProfile"); return 0;
        case 100: case 140: case 150: return 0;
        case 101: out_object(c, "IManagerForApplication"); return 0;
        default: return 0;
        }
    }
    if (IS("pctl") || IS("pctl:a") || IS("pctl:s") || IS("pctl:r")) {
        if (cmd == 0 || cmd == 1) { out_object(c, "IParentalControlService"); return 0; }
        return 0;
    }
    /* Switch 2 sound mixer (audsmx:u): output streams that games feed through a shared ring buffer.
       No audio reaches the Mac yet: streams open and report success, their data events stay quiet. */
    if (IS("audsmx:u")) { if (cmd == 0) { out_object(c, "IAudioSmxSession"); } return 0; }
    if (IS("IAudioSmxSession")) {
        if (cmd == 1) {
            e->aud_streams++;
            elog(e, "[audio] sound mixer stream %u: buffer %llx size %llx\n", e->aud_streams,
                 (unsigned long long)arg64(c, 16), (unsigned long long)arg64(c, 24));
            out_u32(c, e->aud_streams); out_object(c, "IAudioSmxStream"); return 0;
        }
        return 0;                                                   /* engage / disengage */
    }
    if (IS("IAudioSmxStream")) {
        switch (cmd) {
        case 2: out_u32(c, 0); return 0;                            /* state */
        case 3: out_u32(c, 0x3F800000); return 0;                   /* gain 1.0 */
        case 5: out_event(c, 0); return 0;                          /* state-change event */
        case 7: out_event(c, 0); return 0;                          /* data event */
        case 9: case 10: case 11: return 0x5A99;                    /* clock / prime / purge: unsupported */
        default: return 0;
        }
    }
    if (IS("auddmg:u")) { if (cmd == 0) { out_object(c, "IAudioVirtualDeviceManager"); } return 0; }
    if (IS("IAudioVirtualDeviceManager")) {   /* Switch 2 virtual audio outputs: one stereo 48 kHz device */
        static const char dev[] = "AudioVirtualOutput0";
        ipc_buf *ob = out_buf(c);
        switch (cmd) {
        case 0: {                                                   /* ListVirtualOutputDevices */
            uint8_t names[0x100] = {0}; memcpy(names, dev, sizeof dev);
            uint32_t n = ob && ob->size >= 0x100;
            if (n) write_guest(c, ob, names, sizeof names);
            out_u32(c, n); return 0;
        }
        case 100: { uint8_t name[0x100] = {0}; memcpy(name, dev, sizeof dev); if (ob) write_guest(c, ob, name, sizeof name); return 0; }
        case 300: return arg32(c, 12) == 1 ? 0 : 0x5A99;           /* IsStreamFormatSupported(functionality 1) */
        case 401: {                                                 /* GetVirtualOutputDeviceProperties */
            uint8_t b[0x228] = {0};
            memcpy(b, dev, sizeof dev);
            wr32(b + 0x100, 3); wr32(b + 0x104, 2); wr32(b + 0x108, 2); wr32(b + 0x10c, 48000);
            memcpy(b + 0x110, "AudioTvOutput", 14);
            wr64(b + 0x218, 10000000); wr32(b + 0x220, 1);
            if (ob) write_guest(c, ob, b, sizeof b);
            return 0;
        }
        case 500: out_event(c, 0); return 0;
        default: return 0;                                          /* attach/detach streams */
        }
    }
    if (IS("aud:u")) {   /* Switch 2 audio entry: 1 = audio out manager, 2 = audio renderer manager */
        if (cmd == 1) { out_object(c, "audout:u"); return 0; }
        if (cmd == 2) { out_object(c, "audren:u"); return 0; }
        return 0;
    }
    if (IS("audren:u")) {
        if (cmd == 0) {                                   /* OpenAudioRenderer(AudioRendererParameter) */
            e->aud_voices = arg32(c, 16); e->aud_sinks = arg32(c, 20); e->aud_effects = arg32(c, 24);
            e->aud_revision = arg32(c, 48);
            elog(e, "[audio] renderer: %u Hz, %u voices, %u sinks, %u effects, revision %08x\n",
                 arg32(c, 0), e->aud_voices, e->aud_sinks, e->aud_effects, e->aud_revision);
            out_object(c, "IAudioRenderer"); return 0;
        }
        if (cmd == 1) { out_u64(c, 0x10000); return 0; }
        if (cmd == 2 || cmd == 4) { out_object(c, "IAudioDevice"); return 0; }
        return 0;
    }
    if (IS("IAudioRenderer")) {
        if (cmd == 7) {                                             /* QuerySystemEvent: the audio DSP's 5 ms tick */
            out_event(c, 1);
            kservice *t = &e->svcs[c->target];
            if (t->event >= 0 && e->aud_nevents < 4) e->aud_event[e->aud_nevents++] = t->event;
            return 0;
        }
        if (cmd == 4 || cmd == 10) {                                /* RequestUpdate: no mixing yet, report state */
            const ipc_buf *in = c->na && c->a[0].addr ? &c->a[0] : c->nx && c->x[0].addr ? &c->x[0] : NULL;
            ipc_buf *ob = out_buf(c);
            uint32_t rev = e->aud_revision;
            if (in) { uint8_t *p = mem_ptr(e, in->addr, 4); if (p) rev = rd32(p); }
            /* Output: header, then memory pools, voices, effects, sinks, performance, error info and
               render info, each sized for the counts the renderer was opened with. */
            uint32_t rnum = ((rev >> 24) & 0xFF) - '0';
            uint32_t sz_beh = 0xB0, sz_pool = (e->aud_effects + e->aud_voices * 4) * 16, sz_voice = e->aud_voices * 16;
            uint32_t sz_eff = e->aud_effects * (rnum >= 9 ? 0x90 : 0x10), sz_sink = e->aud_sinks * 32, sz_perf = 0x10;
            uint32_t sz_info = rnum >= 5 ? 0x10 : 0;
            uint32_t total = 0x40 + sz_beh + sz_pool + sz_voice + sz_eff + sz_sink + sz_perf + sz_info;
            uint32_t out_rev = (rev & 0xFF00FFFFu) | ((uint32_t)'V' << 16);     /* updates say 'REv?', the renderer 'REV?' */
            if (ob && ob->size >= 0x40) {
                size_t n = total < ob->size ? total : (size_t)ob->size;
                uint8_t *o = calloc(1, n > 0x40 ? n : 0x40);
                if (o) {
                    uint32_t hdr[16] = { out_rev, sz_beh, sz_pool, sz_voice, 0, sz_eff, 0, sz_sink, sz_perf, 0, sz_info, 0, 0, 0, 0, total };
                    for (int k = 0; k < 16; k++) wr32(o + k * 4, hdr[k]);
                    if (sz_info && total <= n) wr64(o + total - 0x10, e->aud_updates);   /* frames elapsed */
                    write_guest(c, ob, o, n);
                    free(o);
                }
            }
            e->aud_updates++;
            if (e->aud_updates == 1) elog(e, "[audio] first renderer update (revision %08x)\n", rev);
            return 0;
        }
        if (cmd == 0) { out_u32(c, 48000); return 0; }
        return 0;
    }
    if (IS("audout:u")) { if (cmd == 1 || cmd == 3) { out_u32(c, 48000); out_u32(c, 2); out_u32(c, 2); out_u32(c, 2); out_object(c, "IAudioOut"); } return 0; }
    if (IS("ILibraryAppletCreator")) {
        elog(e, "[applet] ILibraryAppletCreator cmd %u (applet id %llx)\n", cmd, (unsigned long long)(arg64(c, 0) & 0xFFFFFFFF));
        if (cmd < 10) { out_object(c, "ILibraryAppletAccessor"); return 0; }   /* CreateLibraryApplet (0; 3 on Switch 2) */
        if (cmd == 10 || cmd == 11) { out_object(c, "IStorage(applet)"); return 0; }   /* CreateStorage / CreateTransferMemoryStorage */
        return 0;
    }
    /* A library applet (controller support, software keyboard, ...) that finishes at once:
       it reports completion and hands back a result saying "one player, controller 1". */
    if (IS("ILibraryAppletAccessor")) {
        elog(e, "[applet] accessor cmd %u\n", cmd);
        switch (cmd) {
        case 0: {                                                  /* GetAppletStateChangedEvent */
            out_event(c, 1);                                       /* the stand-in is always finished: it stays signalled */
            kservice *t = &e->svcs[c->target];
            if (t->event >= 0) e->events[t->event].sticky = 1;
            return 0;
        }
        case 1: out_u8(c, 1); return 0;                            /* IsCompleted */
        case 101: out_object(c, "IStorage(applet-out)"); return 0; /* PopOutData */
        case 105: case 106: out_event(c, cmd == 105); return 0;    /* PopOutData / PopInteractiveOutData events */
        case 10: case 90: {                                        /* Start (90 on Switch 2): it finishes at once */
            kservice *t = &e->svcs[c->target];
            if (t->event < 0) t->event = event_new(e, 1); else event_signal(e, t->event);
            return 0;
        }
        default: return 0;                                         /* RequestExit, GetResult, PushInData ... */
        }
    }
    if (IS("IStorage(applet)")) { if (cmd == 0) out_object(c, "IStorageAccessor(applet)"); return 0; }
    if (IS("IStorageAccessor(applet)")) { if (cmd == 0) out_u64(c, 0x1000); return 0; }   /* writes are ignored */
    if (IS("IStorage(applet-out)")) { if (cmd == 0) out_object(c, "IStorageAccessor(applet-out)"); return 0; }
    if (IS("IStorageAccessor(applet-out)")) {
        uint8_t data[0x10] = {0};                                  /* ControllerSupportResultInfo */
        data[0] = 1;                                               /* player count */
        wr32(data + 4, 0);                                         /* selected npad: No1 */
        if (cmd == 0) { out_u64(c, sizeof data); return 0; }
        if (cmd == 11) {
            uint64_t off = arg64(c, 0);
            ipc_buf *b = out_buf(c);
            if (b && off < sizeof data) write_guest(c, b, data + off, sizeof data - (size_t)off);
        }
        return 0;
    }
    if (IS("ns:am2") || IS("ns:ec") || IS("ns:rid") || IS("ns:rt") || IS("ns:web")) {
        out_object(c, "INsInterface"); return 0;
    }
    if (IS("aoc:u")) { if (cmd == 2 || cmd == 102) { out_u32(c, 0); return 0; } if (cmd == 8) { out_event(c, 0); return 0; } return 0; }
    if (IS("ssl")) { if (cmd == 0) { out_object(c, "ISslContext"); return 0; } return 0; }
    if (IS("bsd:u") || IS("bsd:s")) { out_u32(c, 0); out_u32(c, 0); return 0; }
    if (IS("friend:u") || IS("friend:a")) { out_object(c, "IFriendService"); return 0; }
    /* Hardware Opus decoder (hwopus). No decoding yet: each packet is consumed whole and comes
       out as silence of the right length, so streamed music and voice keep advancing. */
    if (IS("hwopus")) {
        if (cmd == 4 || cmd == 0) { out_object(c, "IHardwareOpusDecoder"); return 0; }  /* Open(Ex)(rate, channels, tmem) */
        if (cmd == 1 || cmd == 5 || cmd == 7 || cmd == 8) { out_u32(c, 0x20000); return 0; }  /* work buffer sizes */
        return 0;
    }
    if (IS("IHardwareOpusDecoder")) {
        if (cmd == 0 || cmd == 4 || cmd == 6 || cmd == 8) {                      /* DecodeInterleaved(...) */
            const ipc_buf *in = c->na && c->a[0].addr ? &c->a[0] : c->nx && c->x[0].addr ? &c->x[0] : NULL;
            ipc_buf *ob = out_buf(c);
            uint32_t consumed = 0, samples = 0;
            if (in && in->size >= 9) {
                uint8_t h[10] = {0};
                uint8_t *p = mem_ptr(e, in->addr, in->size >= 10 ? 10 : 9);
                if (p) memcpy(h, p, in->size >= 10 ? 10 : 9);
                uint32_t len = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];   /* big-endian */
                consumed = 8 + len;
                if (consumed > in->size) consumed = (uint32_t)in->size;
                uint8_t toc = h[8], conf = toc >> 3;
                uint32_t dur10 = conf < 12 ? (uint32_t[]){100, 200, 400, 600}[conf & 3] :           /* tenths of ms */
                                 conf < 16 ? (uint32_t[]){100, 200}[conf & 1] : (uint32_t[]){25, 50, 100, 200}[conf & 3];
                uint32_t frames = (toc & 3) == 0 ? 1 : (toc & 3) < 3 ? 2 : (in->size >= 10 ? (h[9] & 0x3F) : 1);
                samples = frames * dur10 * 48 / 10;                                   /* 48 kHz */
            }
            if (ob && ob->addr && ob->size) {                                          /* silence */
                uint8_t *q = mem_ptr(e, ob->addr, ob->size);
                if (q) memset(q, 0, (size_t)ob->size);
            }
            out_u32(c, consumed); out_u32(c, samples);
            if (cmd != 0) out_u64(c, 0);                                               /* time taken (us) */
            return 0;
        }
        return 0;
    }
    if (IS("prepo:u") || IS("prepo:a")) return 0;
    /* amiibo (NFP) and NFC: a reader with no devices attached */
    if (IS("nfp:user") || IS("nfp:sys") || IS("nfp:dbg")) { if (cmd == 0) out_object(c, "INfpUser"); return 0; }
    if (IS("nfc:user") || IS("nfc:sys")) { if (cmd == 0) out_object(c, "INfcUser"); return 0; }
    if (IS("INfpUser")) {
        switch (cmd) {
        case 2: out_u32(c, 0); return 0;                           /* ListDevices -> none */
        case 17: case 18: case 23: out_event(c, 0); return 0;      /* Activate / Deactivate / AvailabilityChange events */
        case 19: out_u32(c, 1); return 0;                          /* GetState: initialized */
        case 20: out_u32(c, 0); return 0;                          /* GetDeviceState */
        default: return 0;
        }
    }
    if (IS("INfcUser")) {
        switch (cmd) {
        case 2: case 402: out_u32(c, 1); return 0;                 /* GetState: initialized */
        case 3: case 403: out_u8(c, 0); return 0;                  /* IsNfcEnabled: no */
        default: return 0;
        }
    }
    if (IS("lm")) { if (cmd == 0) { out_object(c, "ILogger"); } return 0; }
    if (IS("apm") || IS("apm:p")) { if (cmd == 0) { out_object(c, "IApmSession"); return 0; } if (cmd == 1) { out_u32(c, 0); return 0; } return 0; }
    if (IS("nifm:u") || IS("nifm:a") || IS("nifm:s")) { out_object(c, "IGeneralService"); return 0; }
    if (IS("IGeneralService")) { if (cmd == 4) { out_object(c, "IRequest"); return 0; } return 0; }
    if (IS("IRequest")) { if (cmd == 0) { out_u32(c, 1); return 0; } if (cmd == 2) { out_event(c, 1); out_event(c, 1); return 0; } return 0; }

    /* generic: success with zeroed output */
    for (int k = 0; k < 8; k++) out_u64(c, 0);
    return 0;
}

/* ---------------- message parsing / building ---------------- */
static uint32_t do_request(mgr_emu *e, int session) {
    uint64_t tls = cur_tls(e);
    uint8_t *m = mem_ptr(e, tls, 0x100);
    if (!m) return 0xE401;
    uint8_t msg[0x100];
    memcpy(msg, m, sizeof msg);

    uint32_t w0 = rd32(msg), w1 = rd32(msg + 4);
    if (getenv("MGR_IPC_DUMP")) {
        elog(e, "[ipcdump] %s:", e->svcs[session].name);
        for (int k = 0; k < 24; k++) elog(e, " %08x", rd32(msg + k * 4));
        elog(e, "\n");
    }
    uint32_t type = w0 & 0xFFFF;
    int nx = (w0 >> 16) & 15, na = (w0 >> 20) & 15, nb = (w0 >> 24) & 15, nw = (w0 >> 28) & 15;
    uint32_t ndata = w1 & 0x3FF, c_mode = (w1 >> 10) & 15, c_off = (w1 >> 20) & 0x7FF;
    size_t pos = 8;
    ipc_ctx *c = calloc(1, sizeof *c);
    if (!c) return 0xD001;
    c->e = e; c->session = session; c->target = session;
    if (w1 >> 31) {
        uint32_t sh = rd32(msg + pos); pos += 4;
        if (sh & 1) pos += 8;
        int ncopy = (sh >> 1) & 15, nmove = (sh >> 5) & 15;
        for (int k = 0; k < ncopy && k < 8; k++) c->in_copy[c->n_in_copy++] = rd32(msg + pos + (size_t)k * 4);
        pos += (size_t)(ncopy + nmove) * 4;
    }
    for (int k = 0; k < nx && pos + 8 <= sizeof msg; k++, pos += 8) {
        uint32_t d0 = rd32(msg + pos), d1 = rd32(msg + pos + 4);
        if (c->nx < MAX_BUF) { c->x[c->nx].addr = d1 | ((uint64_t)((d0 >> 12) & 15) << 32) | ((uint64_t)((d0 >> 6) & 7) << 36); c->x[c->nx++].size = d0 >> 16; }
    }
    for (int k = 0; k < na + nb + nw && pos + 12 <= sizeof msg; k++, pos += 12) {
        uint32_t s0 = rd32(msg + pos), a0 = rd32(msg + pos + 4), f = rd32(msg + pos + 8);
        ipc_buf bf = { a0 | ((uint64_t)((f >> 28) & 15) << 32) | ((uint64_t)((f >> 2) & 7) << 36),
                       s0 | ((uint64_t)((f >> 24) & 15) << 32) };
        if (k < na) { if (c->na < MAX_BUF) c->a[c->na++] = bf; }
        else if (k < na + nb) { if (c->nb < MAX_BUF) c->b[c->nb++] = bf; }
    }
    size_t raw = pos;
    size_t payload = (raw + 15) & ~(size_t)15;
    if (c_mode >= 2) {
        size_t cp = c_off ? (size_t)c_off * 4 : raw + (size_t)ndata * 4;
        int count = c_mode == 2 ? 1 : (int)c_mode - 2;
        for (int k = 0; k < count && cp + 8 <= sizeof msg && c->nc < MAX_BUF; k++, cp += 8) {
            uint32_t d0 = rd32(msg + cp), d1 = rd32(msg + cp + 4);
            c->c[c->nc].addr = d0 | ((uint64_t)(d1 & 0xFFFF) << 32);
            c->c[c->nc++].size = d1 >> 16;
        }
    }

    kservice *root = &e->svcs[session];
    uint32_t result = 0;
    int domain_close = 0;
    if (type == 2) { free(c); memset(m, 0, 16); return 0; }   /* Close */
    if (type == 15 && (!strncmp(root->name, "nx2:", 4) || !strcmp(root->name, "gsv") || !strcmp(root->name, "sm:"))) {
        free(c); memset(m, 0, 16);                             /* TIPC close (type 15): the reply is "session closed" */
        root->used = 0;
        return 0xF601;
    }

    if (type >= 16) {
        /* TIPC (firmware 12+, used by sm: and newer system services):
           command id = type - 16, raw arguments follow the header directly,
           response raw data = result word followed by output values. */
        c->cmd = type - 16;
        c->args = msg + raw;
        c->args_len = sizeof msg - raw;
        if (e->ipc_log_budget) { e->ipc_log_budget--; elog(e, "[ipc] t%d %s tipc cmd %u\n", e->cur, root->name, c->cmd); }
        result = (root->name[0] == 's' && !strcmp(root->name, "sm:") && c->cmd == 0) ? 0 : dispatch(c);
        uint8_t out[0x100];
        memset(out, 0, sizeof out);
        size_t p = 8;
        int special = c->n_copy || c->n_move;
        if (special) {
            wr32(out + p, ((uint32_t)c->n_copy << 1) | ((uint32_t)c->n_move << 5)); p += 4;
            for (int k = 0; k < c->n_copy; k++, p += 4) wr32(out + p, c->copy_h[k]);
            for (int k = 0; k < c->n_move; k++, p += 4) wr32(out + p, c->move_h[k]);
        }
        size_t rstart = p;
        wr32(out + p, result); p += 4;
        size_t ol = c->out_len;
        if (p + ol > sizeof out) ol = sizeof out - p;
        memcpy(out + p, c->out, ol);
        p += (ol + 3) & ~(size_t)3;
        wr32(out, type);
        wr32(out + 4, (uint32_t)((p - rstart) / 4) | (special ? 0x80000000u : 0));
        memcpy(m, out, sizeof out);
        free(c);
        return 0;
    }

    if ((type == 4 || type == 6) && root->is_domain && payload + 16 <= sizeof msg) {
        c->domain = 1;
        uint8_t dcmd = msg[payload];
        int nin = msg[payload + 1];
        uint16_t dsize = rd16(msg + payload + 2);
        uint32_t objid = rd32(msg + payload + 4);
        payload += 16;
        for (int k = 0; k < nin && k < 8 && payload + dsize + (size_t)k * 4 + 4 <= sizeof msg; k++)
            c->in_objs[c->n_in_objs++] = rd32(msg + payload + dsize + (size_t)k * 4);
        if (objid < MAX_DOMAIN_OBJS && root->domain_objs[objid]) c->target = root->domain_objs[objid];
        else { elog(e, "[ipc] %s: domain object %u not found\n", root->name, objid); result = 0xF601; }
        if (dcmd == 2) {                          /* close: free the object (never the domain's own session) */
            if (objid < MAX_DOMAIN_OBJS) {
                int s = root->domain_objs[objid];
                if (s > 0 && s != session) e->svcs[s].used = 0;
                root->domain_objs[objid] = 0;
            }
            domain_close = 1;
        }
    }

    if (!result && !domain_close && payload + 16 <= sizeof msg) {
        if (rd32(msg + payload) != SFCI) {
            elog(e, "[ipc] %s: bad CMIF magic %08x\n", root->name, rd32(msg + payload));
        }
        c->cmd = rd32(msg + payload + 8);
        c->args = msg + payload + 16;
        c->args_len = sizeof msg - (payload + 16);
        if (type == 5 || type == 7) { /* control (7 = ControlWithContext, newer firmware) */
            if (e->ipc_log_budget) { e->ipc_log_budget--; elog(e, "[ipc] t%d %s control %u\n", e->cur, root->name, c->cmd); }
            switch (c->cmd) {
            case 0: root->is_domain = 1; root->domain_objs[1] = session; out_u32(c, 1); break;
            case 2: case 4: c->move_h[c->n_move++] = handle_new(e, KO_SESSION, session); break;
            case 3: out_u32(c, 0x8000); break;
            default: break;
            }
        } else {
            /* file reads are the bulk of a game's IPC while loading: log a few, without using the budget */
            int bulk = !strncmp(e->svcs[c->target].name, "IStorage(romfs)", 15) || !strncmp(e->svcs[c->target].name, "IFile", 5);
            if (bulk) { if (e->ipc_bulk_logged < 20) { e->ipc_bulk_logged++; elog(e, "[ipc] t%d %s cmd %u\n", e->cur, e->svcs[c->target].name, c->cmd); } }
            else if (e->ipc_log_budget) {
                e->ipc_log_budget--;
                elog(e, "[ipc] t%d %s cmd %u\n", e->cur, e->svcs[c->target].name, c->cmd);
            }
            result = dispatch(c);
        }
    }

    /* build the response */
    uint8_t out[0x100];
    memset(out, 0, sizeof out);
    size_t p = 8;
    int special = c->n_copy || c->n_move;
    if (special) {
        wr32(out + p, ((uint32_t)c->n_copy << 1) | ((uint32_t)c->n_move << 5)); p += 4;
        for (int k = 0; k < c->n_copy; k++, p += 4) wr32(out + p, c->copy_h[k]);
        for (int k = 0; k < c->n_move; k++, p += 4) wr32(out + p, c->move_h[k]);
    }
    size_t rstart = p;
    size_t q = (p + 15) & ~(size_t)15;
    if (c->domain) { wr32(out + q, (uint32_t)c->n_out_objs); q += 16; }
    wr32(out + q, SFCO); wr32(out + q + 4, 0); wr32(out + q + 8, result); wr32(out + q + 12, 0);
    q += 16;
    size_t ol = c->out_len;
    if (q + ol > sizeof out) ol = sizeof out - q;
    memcpy(out + q, c->out, ol);
    q += (ol + 3) & ~(size_t)3;
    if (c->domain) for (int k = 0; k < c->n_out_objs && q + 4 <= sizeof out; k++, q += 4) wr32(out + q, c->out_objs[k]);
    uint32_t words = (uint32_t)((q - rstart + 3) / 4) + 4;
    wr32(out, 0);
    wr32(out + 4, (words & 0x3FF) | (special ? 0x80000000u : 0));
    memcpy(m, out, sizeof out);
    free(c);
    return 0;
}

uint32_t ipc_send_sync_request(mgr_emu *e, uint32_t handle) {
    khandle *k = handle_get(e, handle);
    if (!k || k->type != KO_SESSION) {
        elog(e, "[ipc] SendSyncRequest on invalid handle 0x%x\n", handle);
        emu_backtrace(e);
        return 0xE401;
    }
    return do_request(e, k->idx);
}

uint32_t ipc_connect_named_port(mgr_emu *e, const char *name, uint32_t *out_handle) {
    elog(e, "[kern] ConnectToNamedPort(\"%s\")\n", name);
    int s = service_new(e, name);
    if (!s) return 0xD201;
    *out_handle = handle_new(e, KO_SESSION, s);
    return 0;
}
