/* SPDX-License-Identifier: GPL-2.0-only WITH Classpath-exception-2.0 */
/*
 * CeeSafe's allocator for C code (ADR 0060). ceesafec writes this header beside ceesafe/rt.h in every build, whose
 * directory every C file of the build has on its include path, so a package's C files write
 * `#include "ceesafe/foreign.h"`.
 *
 * With CEE_ALLOC_FOREIGN, an allocation is counted and fault-injected like every allocation of the program (the
 * attempt numbered CEESAFE_FAIL_ALLOC_AT fails, in one sequence with CeeSafe's own), but a block still live at exit is
 * not a leak: C libraries keep global state until the process ends. The report gives those blocks on their own line,
 * `ceesafe alloc: foreign held=<k>`.
 *
 * The calls follow malloc, realloc and free: memory aligned for any C type (max_align_t), NULL on failure,
 * ceesafe_free(NULL, …) does nothing and ceesafe_realloc(NULL, n, …) allocates. A failed realloc leaves the block
 * unchanged and live. A block is freed with the number it was allocated with.
 */
#ifndef CEESAFE_FOREIGN_H
#define CEESAFE_FOREIGN_H

#include <stddef.h>
#include <stdint.h>

#define CEE_ALLOC_FOREIGN 2u

void *ceesafe_alloc(size_t size, uint32_t allocator);
void *ceesafe_realloc(void *p, size_t size, uint32_t allocator);
void ceesafe_free(void *p, uint32_t allocator);

#endif
