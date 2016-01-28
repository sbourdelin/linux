#ifndef __ASM_AVR32_KDEBUG_H
#define __ASM_AVR32_KDEBUG_H

static inline void arch_breakpoint(void)
{
}

/* Grossly misnamed. */
enum die_val {
	DIE_BREAKPOINT,
	DIE_SSTEP,
	DIE_NMI,
	DIE_OOPS,
};

#endif /* __ASM_AVR32_KDEBUG_H */
