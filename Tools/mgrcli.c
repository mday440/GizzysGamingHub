#include <unistd.h>
/* mgrcli - command-line front end for the MikesGameRoom core (testing / scripting).
 *   mgrcli info <game> [--keys prod.keys]
 *   mgrcli run  <game> [--keys prod.keys] [--steps N]
 */
#include "mgr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

static volatile sig_atomic_t g_interrupted;
static void on_sigint(int sig) { (void)sig; g_interrupted = 1; }

static const char *plat(mgr_platform p) {
    switch (p) {
    case MGR_PLATFORM_SWITCH1: return "Switch 1";
    case MGR_PLATFORM_SWITCH2: return "Switch 2";
    case MGR_PLATFORM_HOMEBREW: return "Homebrew";
    default: return "Unknown";
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: mgrcli info|run <game> [--keys FILE] [--steps N]\n"); return 2; }
    unsigned long long steps = 5000000;
    const char *peek = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            int n = mgr_keys_load(argv[++i]);
            fprintf(stderr, "keys: %d loaded\n", n);
        } else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc) peek = argv[++i];
    }
    char err[256] = "";
    mgr_game *g = mgr_open(argv[2], err, sizeof err);
    if (!g) { fprintf(stderr, "open failed: %s\n", err); return 1; }
    const mgr_game_info *in = mgr_info(g);
    printf("name:      %s\n", in->name);
    printf("publisher: %s\n", in->publisher);
    printf("format:    %s\n", in->format_name);
    printf("platform:  %s\n", plat(in->platform));
    printf("title id:  %016llX\n", (unsigned long long)in->title_id);
    printf("version:   %u%s%s\n", in->version, in->display_version[0] ? "  display " : "", in->display_version);
    printf("packages:  %d%s\n", in->package_count, in->update_applied ? " (update applied)" : "");
    printf("romfs:     %llu files\n", (unsigned long long)in->romfs_file_count);
    printf("icon:      %s\n", in->has_icon ? "yes" : "no");
    printf("bootable:  %s\n", in->bootable ? "yes" : "no");
    printf("exefs:    ");
    for (int i = 0; i < in->exefs_count; i++) printf(" %s", in->exefs_names[i]);
    printf("\n");
    if (in->has_build_id) {
        printf("build id:  ");
        for (int i = 0; i < 32; i++) printf("%02X", in->build_id[i]);
        printf("\n");
    }
    if (in->note[0]) printf("note:      %s\n", in->note);

    int rc = 0;
    if (!strcmp(argv[1], "run")) {
        mgr_emu *e = mgr_emu_create(g, err, sizeof err);
        if (!e) { fprintf(stderr, "emu: %s\n", err); mgr_close(g); return 1; }
        /* Run in chunks: the log streams out as it is produced, a progress line goes to the
           terminal (stderr) every chunk, and Ctrl+C stops cleanly with a thread dump. */
        signal(SIGINT, on_sigint);
        char buf[65536];
        size_t n;
        mgr_emu_status st = MGR_EMU_READY;
        mgr_emu_state s;
        uint64_t done = 0, chunk = 200000000ull;
        time_t t0 = time(NULL);
        long max_s = getenv("MGR_MAX_SECONDS") ? atol(getenv("MGR_MAX_SECONDS")) : 0;   /* wall-clock limit */
        while (done < steps && !g_interrupted && !(max_s > 0 && time(NULL) - t0 >= max_s)) {
            uint64_t want = steps - done < chunk ? steps - done : chunk;
            st = mgr_emu_run(e, want);
            while ((n = mgr_emu_take_log(e, buf, sizeof buf)) > 0) fwrite(buf, 1, n, stdout);
            fflush(stdout);
            mgr_emu_get_state(e, &s);
            done = s.instructions;
            double secs = difftime(time(NULL), t0);
            fprintf(stderr, "\r[mgrcli] %.2f billion instructions, %llu svcs, %.0f s, %.0f M/s   ",
                    done / 1e9, (unsigned long long)s.svc_calls, secs, secs > 0 ? done / secs / 1e6 : 0.0);
            if (st != MGR_EMU_RUNNING && st != MGR_EMU_READY) break;
            if (getppid() == 1) { fprintf(stderr, "\n[mgrcli] parent gone (run cancelled): stopping\n"); break; }   /* the auto-runner killed our wrapper */
        }
        fprintf(stderr, "\n%s\n", g_interrupted ? "[mgrcli] stopped by Ctrl+C" : "[mgrcli] finished");
        if (st == MGR_EMU_RUNNING || st == MGR_EMU_READY) mgr_emu_dump_threads(e);
        while ((n = mgr_emu_take_log(e, buf, sizeof buf)) > 0) fwrite(buf, 1, n, stdout);
        mgr_emu_get_state(e, &s);
        printf("\nstatus %d after %llu instructions, %llu svcs, pc=%016llx: %s\n", st,
               (unsigned long long)s.instructions, (unsigned long long)s.svc_calls,
               (unsigned long long)s.pc, s.message);
        for (const char *p = peek; p && *p; ) {   /* --peek ADDR[,ADDR...]: dump 32 bytes at each */
            char *end;
            unsigned long long a = strtoull(p, &end, 16);
            unsigned char m[32];
            if (mgr_emu_read_mem(e, a, m, sizeof m) == 0) {
                printf("peek %016llx:", a);
                for (int k = 0; k < 32; k += 8) { unsigned long long v; memcpy(&v, m + k, 8); printf(" %016llx", v); }
                printf("\n");
            } else printf("peek %016llx: unmapped\n", a);
            p = *end == ',' ? end + 1 : end;
            if (end == p - 1 && *end != ',') break;
            if (!*end) break;
        }
        rc = (st == MGR_EMU_EXITED) ? 0 : 3;
        mgr_emu_destroy(e);
    }
    mgr_close(g);
    return rc;
}
