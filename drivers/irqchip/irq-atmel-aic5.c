/*
 * Atmel AT91 AIC5 (Advanced Interrupt Controller) driver
 *
 *  Copyright (C) 2004 SAN People
 *  Copyright (C) 2004 ATMEL
 *  Copyright (C) Rick Bronson
 *  Copyright (C) 2014 Free Electrons
 *
 *  Author: Boris BREZILLON <boris.brezillon@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/bitmap.h>
#include <linux/types.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irqdomain.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/io.h>

#include <asm/exception.h>
#include <asm/mach/irq.h>

#include "irq-atmel-aic-common.h"

/* Number of irq lines managed by AIC */
#define NR_AIC5_IRQS	128

#define AT91_AIC5_SSR		0x0
#define AT91_AIC5_INTSEL_MSK	(0x7f << 0)

#define AT91_AIC5_SMR			0x4

#define AT91_AIC5_SVR			0x8
#define AT91_AIC5_IVR			0x10
#define AT91_AIC5_FVR			0x14
#define AT91_AIC5_ISR			0x18

#define AT91_AIC5_IPR0			0x20
#define AT91_AIC5_IPR1			0x24
#define AT91_AIC5_IPR2			0x28
#define AT91_AIC5_IPR3			0x2c
#define AT91_AIC5_IMR			0x30
#define AT91_AIC5_CISR			0x34

#define AT91_AIC5_IECR			0x40
#define AT91_AIC5_IDCR			0x44
#define AT91_AIC5_ICCR			0x48
#define AT91_AIC5_ISCR			0x4c
#define AT91_AIC5_EOICR			0x38
#define AT91_AIC5_SPU			0x3c
#define AT91_AIC5_DCR			0x6c

#define AT91_AIC5_FFER			0x50
#define AT91_AIC5_FFDR			0x54
#define AT91_AIC5_FFSR			0x58

static struct irq_domain *aic5_domain;

static asmlinkage void __exception_irq_entry
aic5_handle(struct pt_regs *regs)
{
	struct irq_chip_generic *bgc = irq_get_domain_generic_chip(aic5_domain, 0);
	u32 irqnr;
	u32 irqstat;

	irqnr = irq_reg_readl(bgc, AT91_AIC5_IVR);
	irqstat = irq_reg_readl(bgc, AT91_AIC5_ISR);

	if (!irqstat)
		irq_reg_writel(bgc, 0, AT91_AIC5_EOICR);
	else
		handle_domain_irq(aic5_domain, irqnr, regs);
}

static void __init aic5_hw_init(struct irq_domain *domain)
{
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(domain, 0);
	int i;

	/*
	 * Perform 8 End Of Interrupt Command to make sure AIC
	 * will not Lock out nIRQ
	 */
	for (i = 0; i < 8; i++)
		irq_reg_writel(gc, 0, AT91_AIC5_EOICR);

	/*
	 * Spurious Interrupt ID in Spurious Vector Register.
	 * When there is no current interrupt, the IRQ Vector Register
	 * reads the value stored in AIC_SPU
	 */
	irq_reg_writel(gc, 0xffffffff, AT91_AIC5_SPU);

	/* No debugging in AIC: Debug (Protect) Control Register */
	irq_reg_writel(gc, 0, AT91_AIC5_DCR);

	/* Disable and clear all interrupts initially */
	for (i = 0; i < domain->revmap_size; i++) {
		irq_reg_writel(gc, i, AT91_AIC5_SSR);
		irq_reg_writel(gc, i, AT91_AIC5_SVR);
		irq_reg_writel(gc, 1, AT91_AIC5_IDCR);
		irq_reg_writel(gc, 1, AT91_AIC5_ICCR);
	}
}

static int __init aic5_of_init(struct device_node *node,
			       struct device_node *parent,
			       int nirqs)
{
	struct irq_chip_generic *gc;
	struct irq_domain *domain;
	int nchips;
	int i;

	if (nirqs > NR_AIC5_IRQS)
		return -EINVAL;

	if (aic5_domain)
		return -EEXIST;

	domain = aic_common_of_init(node, "atmel-aic5", nirqs);
	if (IS_ERR(domain))
		return PTR_ERR(domain);

	aic5_domain = domain;
	nchips = aic5_domain->revmap_size / AIC_IRQS_PER_CHIP;
	for (i = 0; i < nchips; i++) {
		gc = irq_get_domain_generic_chip(domain, i * AIC_IRQS_PER_CHIP);

		gc->chip_types[0].regs.eoi = AT91_AIC5_EOICR;
	}

	aic5_hw_init(domain);
	set_handle_irq(aic5_handle);

	return 0;
}

#define NR_SAMA5D2_IRQS		77

static int __init sama5d2_aic5_of_init(struct device_node *node,
				       struct device_node *parent)
{
	return aic5_of_init(node, parent, NR_SAMA5D2_IRQS);
}
IRQCHIP_DECLARE(sama5d2_aic5, "atmel,sama5d2-aic", sama5d2_aic5_of_init);

#define NR_SAMA5D3_IRQS		48

static int __init sama5d3_aic5_of_init(struct device_node *node,
				       struct device_node *parent)
{
	return aic5_of_init(node, parent, NR_SAMA5D3_IRQS);
}
IRQCHIP_DECLARE(sama5d3_aic5, "atmel,sama5d3-aic", sama5d3_aic5_of_init);

#define NR_SAMA5D4_IRQS		68

static int __init sama5d4_aic5_of_init(struct device_node *node,
				       struct device_node *parent)
{
	return aic5_of_init(node, parent, NR_SAMA5D4_IRQS);
}
IRQCHIP_DECLARE(sama5d4_aic5, "atmel,sama5d4-aic", sama5d4_aic5_of_init);
