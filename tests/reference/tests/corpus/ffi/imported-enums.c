/* The C half of ffi/imported-enums.cee: an operation table whose entry is CeeSafe's export, a struct member of an
 * enum type CeeSafe rewrites, and a callback of an enum-typed function pointer type. */
#include <stdio.h>

#include "ceesafe_exports.h"

int32_t c_main(void);
const struct painter_ops *c_ops(void);
int32_t c_each(color_cb cb, void *arg);

/* The table's entry is the export itself: its C type takes the enums, as the field's does. */
static const struct painter_ops ops = { .mix = paint_mix };

const struct painter_ops *c_ops(void) {
    return &ops;
}

int32_t c_each(color_cb cb, void *arg) {
    int32_t rc = cb(arg, RED);
    rc += cb(arg, GREEN);
    rc += cb(arg, BLUE);
    return rc;
}

int32_t c_main(void) {
    struct paint p = { .c = BLUE, .amount = 9 };
    int32_t viaTable = ops.mix(GREEN, BIG);
    int32_t repainted = paint_repaint(&p);
    printf("%d %d %d %d\n", (int)viaTable, (int)repainted, (int)paint_side(), (int)(p.c == GREEN));
    return 0;
}
