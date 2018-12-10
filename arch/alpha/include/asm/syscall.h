/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ALPHA_SYSCALL_H
#define _ASM_ALPHA_SYSCALL_H

#include <uapi/linux/audit.h>

static inline int
syscall_get_nr(struct task_struct *task, struct pt_regs *regs)
{
	return regs->r0;
}

static inline void
syscall_get_arguments(struct task_struct *task, struct pt_regs *regs,
		      unsigned int i, unsigned int n, unsigned long *args)
{
	BUG_ON(i + n > 6);
	memcpy(args, &regs->r16 + i, n * sizeof(args[0]));
}

static inline long
syscall_get_error(struct task_struct *task, struct pt_regs *regs)
{
	return regs->r19 ? -regs->r0 : 0;
}

static inline long
syscall_get_return_value(struct task_struct *task, struct pt_regs *regs)
{
	return regs->r0;
}

static inline int
syscall_get_arch(void)
{
	return AUDIT_ARCH_ALPHA;
}

#endif	/* _ASM_ALPHA_SYSCALL_H */
