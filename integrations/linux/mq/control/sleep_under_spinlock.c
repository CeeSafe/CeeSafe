// SPDX-License-Identifier: GPL-2.0-only
/*
 * sleep_under_spinlock.c - the control for ../negative/sleep-under-spinlock.cee, in C. It allocates with
 * GFP_KERNEL while a spinlock is held, the mistake CeeSafe rejects with N0295. C compiles it without a warning; a
 * kernel built with CONFIG_DEBUG_ATOMIC_SLEEP reports it when it runs ("BUG: sleeping function called from invalid
 * context"), and a kernel without it reports nothing. Loading it is how ../test.py shows that the debug kernel's
 * checks are live.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

static DEFINE_SPINLOCK(latest_lock);
static u64 *latest;

static int __init control_init(void)
{
	u64 *value;

	spin_lock(&latest_lock);
	value = kmalloc(sizeof(*value), GFP_KERNEL); /* may sleep, with the spinlock held */
	if (value)
		*value = 1;
	latest = value;
	spin_unlock(&latest_lock);
	return latest ? 0 : -ENOMEM;
}

static void __exit control_exit(void)
{
	kfree(latest);
}

module_init(control_init);
module_exit(control_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Control: a GFP_KERNEL allocation under a spinlock, in C");
