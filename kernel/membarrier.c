/*
 * Copyright (C) 2010, 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * membarrier system call
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/syscalls.h>
#include <linux/membarrier.h>
#include <linux/tick.h>

/*
 * XXX For cpu_rq(). Should we rather move
 * membarrier_private_expedited() to sched/core.c or create
 * sched/membarrier.c ?
 */
#include "sched/sched.h"

/*
 * Bitmask made from a "or" of all commands within enum membarrier_cmd,
 * except MEMBARRIER_CMD_QUERY.
 */
#define MEMBARRIER_CMD_BITMASK	\
	(MEMBARRIER_CMD_SHARED | MEMBARRIER_CMD_PRIVATE_EXPEDITED)

static void ipi_mb(void *info)
{
	smp_mb();	/* IPIs should be serializing but paranoid. */
}

static void membarrier_private_expedited_ipi_each(void)
{
	int cpu;

	for_each_online_cpu(cpu) {
		struct task_struct *p;

		rcu_read_lock();
		p = task_rcu_dereference(&cpu_rq(cpu)->curr);
		if (p && p->mm == current->mm)
			smp_call_function_single(cpu, ipi_mb, NULL, 1);
		rcu_read_unlock();
	}
}

static void membarrier_private_expedited(void)
{
	int cpu, this_cpu;
	cpumask_var_t tmpmask;

	if (num_online_cpus() == 1)
		return;

	/*
	 * Matches memory barriers around rq->curr modification in
	 * scheduler.
	 */
	smp_mb();	/* system call entry is not a mb. */

	if (!alloc_cpumask_var(&tmpmask, GFP_NOWAIT)) {
		/* Fallback for OOM. */
		membarrier_private_expedited_ipi_each();
		goto end;
	}

	this_cpu = raw_smp_processor_id();
	for_each_online_cpu(cpu) {
		struct task_struct *p;

		if (cpu == this_cpu)
			continue;
		rcu_read_lock();
		p = task_rcu_dereference(&cpu_rq(cpu)->curr);
		if (p && p->mm == current->mm)
			__cpumask_set_cpu(cpu, tmpmask);
		rcu_read_unlock();
	}
	smp_call_function_many(tmpmask, ipi_mb, NULL, 1);
	free_cpumask_var(tmpmask);
end:
	/*
	* Memory barrier on the caller thread _after_ we finished
	* waiting for the last IPI. Matches memory barriers around
	* rq->curr modification in scheduler.
	*/
	smp_mb();	/* exit from system call is not a mb */
}

/**
 * sys_membarrier - issue memory barriers on a set of threads
 * @cmd:   Takes command values defined in enum membarrier_cmd.
 * @flags: Currently needs to be 0. For future extensions.
 *
 * If this system call is not implemented, -ENOSYS is returned. If the
 * command specified does not exist, or if the command argument is invalid,
 * this system call returns -EINVAL. For a given command, with flags argument
 * set to 0, this system call is guaranteed to always return the same value
 * until reboot.
 *
 * All memory accesses performed in program order from each targeted thread
 * is guaranteed to be ordered with respect to sys_membarrier(). If we use
 * the semantic "barrier()" to represent a compiler barrier forcing memory
 * accesses to be performed in program order across the barrier, and
 * smp_mb() to represent explicit memory barriers forcing full memory
 * ordering across the barrier, we have the following ordering table for
 * each pair of barrier(), sys_membarrier() and smp_mb():
 *
 * The pair ordering is detailed as (O: ordered, X: not ordered):
 *
 *                        barrier()   smp_mb() sys_membarrier()
 *        barrier()          X           X            O
 *        smp_mb()           X           O            O
 *        sys_membarrier()   O           O            O
 */
SYSCALL_DEFINE2(membarrier, int, cmd, int, flags)
{
	/* MEMBARRIER_CMD_SHARED is not compatible with nohz_full. */
	if (tick_nohz_full_enabled())
		return -ENOSYS;
	if (unlikely(flags))
		return -EINVAL;
	switch (cmd) {
	case MEMBARRIER_CMD_QUERY:
		return MEMBARRIER_CMD_BITMASK;
	case MEMBARRIER_CMD_SHARED:
		if (num_online_cpus() > 1)
			synchronize_sched();
		return 0;
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
		membarrier_private_expedited();
		return 0;
	default:
		return -EINVAL;
	}
}
