/* The C types ffi/atomic-fields.cee imports (ADR 0041 §3.1): integer members that C and CeeSafe both change atomically. */
#ifndef ATOMIC_FIELDS_TYPES_H
#define ATOMIC_FIELDS_TYPES_H

#include <stddef.h>
#include <stdint.h>

struct af_stats {
    uint64_t packets; /* AtomicU64 */
    uint32_t flags;   /* AtomicU32 */
    size_t bytes;     /* AtomicUSize */
    uint16_t other;   /* a plain member beside them */
};

struct af_stats *af_stats_get(void); /* in atomic-fields.c */
void af_stats_bump(void);

#endif
