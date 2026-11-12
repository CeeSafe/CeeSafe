#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define CLOCK_MAX_LAPS 8

enum clock_mode { CLOCK_STOPPED = -1, CLOCK_RUNNING = 1 };

struct clock {
    uint64_t ticks;
    uint32_t laps;
    enum clock_mode mode;
    const char *name;
};

struct clock *clock_main(void);
void clock_start(struct clock *clock, const char *name);
bool clock_tick(struct clock *clock, uint64_t ticks);
uint64_t clock_elapsed(const struct clock *clock);

#endif
