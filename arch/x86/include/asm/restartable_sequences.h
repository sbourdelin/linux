#ifndef _ASM_X86_RESTARTABLE_SEQUENCES_H
#define _ASM_X86_RESTARTABLE_SEQUENCES_H

#include <asm/processor.h>
#include <asm/ptrace.h>
#include <linux/sched.h>

#ifdef CONFIG_RESTARTABLE_SEQUENCES

static inline unsigned long arch_rseq_in_crit_section(struct task_struct *p,
						struct pt_regs *regs)
{
	unsigned long ip = (unsigned long)regs->ip;

	return rseq_lookup(p, ip);
}

static inline bool arch_rseq_needs_notify_resume(struct task_struct *p)
{
#ifdef CONFIG_PREEMPT
	/*
	 * Under CONFIG_PREEMPT it's possible for regs to be incoherent in the
	 * case that we took an interrupt during syscall entry.  Avoid this by
	 * always deferring to our notify-resume handler.
	 */
	return true;
#else
	return arch_rseq_in_crit_section(p, task_pt_regs(p));
#endif
}

void arch_rseq_handle_notify_resume(struct pt_regs *regs);
void arch_rseq_check_critical_section(struct task_struct *p,
				      struct pt_regs *regs);

#else /* !CONFIG_RESTARTABLE_SEQUENCES */

static inline void arch_rseq_handle_notify_resume(struct pt_regs *regs) {}
static inline void arch_rseq_check_critical_section(struct task_struct *p,
						    struct pt_regs *regs) {}

#endif

#endif /* _ASM_X86_RESTARTABLE_SEQUENCES_H */
