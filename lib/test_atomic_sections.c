// SPDX-License-Identifier: GPL-2.0
/*
 * Atomic section emulation test module
 *
 * Emulates atomic sections by disabling IRQs or preemption
 * and doing a busy wait for a specified amount of time.
 * This can be used for testing of different atomic section
 * tracers such as irqsoff tracers.
 *
 * (c) 2018. Google LLC
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/string.h>

static ulong atomic_time = 100;
static char atomic_mode[10] = "irq";

module_param_named(atomic_time, atomic_time, ulong, S_IRUGO);
module_param_string(atomic_mode, atomic_mode, 10, S_IRUGO);
MODULE_PARM_DESC(atomic_time, "Period in microseconds (100 uS default)");
MODULE_PARM_DESC(atomic_mode, "Mode of the test such as preempt or irq (default irq)");

static void busy_wait(ulong time)
{
	ktime_t start, end;
	start = ktime_get();
	do {
		end = ktime_get();
		if (kthread_should_stop())
			break;
	} while (ktime_to_ns(ktime_sub(end, start)) < (time * 1000));
}

int atomic_sect_run(void *data)
{
	unsigned long flags;

	if (!strcmp(atomic_mode, "irq")) {
		local_irq_save(flags);
		busy_wait(atomic_time);
		local_irq_restore(flags);
	} else if (!strcmp(atomic_mode, "preempt")) {
		preempt_disable();
		busy_wait(atomic_time);
		preempt_enable();
	}

	return 0;
}

static int __init atomic_sect_init(void)
{
	char task_name[50];
	struct task_struct *test_task;

	snprintf(task_name, sizeof(task_name), "%s_test", atomic_mode);

	test_task = kthread_run(atomic_sect_run, NULL, task_name);
	return PTR_ERR_OR_ZERO(test_task);
}

static void __exit atomic_sect_exit(void)
{
	return;
}

module_init(atomic_sect_init)
module_exit(atomic_sect_exit)
MODULE_LICENSE("GPL v2");
