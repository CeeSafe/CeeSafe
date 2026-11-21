/* benchmarks/codecs (ADR 0196 §4.4): the system's zlib on a file, best of `reps` runs, as main.cee.
   main inflate <file.zlib> <size> <reps>; main deflate <file> <level> <reps>. Build: cc -O2 main.c -lz */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

static unsigned char *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc(*n ? *n : 1);
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
    unsigned char *in = read_all(argv[2], &n);
    unsigned long long arg = strtoull(argv[3], 0, 10);
    int reps = atoi(argv[4]);
    unsigned long long best = ~0ull;
    uLongf size = 0;
    int inflating = strcmp(argv[1], "inflate") == 0;
    uLongf cap = inflating ? (uLongf)arg : compressBound((uLong)n);
    unsigned char *out = malloc(cap ? cap : 1);
    for (int r = 0; r < reps; r++) {
        size = cap;
        unsigned long long t = now();
        int rc = inflating ? uncompress(out, &size, in, (uLong)n) : compress2(out, &size, in, (uLong)n, (int)arg);
        unsigned long long d = now() - t;
        if (rc != Z_OK) { fprintf(stderr, "zlib error %d\n", rc); return 1; }
        if (d < best) best = d;
    }
    printf("%llu %lu\n", (unsigned long long)(inflating ? size : n) * 1000ull / best, (unsigned long)size);
    return 0;
}
