/* MSVC /O2 miscompiles this valid C11 (found 2026-11-01 by run-pass/slice-bulk.cee, L1 A4a; ROADMAP §10).
 *
 * The loop stores 70000 into words.e[0..3] through t3.ptr, a copy of a pointer into `words` returned by a function;
 * then memmove overwrites words.e[0..1]. The sum must be 1 + 2 + 70000 = 70003. MSVC 19.44, 19.50 and 19.51
 * (VS 18, toolsets 14.44, 14.50, 14.51), targeting ARM64 and x64, print 3 at /O2 and 70003 at /Od; clang and GCC
 * print 70003. Without the memmove MSVC is right, and so are /O1, /Ob0 (no inlining) and /Oi- (no intrinsics). ceesafec
 * builds with /O2 /Oi- (src/host/compilers.cee; spec/c-backend.md §1.1): when this prints 70003 under plain /O2 on
 * every MSVC ceesafec supports, the workaround can go.
 *
 *   cl /nologo /O2 /std:c11 msvc-o2-store-through-copy.c && msvc-o2-store-through-copy.exe
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint32_t *ptr; uint64_t len; } ceeSm_u32;
typedef struct { const uint32_t *ptr; uint64_t len; } ceeSl_u32;
typedef struct { uint32_t e[4]; } ceeA4_u32;
typedef struct { uint32_t e[2]; } ceeA2_u32;
static void cee_slice_copy(void *d, const void *s, uint64_t count, size_t size) {
    if (count != 0u) memmove(d, s, (size_t)count * size);
}
static ceeSm_u32 same(ceeSm_u32 v) { ceeSm_u32 t1, t2; t1 = (ceeSm_u32){ .ptr = v.ptr, .len = v.len }; t2 = t1; return t2; }
static void run(void) {
    ceeA4_u32 words = {0};
    ceeA2_u32 pair = {0};
    ceeSm_u32 t2, t3, t4;
    ceeSl_u32 t5;
    for (uint64_t i = 0; i < 4u; i++) words.e[i] = 0u;
    t2 = (ceeSm_u32){ .ptr = words.e + ((uint64_t)0), .len = ((uint64_t)4) - ((uint64_t)0) };
    t3 = same(t2);
    for (uint64_t i = 0u; i < t3.len; i++) t3.ptr[i] = 70000u;
    pair = (ceeA2_u32){ .e = { 1u, 2u } };
    t4 = (ceeSm_u32){ .ptr = words.e, .len = 2 };
    t5 = (ceeSl_u32){ .ptr = pair.e, .len = 2 };
    cee_slice_copy(t4.ptr, t5.ptr, t4.len, sizeof *t4.ptr);
    printf("%u\n", words.e[0] + words.e[1] + words.e[3]);
}
int main(void) { run(); return 0; }
