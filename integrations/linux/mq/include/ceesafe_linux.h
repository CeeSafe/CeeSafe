/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ceesafe_linux.h - the C adapter between CeeSafe and Linux for this module (AGENTS.md: "small C adapters ... behind a
 * documented contract"). CeeSafe declares C *functions*; much of Linux's API is macros (mutex_init and spin_lock_init
 * make a lockdep key where they are written, GFP_KERNEL is an expression, pr_info pastes its format), so each one
 * CeeSafe needs is given a function here. Nothing in this file decides anything: each function is one kernel call.
 *
 * Contracts (ownership, lengths, errors):
 * - lx_kmalloc(size): GFP_KERNEL, so it may sleep; NULL when out of memory; a block is at least 8-byte aligned.
 *   lx_kfree(p): p is a block from lx_kmalloc, freed once. Both count in lx_live_blocks (defined by the module).
 * - cee_mutex_*, cee_spin_*: the lock was initialised by its _init and is at its final address. All locks made
 *   here share one lockdep class each (one mutex_init and one spin_lock_init site).
 * - lx_copy_from_user(to, from, n), lx_copy_to_user(to, from, n): the kernel side has n bytes; the user side is
 *   any user pointer, checked by the kernel. They return the bytes NOT copied (0: all), and may sleep.
 * - lx_log(text): a zero-terminated line; never sleeps.
 */
#ifndef CEESAFE_LINUX_H
#define CEESAFE_LINUX_H

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>

extern atomic_long_t lx_live_blocks;

static inline void *lx_kmalloc(size_t size)
{
	void *p = kmalloc(size, GFP_KERNEL);

	if (p)
		atomic_long_inc(&lx_live_blocks);
	return p;
}

static inline void lx_kfree(void *p)
{
	if (p)
		atomic_long_dec(&lx_live_blocks);
	kfree(p);
}

static inline void lx_mutex_init(struct mutex *m)
{
	mutex_init(m);
}

static inline void lx_mutex_lock(struct mutex *m)
{
	mutex_lock(m);
}

static inline void lx_mutex_unlock(struct mutex *m)
{
	mutex_unlock(m);
}

static inline void lx_spin_lock_init(spinlock_t *lock)
{
	spin_lock_init(lock);
}

static inline void lx_spin_lock(spinlock_t *lock)
{
	spin_lock(lock);
}

static inline void lx_spin_unlock(spinlock_t *lock)
{
	spin_unlock(lock);
}

static inline size_t lx_copy_from_user(uint8_t *to, const void *from, size_t n)
{
	return copy_from_user(to, (const void __user *)from, n);
}

static inline size_t lx_copy_to_user(void *to, const uint8_t *from, size_t n)
{
	return copy_to_user((void __user *)to, from, n);
}

static inline void lx_log(const char *text)
{
	pr_info("%s\n", text);
}

#endif
