/* The C types ffi/c-const-objects.cee imports (ADR 0091): a table of handlers and the record that points at it, as a
 * kernel declares a system-call table and a personality. C declares the record const. */
#ifndef C_CONST_OBJECTS_TYPES_H
#define C_CONST_OBJECTS_TYPES_H

#include <stdint.h>

typedef int32_t (*co_handler)(int32_t value);

struct co_record {
    const char *name;
    const co_handler *table;
    uint32_t count;
    int32_t (*first)(int32_t value); /* optional: CeeSafe's record leaves it NULL */
};

struct co_ops {
    int32_t (*twice)(int32_t value);
};

extern const struct co_record co_native; /* defined in CeeSafe, read-only */

int32_t co_c_handler(int32_t value);     /* in c-const-objects.c */
int32_t c_main(void);

#endif
