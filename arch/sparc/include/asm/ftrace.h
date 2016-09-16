#ifndef _ASM_SPARC64_FTRACE
#define _ASM_SPARC64_FTRACE

#ifdef CONFIG_MCOUNT
#define MCOUNT_ADDR		((unsigned long)(_mcount))
#define MCOUNT_INSN_SIZE	4 /* sizeof mcount call */

#ifndef __ASSEMBLY__

#if defined(CONFIG_FTRACE_SYSCALLS) && defined(CONFIG_COMPAT)
#include <asm/compat.h>

#define ARCH_COMPAT_SYSCALL_NUMBERS_OVERLAP 1
static inline bool arch_trace_is_compat_syscall(struct pt_regs *regs)
{
	return in_compat_syscall();
}
#endif /* CONFIG_FTRACE_SYSCALLS && CONFIG_COMPAT */
void _mcount(void);
#endif

#endif

#ifdef CONFIG_DYNAMIC_FTRACE
/* reloction of mcount call site is the same as the address */
static inline unsigned long ftrace_call_adjust(unsigned long addr)
{
	return addr;
}

struct dyn_arch_ftrace {
};
#endif /*  CONFIG_DYNAMIC_FTRACE */

unsigned long prepare_ftrace_return(unsigned long parent,
				    unsigned long self_addr,
				    unsigned long frame_pointer);

#endif /* _ASM_SPARC64_FTRACE */
