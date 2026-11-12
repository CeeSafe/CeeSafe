#include "ceesafe/rt.h"
#include "ceesafe_exports.h"
#include <stdio.h>
#include <stdlib.h>

/* The function the package declares in its extern "C" block. */
void board_log(uint64_t value);

void board_log(uint64_t value) {
    printf("log: %llu\n", (unsigned long long)value);
}

/* Every panic of the CeeSafe code arrives here. It must not return. */
_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc) {
    fflush(stdout);
    fprintf(stderr, "board: stopped: %s at %s:%u:%u\n", ceesafe_panic_reason(kind), loc.file, (unsigned)loc.line,
        (unsigned)loc.column);
    exit(3);
}

int main(void) {
    const uint8_t readings[4] = {10, 20, 30, 40};
    printf("sum: %u\n", (unsigned)gauge_sum(readings, 4));
    printf("squares: %u\n", (unsigned)gauge_squares(5));
    printf("squares: %u\n", (unsigned)gauge_squares(100));
    printf("percent: %u\n", (unsigned)gauge_percent(3, 12));
    printf("percent: %u\n", (unsigned)gauge_percent(3, 0));
    return 0;
}
