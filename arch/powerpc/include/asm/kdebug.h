#ifndef _ASM_POWERPC_KDEBUG_H
#define _ASM_POWERPC_KDEBUG_H
#ifdef __KERNEL__

/* Grossly misnamed. */
enum die_val {
	DIE_OOPS = 1,
	DIE_IABR_MATCH,
	DIE_DABR_MATCH,
	DIE_BPT,
	DIE_SSTEP,
};

static inline void arch_breakpoint(void)
{
	asm(".long 0x7d821008"); /* twge r2, r2 */
}

#endif /* __KERNEL__ */
#endif /* _ASM_POWERPC_KDEBUG_H */
