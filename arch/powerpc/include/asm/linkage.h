#ifndef _ASM_POWERPC_LINKAGE_H
#define _ASM_POWERPC_LINKAGE_H

#include <asm/types.h>

#ifdef PPC64_ELF_ABI_v1
#define cond_syscall(x) \
	asm ("\t.weak " #x "\n\t.set " #x ", sys_ni_syscall\n"		\
	     "\t.weak ." #x "\n\t.set ." #x ", .sys_ni_syscall\n")
#define SYSCALL_ALIAS(alias, name)					\
	asm ("\t.globl " #alias "\n\t.set " #alias ", " #name "\n"	\
	     "\t.globl ." #alias "\n\t.set ." #alias ", ." #name)
#endif

#ifndef __ASSEMBLY__
/*
 * Helper macro for exception table entries
 */
#define EX_TABLE(_fault, _target)	\
	".section __ex_table,\"a\"\n"	\
		PPC_LONG_ALIGN "\n"	\
		PPC_LONG #_fault "\n"	\
		PPC_LONG #_target "\n"	\
	".previous\n"

#else /* __ASSEMBLY__ */

#define EX_TABLE(_fault, _target)	\
	.section __ex_table,"a"	;	\
		PPC_LONG_ALIGN ;	\
		PPC_LONG _fault	;	\
		PPC_LONG _target ;	\
	.previous

#endif /* __ASSEMBLY__ */

#endif	/* _ASM_POWERPC_LINKAGE_H */
