/* Differential FP/SIMD test: compiled natively (-DHOST) and for the guest.
 * Both builds must print identical text. Covers scalar float/double math,
 * conversions, compares, rounding, and auto-vectorized loops (NEON). */
#ifdef HOST
#include <stdio.h>
#include <string.h>
#include <math.h>
#define OUT(s) fputs(s, stdout)
#else
typedef unsigned long size_t;
static inline void svc_out(const char *s, unsigned long n) {
    register unsigned long x0 __asm__("x0") = (unsigned long)s, x1 __asm__("x1") = n;
    __asm__ volatile("svc 0x27" : "+r"(x0), "+r"(x1) :: "memory");
}
static void OUT(const char *s) { unsigned long n = 0; while (s[n]) n++; svc_out(s, n); }
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
double sqrt(double x) { return __builtin_sqrt(x); }
float sqrtf(float x) { return __builtin_sqrtf(x); }
double fabs(double x) { return __builtin_fabs(x); }
double floor(double x) { return __builtin_floor(x); }
double ceil(double x) { return __builtin_ceil(x); }
double trunc(double x) { return __builtin_trunc(x); }
double round(double x) { return __builtin_round(x); }
float floorf(float x) { return __builtin_floorf(x); }
double fma(double a, double b, double c) { return __builtin_fma(a, b, c); }
float fmaf(float a, float b, float c) { return __builtin_fmaf(a, b, c); }
double fmin(double a, double b) { return __builtin_fmin(a, b); }
double fmax(double a, double b) { return __builtin_fmax(a, b); }
float fminf(float a, float b) { return __builtin_fminf(a, b); }
float fmaxf(float a, float b) { return __builtin_fmaxf(a, b); }
double nearbyint(double x) { return __builtin_nearbyint(x); }
long lround(double x) { return __builtin_lround(x); }
#endif

typedef unsigned long long u64;
typedef unsigned int u32;

static char line[256];
static int lp;
static void put(const char *s) { while (*s) line[lp++] = *s++; }
static void hex64(u64 v) { for (int i = 60; i >= 0; i -= 4) line[lp++] = "0123456789abcdef"[(v >> i) & 15]; }
static void dbits(const char *k, double d) { u64 v; memcpy(&v, &d, 8); put(k); put("="); hex64(v); put(" "); }
static void fbits(const char *k, float f) { u32 v; memcpy(&v, &f, 4); put(k); put("="); hex64(v); put(" "); }
static void ival(const char *k, long long v) { put(k); put("="); hex64((u64)v); put(" "); }
static void endl(void) { line[lp++] = '\n'; line[lp] = 0; OUT(line); lp = 0; }

static volatile double dv[8] = { 1.5, -2.25, 3.0e10, -7.75, 0.1, 1e-300, 123456.789, -0.0 };
static volatile float fv[8] = { 1.5f, -2.25f, 3.0e10f, -7.75f, 0.1f, 1e-30f, 123456.789f, 2.5f };
static volatile long long iv[4] = { -5, 1234567890123LL, 7, -9876543210LL };
static volatile unsigned long long uv[2] = { 18446744073709551000ULL, 3 };

static float A[64], B[64], C[64];
static int IA[64], IB[64], IC[64];
static short SA[64];
static unsigned char UA[64], UB[64];
static double DA[32], DB[32];

#ifdef HOST
int main(void)
#else
__attribute__((section(".text._start"))) void _start(void)
#endif
{
    for (int i = 0; i < 8; i++) {
        double a = dv[i], b = dv[(i + 3) & 7];
        dbits("add", a + b); dbits("sub", a - b); dbits("mul", a * b); dbits("div", a / (b == 0 ? 3.0 : b));
        endl();
        dbits("fma", fma(a, b, 0.5)); dbits("sqrt", sqrt(fabs(a))); dbits("neg", -a); dbits("min", fmin(a, b)); dbits("max", fmax(a, b));
        endl();
        dbits("floor", floor(a)); dbits("ceil", ceil(a)); dbits("trunc", trunc(a)); dbits("round", round(a)); dbits("rint", nearbyint(a));
        endl();
        float f = fv[i], g = fv[(i + 5) & 7];
        fbits("fadd", f + g); fbits("fmul", f * g); fbits("fdiv", f / g); fbits("ffma", fmaf(f, g, -1.0f)); fbits("fsq", sqrtf(f < 0 ? -f : f));
        endl();
        fbits("d2f", (float)a); dbits("f2d", (double)f); fbits("fmin", fminf(f, g)); fbits("fmax", fmaxf(f, g)); fbits("ffloor", floorf(f));
        endl();
        ival("lt", a < b); ival("le", a <= b); ival("eq", a == b); ival("flt", f < g); ival("sel", (long long)(a > b ? 11 : 22));
        endl();
        if (a > -1e18 && a < 1e18) { ival("d2i", (long long)a); ival("d2u", (long long)(unsigned long long)(a < 0 ? -a : a)); ival("lround", lround(a)); }
        if (f > -1e9f && f < 1e9f) { ival("f2i", (int)f); }
        endl();
    }
    for (int i = 0; i < 4; i++) {
        long long x = iv[i];
        dbits("i2d", (double)x); fbits("i2f", (float)x); dbits("u2d", (double)(unsigned long long)x); fbits("w2f", (float)(int)x);
        endl();
    }
    dbits("u64big", (double)uv[0]); fbits("u64bigf", (float)uv[0]); ival("d2u64", (long long)(unsigned long long)3.0e18);
    endl();

    /* auto-vectorized loops */
    for (int i = 0; i < 64; i++) {
        A[i] = (float)fv[i & 7] * (float)(i + 1); B[i] = (float)(i * 3 - 50) * 0.25f;
        IA[i] = (int)iv[i & 3] ^ (i * 77); IB[i] = i * i - 300;
        SA[i] = (short)(i * 1000 - 20000);
        UA[i] = (unsigned char)(i * 13); UB[i] = (unsigned char)(255 - i * 7);
    }
    for (int i = 0; i < 32; i++) { DA[i] = dv[i & 7] * i; DB[i] = (double)(i - 16); }
    for (int i = 0; i < 64; i++) C[i] = A[i] * B[i] + A[i];
    for (int i = 0; i < 64; i++) IC[i] = IA[i] + IB[i] * 3 - (IA[i] >> 2);
    float fsum = 0, fmx = -1e30f;
    for (int i = 0; i < 64; i++) { fsum += C[i]; fmx = C[i] > fmx ? C[i] : fmx; }
    int isum = 0, imax = -2147483647, cnt = 0;
    for (int i = 0; i < 64; i++) { isum += IC[i]; imax = IC[i] > imax ? IC[i] : imax; cnt += IA[i] > IB[i]; }
    unsigned usum = 0; unsigned char umax = 0;
    for (int i = 0; i < 64; i++) { unsigned char s = (unsigned char)(UA[i] + UB[i]); usum += s; umax = UA[i] > umax ? UA[i] : umax; }
    long long ssum = 0;
    for (int i = 0; i < 64; i++) ssum += SA[i] * 3;
    double dsum = 0;
    for (int i = 0; i < 32; i++) dsum += DA[i] * DB[i];
    for (int i = 0; i < 64; i++) IC[i] = IA[i] < IB[i] ? IA[i] : IB[i];
    int msum = 0; for (int i = 0; i < 64; i++) msum += IC[i];
    for (int i = 0; i < 64; i++) C[i] = (float)IA[i] * 0.5f;
    for (int i = 0; i < 64; i++) IC[i] = (int)(B[i] * 3.0f);
    long long csum = 0; for (int i = 0; i < 64; i++) csum += IC[i] + (long long)C[i];
    fbits("vfsum", fsum); fbits("vfmax", fmx); ival("visum", isum); ival("vimax", imax); ival("vcnt", cnt);
    endl();
    ival("vusum", usum); ival("vumax", umax); ival("vssum", ssum); dbits("vdsum", dsum); ival("vmin", msum); ival("vcvt", csum);
    endl();
    OUT("FPTEST DONE\n");
#ifndef HOST
    __asm__ volatile("svc 0x7");
    for (;;) {}
#else
    return 0;
#endif
}
