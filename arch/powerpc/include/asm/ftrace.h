#ifndef _ASM_POWERPC_FTRACE
#define _ASM_POWERPC_FTRACE

#include <asm/types.h>

#ifdef CONFIG_FUNCTION_TRACER
#define MCOUNT_ADDR		((unsigned long)(_mcount))
#define MCOUNT_INSN_SIZE	4 /* sizeof mcount call */

#ifdef __ASSEMBLY__

/* Based off of objdump optput from glibc */

#define MCOUNT_SAVE_FRAME			\
	stwu	r1,-48(r1);			\
	stw	r3, 12(r1);			\
	stw	r4, 16(r1);			\
	stw	r5, 20(r1);			\
	stw	r6, 24(r1);			\
	mflr	r3;				\
	lwz	r4, 52(r1);			\
	mfcr	r5;				\
	stw	r7, 28(r1);			\
	stw	r8, 32(r1);			\
	stw	r9, 36(r1);			\
	stw	r10,40(r1);			\
	stw	r3, 44(r1);			\
	stw	r5, 8(r1)

#define MCOUNT_RESTORE_FRAME			\
	lwz	r6, 8(r1);			\
	lwz	r0, 44(r1);			\
	lwz	r3, 12(r1);			\
	mtctr	r0;				\
	lwz	r4, 16(r1);			\
	mtcr	r6;				\
	lwz	r5, 20(r1);			\
	lwz	r6, 24(r1);			\
	lwz	r0, 52(r1);			\
	lwz	r7, 28(r1);			\
	lwz	r8, 32(r1);			\
	mtlr	r0;				\
	lwz	r9, 36(r1);			\
	lwz	r10,40(r1);			\
	addi	r1, r1, 48

#else /* !__ASSEMBLY__ */
extern void _mcount(void);

#ifdef CONFIG_DYNAMIC_FTRACE
# define FTRACE_ADDR ((unsigned long)ftrace_caller)
# define FTRACE_REGS_ADDR FTRACE_ADDR
static inline unsigned long ftrace_call_adjust(unsigned long addr)
{
       /* reloction of mcount call site is the same as the address */
       return addr;
}

struct dyn_arch_ftrace {
	struct module *mod;
};
#endif /*  CONFIG_DYNAMIC_FTRACE */
#endif /* __ASSEMBLY__ */

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_REGS
#define ARCH_SUPPORTS_FTRACE_OPS 1
#endif
#endif

#if defined(CONFIG_FTRACE_SYSCALLS) && !defined(__ASSEMBLY__)
#ifdef PPC64_ELF_ABI_v1
#define ARCH_HAS_SYSCALL_MATCH_SYM_NAME
static inline bool arch_syscall_match_sym_name(const char *sym, const char *name)
{
	/*
	 * Compare the symbol name with the system call name. Skip the .sys,
	 * .SyS or .compat_sys prefix from the symbol name and the sys prefix
	 * from the system call name and just match the rest. This is only
	 * needed on ppc64 since symbol names on 32bit do not start with a
	 * period so the generic function will work.
	 */
	int prefix_len = 3;

	if (!strncasecmp(name, "compat_", 7))
		prefix_len = 10;
	return !strcmp(sym + prefix_len + 1, name + prefix_len);
}
#endif

#if defined(CONFIG_COMPAT)
#include <linux/compat.h>
#include <asm/compat.h>

#define ARCH_COMPAT_SYSCALL_NUMBERS_OVERLAP 1
static inline bool arch_trace_is_compat_syscall(struct pt_regs *regs)
{
	return in_compat_syscall();
}
#endif /* CONFIG_COMPAT */
#endif /* CONFIG_FTRACE_SYSCALLS && !__ASSEMBLY__ */

#endif /* _ASM_POWERPC_FTRACE */
