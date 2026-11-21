/* The environment for freestanding/wipe.cee: ceesafe_panic, and a caller. */
#include <stdio.h>
#include <stdlib.h>

#include "ceesafe/rt.h"
#include "ceesafe_exports.h"

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc) {
    fprintf(stderr, "ceesafe panic: %d at %s:%u:%u\n", (int)kind, loc.file, (unsigned)loc.line, (unsigned)loc.column);
    exit(134);
}

int main(void) {
    printf("wipe %u\n", (unsigned)wipe_demo(1));
    return 0;
}
