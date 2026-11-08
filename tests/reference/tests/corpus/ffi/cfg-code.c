/* The C half of ffi/cfg-code.cee. */
#include "cfg-code.types.h"

static struct engine engine = { .blocks = 10 };

struct engine *the_engine(void) {
    return &engine;
}

int32_t on_only(int32_t x) {
    return x * 100;
}
