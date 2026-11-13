/* Core regression tests: every supported format must open, expose RomFS, and
   run the guest test program to the same checksum. Run with Tests/run_tests.sh. */
#include "mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void run_one(const char *path, const char *romfs_path, const char *expect_name) {
    char err[256] = "";
    printf("%s\n", path);
    mgr_game *g = mgr_open(path, err, sizeof err);
    CHECK(g, "open failed: %s", err);
    if (!g) return;
    const mgr_game_info *in = mgr_info(g);
    CHECK(in->bootable, "not bootable (%s)", in->note);
    if (expect_name) CHECK(strcmp(in->name, expect_name) == 0, "name '%s' != '%s'", in->name, expect_name);
    if (romfs_path) {
        uint8_t *b; size_t l;
        CHECK(mgr_romfs_read(g, romfs_path, &b, &l) == 0, "romfs read of %s failed", romfs_path);
        if (romfs_path && l) free(b);
    }
    mgr_emu *e = mgr_emu_create(g, err, sizeof err);
    CHECK(e, "emu create: %s", err);
    if (e) {
        mgr_emu_status st = mgr_emu_run(e, 10000000);
        static char log[1 << 16];
        size_t n = mgr_emu_take_log(e, log, sizeof log);
        log[n] = 0;
        CHECK(st == MGR_EMU_EXITED, "status %d", st);
        CHECK(strstr(log, "CHECK=0x00fd5bdf028b9ced") != NULL, "checksum missing");
        mgr_emu_destroy(e);
    }
    mgr_close(g);
}

/* Runs a program and checks that every expected line appears in its log. */
static void run_expect(const char *path, const char *expected_file, const char *const *must) {
    char err[256] = "";
    printf("%s\n", path);
    mgr_game *g = mgr_open(path, err, sizeof err);
    CHECK(g, "open failed: %s", err);
    if (!g) return;
    mgr_emu *e = mgr_emu_create(g, err, sizeof err);
    CHECK(e, "emu create: %s", err);
    if (e) {
        mgr_emu_status st = mgr_emu_run(e, 50000000);
        static char log[1 << 18];
        size_t total = 0, n;
        while ((n = mgr_emu_take_log(e, log + total, sizeof log - total)) > 0) total += n;
        log[total] = 0;
        CHECK(st == MGR_EMU_EXITED, "status %d", st);
        if (expected_file) {
            FILE *f = fopen(expected_file, "r");
            CHECK(f, "missing %s", expected_file);
            char line[512]; int lines = 0, bad = 0;
            while (f && fgets(line, sizeof line, f)) {
                lines++;
                char want[600]; snprintf(want, sizeof want, "[guest] %s", line);
                if (!strstr(log, want)) { if (!bad) printf("  first mismatch: %s", line); bad++; }
            }
            if (f) fclose(f);
            CHECK(bad == 0, "%d of %d expected lines differ", bad, lines);
        }
        for (int k = 0; must && must[k]; k++) CHECK(strstr(log, must[k]) != NULL, "missing '%s'", must[k]);
        mgr_emu_destroy(e);
    }
    mgr_close(g);
}

int main(int argc, char **argv) {
    const char *F = argc > 1 ? argv[1] : "Tests/fixtures";
    char p[1024], k[1024];
    snprintf(p, sizeof p, "%s/dump", F);            run_one(p, "data/readme.txt", "dump");
    snprintf(p, sizeof p, "%s/hello.nup", F);       run_one(p, "data/readme.txt", "Hello NUP");
    snprintf(p, sizeof p, "%s/hello_spec.nup", F);  run_one(p, "data/readme.txt", "Hello Spec");
    snprintf(p, sizeof p, "%s/hello_raw.nso", F);   run_one(p, NULL, NULL);
    snprintf(p, sizeof p, "%s/hello.nro", F);       run_one(p, NULL, "Hello Brewery");
    snprintf(p, sizeof p, "%s/hello_zbic.nso", F);  run_one(p, NULL, NULL);  /* Switch 2 ZBIC-compressed module */
    snprintf(k, sizeof k, "%s/enc/test.keys", F);
    CHECK(mgr_keys_load(k) == 2, "test keys");
    snprintf(p, sizeof p, "%s/enc/test.nsp", F);    run_one(p, "data/readme.txt", "NCA Test Game");
    snprintf(p, sizeof p, "%s/enc/test.xci", F);    run_one(p, "data/readme.txt", "NCA Test Game");
    snprintf(p, sizeof p, "%s/enc/program.nca", F); run_one(p, "data/readme.txt", NULL);
    static const char *const sys_expect[] = {
        "threads: counter=6000 woke=7 slept_ms=5", "sm:RegisterClient r=0x00000000",
        "sm:GetServiceHandle(fsp-srv) r=0x00000000 handle ok", "fsp:OpenDataStorage r=0x00000000 handle ok",
        "IStorage:Read r=0x00000000", "dir=data file=readme.txt content=romfs works", "SYSTEST DONE", NULL };
    snprintf(p, sizeof p, "%s/fptest.nso", F); snprintf(k, sizeof k, "%s/fptest_expected.txt", F);
    run_expect(p, k, NULL);
    snprintf(p, sizeof p, "%s/sys", F);          run_expect(p, NULL, sys_expect);
    snprintf(p, sizeof p, "%s/systest.nup", F);  run_expect(p, NULL, sys_expect);
    printf(fails ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", fails);
    return fails != 0;
}
