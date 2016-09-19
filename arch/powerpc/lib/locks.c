/*
 * Spin and read/write lock operations.
 *
 * Copyright (C) 2001-2004 Paul Mackerras <paulus@au.ibm.com>, IBM
 * Copyright (C) 2001 Anton Blanchard <anton@au.ibm.com>, IBM
 * Copyright (C) 2002 Dave Engebretsen <engebret@us.ibm.com>, IBM
 *   Rework to support virtual processors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/export.h>
#include <linux/stringify.h>
#include <linux/smp.h>

/* waiting for a spinlock... */
#if defined(CONFIG_PPC_SPLPAR)
#include <asm/hvcall.h>
#include <asm/smp.h>

/*
 * confer our slices to a specified cpu and return. If it is already running or
 * cpu is -1, then we will check confer. If confer is NULL, we will return
 * otherwise we confer our slices to lpar.
 */
void __spin_yield_cpu(int cpu, int confer)
{
	unsigned int holder_cpu = cpu, yield_count;

	if (cpu == -1)
		goto yield_to_lpar;

	BUG_ON(holder_cpu >= nr_cpu_ids);
	yield_count = be32_to_cpu(lppaca_of(holder_cpu).yield_count);

	/* if cpu is running, confer slices to lpar conditionally*/
	if ((yield_count & 1) == 0)
		goto yield_to_lpar;

	plpar_hcall_norets(H_CONFER,
		get_hard_smp_processor_id(holder_cpu), yield_count);
	return;

yield_to_lpar:
	if (confer)
		plpar_hcall_norets(H_CONFER, -1, 0);
}
EXPORT_SYMBOL_GPL(__spin_yield_cpu);

void __spin_wake_cpu(int cpu)
{
	unsigned int holder_cpu = cpu;

	BUG_ON(holder_cpu >= nr_cpu_ids);
	/*
	 * NOTE: we should always do this hcall regardless of
	 * the yield_count of the holder_cpu.
	 * as thers might be a case like below;
	 * CPU 	1				2
	 *				yielded = true
	 *	if (yielded)
	 * 	__spin_wake_cpu()
	 * 				__spin_yield_cpu()
	 *
	 * So we might lose a wake if we check the yield_count and
	 * return directly if the holder_cpu is running.
	 * IOW. do NOT code like below.
	 *  yield_count = be32_to_cpu(lppaca_of(holder_cpu).yield_count);
	 *  if ((yield_count & 1) == 0)
	 *  	return;
	 *
	 * a PROD hcall marks the target_cpu proded, which cause the next cede or confer
	 * called on the target_cpu invalid.
	 */
	plpar_hcall_norets(H_PROD,
		get_hard_smp_processor_id(holder_cpu));
}
EXPORT_SYMBOL_GPL(__spin_wake_cpu);

#ifndef CONFIG_QUEUED_SPINLOCKS
void __spin_yield(arch_spinlock_t *lock)
{
	unsigned int lock_value, holder_cpu, yield_count;

	lock_value = lock->slock;
	if (lock_value == 0)
		return;
	holder_cpu = lock_value & 0xffff;
	BUG_ON(holder_cpu >= NR_CPUS);
	yield_count = be32_to_cpu(lppaca_of(holder_cpu).yield_count);
	if ((yield_count & 1) == 0)
		return;		/* virtual cpu is currently running */
	rmb();
	if (lock->slock != lock_value)
		return;		/* something has changed */
	plpar_hcall_norets(H_CONFER,
		get_hard_smp_processor_id(holder_cpu), yield_count);
}
EXPORT_SYMBOL_GPL(__spin_yield);
#endif

/*
 * Waiting for a read lock or a write lock on a rwlock...
 * This turns out to be the same for read and write locks, since
 * we only know the holder if it is write-locked.
 */
void __rw_yield(arch_rwlock_t *rw)
{
	int lock_value;
	unsigned int holder_cpu, yield_count;

	lock_value = rw->lock;
	if (lock_value >= 0)
		return;		/* no write lock at present */
	holder_cpu = lock_value & 0xffff;
	BUG_ON(holder_cpu >= NR_CPUS);
	yield_count = be32_to_cpu(lppaca_of(holder_cpu).yield_count);
	if ((yield_count & 1) == 0)
		return;		/* virtual cpu is currently running */
	rmb();
	if (rw->lock != lock_value)
		return;		/* something has changed */
	plpar_hcall_norets(H_CONFER,
		get_hard_smp_processor_id(holder_cpu), yield_count);
}
#endif

#ifdef CONFIG_QUEUED_SPINLOCKS
/*
 * This forbid we load an old value in another LL/SC. Because the SC here force
 * another LL/SC repeat. So we guarantee all loads in another LL and SC will
 * read correct value.
 */
static inline u32 atomic_read_sync(atomic_t *v)
{
	u32 val;

	__asm__ __volatile__(
"1:	" PPC_LWARX(%0, 0, %2, 0) "\n"
"	stwcx. %0, 0, %2\n"
"	bne- 1b\n"
	: "=&r" (val), "+m" (*v)
	: "r" (v)
	: "cr0", "xer");

	return val;
}

void queued_spin_unlock_wait(struct qspinlock *lock)
{

	u32 val;

	smp_mb();

	/*
	 * copied from generic queue_spin_unlock_wait with little modification
	 */
	for (;;) {
		/* need _sync, as we might race with another LL/SC in lock()*/
		val = atomic_read_sync(&lock->val);

		if (!val) /* not locked, we're done */
			goto done;

		if (val & _Q_LOCKED_MASK) /* locked, go wait for unlock */
			break;

		/* not locked, but pending, wait until we observe the lock */
		cpu_relax();
	}

	/*
	 * any unlock is good. And need not _sync, as ->val is set by the SC in
	 * unlock(), any loads in lock() must see the correct value.
	 */
	while (atomic_read(&lock->val) & _Q_LOCKED_MASK) {
		HMT_low();
		if (SHARED_PROCESSOR)
			__spin_yield_cpu(spin_lock_holder(lock), 0);
	}
	HMT_medium();
done:
	smp_mb();
}
EXPORT_SYMBOL(queued_spin_unlock_wait);
#endif
