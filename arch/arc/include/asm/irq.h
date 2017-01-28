/*
 * Copyright (C) 2004, 2007-2010, 2011-2012 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ASM_ARC_IRQ_H
#define __ASM_ARC_IRQ_H

#ifdef CONFIG_ISA_ARCV2

/*
 * A maximum number of supported interrupts in the core interrupt controller.
 * This number is not equal to the maximum interrupt number (256) because
 * first 16 lines are reserved for exceptions and are not configurable.
 */
#define NR_CPU_IRQS	240

/* A fixed number of exceptions which occupy first interrupt lines */
#define NR_EXCEPTIONS	16

/*
 * ARCv2 can support 240 interrupts in the core interrupts controllers and
 * 128 interrupts in IDU. Thus 512 virtual IRQs must be enough for most
 * configurations of boards.
 */
#define NR_IRQS		512

/* Platform Independent IRQs */
#define IPI_IRQ		19
#define SOFTIRQ_IRQ	21
#define FIRST_EXT_IRQ	24

#else

#define NR_CPU_IRQS	32  /* number of interrupt lines of ARC770 CPU */
#define NR_IRQS		128 /* allow some CPU external IRQ handling */

#endif

#ifndef __ASSEMBLY__

#include <linux/interrupt.h>
#include <asm-generic/irq.h>

extern void arc_init_IRQ(void);

#endif

#endif
