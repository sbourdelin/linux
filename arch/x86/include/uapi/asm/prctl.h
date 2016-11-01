#ifndef _ASM_X86_PRCTL_H
#define _ASM_X86_PRCTL_H

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/* Get/set the process' ability to use the CPUID instruction */
#define ARCH_GET_CPUID 0x1005
#define ARCH_SET_CPUID 0x1006
# define ARCH_CPUID_ENABLE		1	/* allow the use of the CPUID instruction */
# define ARCH_CPUID_SIGSEGV		2	/* throw a SIGSEGV instead of reading the CPUID */

#ifdef CONFIG_CHECKPOINT_RESTORE
# define ARCH_MAP_VDSO_X32	0x2001
# define ARCH_MAP_VDSO_32	0x2002
# define ARCH_MAP_VDSO_64	0x2003
#endif

#endif /* _ASM_X86_PRCTL_H */
