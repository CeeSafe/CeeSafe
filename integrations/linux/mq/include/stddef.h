/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * <stddef.h> inside a Linux kernel build: size_t, ptrdiff_t, NULL and offsetof are the kernel's (see stdint.h here).
 * The kernel has no max_align_t. C11's is the type with the greatest fundamental alignment, which is 16 bytes on
 * x86-64 and AArch64 (that of `long double`, which kernel code must not name).
 */
#ifndef CEESAFE_LINUX_STDDEF_H
#define CEESAFE_LINUX_STDDEF_H

#include <linux/stddef.h>
#include <linux/types.h>

typedef struct {
	long long ceesafe_ll;
	void *ceesafe_p;
} __attribute__((aligned(16))) max_align_t;
_Static_assert(_Alignof(max_align_t) == 16, "max_align_t: the greatest fundamental alignment");

#endif
