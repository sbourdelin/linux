#ifndef _ASM_MIPS_KDEBUG_H
#define _ASM_MIPS_KDEBUG_H

#include <linux/notifier.h>

enum die_val {
	DIE_OOPS = 1,
	DIE_FP,
	DIE_TRAP,
	DIE_RI,
	DIE_PAGE_FAULT,
	DIE_BREAK,
	DIE_SSTEPBP,
	DIE_MSAFP,
	DIE_UPROBE,
	DIE_UPROBE_XOL,
};


void arch_breakpoint(void)
{
	__asm__ __volatile__(
		".globl breakinst\n\t"
		".set\tnoreorder\n\t"
		"nop\n"
		"breakinst:\tbreak\n\t"
		"nop\n\t"
		".set\treorder");
}

#endif /* _ASM_MIPS_KDEBUG_H */
