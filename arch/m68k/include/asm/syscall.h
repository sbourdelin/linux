/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_M68K_SYSCALL_H
#define _ASM_M68K_SYSCALL_H

#include <asm/ptrace.h>
#include <asm-generic/syscall.h>
#include <linux/err.h>
#include <uapi/linux/audit.h>

static inline int
syscall_get_nr(struct task_struct *task, struct pt_regs *regs)
{
	return regs->orig_d0;
}

static inline void
__syscall_get_arguments(struct task_struct *task, struct pt_regs *regs,
			unsigned int i, unsigned int n, unsigned long *args)
{
	BUILD_BUG_ON(sizeof(regs->d1) != sizeof(args[0]));
	memcpy(args, &regs->d1 + i, n * sizeof(args[0]));
}

static inline long
syscall_get_error(struct task_struct *task, struct pt_regs *regs)
{
	return IS_ERR_VALUE(regs->d0) ? regs->d0 : 0;
}

static inline long
syscall_get_return_value(struct task_struct *task, struct pt_regs *regs)
{
	return regs->d0;
}

static inline int
syscall_get_arch(struct task_struct *task)
{
	return AUDIT_ARCH_M68K;
}

#endif	/* _ASM_M68K_SYSCALL_H */
