/* For the leak and crash tests (tools/test_command_check.py): the runtime's allocator, and an abort. */
#include "ceesafe/rt.h"
#include <stdlib.h>

void tc_leak(void);
void tc_crash(void);

void tc_leak(void) { (void)ceesafe_alloc(16u, 0u); }

void tc_crash(void) { abort(); }
