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
#include <asm/cpufeature.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>


/*
 * When ICC_PMR_EL1 is used for interrupt masking, only the bit indicating
 * whether the normal interrupts are masked is kept along with the daif
 * flags.
 */
#define ARCH_FLAG_PMR_EN 0x1

#define MAKE_ARCH_FLAGS(daif, pmr)					\
	((daif) | (((pmr) >> ICC_PMR_EL1_EN_SHIFT) & ARCH_FLAG_PMR_EN))

#define ARCH_FLAGS_GET_PMR(flags)				\
	((((flags) & ARCH_FLAG_PMR_EN) << ICC_PMR_EL1_EN_SHIFT) \
		| ICC_PMR_EL1_MASKED)

#define ARCH_FLAGS_GET_DAIF(flags) ((flags) & ~ARCH_FLAG_PMR_EN)

/*
 * Aarch64 has flags for masking: Debug, Asynchronous (serror), Interrupts and
 * FIQ exceptions, in the 'daif' register. We mask and unmask them in 'dai'
 * order:
 * Masking debug exceptions causes all other exceptions to be masked too/
 * Masking SError masks irq, but not debug exceptions. Masking irqs has no
 * side effects for other flags. Keeping to this order makes it easier for
 * entry.S to know which exceptions should be unmasked.
 *
 * FIQ is never expected, but we mask it when we disable debug exceptions, and
 * unmask it at all other times.
 */

/*
 * CPU interrupt mask handling.
 */
static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags, masked = ICC_PMR_EL1_MASKED;
	unsigned long pmr = 0;

	asm volatile(ALTERNATIVE(
		"mrs	%0, daif		// arch_local_irq_save\n"
		"msr	daifset, #2\n"
		"mov	%1, #" __stringify(ICC_PMR_EL1_UNMASKED),
		/* --- */
		"mrs	%0, daif\n"
		"mrs_s  %1, " __stringify(SYS_ICC_PMR_EL1) "\n"
		"msr_s	" __stringify(SYS_ICC_PMR_EL1) ", %2",
		ARM64_HAS_IRQ_PRIO_MASKING)
		: "=&r" (flags), "=&r" (pmr)
		: "r" (masked)
		: "memory");

	return MAKE_ARCH_FLAGS(flags, pmr);
}

static inline void arch_local_irq_enable(void)
{
	unsigned long unmasked = ICC_PMR_EL1_UNMASKED;

	asm volatile(ALTERNATIVE(
		"msr	daifclr, #2		// arch_local_irq_enable\n"
		"nop",
		"msr_s  " __stringify(SYS_ICC_PMR_EL1) ",%0\n"
		"dsb	sy",
		ARM64_HAS_IRQ_PRIO_MASKING)
		:
		: "r" (unmasked)
		: "memory");
}

static inline void arch_local_irq_disable(void)
{
	unsigned long masked = ICC_PMR_EL1_MASKED;

	asm volatile(ALTERNATIVE(
		"msr	daifset, #2		// arch_local_irq_disable",
		"msr_s  " __stringify(SYS_ICC_PMR_EL1) ",%0",
		ARM64_HAS_IRQ_PRIO_MASKING)
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
	unsigned long pmr = 0;

	asm volatile(ALTERNATIVE(
		"mrs	%0, daif		// arch_local_save_flags\n"
		"mov	%1, #" __stringify(ICC_PMR_EL1_UNMASKED),
		"mrs	%0, daif\n"
		"mrs_s  %1, " __stringify(SYS_ICC_PMR_EL1),
		ARM64_HAS_IRQ_PRIO_MASKING)
		: "=r" (flags), "=r" (pmr)
		:
		: "memory");

	return MAKE_ARCH_FLAGS(flags, pmr);
}

/*
 * restore saved IRQ state
 */
static inline void arch_local_irq_restore(unsigned long flags)
{
	unsigned long pmr = ARCH_FLAGS_GET_PMR(flags);

	flags = ARCH_FLAGS_GET_DAIF(flags);

	asm volatile(ALTERNATIVE(
		"msr	daif, %0		// arch_local_irq_restore\n"
		"nop\n"
		"nop",
		"msr	daif, %0\n"
		"msr_s  " __stringify(SYS_ICC_PMR_EL1) ",%1\n"
		"dsb	sy",
		ARM64_HAS_IRQ_PRIO_MASKING)
	:
	: "r" (flags), "r" (pmr)
	: "memory");
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return (ARCH_FLAGS_GET_DAIF(flags) & (PSR_I_BIT)) |
		!(ARCH_FLAGS_GET_PMR(flags) & ICC_PMR_EL1_EN_BIT);
}

void maybe_switch_to_sysreg_gic_cpuif(void);

#endif
#endif
