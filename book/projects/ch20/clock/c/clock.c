#include "clock.h"

static struct clock main_clock;

struct clock *clock_main(void) {
    return &main_clock;
}

void clock_start(struct clock *clock, const char *name) {
    clock->ticks = 0;
    clock->laps = 0;
    clock->mode = CLOCK_RUNNING;
    clock->name = name;
}

bool clock_tick(struct clock *clock, uint64_t ticks) {
    if (clock->mode != CLOCK_RUNNING || clock->laps == CLOCK_MAX_LAPS) {
        return false;
    }
    clock->ticks += ticks;
    clock->laps += 1;
    return true;
}

uint64_t clock_elapsed(const struct clock *clock) {
    return clock->ticks;
}
