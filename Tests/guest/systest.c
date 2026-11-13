/* System test for the HLE kernel + IPC, written like a tiny Switch program.
 * Threads: 3 workers + atomic counter + yields, WaitForAddress/SignalToAddress, sleep.
 * IPC: sm: over TIPC -> fsp-srv over CMIF -> DataStorage -> read RomFS image -> find a file. */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef long s64;
typedef unsigned long size_t;

void *memcpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }

#define SVC(n) __asm__ volatile("svc " #n : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5) :: "memory")
static u64 svc6(int n, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 *out1) {
    register u64 x0 __asm__("x0") = a0, x1 __asm__("x1") = a1, x2 __asm__("x2") = a2,
                 x3 __asm__("x3") = a3, x4 __asm__("x4") = a4, x5 __asm__("x5") = a5;
    switch (n) {
    case 0x08: SVC(0x08); break;  case 0x09: SVC(0x09); break;  case 0x0A: SVC(0x0A); break;
    case 0x0B: SVC(0x0B); break;  case 0x18: SVC(0x18); break;  case 0x1F: SVC(0x1F); break;
    case 0x21: SVC(0x21); break;  case 0x27: SVC(0x27); break;  case 0x34: SVC(0x34); break;
    case 0x35: SVC(0x35); break;  case 0x07: SVC(0x07); break;  case 0x1E: SVC(0x1E); break;
    }
    if (out1) *out1 = x1;
    return x0;
}

static char line[256]; static int lp;
static void put(const char *s) { while (*s) line[lp++] = *s++; }
static void dec(s64 v) { char t[24]; int n = 0; if (v < 0) { line[lp++] = '-'; v = -v; } do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) line[lp++] = t[--n]; }
static void hex(u64 v) { put("0x"); for (int i = 28; i >= 0; i -= 4) line[lp++] = "0123456789abcdef"[(v >> i) & 15]; }
static void endl(void) { svc6(0x27, (u64)line, (u64)lp, 0, 0, 0, 0, 0); lp = 0; }

/* ---------------- threads ---------------- */
static volatile u64 counter;
static volatile u32 flag;
static volatile int woke;
static u8 stacks[4][0x4000] __attribute__((aligned(16)));

static void worker(u64 arg) {
    for (int i = 0; i < 1000; i++) {
        __atomic_fetch_add(&counter, arg, __ATOMIC_SEQ_CST);
        if (i % 100 == 0) svc6(0x0B, 0, 0, 0, 0, 0, 0, 0);       /* yield */
    }
    svc6(0x0A, 0, 0, 0, 0, 0, 0, 0);                              /* ExitThread */
}
static void waiter(u64 arg) {
    (void)arg;
    /* WaitForAddress(addr, WaitIfLessThan, value 1, infinite) */
    svc6(0x34, (u64)&flag, 0, 1, (u64)-1, 0, 0, 0);
    woke = (int)flag;
    svc6(0x0A, 0, 0, 0, 0, 0, 0, 0);
}

/* ---------------- IPC ---------------- */
static u32 *tls(void) { u64 t; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(t)); return (u32 *)t; }

/* TIPC request with no handles; returns result word; *moved receives first moved handle */
static u32 tipc(u32 session, u32 cmd, const void *args, u32 argwords, u32 *moved, int send_pid) {
    u32 *m = tls();
    memset(m, 0, 0x100);
    m[0] = 16 + cmd;
    u32 *p = m + 2;
    if (send_pid) { m[1] = argwords | 0x80000000u; *p++ = 1; p += 2; }
    else m[1] = argwords;
    memcpy(p, args, argwords * 4);
    u64 r = svc6(0x21, session, 0, 0, 0, 0, 0, 0);
    if (r) return (u32)r;
    p = m + 2;
    if (m[1] >> 31) { u32 sh = *p++; int nc = (sh >> 1) & 15, nm = (sh >> 5) & 15; if (moved && nm) *moved = p[nc]; p += nc + nm; }
    return *p;
}

/* CMIF request; optional B buffer; returns result, copies outputs, first moved handle */
static u32 cmif(u32 session, u32 cmd, const void *args, u32 argbytes, void *bbuf, u64 bsize, void *out, u32 outbytes, u32 *moved) {
    u32 *m = tls();
    memset(m, 0, 0x100);
    int nb = bbuf ? 1 : 0;
    m[0] = 4 | ((u32)nb << 24);
    u32 *p = m + 2;
    if (nb) {
        u64 a = (u64)bbuf;
        p[0] = (u32)bsize; p[1] = (u32)a;
        p[2] = (u32)(((a >> 36) & 7) << 2) | (u32)(((bsize >> 32) & 15) << 24) | (u32)(((a >> 32) & 15) << 28);
        p += 3;
    }
    u32 *raw = (u32 *)(((u64)p + 15) & ~15ull);
    raw[0] = 0x49434653; raw[1] = 0; raw[2] = cmd; raw[3] = 0;
    memcpy(raw + 4, args, argbytes);
    m[1] = (u32)(((u64)(raw + 4) + argbytes - (u64)p + 3) / 4 + 4);
    u64 r = svc6(0x21, session, 0, 0, 0, 0, 0, 0);
    if (r) return (u32)r;
    p = m + 2;
    if (m[1] >> 31) { u32 sh = *p++; int nc = (sh >> 1) & 15, nm = (sh >> 5) & 15; if (moved && nm) *moved = p[nc]; p += nc + nm; }
    u32 *resp = (u32 *)(((u64)p + 15) & ~15ull);
    if (resp[0] != 0x4F434653) return 0xBAD;
    if (out) memcpy(out, resp + 4, outbytes);
    return resp[2];
}

static u8 image[0x8000] __attribute__((aligned(16)));

__attribute__((section(".text._start"))) void _start(void) {
    /* threads */
    u64 h[4];
    for (int t = 0; t < 3; t++) {
        svc6(0x08, 0, (u64)worker, (u64)(t + 1), (u64)(stacks[t] + sizeof stacks[t]), 44, (u64)-2, &h[t]);
        svc6(0x09, h[t], 0, 0, 0, 0, 0, 0);
    }
    svc6(0x08, 0, (u64)waiter, 0, (u64)(stacks[3] + sizeof stacks[3]), 44, (u64)-2, &h[3]);
    svc6(0x09, h[3], 0, 0, 0, 0, 0, 0);
    u64 t0 = svc6(0x1E, 0, 0, 0, 0, 0, 0, 0);
    svc6(0x0B, 5000000, 0, 0, 0, 0, 0, 0);                     /* sleep 5 ms */
    u64 t1 = svc6(0x1E, 0, 0, 0, 0, 0, 0, 0);
    flag = 7;
    svc6(0x35, (u64)&flag, 0, 0, 1, 0, 0, 0);                  /* SignalToAddress(Signal, count 1) */
    for (int t = 0; t < 4; t++) {
        u64 idx;
        u32 hh = (u32)h[t];
        svc6(0x18, 0, (u64)&hh, 1, (u64)-1, 0, 0, &idx);       /* WaitSynchronization(thread) */
    }
    put("threads: counter="); dec((s64)counter); put(" woke="); dec(woke);
    put(" slept_ms="); dec((s64)((t1 - t0) / 19200)); endl();

    /* sm: over TIPC */
    u64 smh;
    svc6(0x1F, 0, (u64)"sm:", 0, 0, 0, 0, &smh);
    u64 zero = 0;
    u32 r = tipc((u32)smh, 0, &zero, 2, 0, 1);                 /* RegisterClient */
    put("sm:RegisterClient r="); hex(r); endl();
    u64 name = 0; memcpy(&name, "fsp-srv", 7);
    u32 fsp = 0;
    r = tipc((u32)smh, 1, &name, 2, &fsp, 0);                  /* GetServiceHandle */
    put("sm:GetServiceHandle(fsp-srv) r="); hex(r); put(fsp ? " handle ok" : " no handle"); endl();

    /* fsp-srv over CMIF */
    u32 storage = 0;
    r = cmif(fsp, 200, 0, 0, 0, 0, 0, 0, &storage);            /* OpenDataStorageByCurrentProcess */
    put("fsp:OpenDataStorage r="); hex(r); put(storage ? " handle ok" : " no handle"); endl();
    u64 size = 0;
    r = cmif(storage, 4, 0, 0, 0, 0, &size, 8, 0);             /* GetSize */
    put("IStorage:GetSize r="); hex(r); put(" size="); dec((s64)size); endl();
    u64 rd[2] = { 0, size < sizeof image ? size : sizeof image };
    r = cmif(storage, 0, rd, 16, image, rd[1], 0, 0, 0);       /* Read */
    put("IStorage:Read r="); hex(r); endl();

    /* walk the RomFS image: root -> first child dir -> its first file */
    u64 *hdr = (u64 *)image;
    u64 dir_meta = hdr[3], file_meta = hdr[7], data_off = hdr[9];
    u32 *root = (u32 *)(image + dir_meta);
    u32 child = root[2];
    u32 *dir = (u32 *)(image + dir_meta + child);
    char dname[32] = {0}; memcpy(dname, (u8 *)dir + 0x18, dir[5]);
    u32 *f = (u32 *)(image + file_meta + dir[3]);
    char fname[32] = {0}; memcpy(fname, (u8 *)f + 0x20, f[7]);
    u64 foff = *(u64 *)(f + 2), fsize = *(u64 *)(f + 4);
    char content[64] = {0}; memcpy(content, image + data_off + foff, fsize < 63 ? fsize : 63);
    put("romfs: hdr="); hex(hdr[0]); put(" dir="); put(dname); put(" file="); put(fname); put(" content="); put(content);
    if (lp && line[lp - 1] == '\n') lp--;
    endl();

    put("SYSTEST DONE"); endl();
    svc6(0x07, 0, 0, 0, 0, 0, 0, 0);
    for (;;) {}
}
