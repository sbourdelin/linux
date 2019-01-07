/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_UNICORE_SYSCALL_H
#define _ASM_UNICORE_SYSCALL_H

#include <linux/err.h>
#include <asm/ptrace.h>
#include <asm-generic/syscall.h>
#include <uapi/linux/audit.h>

static inline int
syscall_get_nr(struct task_struct *task, struct pt_regs *regs)
{
	return task_thread_info(task)->syscall;
}

static inline void
__syscall_get_arguments(struct task_struct *task, struct pt_regs *regs,
			unsigned int i, unsigned int n, unsigned long *args)
{
	if (i == 0) {
		args[0] = regs->UCreg_ORIG_00;
		args++;
		i++;
		n--;
	}
	memcpy(args, &regs->UCreg_00 + i, n * sizeof(args[0]));
}

static inline long
syscall_get_error(struct task_struct *task, struct pt_regs *regs)
{
	return IS_ERR_VALUE(regs->UCreg_00) ? regs->UCreg_00 : 0;
}

static inline long
syscall_get_return_value(struct task_struct *task, struct pt_regs *regs)
{
	return regs->UCreg_00;
}

static inline int
syscall_get_arch(void)
{
	return AUDIT_ARCH_UNICORE;
}

#endif	/* _ASM_UNICORE_SYSCALL_H */
