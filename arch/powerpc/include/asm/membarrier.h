#ifndef _ASM_POWERPC_MEMBARRIER_H
#define _ASM_POWERPC_MEMBARRIER_H

static inline void membarrier_arch_sched_in(struct task_struct *prev,
		struct task_struct *next)
{
	/*
	 * Only need the full barrier when switching between processes.
	 */
	if (likely(!test_thread_flag(TIF_MEMBARRIER_PRIVATE_EXPEDITED)
			|| prev->mm == next->mm))
		return;

	/*
	 * The membarrier system call requires a full memory barrier
	 * after storing to rq->curr, before going back to user-space.
	 */
	smp_mb();
}
static inline void membarrier_arch_fork(struct task_struct *t,
		unsigned long clone_flags)
{
	/*
	 * Coherence of TIF_MEMBARRIER_PRIVATE_EXPEDITED against thread
	 * fork is protected by siglock. membarrier_arch_fork is called
	 * with siglock held.
	 */
	if (t->mm->membarrier_private_expedited)
		set_ti_thread_flag(t, TIF_MEMBARRIER_PRIVATE_EXPEDITED);
}
static inline void membarrier_arch_execve(struct task_struct *t)
{
	clear_ti_thread_flag(t, TIF_MEMBARRIER_PRIVATE_EXPEDITED);
}
void membarrier_arch_register_private_expedited(struct task_struct *t);

#endif /* _ASM_POWERPC_MEMBARRIER_H */
