/*
 * host_test.c - C8's host test: the CeeSafe module (c8/module.cee) through its two entry points, as the generated glue
 * calls them, linked with CosmoOS's spinlock shim, harness and kernel/object/object.c, and built with its HOST_CFLAGS
 * by c8.py.
 */
#include "harness.h"

#include <stdint.h>
#include <string.h>

int32_t ceemod_init(void **out);
void ceemod_shutdown(void *state);
void c8_host_fire_timers(void);
unsigned c8_host_armed(void);
unsigned c8_host_threads(void);
const char *c8_host_trace(void);
void c8_host_reset(void);
long c7_live_blocks(void);
void c7_fail_kmalloc_at(unsigned long n);
unsigned long c7_kmalloc_calls(void);

/* init starts a timer and a thread; the timer re-arms itself each time it fires; shutdown ends both, in field order:
 * the timer is cancelled, then the thread is stopped and joined; no block, timer or thread is left. */
static void test_lifecycle(void)
{
    void *state = NULL;
    c8_host_reset();
    EXPECT(ceemod_init(&state) == 0 && state != NULL);
    EXPECT(c8_host_armed() == 1);
    EXPECT(c8_host_threads() == 1);
    for (int i = 0; i < 5; i++)
        c8_host_fire_timers();
    EXPECT(c8_host_armed() == 1);   /* re-armed by its handler */
    c8_host_reset();
    ceemod_shutdown(state);
    EXPECT(strcmp(c8_host_trace(), "shutdown timer_cancel_sync thread_join ") == 0);
    EXPECT(c8_host_armed() == 0);
    EXPECT(c8_host_threads() == 0);
    EXPECT(c7_live_blocks() == 0);
}

/* Every allocation init makes, failing in turn: init returns -ENOMEM each time, and whatever it had made is dropped
 * again before it returns (a thread started is joined, a timer armed is cancelled), so nothing is left. */
static void test_init_failure_sweep(void)
{
    void *state = NULL;
    c7_fail_kmalloc_at(0);
    EXPECT(ceemod_init(&state) == 0);
    unsigned long allocations = c7_kmalloc_calls();
    ceemod_shutdown(state);
    EXPECT(allocations > 0);
    for (unsigned long k = 1; k <= allocations; k++) {
        state = NULL;
        c7_fail_kmalloc_at(k);
        EXPECT(ceemod_init(&state) == -12);
        EXPECT(state == NULL);
        EXPECT(c8_host_armed() == 0);
        EXPECT(c8_host_threads() == 0);
        EXPECT(c7_live_blocks() == 0);
    }
    c7_fail_kmalloc_at(0);
}

static const struct host_test tests[] = {
    { "c8-lifecycle", test_lifecycle },
    { "c8-init-failure-sweep", test_init_failure_sweep },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
