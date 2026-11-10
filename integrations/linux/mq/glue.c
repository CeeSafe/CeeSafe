// SPDX-License-Identifier: GPL-2.0-only
/*
 * glue.c - what Linux requires to be written in C for ceesafe_mq: the module's entry and exit, its licence, and the
 * file_operations table of /dev/ceesafe_mq. Each function forwards to one export of src/mq.cee; the device's state,
 * its locking and its memory are CeeSafe. It also binds CeeSafe's panic to the kernel (spec/profiles.md §4.2, §5).
 */
#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/printk.h>

#include "ceesafe/rt.h"
#include "ceesafe_exports.h"

/* Kernel blocks CeeSafe has allocated and not yet freed (include/ceesafe_linux.h counts them). */
atomic_long_t lx_live_blocks = ATOMIC_LONG_INIT(0);

/* The device: a Box<Device, KernelHeap> from ceemq_create, given back to ceemq_destroy at exit. */
static void *mq_device;

/*
 * A CeeSafe panic (a failed bounds or overflow check, an unwrap of nothing) is a bug in the module: report it and
 * stop the thread, as BUG() does for C. It must not return.
 */
void ceesafe_panic(cee_panic_kind kind, cee_loc loc)
{
	pr_emerg("ceesafe_mq: panic: %s at %s:%u:%u\n", ceesafe_panic_reason(kind), loc.file, (unsigned int)loc.line,
		 (unsigned int)loc.column);
	BUG();
}

static ssize_t mq_write(struct file *file, const char __user *buf, size_t n, loff_t *pos)
{
	return ceemq_write(mq_device, (const void *)buf, n);
}

static ssize_t mq_read(struct file *file, char __user *buf, size_t n, loff_t *pos)
{
	return ceemq_read(mq_device, (void *)buf, n);
}

/* .owner keeps the module loaded while the device is open, so no read or write runs during ceemq_destroy. */
static const struct file_operations mq_fops = {
	.owner = THIS_MODULE,
	.read = mq_read,
	.write = mq_write,
	.llseek = noop_llseek,
};

static struct miscdevice mq_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ceesafe_mq",
	.fops = &mq_fops,
};

static int __init mq_init(void)
{
	int error = ceemq_create(&mq_device);

	if (error)
		return error;
	error = misc_register(&mq_misc);
	if (error)
		ceemq_destroy(mq_device);
	return error;
}

static void __exit mq_exit(void)
{
	misc_deregister(&mq_misc);
	ceemq_destroy(mq_device);
	pr_info("ceesafe_mq: unloaded, %ld kernel block(s) live\n", atomic_long_read(&lx_live_blocks));
}

module_init(mq_init);
module_exit(mq_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A message queue device written in CeeSafe");
