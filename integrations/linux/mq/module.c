// SPDX-License-Identifier: GPL-2.0-only
/*
 * module.c - ceesafe_mq as one translation unit: CeeSafe's core runtime, the C generated from src/, and the glue.
 *
 * They are one unit because of the panic function. Generated code calls ceesafe_panic, which never returns, and
 * emits nothing after the call. On x86-64 the kernel's objtool checks every function's control flow, and it knows
 * that a function does not return only from its own list of names or from seeing the function's body in the same
 * object. Compiled as separate objects, every caller of ceesafe_panic is reported ("falls through to next
 * function"); with glue.c's definition in the same unit, objtool sees that it ends in BUG(). The kernel builds
 * its scheduler the same way (kernel/sched/build_policy.c).
 */
#include "gen/ceesafe_rt_core.c"
#include "gen/main.c"
#include "glue.c"
