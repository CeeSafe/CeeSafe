/* The oracle of ffi/async-set-spread.cee: the address of a thread-local byte, which differs between the threads that
 * are alive at once, so two children that see different tags ran on two OS threads. Never 0. */
#include <stdint.h>

#include "ceesafe/rt.h"
#include "async-set-spread.types.h"

static _Thread_local char tag;

uint64_t thread_tag(void) { return (uint64_t)(uintptr_t)&tag; }
