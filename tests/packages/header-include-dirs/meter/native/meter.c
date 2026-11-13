#include "meter.h"

static struct meter main_meter;

struct meter *meter_main(void) {
    return &main_meter;
}

void meter_start(struct meter *meter, uint32_t step) {
    meter->count = 0;
    meter->step = step;
}

void meter_tick(struct meter *meter) {
    meter->count += meter->step;
}
