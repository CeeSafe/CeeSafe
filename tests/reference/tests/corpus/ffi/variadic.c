/* The C half of ffi/variadic.cee: variadic functions that read their arguments with va_arg, as the format or the count
 * says, and print what they read. */
#include <stdarg.h>
#include <stdio.h>

#include "variadic.types.h"

int64_t va_sum(int32_t count, ...) {
    va_list ap;
    va_start(ap, count);
    int64_t sum = 0;
    for (int32_t k = 0; k < count; k++)
        sum += va_arg(ap, int64_t);
    va_end(ap);
    return sum;
}

int va_log(uint32_t level, const char *format, ...) {
    char line[128];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    printf("<%u> %s\n", (unsigned)level, line);
    return n;
}

const char *va_kind(const char *format, ...) { return format; }
