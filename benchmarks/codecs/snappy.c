/* benchmarks/codecs (ADR 0196 §4.4): the system's libsnappy on a file, best of `reps` runs, as main.cee's snappy modes.
   snappy unsnappy <file.snappy> <size> <reps>; snappy snappy <file> 0 <reps>. Build: cc -O2 snappy.c -lsnappy */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <snappy-c.h>

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
    unsigned long long arg = strtoull(argv[3], 0, 10);
    int reps = atoi(argv[4]);
    unsigned long long best = ~0ull;
    size_t size = 0;
    int decoding = strcmp(argv[1], "unsnappy") == 0;
    size_t cap = decoding ? (size_t)arg : snappy_max_compressed_length(n);
    char *out = malloc(cap ? cap : 1);
    for (int r = 0; r < reps; r++) {
        size = cap;
        unsigned long long t = now();
        snappy_status rc = decoding ? snappy_uncompress(in, n, out, &size) : snappy_compress(in, n, out, &size);
        unsigned long long d = now() - t;
        if (rc != SNAPPY_OK) { fprintf(stderr, "snappy error %d\n", (int)rc); return 1; }
        if (d < best) best = d;
    }
    printf("%llu %lu\n", (unsigned long long)(decoding ? size : n) * 1000ull / best, (unsigned long)size);
    return 0;
}
