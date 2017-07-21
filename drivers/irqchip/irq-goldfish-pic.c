/*
 * Copyright (C) 2017 Imagination Technologies Ltd.	All rights reserved
 *	Author: Miodrag Dinic <miodrag.dinic@imgtec.com>
 *
 * This file implements interrupt controller driver for MIPS Goldfish PIC.
 *
 * This program is free software; you can redistribute	it and/or modify it
 * under  the terms of	the GNU General	 Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include <asm/setup.h>

/* 0..7 MIPS CPU interrupts */
#define GF_CPU_IRQ_PIC		(MIPS_CPU_IRQ_BASE + 2)
#define GF_CPU_IRQ_COMPARE	(MIPS_CPU_IRQ_BASE + 7)

#define GF_NR_IRQS		40
/* 8..39 Cascaded Goldfish PIC interrupts */
#define GF_IRQ_OFFSET		8

#define GF_PIC_NUMBER		0x04
#define GF_PIC_DISABLE_ALL	0x08
#define GF_PIC_DISABLE		0x0c
#define GF_PIC_ENABLE		0x10

static struct irq_domain *irq_domain;
static void __iomem *gf_pic_base;

static inline void unmask_goldfish_irq(struct irq_data *d)
{
	writel(d->hwirq - GF_IRQ_OFFSET,
		gf_pic_base + GF_PIC_ENABLE);
	irq_enable_hazard();
}

static inline void mask_goldfish_irq(struct irq_data *d)
{
	writel(d->hwirq - GF_IRQ_OFFSET,
		gf_pic_base + GF_PIC_DISABLE);
	irq_disable_hazard();
}

static struct irq_chip goldfish_irq_controller = {
	.name		= "Goldfish PIC",
	.irq_ack	= mask_goldfish_irq,
	.irq_mask	= mask_goldfish_irq,
	.irq_mask_ack	= mask_goldfish_irq,
	.irq_unmask	= unmask_goldfish_irq,
	.irq_eoi	= unmask_goldfish_irq,
	.irq_disable	= mask_goldfish_irq,
	.irq_enable	= unmask_goldfish_irq,
};

static void goldfish_irq_dispatch(void)
{
	uint32_t irq;
	uint32_t virq;

	irq = readl(gf_pic_base + GF_PIC_NUMBER);
	if (irq == 0) {
		/* Timer interrupt */
		do_IRQ(GF_CPU_IRQ_COMPARE);
		return;
	}

	virq = irq_linear_revmap(irq_domain, irq);
	virq += GF_IRQ_OFFSET;
	do_IRQ(virq);
}

static void goldfish_ip2_irq_dispatch(struct irq_desc *desc)
{
	unsigned long pending = read_c0_cause() & read_c0_status() & ST0_IM;

	if (pending & CAUSEF_IP2)
		goldfish_irq_dispatch();
	else
		spurious_interrupt();
}

static int goldfish_pic_map(struct irq_domain *d, unsigned int irq,
			    irq_hw_number_t hw)
{
	if (cpu_has_vint)
		set_vi_handler(hw, goldfish_irq_dispatch);

	irq_set_chip_and_handler(irq, &goldfish_irq_controller,
				 handle_level_irq);

	return 0;
}

static const struct irq_domain_ops gf_pic_irq_domain_ops = {
	.map = goldfish_pic_map,
	.xlate = irq_domain_xlate_onetwocell,
};

static struct irqaction cascade = {
	.handler	= no_action,
	.flags		= IRQF_PROBE_SHARED,
	.name		= "cascade",
};

static void __init __goldfish_pic_init(struct device_node *of_node)
{
	gf_pic_base = of_iomap(of_node, 0);
	if (!gf_pic_base)
		panic("Failed to map Goldfish PIC base : No such device!");

	/* Mask interrupts. */
	writel(1, gf_pic_base + GF_PIC_DISABLE_ALL);

	if (!cpu_has_vint)
		irq_set_chained_handler(GF_CPU_IRQ_PIC,
					goldfish_ip2_irq_dispatch);

	setup_irq(GF_CPU_IRQ_PIC, &cascade);

	irq_domain = irq_domain_add_legacy(of_node, GF_NR_IRQS,
		GF_IRQ_OFFSET, 0, &gf_pic_irq_domain_ops, NULL);
	if (!irq_domain)
		panic("Failed to add irqdomain for Goldfish PIC");
}

int __init goldfish_pic_of_init(struct device_node *of_node,
				struct device_node *parent)
{
	__goldfish_pic_init(of_node);
	return 0;
}
IRQCHIP_DECLARE(google_gf_pic, "google,goldfish-pic", goldfish_pic_of_init);
