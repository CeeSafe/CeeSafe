/* The C half of ffi/c-data.cee. */
#include <stdio.h>

#include "ceesafe_exports.h"

const char *const data_names[2] = { "alpha", "beta" };
uint32_t g_data_counter = 40;
uint32_t g_data_flags = 6;

static struct dev d1 = { 7 }, d2 = { 9 };
static struct pool the_pool;

int32_t data_mac(char domain, const uint8_t *p) {
    return (int32_t)(unsigned char)domain * 100 + p[0];
}

struct pool *data_pool(void) {
    the_pool.count = 2;
    the_pool.spare[0] = &d1;
    the_pool.spare[3] = &d2;
    the_pool.tags[1] = "two";
    __atomic_store_n(&the_pool.dumpable, 3, __ATOMIC_SEQ_CST);
    return &the_pool;
}

void stats(struct stats *out) {
    out->reads = 11;
}

void data_loadavg(unsigned long out[3]) {
    out[0] = 1;
    out[1] = 22;
    out[2] = 333;
}

int32_t c_main(void) {
    g_data_rate = 7;
    __atomic_fetch_add(&g_data_hits, 30, __ATOMIC_SEQ_CST);
    printf("%d %u\n", (int)data_ops.twice(21), (unsigned)data_ops.flags);
    return data_side();
}
