/*
 * include/linux/irqchip/arm-gic-common.h
 *
 * Copyright (C) 2016 ARM Limited, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __LINUX_IRQCHIP_ARM_GIC_COMMON_H
#define __LINUX_IRQCHIP_ARM_GIC_COMMON_H

#include <linux/types.h>
#include <linux/ioport.h>

#define GIC_FIRST_SGI_IRQ	0
#define GIC_LAST_SGI_IRQ	15
#define GIC_NR_SGI		(GIC_LAST_SGI_IRQ - GIC_FIRST_SGI_IRQ + 1)
#define GIC_FIRST_PPI_IRQ	16
#define GIC_LAST_PPI_IRQ	31
#define GIC_NR_PPI		(GIC_LAST_PPI_IRQ - GIC_FIRST_PPI_IRQ + 1)
#define GIC_FIRST_SPI_IRQ	32
#define GIC_LAST_SPI_IRQ	1019
#define GIC_MAX_IRQ		1020
#define GIC_FIRST_SPECIAL_IRQ	1020
#define GIC_SPURIOUS_IRQ	1023

enum gic_type {
	GIC_V2,
	GIC_V3,
};

struct gic_kvm_info {
	/* GIC type */
	enum gic_type	type;
	/* Virtual CPU interface */
	struct resource vcpu;
	/* Interrupt number */
	unsigned int	maint_irq;
	/* Virtual control interface */
	struct resource vctrl;
};

const struct gic_kvm_info *gic_get_kvm_info(void);

#endif /* __LINUX_IRQCHIP_ARM_GIC_COMMON_H */
