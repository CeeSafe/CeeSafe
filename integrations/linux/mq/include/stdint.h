/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * <stdint.h> for CeeSafe's generated C inside a Linux kernel build. The kernel compiles with -nostdinc and defines
 * the fixed-width types itself (int64_t is `long long` on every architecture, where a compiler's own <stdint.h>
 * may say `long`), so the standard names must come from the kernel: one definition of each type in a translation
 * unit that includes both ceesafe/rt.h and kernel headers. The limits C11 7.20.2 requires and <linux/limits.h> does
 * not spell this way are given as literals, so they also work in #if.
 */
#ifndef CEESAFE_LINUX_STDINT_H
#define CEESAFE_LINUX_STDINT_H

#include <linux/limits.h>
#include <linux/types.h>

#ifndef INT8_MAX
#define INT8_MAX 127
#define INT8_MIN (-128)
#define UINT8_MAX 255
#define INT16_MAX 32767
#define INT16_MIN (-32768)
#define UINT16_MAX 65535
#define INT32_MAX 2147483647
#define INT32_MIN (-2147483647 - 1)
#define UINT32_MAX 4294967295U
#define INT64_MAX 9223372036854775807LL
#define INT64_MIN (-9223372036854775807LL - 1)
#define UINT64_MAX 18446744073709551615ULL
#endif

#endif
