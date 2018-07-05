/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Contains the definition of registers common to all PowerPC variants.
 * If a register definition has been changed in a different PowerPC
 * variant, we will case it in #ifndef XXX ... #endif, and have the
 * number used in the Programming Environments Manual For 32-Bit
 * Implementations of the PowerPC Architecture (a.k.a. Green Book) here.
 */

#ifndef _ASM_POWERPC_REG_FTR_H
#define _ASM_POWERPC_REG_FTR_H
#ifdef __KERNEL__

#include <linux/stringify.h>
#include <asm/cputable.h>
#include <asm/feature-fixups.h>
#include <asm/reg.h>

#ifdef CONFIG_PPC_BOOK3S_64

#define GET_PACA(rX)					\
	BEGIN_FTR_SECTION_NESTED(66);			\
	mfspr	rX,SPRN_SPRG_PACA;			\
	FTR_SECTION_ELSE_NESTED(66);			\
	mfspr	rX,SPRN_SPRG_HPACA;			\
	ALT_FTR_SECTION_END_NESTED_IFCLR(CPU_FTR_HVMODE, 66)

#define SET_PACA(rX)					\
	BEGIN_FTR_SECTION_NESTED(66);			\
	mtspr	SPRN_SPRG_PACA,rX;			\
	FTR_SECTION_ELSE_NESTED(66);			\
	mtspr	SPRN_SPRG_HPACA,rX;			\
	ALT_FTR_SECTION_END_NESTED_IFCLR(CPU_FTR_HVMODE, 66)

#define GET_SCRATCH0(rX)				\
	BEGIN_FTR_SECTION_NESTED(66);			\
	mfspr	rX,SPRN_SPRG_SCRATCH0;			\
	FTR_SECTION_ELSE_NESTED(66);			\
	mfspr	rX,SPRN_SPRG_HSCRATCH0;			\
	ALT_FTR_SECTION_END_NESTED_IFCLR(CPU_FTR_HVMODE, 66)

#define SET_SCRATCH0(rX)				\
	BEGIN_FTR_SECTION_NESTED(66);			\
	mtspr	SPRN_SPRG_SCRATCH0,rX;			\
	FTR_SECTION_ELSE_NESTED(66);			\
	mtspr	SPRN_SPRG_HSCRATCH0,rX;			\
	ALT_FTR_SECTION_END_NESTED_IFCLR(CPU_FTR_HVMODE, 66)

#else /* CONFIG_PPC_BOOK3S_64 */
#define GET_SCRATCH0(rX)	mfspr	rX,SPRN_SPRG_SCRATCH0
#define SET_SCRATCH0(rX)	mtspr	SPRN_SPRG_SCRATCH0,rX

#endif

#ifdef CONFIG_PPC_BOOK3E_64

#define SET_PACA(rX)	mtspr	SPRN_SPRG_PACA,rX
#define GET_PACA(rX)	mfspr	rX,SPRN_SPRG_PACA

#endif

#ifndef __ASSEMBLY__
static inline void mtmsr_isync(unsigned long val)
{
	asm volatile(__MTMSR " %0; " ASM_FTR_IFCLR("isync", "nop", %1) : :
			"r" (val), "i" (CPU_FTR_ARCH_206) : "memory");
}
#endif

#endif /* __KERNEL__ */
#endif /* _ASM_POWERPC_REG_FTR_H */
