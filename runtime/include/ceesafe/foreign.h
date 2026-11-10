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
 * The calls follow malloc, realloc and free: memory aligned to max(16, alignof(max_align_t)), so for any C type (ADR
 * 0088 §2.5), NULL on failure, ceesafe_free(NULL, …) does nothing and ceesafe_realloc(NULL, n, …) allocates. A failed
 * realloc leaves the block unchanged and live. A block is freed with the number it was allocated with, or with the
 * other foreign number. In a host that installed an allocator with ceesafe_init (ceesafe/embed.h), the blocks are
 * the host's allocator's.
 */
#ifndef CEESAFE_FOREIGN_H
#define CEESAFE_FOREIGN_H

#include <stddef.h>
#include <stdint.h>

#define CEE_ALLOC_FOREIGN 2u

/*
 * For a C library's one-time initialisation, which cannot survive a failed allocation (ADR 0061: OpenSSL's raises an
 * error that re-enters the pthread_once running it). Never failed and not counted as an attempt, so a sweep's numbering
 * is the program's own; its blocks count with CEE_ALLOC_FOREIGN's in "foreign held", and a block may be reallocated
 * or freed under either number. Use it for the initialisation only: everything else allocated under it escapes the
 * allocation-failure sweep.
 */
#define CEE_ALLOC_FOREIGN_INIT 3u

void *ceesafe_alloc(size_t size, uint32_t allocator);
void *ceesafe_realloc(void *p, size_t size, uint32_t allocator);
void ceesafe_free(void *p, uint32_t allocator);

#endif
