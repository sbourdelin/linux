/*
 * Restartable Sequences: x86 ABI.
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
 *
 * Copyright (C) 2015, Google, Inc.,
 * Paul Turner <pjt@google.com> and Andrew Hunter <ahh@google.com>
 *
 */

#include <linux/sched.h>
#include <linux/uaccess.h>
#include <asm/restartable_sequences.h>
#include <asm/restartable_sequences.h>

void arch_rseq_check_critical_section(struct task_struct *p,
				      struct pt_regs *regs)
{
	unsigned long ip = arch_rseq_in_crit_section(p, regs);

	if (!ip)
		return;

	/* RSEQ only applies to user-mode execution */
	BUG_ON(!user_mode(regs));

	regs->ip = ip;
}

void arch_rseq_handle_notify_resume(struct pt_regs *regs)
{
	struct restartable_sequence_state *rseq_state = &current->rseq_state;

	/* If this update fails our user-state is incoherent. */
	if (put_user(task_cpu(current), rseq_state->cpu_pointer))
		force_sig(SIGSEGV, current);

	arch_rseq_check_critical_section(current, regs);
}
