/* The C functions ffi/variadic.cee declares (ADR 0041 §3.3): variadic, as CosmoOS's klog and ksnprintf are. */
#ifndef VARIADIC_TYPES_H
#define VARIADIC_TYPES_H

#include <stddef.h>
#include <stdint.h>

int64_t va_sum(int32_t count, ...);                       /* `count` int64_t arguments, summed */
#if defined(__GNUC__)
#define VA_PRINTF __attribute__((format(printf, 2, 3))) /* as CosmoOS's __printf: C checks literal formats */
#else
#define VA_PRINTF
#endif

int va_log(uint32_t level, const char *format, ...) VA_PRINTF; /* a format, printed with its level; the length */
const char *va_kind(const char *format, ...);              /* returns its format */

#endif
