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

#define NUM_LOCK_CPU_ENTRY_SHIFT 16
#define NUM_LOCK_CPU_ENTRY (1 << NUM_LOCK_CPU_ENTRY_SHIFT)
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
		((int)*hash(l) - 1)

#define lock_set_holder(l)	\
		(*hash(l) = raw_smp_processor_id() + 1)

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
	this_cpu_dec(node.count);
}

static void __native_queued_spin_unlock(struct qspinlock *lock)
{
	native_queued_spin_unlock(lock);
}

static void __pv_lock(struct qspinlock *lock, u32 val)
{
	cpu_save_lock(lock);
	__pv_queued_spin_lock_slowpath(lock, val);
	cpu_remove_lock(lock);
	lock_set_holder(lock);
}

static void __pv_unlock(struct qspinlock *lock)
{
	__pv_queued_spin_unlock(lock);
}

static void __pv_wait(u8 *ptr, u8 val, int cpu)
{
	void *l = this_cpu_lock();

	HMT_low();
	while (READ_ONCE(*ptr) == val) {
		cpu = lock_get_holder(l);
		__spin_yield_cpu(cpu);
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

void __init pv_lock_init(void)
{
	if (SHARED_PROCESSOR) {
		init_hash();
		__pv_init_lock_hash();
		pv_lock_op.lock = __pv_lock;
		pv_lock_op.unlock = __pv_unlock;
		pv_lock_op.wait = __pv_wait;
		pv_lock_op.kick = __pv_kick;
	}
}
