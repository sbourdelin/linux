/*
 * Copyright (C) 2017 Linaro Ltd. <ard.biesheuvel@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#ifndef __ASM_SIMD_H
#define __ASM_SIMD_H

#include <linux/compiler.h>
#include <linux/irqflags.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/types.h>

#ifdef CONFIG_KERNEL_MODE_NEON

DECLARE_PER_CPU(bool, kernel_neon_busy);

/*
 * may_use_simd - whether it is allowable at this time to issue SIMD
 *                instructions or access the SIMD register file
 *
 * Callers must not assume that the result remains true beyond the next
 * preempt_enable() or return from softirq context.
 */
static __must_check inline bool may_use_simd(void)
{
	/*
	 * Operations for contexts where we do not want to do any checks for
	 * preemptions.  Unless strictly necessary, always use this_cpu_*()
	 * instead.
	 */
	return !in_irq() && !irqs_disabled() && !in_nmi() &&
		!this_cpu_read(kernel_neon_busy);
}

#else /* ! CONFIG_KERNEL_MODE_NEON */

static __must_check inline bool may_use_simd(void) {
	return false;
}

#endif /* ! CONFIG_KERNEL_MODE_NEON */

#endif
