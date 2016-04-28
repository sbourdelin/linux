/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/spinlock.h>

static void __native_queued_spin_unlock(struct qspinlock *lock)
{
	native_queued_spin_unlock(lock);
}

static void __native_wait(u8 *ptr, u8 val, int cpu)
{
}

static void __native_kick(int cpu)
{
}

static void __pv_wait(u8 *ptr, u8 val, int cpu)
{
	HMT_low();
	__spin_yield_cpu(cpu);
	HMT_medium();
}

static void __pv_kick(int cpu)
{
	__spin_wake_cpu(cpu);
}

struct pv_lock_ops pv_lock_op = {
	.lock = native_queued_spin_lock_slowpath,
	.unlock = __native_queued_spin_unlock,
	.wait = __native_wait,
	.kick = __native_kick,
};
EXPORT_SYMBOL(pv_lock_op);

void __init pv_lock_init(void)
{
	if (SHARED_PROCESSOR) {
		__pv_init_lock_hash();
		pv_lock_op.lock = __pv_queued_spin_lock_slowpath;
		pv_lock_op.unlock = __pv_queued_spin_unlock;
		pv_lock_op.wait = __pv_wait;
		pv_lock_op.kick = __pv_kick;
	}
}
