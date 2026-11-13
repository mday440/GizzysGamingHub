/* Freestanding AArch64 test program for the MikesGameRoom CPU.
 * Exercises integer ALU, division, 128-bit multiply, bitfields, jump tables,
 * recursion, memcpy/memset (SIMD), LSE atomics and Horizon SVCs.
 * Prints results with svcOutputDebugString and exits with svcExitProcess. */
typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned char u8;

static inline u64 svc_out(const char *s, u64 n) {
    register u64 x0 __asm__("x0") = (u64)s, x1 __asm__("x1") = n;
    __asm__ volatile("svc 0x27" : "+r"(x0), "+r"(x1) :: "memory");
    return x0;
}
static inline u64 svc_getinfo(u64 id0, u64 id1) {
    register u64 x0 __asm__("x0"), x1 __asm__("x1") = id0, x2 __asm__("x2") = 0, x3 __asm__("x3") = id1;
    __asm__ volatile("svc 0x29" : "=r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    return x1;
}
static inline u64 svc_heap(u64 size) {
    register u64 x0 __asm__("x0"), x1 __asm__("x1") = size;
    __asm__ volatile("svc 0x1" : "=r"(x0), "+r"(x1) :: "memory");
    return x1;
}
static inline void svc_exit(void) { __asm__ volatile("svc 0x7"); for (;;) {} }

void *memcpy(void *d, const void *s, u64 n) {
    u8 *dd = d; const u8 *ss = s;
    while (n >= 16) { __builtin_memcpy(dd, ss, 16); dd += 16; ss += 16; n -= 16; }
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, u64 n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }

static char out[512];
static int pos;
static void puts_(const char *s) { while (*s) out[pos++] = *s++; }
static void hex(u64 v) { puts_("0x"); for (int i = 60; i >= 0; i -= 4) out[pos++] = "0123456789abcdef"[(v >> i) & 15]; }
static void dec(s64 v) {
    char t[24]; int n = 0;
    if (v < 0) { out[pos++] = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[pos++] = t[--n];
}
static void flush(void) { svc_out(out, (u64)pos); pos = 0; }

__attribute__((noinline)) static u64 fib(u64 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

__attribute__((noinline)) static int classify(int k) {
    switch (k) { case 0: return 11; case 1: return 22; case 2: return 33; case 3: return 44; case 4: return 55; case 5: return 66; default: return -1; }
}

static const char msg[] = "MikesGameRoom guest says hello from AArch64!";
static u64 counter = 0;
static volatile u64 data_word = 0x5555;
static char buf[256];

__attribute__((section(".text._start"))) void _start(void) {
    u64 check = 0;
    puts_(msg); flush();

    puts_("fib(20)="); dec((s64)fib(20)); flush();                 /* 6765 */
    check += fib(20);

    volatile s64 a = -123456789, b = 1000;
    puts_("div="); dec(a / b); puts_(" mod="); dec(a % b); flush(); /* -123456 -789 */
    check += (u64)(a / b) ^ (u64)(a % b);

    volatile u64 m1 = 0xDEADBEEFCAFEBABEul, m2 = 0x123456789ABCDEFul;
    unsigned __int128 p = (unsigned __int128)m1 * m2;
    puts_("mulhi="); hex((u64)(p >> 64)); flush();
    check += (u64)(p >> 64);

    int js = 0;
    for (volatile int k = 0; k < 7; k++) js += classify(k);
    puts_("switch="); dec(js); flush();                             /* 230 */
    check += (u64)js;

    memset(buf, 'x', sizeof buf);
    memcpy(buf + 3, msg, 20);
    buf[23] = 0;
    puts_("memcpy="); puts_(buf + 3); flush();

    volatile u32 bf = 0xABCD1234;
    u32 ext = (bf >> 7) & 0x1FF;
    puts_("bitfield="); dec(ext); puts_(" clz="); dec(__builtin_clz(bf >> 9)); puts_(" rev="); hex(__builtin_bswap64(m1)); flush();
    check += ext;

    for (int k = 0; k < 10; k++) __atomic_fetch_add(&counter, (u64)k, __ATOMIC_SEQ_CST);
    u64 old = __atomic_exchange_n(&counter, 1000, __ATOMIC_SEQ_CST);
    u64 expected = 1000;
    __atomic_compare_exchange_n(&counter, &expected, 2000, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    puts_("atomics="); dec((s64)old); puts_("/"); dec((s64)counter); flush(); /* 45/2000 */
    check += old + counter;

    u64 heap = svc_heap(0x200000);
    volatile u64 *hp = (u64 *)heap;
    for (int k = 0; k < 1000; k++) hp[k] = (u64)k * k;
    u64 sum = 0;
    for (int k = 0; k < 1000; k++) sum += hp[k];
    puts_("heap@"); hex(heap); puts_(" sum="); dec((s64)sum); flush();  /* 332833500 */
    check += sum;

    puts_("program_id="); hex(svc_getinfo(18, 0)); flush();

    check += data_word;
    puts_("CHECK="); hex(check); flush();
    svc_exit();
}
