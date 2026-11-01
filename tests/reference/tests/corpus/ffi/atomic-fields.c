/* The C half of ffi/atomic-fields.cee: owns an af_stats that CeeSafe changes atomically, and changes CeeSafe's exported
 * af_counters atomically; both sides use the same atomic operations on the same integer members. */
#include <stdio.h>

#include "ceesafe_exports.h"

int32_t c_main(void);

static struct af_stats g_stats = { .packets = 10, .flags = 0x10, .bytes = 100 };

struct af_stats *af_stats_get(void) { return &g_stats; }

void af_stats_bump(void) {
    __atomic_fetch_add(&g_stats.packets, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&g_stats.flags, 0x1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_stats.bytes, 5, __ATOMIC_RELAXED);
}

int32_t c_main(void) {
    af_use_stats();
    printf("%llu %x %zu %u\n", (unsigned long long)g_stats.packets, (unsigned)g_stats.flags, g_stats.bytes,
           (unsigned)g_stats.other);
    struct af_counters *c = (struct af_counters *)(uintptr_t)af_counters_get(); /* shared: changed atomically */
    printf("%llu\n", (unsigned long long)af_counters_hit());
    __atomic_fetch_add(&c->hits, 10, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&c->mask, 0x100, __ATOMIC_SEQ_CST);
    __atomic_fetch_sub(&c->total, 2, __ATOMIC_SEQ_CST);
    af_report();
    printf("%llu %x %llu %u\n", (unsigned long long)__atomic_load_n(&c->hits, __ATOMIC_SEQ_CST),
           (unsigned)__atomic_load_n(&c->mask, __ATOMIC_SEQ_CST),
           (unsigned long long)__atomic_load_n(&c->total, __ATOMIC_SEQ_CST), (unsigned)c->size);
    return 0;
}
