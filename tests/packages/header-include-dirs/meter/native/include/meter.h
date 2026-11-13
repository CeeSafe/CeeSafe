#ifndef METER_H
#define METER_H

#include <stdint.h>

struct meter {
    uint32_t count;
    uint32_t step;
};

struct meter *meter_main(void);
void meter_start(struct meter *meter, uint32_t step);
void meter_tick(struct meter *meter);

#endif
