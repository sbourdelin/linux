/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/hash.h>
#include <linux/bootmem.h>

/* +2 here is to make sure there is not many conflict*/
#define NUM_LOCK_CPU_ENTRY_SHIFT (order_base_2(NR_CPUS) + 2)
#define NUM_LOCK_CPU_ENTRY (1 << NUM_LOCK_CPU_ENTRY_SHIFT)
/* we can only spin on 4 locks at same time on same cpu*/
#define NUM_LOCKS_PER_CPU 4

static u16 *hash_lock_cpu_ptr;

struct locks_on_cpu {
	void *l[NUM_LOCKS_PER_CPU];
	int count;
};

static DEFINE_PER_CPU(struct locks_on_cpu, node);

static u16 *hash(void *l)
{
	int val = hash_ptr(l, NUM_LOCK_CPU_ENTRY_SHIFT);

	return &hash_lock_cpu_ptr[val];
}

static void __init init_hash(void)
{
	int size = NUM_LOCK_CPU_ENTRY * sizeof(*hash_lock_cpu_ptr);

	hash_lock_cpu_ptr = memblock_virt_alloc(size, 0);
	memset(hash_lock_cpu_ptr, 0, size);
}

#define lock_get_holder(l)	\
		((int)(*hash(l) - 1))

#define lock_set_holder(l)	\
		(*hash(l) = raw_smp_processor_id() + 1)

int spin_lock_holder(void *lock)
{
	/* we might run on PowerNV, which has no hash table ptr*/
	if (hash_lock_cpu_ptr)
		return lock_get_holder(lock);
	return -1;
}
EXPORT_SYMBOL(spin_lock_holder);

static void *this_cpu_lock(void)
{
	struct locks_on_cpu *this_node = this_cpu_ptr(&node);
	int i = this_node->count - 1;

	return this_node->l[i];
}

static void cpu_save_lock(void *l)
{
	struct locks_on_cpu *this_node = this_cpu_ptr(&node);
	int i = this_node->count++;

	this_node->l[i] = l;
}

static void cpu_remove_lock(void *l)
{
	__this_cpu_dec(node.count);
}

static void __native_queued_spin_unlock(struct qspinlock *lock)
{
	native_queued_spin_unlock(lock);
}

static void __pv_lock(struct qspinlock *lock, u32 val)
{
	/*
	 * save the lock we are spinning on
	 * pv_wait need know this lock
	 */
	cpu_save_lock(lock);

	__pv_queued_spin_lock_slowpath(lock, val);

	/* as we win the lock, remove it*/
	cpu_remove_lock(lock);

	/*
	 * let other spinner know who is the lock holder
	 * we does not need to unset lock holder in unlock()
	 */
	lock_set_holder(lock);
}

static void __pv_wait(u8 *ptr, u8 val)
{
	void *l = this_cpu_lock();
	int cpu;
	int always_confer = !in_interrupt();

	while (READ_ONCE(*ptr) == val) {
		HMT_low();
		/*
		 * the lock might be unlocked once and locked again
		 */
		cpu = lock_get_holder(l);

		/*
		 * the default behavior of __spin_yield_cpu is yielding
		 * our cpu slices to target vcpu or lpar(pHyp or KVM).
		 * consider the latency of hcall itself and the priority of
		 * current task, we can do a optimisation.
		 * IOW, if we are in interrupt, and the target vcpu is running
		 * we do not yield ourself to lpar.
		 */
		__spin_yield_cpu(cpu, always_confer);
	}
	HMT_medium();
}

static void __pv_kick(int cpu)
{
	__spin_wake_cpu(cpu);
}

struct pv_lock_ops pv_lock_op = {
	.lock = native_queued_spin_lock_slowpath,
	.unlock = __native_queued_spin_unlock,
	.wait = NULL,
	.kick = NULL,
};
EXPORT_SYMBOL(pv_lock_op);

struct static_key_true sharedprocessor_key = STATIC_KEY_TRUE_INIT;
EXPORT_SYMBOL(sharedprocessor_key);

void __init pv_lock_init(void)
{
	if (SHARED_PROCESSOR) {
		init_hash();
		__pv_init_lock_hash();
		pv_lock_op.lock = __pv_lock;
		pv_lock_op.unlock = __pv_queued_spin_unlock;
		pv_lock_op.wait = __pv_wait;
		pv_lock_op.kick = __pv_kick;
		static_branch_disable(&sharedprocessor_key);
	}
}
