/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_SYNCH_H 
#define _ASM_POWERPC_SYNCH_H 
#ifdef __KERNEL__

#ifndef __ASSEMBLY__
extern unsigned int __start___lwsync_fixup, __stop___lwsync_fixup;
extern void do_lwsync_fixups(unsigned long value, void *fixup_start,
			     void *fixup_end);

static inline void eieio(void)
{
	__asm__ __volatile__ ("eieio" : : : "memory");
}

static inline void isync(void)
{
	__asm__ __volatile__ ("isync" : : : "memory");
}
#endif /* __ASSEMBLY__ */

#endif /* __KERNEL__ */
#endif	/* _ASM_POWERPC_SYNCH_H */
