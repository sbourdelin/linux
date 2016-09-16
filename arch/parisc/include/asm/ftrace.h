#ifndef _ASM_PARISC_FTRACE_H
#define _ASM_PARISC_FTRACE_H

#ifndef __ASSEMBLY__
extern void mcount(void);

#define MCOUNT_INSN_SIZE 4

extern unsigned long sys_call_table[];

extern unsigned long return_address(unsigned int);

#define ftrace_return_address(n) return_address(n)

#if defined(CONFIG_FTRACE_SYSCALLS) && defined(CONFIG_COMPAT)
#include <linux/compat.h>

#define ARCH_COMPAT_SYSCALL_NUMBERS_OVERLAP 1
static inline bool arch_trace_is_compat_syscall(struct pt_regs *regs)
{
	return in_compat_syscall();
}
#endif /* CONFIG_FTRACE_SYSCALLS && CONFIG_COMPAT */

#endif /* __ASSEMBLY__ */

#endif /* _ASM_PARISC_FTRACE_H */
