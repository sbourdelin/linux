#ifndef _ASM_X86_UMIP_H
#define _ASM_X86_UMIP_H

#include <linux/types.h>
#include <asm/ptrace.h>
#include <asm/insn.h>

#ifdef CONFIG_X86_INTEL_UMIP
int fixup_umip_exception(struct pt_regs *regs);
#else
static inline int fixup_umip_exception(struct pt_regs *regs)
{
	return -EINVAL;
}
#endif  /* CONFIG_X86_INTEL_UMIP */
#endif  /* _ASM_X86_UMIP_H */
