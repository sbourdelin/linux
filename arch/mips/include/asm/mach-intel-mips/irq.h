/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (C) 2014 Lei Chuanhua <Chuanhua.lei@lantiq.com>
 *  Copyright (C) 2018 Intel Corporation.
 */

#ifndef __INTEL_MIPS_IRQ_H
#define __INTEL_MIPS_IRQ_H

#define MIPS_CPU_IRQ_BASE	0
#define MIPS_GIC_IRQ_BASE	(MIPS_CPU_IRQ_BASE + 8)

#define NR_IRQS 256

#include_next <irq.h>

#endif /* __INTEL_MIPS_IRQ_H */
