/*
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_IRQFLAGS_H
#define __ASM_IRQFLAGS_H

#ifdef __KERNEL__

#include <asm/alternative.h>
#include <asm/arch_gicv3.h>
#include <asm/cpufeature.h>
#include <asm/ptrace.h>

#ifndef CONFIG_USE_ICC_SYSREGS_FOR_IRQFLAGS

/*
 * CPU interrupt mask handling.
 */
static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags;
	asm volatile(
		"mrs	%0, daif		// arch_local_irq_save\n"
		"msr	daifset, #2"
		: "=r" (flags)
		:
		: "memory");
	return flags;
}

static inline void arch_local_irq_enable(void)
{
	asm volatile(
		"msr	daifclr, #2		// arch_local_irq_enable"
		:
		:
		: "memory");
}

static inline void arch_local_irq_disable(void)
{
	asm volatile(
		"msr	daifset, #2		// arch_local_irq_disable"
		:
		:
		: "memory");
}

/*
 * Save the current interrupt enable state.
 */
static inline unsigned long arch_local_save_flags(void)
{
	unsigned long flags;
	asm volatile(
		"mrs	%0, daif		// arch_local_save_flags"
		: "=r" (flags)
		:
		: "memory");
	return flags;
}

/*
 * restore saved IRQ state
 */
static inline void arch_local_irq_restore(unsigned long flags)
{
	asm volatile(
		"msr	daif, %0		// arch_local_irq_restore"
	:
	: "r" (flags)
	: "memory");
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return flags & PSR_I_BIT;
}

static inline void maybe_switch_to_sysreg_gic_cpuif(void) {}

#else /* CONFIG_IRQFLAGS_GIC_MASKING */

/*
 * CPU interrupt mask handling.
 */
static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags, masked = ICC_PMR_EL1_MASKED;

	asm volatile(ALTERNATIVE(
		"mrs	%0, daif		// arch_local_irq_save\n"
		"msr	daifset, #2",
		/* --- */
		"mrs_s  %0, " __stringify(ICC_PMR_EL1) "\n"
		"msr_s	" __stringify(ICC_PMR_EL1) ",%1",
		ARM64_HAS_SYSREG_GIC_CPUIF)
		: "=&r" (flags)
		: "r" (masked)
		: "memory");

	return flags;
}

static inline void arch_local_irq_enable(void)
{
	unsigned long unmasked = ICC_PMR_EL1_UNMASKED;

	asm volatile(ALTERNATIVE(
		"msr	daifclr, #2		// arch_local_irq_enable",
		"msr_s  " __stringify(ICC_PMR_EL1) ",%0",
		ARM64_HAS_SYSREG_GIC_CPUIF)
		:
		: "r" (unmasked)
		: "memory");
}

static inline void arch_local_irq_disable(void)
{
	unsigned long masked = ICC_PMR_EL1_MASKED;

	asm volatile(ALTERNATIVE(
		"msr	daifset, #2		// arch_local_irq_disable",
		"msr_s  " __stringify(ICC_PMR_EL1) ",%0",
		ARM64_HAS_SYSREG_GIC_CPUIF)
		:
		: "r" (masked)
		: "memory");
}

/*
 * Save the current interrupt enable state.
 */
static inline unsigned long arch_local_save_flags(void)
{
	unsigned long flags;

	asm volatile(ALTERNATIVE(
		"mrs	%0, daif		// arch_local_save_flags",
		"mrs_s  %0, " __stringify(ICC_PMR_EL1),
		ARM64_HAS_SYSREG_GIC_CPUIF)
		: "=r" (flags)
		:
		: "memory");

	return flags;
}

/*
 * restore saved IRQ state
 */
static inline void arch_local_irq_restore(unsigned long flags)
{
	asm volatile(ALTERNATIVE(
		"msr	daif, %0		// arch_local_irq_restore",
		"msr_s  " __stringify(ICC_PMR_EL1) ",%0",
		ARM64_HAS_SYSREG_GIC_CPUIF)
	:
	: "r" (flags)
	: "memory");
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	asm volatile(ALTERNATIVE(
		"and	%0, %0, #" __stringify(PSR_I_BIT) "\n"
		"nop",
		/* --- */
		"and	%0, %0, # " __stringify(ICC_PMR_EL1_G_BIT) "\n"
		"eor	%0, %0, # " __stringify(ICC_PMR_EL1_G_BIT),
		ARM64_HAS_SYSREG_GIC_CPUIF)
		: "+r" (flags));

	return flags;
}

void maybe_switch_to_sysreg_gic_cpuif(void);

#endif /* CONFIG_IRQFLAGS_GIC_MASKING */

#define local_fiq_enable()	asm("msr	daifclr, #1" : : : "memory")
#define local_fiq_disable()	asm("msr	daifset, #1" : : : "memory")

#define local_async_enable()	asm("msr	daifclr, #4" : : : "memory")
#define local_async_disable()	asm("msr	daifset, #4" : : : "memory")

/*
 * save and restore debug state
 */
#define local_dbg_save(flags)						\
	do {								\
		typecheck(unsigned long, flags);			\
		asm volatile(						\
		"mrs    %0, daif		// local_dbg_save\n"	\
		"msr    daifset, #8"					\
		: "=r" (flags) : : "memory");				\
	} while (0)

#define local_dbg_restore(flags)					\
	do {								\
		typecheck(unsigned long, flags);			\
		asm volatile(						\
		"msr    daif, %0		// local_dbg_restore\n"	\
		: : "r" (flags) : "memory");				\
	} while (0)

#endif
#endif
