/* mgrzbic: decompress one ZBIC (or plain zstd) segment with MikesGameRoom's own decoder.
 * usage: mgrzbic <decompressed size> < segment > image
 * Used by the imported nxnative builder (Engines/nxnative/dm/tools/nso.py) in place of the GPLv2
 * "zbic" Python package, which MikesGameRoom does not use. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../MGRCore/mgr_internal.h"
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: mgrzbic <decompressed size> < in > out\n"); return 2; }
    size_t out_sz = strtoull(argv[1], NULL, 0), cap = 1 << 20, n = 0;
    uint8_t *in = malloc(cap), *out = malloc(out_sz ? out_sz : 1);
    for (size_t r; in && (r = fread(in + n, 1, cap - n, stdin)) > 0;) { n += r; if (n == cap) in = realloc(in, cap *= 2); }
    if (!in || !out) { fprintf(stderr, "out of memory\n"); return 1; }
    if (zstd_decompress_frame(in, n, out, out_sz)) { fprintf(stderr, "ZBIC does not decompress to %zu bytes\n", out_sz); return 1; }
    fwrite(out, 1, out_sz, stdout);
    return 0;
}
