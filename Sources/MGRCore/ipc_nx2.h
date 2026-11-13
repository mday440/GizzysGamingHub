/*
 * Switch 2 graphics-system services (included by ipc.c).
 *
 * On Switch 2 the SDK reaches the GPU stack through a "graphics system" session
 * (gsv) that ISelfController 2000 hands out, and gsv in turn opens syncpoint,
 * GPU, display (vi2) and NVIDIA driver (nvsrv) sessions. They speak TIPC.
 *
 * Command meanings follow the Eden-NX2 project's published reverse-engineering
 * notes (NX2_NOTES.md); this is an independent implementation. Unknown commands
 * answer success with no data and are logged, so new needs show up in the log.
 */

/* Return a new TIPC session as a copy handle (how gsv hands out its sub-services). */
static void out_session_copy(ipc_ctx *c, const char *name) {
    int s = service_new(c->e, name);
    c->copy_h[c->n_copy++] = handle_new(c->e, KO_SESSION, s);
    if (c->e->ipc_log_budget) elog(c->e, "[nx2] opened session %s\n", name);
}

static void out_new_event(ipc_ctx *c, int signaled) {
    int ev = event_new(c->e, signaled);
    c->copy_h[c->n_copy++] = handle_new(c->e, KO_EVENT, ev);
}

static uint32_t args32(ipc_ctx *c, int i) { return (size_t)i * 4 + 4 <= c->args_len ? rd32(c->args + i * 4) : 0; }

static ktmem *in_tmem(ipc_ctx *c, int k) {
    if (k >= c->n_in_copy) return NULL;
    khandle *h = handle_get(c->e, c->in_copy[k]);
    return (h && h->type == KO_TMEM) ? &c->e->tmem[h->idx] : NULL;
}

/* Syncpoint shared pages (layout from gsv 204): RO page holds a header and one u64 value per
   syncpoint; there is no GPU yet, so every syncpoint reads as far ahead (waits succeed). */
#define NX2_SP_VALUES_OFF   0x40
#define NX2_SP_RO_SUMMARY   0x1040
#define NX2_SP_RO_BITS      0x1100
#define NX2_SP_RW_SUMMARY   0x0
#define NX2_SP_RW_BITS      0x100
#define NX2_SP_FAR_AHEAD    0x7FFFFFFFull

/* nx2:gpu job-slot events (stored +1 in the emulator so zero means "not created"). */
#define NX2_GPU_SLOTS 32
static int gpu_slot_event(mgr_emu *e, uint32_t id) {
    id %= NX2_GPU_SLOTS;
    if (!e->nx2_gpu_ev[id]) e->nx2_gpu_ev[id] = event_new(e, 0) + 1;
    return e->nx2_gpu_ev[id] - 1;
}

/* Native image layout from an image preference (16 words): word 2 = (w-1) | (h-1) << 16,
   word 4 = block height log2 (GOBs), word 5 = pitch in 64-byte GOBs; 4 bytes per pixel. */
typedef struct { uint32_t w, h, bh_log2; uint64_t pitch, size, align; } nx2_layout;
static nx2_layout nx2_image_layout(const uint32_t *p) {
    nx2_layout L;
    L.w = (p[2] & 0xFFFF) + 1; L.h = (p[2] >> 16) + 1;
    uint64_t gobs = p[5];
    if (p[4] <= 8 && gobs >= ((uint64_t)L.w * 4 + 63) / 64 && gobs <= 4096) { L.pitch = gobs * 64; L.bh_log2 = p[4]; }
    else { L.pitch = ((uint64_t)L.w * 4 + 63) & ~63ull; L.bh_log2 = 7; }
    uint64_t rows = 1ull << L.bh_log2;
    L.size = L.pitch * (((uint64_t)L.h + rows - 1) / rows * rows);
    L.align = 64 * rows > 0x1000 ? 64 * rows : 0x1000;
    return L;
}

#include "gpu_image.h"
/* MGR_FRAME_DUMP=N: every Nth presented image is saved as a PNG (quarter size) in
   $MGR_DUMP_DIR (default /tmp/mgr_dump/frames). */
static void nx2_capture_frame(mgr_emu *e, uint32_t img) {
    static int every = -1; static char dir[400];
    if (every < 0) {
        every = getenv("MGR_FRAME_DUMP") ? atoi(getenv("MGR_FRAME_DUMP")) : 0;
        snprintf(dir, sizeof dir, "%s", getenv("MGR_DUMP_DIR") ? getenv("MGR_DUMP_DIR") : "/tmp/mgr_dump/frames");
        if (every > 0) { char cmd[460]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir); if (system(cmd)) {} }
    }
    if (every <= 0 || e->nx2_presents % (uint64_t)every) return;
    for (uint32_t k = 1; k < 256 && e->nx2_last_draw_rt; k++)      /* the image the frame was drawn into */
        if (e->nx2_img[k].memory && e->nx2_img[k].memory < 256 && e->nx2_nmem_addr[e->nx2_img[k].memory] == e->nx2_last_draw_rt) { img = k; break; }
    if (img >= 256 || !e->nx2_img[img].memory) return;
    uint64_t base = e->nx2_img[img].memory < 256 ? e->nx2_nmem_addr[e->nx2_img[img].memory] : 0;
    nx2_layout L = nx2_image_layout(e->nx2_img[img].pref);
    if (!base || !L.size) return;
    uint8_t *src = mem_ptr(e, base, L.size);
    if (!src) return;
    uint32_t bh = (uint32_t)(1u << L.bh_log2) / 8; if (!bh) bh = 1;      /* GOBs per block */
    uint32_t ow = L.w / 4, oh = L.h / 4;
    uint8_t *rgb = malloc((size_t)ow * oh * 3); if (!rgb) return;
    uint64_t nonzero = 0;
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++) {
            uint64_t o = bl_offset(x * 4 * 4, y * 4, (uint32_t)L.pitch, bh);
            const uint8_t *p = o + 4 <= L.size ? src + o : (const uint8_t *)"\0\0\0\0";
            uint8_t *q = rgb + ((size_t)y * ow + x) * 3; q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
            nonzero += p[0] | p[1] | p[2];
        }
    char path[480]; snprintf(path, sizeof path, "%s/frame_%06llu.png", dir, (unsigned long long)e->nx2_presents);
    png_write_rgb(path, rgb, ow, oh);
    free(rgb);
    elog(e, "[nx2] saved %s (image %u at %llx, %ux%u, %s)\n", path, img, (unsigned long long)base, L.w, L.h, nonzero ? "has content" : "all black");
}

/* vi2 32 carries a handle descriptor before its index word when the special header is set. */
static uint32_t nx2_vi_dup_index(ipc_ctx *c) { return args32(c, 0); }

static int nx2_dispatch(ipc_ctx *c, uint32_t *result) {
    mgr_emu *e = c->e;
    const char *name = e->svcs[c->target].name;
    uint32_t cmd = c->cmd;
    *result = 0;

    if (!strcmp(name, "gsv")) {
        if (cmd >= 900 && cmd < 1100) elog(e, "[vdec] t%d gsv cmd %u (%08x %08x)\n", e->cur, cmd, args32(c, 0), args32(c, 1));
        switch (cmd) {
        case 115: out_u32(c, args32(c, 0)); out_u32(c, args32(c, 1)); return 1;   /* QueryGrantedFeatures: echo */
        case 126: out_u32(c, 0); return 1;                                         /* GetSessionFlag */
        case 202: out_session_copy(c, "nx2:syncpt"); return 1;                     /* OpenSyncptSession */
        case 305: out_session_copy(c, "nx2:vi2"); return 1;                        /* OpenViSession */
        case 497: out_session_copy(c, "nx2:gpu"); return 1;                        /* OpenGpuSession */
        case 1402: out_session_copy(c, "nx2:nvsrv"); return 1;                     /* OpenNvsrvSession */
        case 204: {                                                                /* AttachSyncptMemory(version, max) + rw, ro tmem */
            uint32_t maxsp = args32(c, 1);
            if (maxsp > (0x1000 - 0x40) / 8) maxsp = (0x1000 - 0x40) / 8;
            ktmem *rw = in_tmem(c, 0), *ro = in_tmem(c, 1);
            if (rw && ro && ro->size >= 0x2000) {
                e->nx2_syncpt_rw = rw->addr; e->nx2_syncpt_ro = ro->addr; e->nx2_syncpt_count = maxsp;
                uint64_t r = ro->addr;
                mwr(e, r + 0x00, 4, 1);                   /* version */
                mwr(e, r + 0x04, 4, 0);                   /* base id */
                mwr(e, r + 0x08, 4, maxsp);
                mwr(e, r + 0x0c, 4, NX2_SP_VALUES_OFF);
                mwr(e, r + 0x10, 4, NX2_SP_RW_SUMMARY);
                mwr(e, r + 0x14, 4, NX2_SP_RO_SUMMARY);
                mwr(e, r + 0x18, 4, NX2_SP_RW_BITS);
                mwr(e, r + 0x1c, 4, NX2_SP_RO_BITS);
                for (uint32_t k = 0; k < maxsp; k++) mwr(e, r + NX2_SP_VALUES_OFF + k * 8, 8, NX2_SP_FAR_AHEAD);
                elog(e, "[nx2] syncpoint pages rw=%llx ro=%llx count=%u\n", (unsigned long long)rw->addr,
                     (unsigned long long)ro->addr, maxsp);
            } else elog(e, "[nx2] gsv 204: syncpoint transfer memory missing\n");
            out_new_event(c, 0);     /* request event (signalled by the client) */
            {   /* notify event: signalled whenever a syncpoint advances */
                int ev = event_new(e, 1);
                c->copy_h[c->n_copy++] = handle_new(e, KO_EVENT, ev);
                if (e->nx2_sp_nev < 16) e->nx2_sp_ev[e->nx2_sp_nev++] = ev;
            }
            return 1;
        }
        case 1510: {                                                               /* CalculateRequirements */
            uint32_t pref[16]; for (int k = 0; k < 16; k++) pref[k] = args32(c, k);
            nx2_layout L = nx2_image_layout(pref);
            int simple = args32(c, 16) == 1 && pref[0] == 0 && pref[1] == 2 && pref[3] == 0 && pref[4] >= 3 &&
                         pref[4] <= 8 && pref[6] == 1 && pref[7] == 1 && L.pitch == (uint64_t)pref[5] * 64;
            for (int k = 8; k < 16; k++) if (pref[k]) simple = 0;
            out_u64(c, L.size); out_u64(c, simple ? 0x1000 : L.align);
            return 1;
        }
        case 435: {                                                                /* CalculateNativeImageSize */
            uint32_t pref[16]; for (int k = 0; k < 16; k++) pref[k] = args32(c, k);
            uint32_t n = args32(c, 16) ? args32(c, 16) : 1;
            out_u64(c, nx2_image_layout(pref).size * n);
            return 1;
        }
        case 111: out_u32(c, ++e->nx2_next_native_mem); return 1;                 /* CreateNativeMemory -> id */
        case 423: {                                                                /* bind native memory (id, address, size, gpu handle, offset) */
            uint32_t id = args32(c, 0);
            if (id < 256) e->nx2_nmem_addr[id] = (uint64_t)args32(c, 1) | (uint64_t)args32(c, 2) << 32;
            return 1;
        }
        case 429: {                                                                /* CreateNativeImage -> id (ids id..id+count-1) */
            uint32_t n = args32(c, 16) ? args32(c, 16) : 1;
            if (!e->nx2_next_image) e->nx2_next_image = 1;
            uint32_t id = e->nx2_next_image;
            e->nx2_next_image += n;
            if (id < 256) {
                for (int k = 0; k < 16; k++) e->nx2_img[id].pref[k] = args32(c, k);
                e->nx2_img[id].count = n; e->nx2_img[id].memory = args32(c, 17);
                e->nx2_img[id].size = nx2_image_layout(e->nx2_img[id].pref).size;
                elog(e, "[nx2] native image %u: pref %x %x %x %x %x %x %x %x, memory %u\n", id, e->nx2_img[id].pref[0], e->nx2_img[id].pref[1],
                     e->nx2_img[id].pref[2], e->nx2_img[id].pref[3], e->nx2_img[id].pref[4], e->nx2_img[id].pref[5], e->nx2_img[id].pref[6],
                     e->nx2_img[id].pref[7], e->nx2_img[id].memory);
                elog(e, "[nx2] native image %u: %ux%u x%u, %llu bytes\n", id, (e->nx2_img[id].pref[2] & 0xFFFF) + 1,
                     (e->nx2_img[id].pref[2] >> 16) + 1, n, (unsigned long long)e->nx2_img[id].size);
            }
            out_u32(c, id); return 1;
        }
        case 991: out_u32(c, 0x300000); out_u32(c, 0); return 1;                  /* decoder output image size (fits 1920x1088 NV12) */
        case 961: {                                                                /* GetCpuUserImageDescriptor(image id) */
            /* nn::gs::sdk::ReadUserImageNv12 reads this: kind 0x0A = pitch-linear NV12,
               +8 luma offset, +16 luma pitch, +24 chroma offset, +32 chroma pitch. */
            uint8_t d[0x80] = {0};
            d[0] = 0x0A;
            wr32(d + 8, 0); wr64(d + 16, 1920); wr32(d + 24, 1920 * 1088); wr64(d + 32, 1920);
            out_bytes(c, d, sizeof d);
            return 1;
        }
        case 911: {                                                                /* CreateNativeImage (decoder output) -> handle */
            uint32_t n = args32(c, 1) ? args32(c, 1) : 1;
            if (!e->nx2_next_image) e->nx2_next_image = 1;
            uint32_t id = e->nx2_next_image; e->nx2_next_image += n;
            out_u32(c, id); return 1;
        }
        case 906: out_session_copy(c, "nx2:vdec"); return 1;                       /* video decoder server context (movies) */
        case 1002: out_session_copy(c, "nx2:venc"); return 1;                      /* video encoder server context */
        default: return 1;                                                         /* boot / permission checks: success */
        }
    }
    /* Video codec server (nn::codec::video): the movie player's AV1/VP9 decoders. There is no
       decoder yet: sessions open and every call succeeds, and each call is logged so the
       protocol shows up in the run log. */
    if (!strcmp(name, "nx2:vdec") || !strcmp(name, "nx2:vdec-decoder") || !strcmp(name, "nx2:venc")) {
        static unsigned vlog;
        static uint32_t out_ids[16], n_ids, next_out, vw = 1920, vh = 1080, fps = 60;
        static uint64_t frames;
        static uint32_t pending, eos, eos_sent;               /* frames submitted but not yet handed back; end of stream */
        if (vlog < 300) { vlog++; elog(e, "[vdec] t%d %s cmd %u (%08x %08x %08x %08x)\n", e->cur, name, cmd, args32(c, 0), args32(c, 1), args32(c, 2), args32(c, 3)); }
        else {                                          /* after that: new command numbers, and every 1000th call */
            static uint8_t seen[1024]; static uint64_t total;
            total++;
            if ((cmd < 1024 && !seen[cmd]) || total % 1000 == 0)
                elog(e, "[vdec] %s cmd %u (%08x %08x %08x %08x) [call %llu]\n", name, cmd, args32(c, 0), args32(c, 1),
                     args32(c, 2), args32(c, 3), (unsigned long long)total);
            if (cmd < 1024) seen[cmd] = 1;
        }
        if (!strcmp(name, "nx2:vdec") && cmd == 1) {                                      /* CreateVideoDecoderService */
            n_ids = next_out = 0; frames = 0; pending = eos = eos_sent = 0;                   /* a new movie: fresh state */
            out_session_copy(c, "nx2:vdec-decoder");
            return 1;
        }
        if (!strcmp(name, "nx2:vdec-decoder")) {
            static int dec_ev = -1;
            if (dec_ev >= 0 && (pending || eos || cmd != 213)) event_signal(e, dec_ev);   /* "decoder has news" */
            switch (cmd) {
            case 3: {                                              /* GetEvent: the decoder's system event */
                dec_ev = event_new(e, 1);
                c->copy_h[c->n_copy++] = handle_new(e, KO_EVENT, dec_ev);
                return 1;
            }
            case 109: {                                            /* submit input (one compressed frame) */
                static uint32_t n109;
                n109++;
                if (args32(c, 0) == 0xFFFFFFFFu && args32(c, 1) == 0xFFFFFFFFu) eos = !getenv("MGR_VDEC_NOEOS");   /* end-of-stream marker */
                else pending++;
                if (dec_ev >= 0) event_signal(e, dec_ev);
                if (args32(c, 1) || args32(c, 2) || args32(c, 3) || args32(c, 4) || args32(c, 5))
                    if (n109 < 50 || n109 % 500 == 0 || args32(c, 2) & ~0x700u)
                        elog(e, "[vdec] submit #%u: %08x %08x %08x %08x %08x %08x %08x %08x\n", n109, args32(c, 0), args32(c, 1),
                             args32(c, 2), args32(c, 3), args32(c, 4), args32(c, 5), args32(c, 6), args32(c, 7));
                return 1;
            }
            case 101:                                              /* Configure: a new stream starts */
                if (args32(c, 0)) { vw = args32(c, 0); vh = args32(c, 1); fps = args32(c, 2) ? args32(c, 2) : 60; }
                n_ids = next_out = 0; frames = 0; pending = eos = eos_sent = 0;
                return 1;
            case 207: case 115:                                    /* teardown: forget the old images */
                n_ids = next_out = 0; pending = eos = eos_sent = 0;
                return 1;
            case 311: {                                                /* register an output image (id = handle + index) */
                uint32_t id = args32(c, 0), k;
                for (k = 0; k < n_ids && out_ids[k] != id; k++) {}
                if (k == n_ids && n_ids < 16) out_ids[n_ids++] = id;
                return 1;
            }
            case 213: {                                                /* next decoder event: "output image ready" */
                /* No real decoding yet: hand the registered images back in turn as decoded
                   frames, so movies play through (as black frames) instead of waiting forever. */
                /* One frame comes back per submitted input; after the end-of-stream marker the
                   next event carries the end-of-stream flag (+32), so the player finishes. */
                uint8_t ev[36] = {0};
                int is_eos = 0;
                if (pending) pending--;
                else if (eos) { ev[32] = 1; is_eos = 1; if (!eos_sent++) elog(e, "[vdec] end of stream\n"); }
                if (n_ids && !(is_eos && getenv("MGR_VDEC_EOSNOIMG"))) {   /* (an end-of-stream event without a picture stalls the player) */
                    wr32(ev + 0, out_ids[next_out++ % n_ids]);        /* image id */
                    wr32(ev + 4, vw); wr32(ev + 8, vh);               /* width, height */
                    wr32(ev + 12, vw); wr32(ev + 16, vh);             /* display width, height */
                    uint64_t pts = frames++ * 1000000000ull / fps;    /* presentation time (ns) */
                    for (int b = 0; b < 8; b++) ev[24 + b] = (uint8_t)(pts >> (8 * b));
                }
                out_bytes(c, ev, sizeof ev);
                return 1;
            }
            default: return 1;
            }
        }
        return 1;
    }
    if (!strcmp(name, "nx2:syncpt")) {
        switch (cmd) {
        case 0: out_u32(c, args32(c, 0)); out_u32(c, args32(c, 1)); return 1;     /* Handshake: echo the token */
        case 1: out_u32(c, 32); return 1;                                          /* GetCount (< 0x400) */
        case 22: {                                                                 /* GetSlotEvent: signalled on progress */
            int ev = event_new(e, 1);
            c->copy_h[c->n_copy++] = handle_new(e, KO_EVENT, ev);
            if (e->nx2_sp_nev < 16) e->nx2_sp_ev[e->nx2_sp_nev++] = ev;
            return 1;
        }
        case 103: out_u32(c, nx2_syncpt_value(e, args32(c, 0))); out_u32(c, 0); out_u32(c, 0); return 1; /* QueryCounter */
        default: return 1;
        }
    }
    if (!strcmp(name, "nx2:vi2")) {
        switch (cmd) {
        case 1: case 3: out_u32(c, 1); return 1;
        case 5: out_u32(c, 7); return 1;                                           /* timestamp recorder count */
        case 11: out_new_event(c, 1); return 1;
        case 31: case 32: {                                                        /* native event by index (vsync by default) */
            uint32_t idx = cmd == 31 ? args32(c, 0) : nx2_vi_dup_index(c);
            if (idx >= 16) { *result = 0x72; return 1; }
            if (!e->nx2_vi_ev[idx]) {
                e->nx2_vi_ev[idx] = event_new(e, 0) + 1;
                e->nx2_vsync_mask |= 1u << idx;
                if (!e->nx2_next_vsync) e->nx2_next_vsync = e->ticks + TICK_HZ / 60;
            }
            c->copy_h[c->n_copy++] = handle_new(e, KO_EVENT, e->nx2_vi_ev[idx] - 1);
            return 1;
        }
        case 261: { uint32_t idx = args32(c, 2); if (idx < 16 && e->nx2_vi_ev[idx]) e->nx2_vsync_mask |= 1u << idx; return 1; }
        case 263: case 265: case 267: case 269: case 271: {                       /* change notifications: fire once */
            uint32_t idx = args32(c, 2);
            if (idx < 16 && e->nx2_vi_ev[idx]) { e->nx2_vsync_mask &= ~(1u << idx); event_signal(e, e->nx2_vi_ev[idx] - 1); }
            return 1;
        }
        case 103: out_u32(c, 60); out_u32(c, 0); return 1;                         /* refresh rate */
        case 106: out_u64(c, 2); return 1;                                         /* max buffer resolution mode: handheld */
        case 212: out_u64(c, 0x7fff); return 1;                                    /* free layer buffer slots */
        case 228: out_u64(c, 0x7fff); out_u64(c, 0); out_u64(c, 0); return 1;     /* cancel */
        case 229:                                                                  /* present */
            e->nx2_presents++;
            nx2_gpu_poll(e);
            nx2_capture_frame(e, args32(c, 3));
            nx2_soft_frame(e, e->nx2_presents);
            if (e->nx2_presents <= 4 || e->nx2_presents % 600 == 0)
                elog(e, "[nx2] present #%llu image %u (vsync %llu)\n", (unsigned long long)e->nx2_presents,
                     args32(c, 3), (unsigned long long)e->nx2_vsyncs);
            out_u64(c, 0x7fff); out_u64(c, 0); out_u64(c, 0); return 1;
        default: return 1;
        }
    }
    if (!strcmp(name, "nx2:gpu")) {
        /* nn::gpu job slots: each id has an event the SDK waits on after a job; there is no
           GPU-side work yet, so jobs complete as soon as they are submitted. */
        #define GPU_SLOT_EVENT(id) gpu_slot_event(e, (id))
        switch (cmd) {
        case 11: out_u32(c, 0); out_u32(c, NX2_GPU_SLOTS - 1); return 1;          /* id range */
        case 12: {                                                                 /* event of a slot */
            int ev = GPU_SLOT_EVENT(args32(c, 0));
            c->copy_h[c->n_copy++] = handle_new(e, KO_EVENT, ev);
            return 1;
        }
        case 21: event_signal(e, GPU_SLOT_EVENT(args32(c, 0))); return 1;          /* start: completes */
        case 22: out_session_copy(c, "nx2:gpu"); return 1;                         /* sub-session (same object) */
        case 33: out_u32(c, 1); return 1;
        case 41: {                                                                 /* CreateGraphicsDevice -> id (never 0) */
            uint32_t id = 0;
            for (uint32_t k = 1; k < 32; k++) if (!(e->nx2_gpu_devices & (1u << k))) { e->nx2_gpu_devices |= 1u << k; id = k; break; }
            elog(e, "[nx2] gpu create graphics device -> id %u\n", id);
            if (!id) { *result = 0x1A01; return 1; }
            out_u32(c, id); return 1;
        }
        case 42: { uint32_t id = args32(c, 0) & 31; e->nx2_gpu_devices &= ~(1u << id); return 1; }
        case 201: out_u64(c, 0x8000000ull); out_u64(c, 0xC000000000ull - 0x8000000ull); return 1;  /* user partition */
        case 202: out_u64(c, ~0ull); out_u64(c, 0); return 1;                     /* gsv partition: empty (min > max) */
        case 203: out_u64(c, 1); out_u64(c, 0x8000000ull); out_u64(c, 0xC000000000ull - 0x8000000ull); return 1; /* VA range */
        case 101: {                                                                /* CreateGpuAccessibleMemory -> handle */
            uint64_t start = (uint64_t)args32(c, 0) | (uint64_t)args32(c, 1) << 32;
            uint64_t last  = (uint64_t)args32(c, 2) | (uint64_t)args32(c, 3) << 32;
            uint32_t h = ++e->nx2_gpu_next_mem;
            if (h < NX2_MAX_MEM && last > start) {
                e->nx2_mem[h].cpu = start; e->nx2_mem[h].size = last - start + 1;
                nx2_gpu_map(e, start, start, last - start + 1);                   /* also visible at its own address */
            }
            if (e->ipc_log_budget) elog(e, "[nx2] gpu memory [%llx, %llx] -> handle %u\n", (unsigned long long)start,
                 (unsigned long long)last, h);
            out_u32(c, h); return 1;
        }
        case 102: {                                                                /* DestroyGpuAccessibleMemory */
            uint32_t h = args32(c, 0);
            if (h < NX2_MAX_MEM && e->nx2_mem[h].size) {
                nx2_gpu_unmap(e, e->nx2_mem[h].cpu, e->nx2_mem[h].size);
                e->nx2_mem[h].size = 0;
            }
            event_signal(e, GPU_SLOT_EVENT(args32(c, 1))); return 1;
        }
        case 211: {                                                                /* map [first, last] -> handle + offset */
            uint64_t a = (uint64_t)args32(c, 1) | (uint64_t)args32(c, 2) << 32;
            uint64_t b = (uint64_t)args32(c, 3) | (uint64_t)args32(c, 4) << 32;
            uint32_t h = args32(c, 5);
            uint64_t off = (uint64_t)args32(c, 6) | (uint64_t)args32(c, 7) << 32;
            { static int nl; if (nl < 400 && (a >> 36)) { nl++; elog(e, "[nx2] gpu map [%llx, %llx] -> handle %u + %llx\n", (unsigned long long)a, (unsigned long long)b, h, (unsigned long long)off); } }
            if (h < NX2_MAX_MEM && e->nx2_mem[h].size && b > a) nx2_gpu_map(e, a, e->nx2_mem[h].cpu + off, b - a + 1);
            else elog(e, "[nx2] gpu map of unknown handle %u\n", h);
            event_signal(e, GPU_SLOT_EVENT(args32(c, 15))); return 1;
        }
        case 212: event_signal(e, GPU_SLOT_EVENT(args32(c, 8))); out_u32(c, args32(c, 5) & 0xff); return 1; /* modify kind */
        case 213: {                                                                /* unmap [first, last] */
            uint64_t a = (uint64_t)args32(c, 1) | (uint64_t)args32(c, 2) << 32;
            uint64_t b = (uint64_t)args32(c, 3) | (uint64_t)args32(c, 4) << 32;
            if (b > a) nx2_gpu_unmap(e, a, b - a + 1);
            event_signal(e, GPU_SLOT_EVENT(args32(c, 6))); return 1;
        }
        case 214: { static int nl; if (nl++ < 200) { elog(e, "[nx2] gpu region (214):"); for (int k = 0; k < 10; k++) elog(e, " %08x", args32(c, k)); elog(e, "\n"); } }
            event_signal(e, GPU_SLOT_EVENT(args32(c, 9))); return 1;         /* GPU-only / sparse region */
        case 215: event_signal(e, GPU_SLOT_EVENT(args32(c, 6))); return 1;         /* destroy sparse region */
        default: return 1;
        }
        #undef GPU_SLOT_EVENT
    }
    (void)result;
    return 0;
}

/* ======================= NVIDIA driver (nvsrv / nvdrv protocol) ======================= */

#define NV_MAX_FD 64
static char nv_fd_path[NV_MAX_FD][48];   /* device path per fd (per process; one game at a time) */

static int buf_read(ipc_ctx *c, const ipc_buf *b, void *dst, size_t n) {
    if (!b || !b->addr || !n) return -1;
    uint8_t *p = mem_ptr(c->e, b->addr, n);
    if (!p) return -1;
    memcpy(dst, p, n);
    return 0;
}

/* Input buffer for an ioctl: A (send) or X (pointer), whichever carries data. */
/* k-th input buffer: A (send) then X (pointer), skipping empty auto-select slots. */
static const ipc_buf *nv_in_buf_k(ipc_ctx *c, int k) {
    int seen = 0;
    for (int i = 0; i < c->na; i++) if (c->a[i].addr) { if (seen++ == k) return &c->a[i]; }
    for (int i = 0; i < c->nx; i++) if (c->x[i].addr) { if (seen++ == k) return &c->x[i]; }
    return NULL;
}
static const ipc_buf *nv_in_buf(ipc_ctx *c) { return nv_in_buf_k(c, 0); }
/* k-th output buffer: B (receive) or C (receive list), skipping empty auto-select slots. */
static const ipc_buf *nv_out_buf(ipc_ctx *c, int k) {
    int seen = 0;
    for (int i = 0; i < c->nb; i++) if (c->b[i].addr) { if (seen++ == k) return &c->b[i]; }
    for (int i = 0; i < c->nc; i++) if (c->c[i].addr) { if (seen++ == k) return &c->c[i]; }
    return NULL;
}

/* GPU characteristics block (Switch 1 layout 0xA0, Switch 2 0xD0). Switch 2 reports the Ampere
   GPU (chip 0x17E); values follow Eden-NX2's notes. */
static void nv_fill_characteristics(uint8_t *gc, size_t len, int nx2) {
    uint8_t g[0xD0] = {0};
    wr32(g + 0x00, 0x120); wr32(g + 0x04, 0xB); wr32(g + 0x08, 0xA1); wr32(g + 0x0C, 1);
    wr64(g + 0x10, 0x40000); wr32(g + 0x20, 2); wr32(g + 0x24, 0x20); wr32(g + 0x28, 0x20000);
    wr32(g + 0x2C, 0x20000); wr32(g + 0x30, 0x1B); wr32(g + 0x34, 0x30000); wr32(g + 0x38, 1);
    wr32(g + 0x3C, 0x503); wr32(g + 0x40, 0x503); wr32(g + 0x44, 0x80); wr32(g + 0x48, 0x28);
    wr64(g + 0x50, 0x55);
    wr32(g + 0x58, 0x902D); wr32(g + 0x5C, 0xB197); wr32(g + 0x60, 0xB1C0); wr32(g + 0x64, 0xB06F);
    wr32(g + 0x68, 0xA140); wr32(g + 0x6C, 0xB0B5);
    wr32(g + 0x70, 1); wr32(g + 0x78, 2); wr32(g + 0x7C, 1); wr32(g + 0x84, 1); wr32(g + 0x88, 0x21D70);
    wr64(g + 0x90, 0x6230326D67ull);                      /* "gm20b" */
    if (nx2) {
        wr32(g + 0x00, 0x170); wr32(g + 0x04, 0xE);       /* Ampere, chip 0x17E */
        wr32(g + 0x20, 6);
        wr32(g + 0x3C, 0x808); wr32(g + 0x40, 0x808);     /* SM 8.8 (NVN2 requires it) */
        wr32(g + 0x5C, 0xC797); wr32(g + 0x60, 0xC7C0);   /* AMPERE_B, AMPERE_COMPUTE_B */
        wr32(g + 0x64, 0xC56F); wr32(g + 0x6C, 0xC7B5);   /* AMPERE GPFIFO, AMPERE DMA COPY */
        wr64(g + 0x90, 0x6631306167ull);                  /* "ga10f" */
        uint64_t flags = 0x55 | 0x100 | (1ull << 22) | (1ull << 26) | (1ull << 34) | (1ull << 35) | (0x1Full << 40);
        wr64(g + 0x50, flags);                            /* TSG, subcontexts, syncpoint address, mapping modify, remap, engines */
        for (int off = 0x70; off <= 0x80; off += 2) { g[off] = 0x40; g[off + 1] = 0; }  /* interface versions */
        wr32(g + 0xB4, 64);                               /* subcontexts (VEIDs) per TSG */
    }
    memcpy(gc, g, len < sizeof g ? len : sizeof g);
}

/* Device ioctls beyond GET_CHARACTERISTICS. buf holds the argument struct (in/out).
   Returns 1 when handled. Values follow the nvgpu driver ABI; GPU topology is the
   Switch 2 one we report in the characteristics (1 GPC, 2 TPCs per GPC shown as mask 3). */
static int nv_ioctl_dev(ipc_ctx *c, const char *dev, uint32_t type, uint32_t nr,
                        uint8_t *buf, uint32_t size, int has_inline_out) {
    mgr_emu *e = c->e;
    const ipc_buf *io = has_inline_out ? nv_out_buf(c, 1) : NULL;
    if (!strcmp(dev, "/dev/nvhost-ctrl-gpu") && type == 'G') {
        switch (nr) {
        case 0x01: wr32(buf, 1); return 1;                                   /* ZCULL_GET_CTX_SIZE */
        case 0x02: {                                                         /* ZCULL_GET_INFO */
            static const uint32_t z[10] = { 0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xc0, 0x20, 0x40, 0x10 };
            for (int k = 0; k < 10 && (uint32_t)k * 4 + 4 <= size; k++) wr32(buf + k * 4, z[k]);
            return 1;
        }
        case 0x06: {                                                         /* GET_TPC_MASKS */
            if (rd32(buf)) wr32(buf + 16, 3);
            if (io) { uint8_t m[4]; wr32(m, rd32(buf + 16)); write_guest(c, io, m, 4); }
            return 1;
        }
        case 0x13: {                                                         /* VSMS_MAPPING: {gpc, tpc} per SM */
            if (io && io->size >= 2) {
                size_t n = io->size > 0x400 ? 0x400 : (size_t)io->size;
                uint8_t m[0x400];
                for (size_t k = 0; k + 1 < n; k += 2) { m[k] = 0; m[k + 1] = (uint8_t)(k / 4); }
                write_guest(c, io, m, n);
            }
            return 1;
        }
        case 0x14: wr32(buf, 7); wr32(buf + 4, 1); return 1;                 /* ZBC_GET_ACTIVE_SLOT_MASK */
        case 0x15: wr32(buf, 100); return 1;                                 /* PMU_GET_GPU_LOAD */
        case 0x1c: wr64(buf, (uint64_t)e->ticks * 52); return 1;             /* GET_GPU_TIME (ns, 19.2 MHz ticks) */
        case 0x1f: {                                                         /* GET_ENGINE_INFO: GR + async copy */
            static const uint32_t items[8] = { 0, 0, 0, 0,   2, 0, 1, 0 };
            uint32_t want = rd32(buf);
            if (want >= sizeof items && io && io->size >= sizeof items) {
                uint8_t t[sizeof items];
                for (int k = 0; k < 8; k++) wr32(t + k * 4, items[k]);
                write_guest(c, io, t, sizeof t);
            }
            wr32(buf, sizeof items);
            return 1;
        }
        default:                                                             /* unknown Switch 2 query: zeroed */
            memset(buf, 0, size);
            elog(e, "[nv] ctrl-gpu G#%02x size 0x%x (zeroed)\n", nr, size);
            return 1;
        }
    }
    if (!strcmp(dev, "/dev/nvhost-gpu") && type == 'H') {
        uint32_t fd = c->nv_cur_fd < NV_MAX_FD ? c->nv_cur_fd : 0;
        if (!e->nx2_ch_syncpt[fd]) e->nx2_ch_syncpt[fd] = ++e->nx2_next_syncpt;
        uint32_t sp = e->nx2_ch_syncpt[fd];
        switch (nr) {
        case 0x18: case 0x1a:                                                /* ALLOC_GPFIFO_EX(2): fence_out */
            wr32(buf + 12, sp); wr32(buf + 16, e->nx2_sp_max[sp & 511]);
            if (e->nx2_syncpt_ro && sp < e->nx2_syncpt_count)                   /* this syncpoint now counts real work */
                mwr(e, e->nx2_syncpt_ro + NX2_SP_VALUES_OFF + sp * 8ull, 8, nx2_syncpt_value(e, sp));
            elog(e, "[nv] gpu channel fd %u: %u GPFIFO entries, syncpoint %u\n", fd, rd32(buf), sp);
            return 1;
        case 0x08: case 0x1b: {                                              /* SUBMIT_GPFIFO */
            uint64_t list = rd64(buf); uint32_t n = rd32(buf + 8), fl = rd32(buf + 12);
            uint32_t inc = ((fl & 2) ? 2 : 0) + ((fl & 0x100) ? rd32(buf + 20) : 0);
            e->nx2_sp_max[sp & 511] += inc;
            uint32_t fence_inc = inc;
            const ipc_buf *inl = c->cmd == 11 ? nv_in_buf_k(c, 1) : NULL;
            if (inl && nr == 0x1b) {                                         /* Ioctl2: entries in the inline buffer */
                static uint64_t ents[8192]; uint32_t k = 0;      /* Zelda submits up to ~4096 at once */
                for (; k < n && k < 8192 && (k + 1) * 8ull <= inl->size; k++) ents[k] = mrd(e, inl->addr + k * 8ull, 8);
                if (k < n) { static int nt; if (nt++ < 8) elog(e, "[nv] SUBMIT_GPFIFO: only %u of %u entries taken (inline buffer 0x%llx bytes)\n", k, n, (unsigned long long)inl->size); }
                nx2_gpfifo_submit_entries(e, ents, k, sp);
            } else if (nr == 0x1b) nx2_gpfifo_submit(e, list, n, sp);
            else {                                                           /* entries follow the struct */
                static uint64_t ents[2048]; uint32_t k = 0;
                for (; k < n && k < 2048 && 24 + k * 8 + 8 <= sizeof(uint8_t[0x4000]); k++) ents[k] = rd64(buf + 24 + k * 8);
                if (k < n) { static int nt; if (nt++ < 8) elog(e, "[nv] SUBMIT_GPFIFO: only %u of %u entries taken\n", k, n); }
                nx2_gpfifo_submit_entries(e, ents, k, sp);
            }
            nx2_gpfifo_fence(e, sp, fence_inc);
            wr32(buf + 12, 0); wr32(buf + 16, sp); wr32(buf + 20, e->nx2_sp_max[sp & 511]);
            return 1;
        }
        default: return 0;                                                   /* timeouts, priorities, ctx: echo */
        }
    }
    if (!strcmp(dev, "/dev/nvhost-as-gpu") && type == 'A') {
        switch (nr) {
        case 0x09: {                                                         /* ALLOC_AS_EX */
            uint32_t big = rd32(buf + 8);
            uint64_t start = rd64(buf + 16), end = rd64(buf + 24), split = rd64(buf + 32);
            if (!big) big = 0x20000;
            if (!start) { start = (uint64_t)big << 10; end = 1ull << 37; split = 1ull << 34; }
            if (!split) { split = end; end += 4ull << 30; }     /* small pages over the whole range */
            e->nx2_as_start = start; e->nx2_as_split = split; e->nx2_as_end = end; e->nx2_as_big = big;
            elog(e, "[nv] as-gpu ALLOC_AS_EX flags %x [%llx, split %llx, end %llx) big 0x%x\n", rd32(buf),
                 (unsigned long long)start, (unsigned long long)split, (unsigned long long)end, big);
            return 1;
        }
        case 0x08: {                                                         /* GET_VA_REGIONS */
            uint8_t r[0x30] = {0};
            wr64(r + 0x00, e->nx2_as_start); wr32(r + 0x08, 0x1000);
            wr64(r + 0x10, (e->nx2_as_split - e->nx2_as_start) >> 12);
            wr64(r + 0x18, e->nx2_as_split); wr32(r + 0x20, e->nx2_as_big);
            wr64(r + 0x28, e->nx2_as_big ? (e->nx2_as_end - e->nx2_as_split) / e->nx2_as_big : 0);
            wr32(buf + 8, sizeof r);
            if (size >= 16 + sizeof r) memcpy(buf + 16, r, sizeof r);
            if (io) write_guest(c, io, r, sizeof r);
            return 1;
        }
        case 0x0b: {                                                         /* Switch 2: syncpoint shim page */
            if (!e->nx2_as_end) return 0;
            wr64(buf, e->nx2_as_end - 0x1000); wr32(buf + 8, 4);
            wr32(buf + 12, e->nx2_syncpt_count ? e->nx2_syncpt_count : 512);
            elog(e, "[nv] as-gpu syncpoint shim at %llx\n", (unsigned long long)(e->nx2_as_end - 0x1000));
            return 1;
        }
        default: return 0;                                                   /* bind/alloc/map: echo */
        }
    }
    return 0;
}

static uint32_t nv_ioctl(ipc_ctx *c, uint32_t fd, uint32_t code, int has_inline_out) {
    uint32_t size = (code >> 16) & 0x3FFF, nr = code & 0xFF, type = (code >> 8) & 0xFF;
    uint8_t buf[0x4000] = {0};
    const ipc_buf *in = nv_in_buf(c);
    if (in) buf_read(c, in, buf, size < in->size ? size : (size_t)in->size);
    const char *dev = fd < NV_MAX_FD ? nv_fd_path[fd] : "?";
    int nx2 = 1;
    if (!strcmp(dev, "/dev/nvhost-ctrl-gpu") && type == 'G' && nr == 5) {          /* GET_CHARACTERISTICS */
        uint64_t want = rd64(buf);
        size_t gclen = want >= 0xD0 ? 0xD0 : 0xA0;
        wr64(buf, gclen);
        if (rd64(buf + 8) == 0) wr64(buf + 8, 0xdeadbeef);
        nv_fill_characteristics(buf + 16, size > 16 ? size - 16 : 0, nx2 && gclen == 0xD0);
        if (has_inline_out) {
            const ipc_buf *io = nv_out_buf(c, 1);
            uint8_t gc[0xD0];
            nv_fill_characteristics(gc, sizeof gc, gclen == 0xD0);
            if (io) write_guest(c, io, gc, gclen);
        }
        elog(c->e, "[nv] %s GET_CHARACTERISTICS -> %s\n", dev, gclen == 0xD0 ? "Ampere 0x17E (Switch 2)" : "Maxwell GM20B");
    } else if (!nv_ioctl_dev(c, dev, type, nr, buf, size, has_inline_out) && c->e->ipc_log_budget) {
        elog(c->e, "[nv] %s ioctl %c#%02x size 0x%x (echo)\n", dev, type >= 32 && type < 127 ? type : '?', nr, size);
    }
    const ipc_buf *out = nv_out_buf(c, 0);
    if (out) write_guest(c, out, buf, size);
    return 0;
}

static int nvsrv_dispatch(ipc_ctx *c) {
    uint32_t cmd = c->cmd;
    switch (cmd) {
    case 0: {                                                    /* Open(path) -> fd, error */
        char path[48] = {0};
        const ipc_buf *p = nv_in_buf(c);
        if (p) buf_read(c, p, path, p->size < sizeof path - 1 ? (size_t)p->size : sizeof path - 1);
        uint32_t fd = 0;
        for (uint32_t k = 1; k < NV_MAX_FD; k++) if (!nv_fd_path[k][0]) { fd = k; break; }
        if (fd) mgr_strlcpy(nv_fd_path[fd], path, sizeof nv_fd_path[fd]);
        elog(c->e, "[nv] open %s -> fd %u\n", path, fd);
        out_u32(c, fd); out_u32(c, fd ? 0 : 0x1);
        return 1;
    }
    case 1: case 11: case 12: {                                  /* Ioctl / Ioctl2 / Ioctl3 (fd, code) */
        uint32_t fd = (uint32_t)arg64(c, 0), code = (uint32_t)(arg64(c, 0) >> 32);
        c->nv_cur_fd = fd;
        out_u32(c, nv_ioctl(c, fd, code, cmd == 12));
        return 1;
    }
    case 2: {                                                    /* Close(fd) */
        uint32_t fd = (uint32_t)arg64(c, 0);
        if (fd < NV_MAX_FD) nv_fd_path[fd][0] = 0;
        out_u32(c, 0);
        return 1;
    }
    case 4: out_event(c, 0); out_u32(c, 0); return 1;            /* QueryEvent */
    default: out_u32(c, 0); return 1;                            /* Initialize, SetClientPid, 13, ... */
    }
}
