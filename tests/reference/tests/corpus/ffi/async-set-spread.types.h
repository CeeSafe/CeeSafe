/* ffi/async-set-spread.cee's C half: names the OS thread that calls it. */
#ifndef ASYNC_SET_SPREAD_TYPES_H
#define ASYNC_SET_SPREAD_TYPES_H

#include <stdint.h>

uint64_t thread_tag(void);

#endif
