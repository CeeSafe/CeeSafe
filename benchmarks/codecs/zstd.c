/* benchmarks/codecs (ADR 0196 §4.4): the system's libzstd decoding a file, best of `reps` runs, as main.cee's unzstd
   mode. zstd unzstd <file.zst> <size> <reps>. Build: cc -O2 zstd.c -lzstd */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <zstd.h>

static char *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc(*n ? *n : 1);
    if (fread(p, 1, *n, f) != *n) { perror(path); exit(1); }
    fclose(f);
    return p;
}

static unsigned long long now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage\n"); return 2; }
    size_t n;
    char *in = read_all(argv[2], &n);
    size_t cap = (size_t)strtoull(argv[3], 0, 10);
    int reps = atoi(argv[4]);
    unsigned long long best = ~0ull;
    size_t size = 0;
    char *out = malloc(cap ? cap : 1);
    ZSTD_DCtx *d = ZSTD_createDCtx();
    for (int r = 0; r < reps; r++) {
        unsigned long long t = now();
        size = ZSTD_decompressDCtx(d, out, cap, in, n);
        unsigned long long dt = now() - t;
        if (ZSTD_isError(size)) { fprintf(stderr, "zstd error %s\n", ZSTD_getErrorName(size)); return 1; }
        if (dt < best) best = dt;
    }
    printf("%llu %lu\n", (unsigned long long)size * 1000ull / best, (unsigned long)n);
    return 0;
}
