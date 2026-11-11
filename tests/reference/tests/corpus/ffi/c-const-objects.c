/* The C half of ffi/c-const-objects.cee: reads CeeSafe's read-only record and calls through its table. */
#include <stdio.h>

#include "ceesafe_exports.h"

int32_t co_c_handler(int32_t value) {
    return value + 100;
}

int32_t c_main(void) {
    const struct co_record *r = &co_native;
    printf("%s %u %d\n", r->name, (unsigned)r->count, r->first == NULL);
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->table[i] == NULL) {
            printf("%u null\n", (unsigned)i);
        } else {
            printf("%u %d\n", (unsigned)i, (int)r->table[i](7));
        }
    }
    printf("%d %d\n", (int)co_through_table(1, 5), (int)co_through_ops(21));
    return 0;
}
