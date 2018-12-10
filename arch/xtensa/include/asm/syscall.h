/*
 * include/asm-xtensa/syscall.h
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2001 - 2007 Tensilica Inc.
 */

#include <uapi/linux/audit.h>

static inline int
syscall_get_nr(struct task_struct *task, struct pt_regs *regs)
{
	return regs->syscall;
}

static inline void
syscall_get_arguments(struct task_struct *task, struct pt_regs *regs,
		      unsigned int i, unsigned int n, unsigned long *args)
{
	switch (i) {
	case 0:
		if (!n--)
			break;
		*args++ = regs->areg[6];
		/* fall through */
	case 1:
		if (!n--)
			break;
		*args++ = regs->areg[3];
		/* fall through */
	case 2:
		if (!n--)
			break;
		*args++ = regs->areg[4];
		/* fall through */
	case 3:
		if (!n--)
			break;
		*args++ = regs->areg[5];
		/* fall through */
	case 4:
		if (!n--)
			break;
		*args++ = regs->areg[8];
		/* fall through */
	case 5:
		if (!n--)
			break;
		*args++ = regs->areg[9];
		/* fall through */
	case 6:
		if (!n--)
			break;
		/* fall through */
	default:
		BUG();
	}
}

static inline long
syscall_get_error(struct task_struct *task, struct pt_regs *regs)
{
	return IS_ERR_VALUE(regs->areg[2]) ? regs->areg[2] : 0;

static inline long
syscall_get_return_value(struct task_struct *task, struct pt_regs *regs)
{
	return regs->areg[2];
}

static inline int
syscall_get_arch(struct task_struct *task)
{
	return AUDIT_ARCH_XTENSA;
}

struct pt_regs;
asmlinkage long xtensa_ptrace(long, long, long, long);
asmlinkage long xtensa_sigreturn(struct pt_regs*);
asmlinkage long xtensa_rt_sigreturn(struct pt_regs*);
asmlinkage long xtensa_shmat(int, char __user *, int);
asmlinkage long xtensa_fadvise64_64(int, int,
				    unsigned long long, unsigned long long);

/* Should probably move to linux/syscalls.h */
struct pollfd;
asmlinkage long sys_pselect6(int n, fd_set __user *inp, fd_set __user *outp,
			     fd_set __user *exp, struct timespec __user *tsp,
			     void __user *sig);
asmlinkage long sys_ppoll(struct pollfd __user *ufds, unsigned int nfds,
			  struct timespec __user *tsp,
			  const sigset_t __user *sigmask,
			  size_t sigsetsize);
