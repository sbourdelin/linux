/*
 * Atmel AT91 AIC (Advanced Interrupt Controller) driver
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
#define NR_AIC_IRQS	32

#define AT91_AIC_SMR(n)			((n) * 4)

#define AT91_AIC_SVR(n)			(0x80 + ((n) * 4))
#define AT91_AIC_IVR			0x100
#define AT91_AIC_FVR			0x104
#define AT91_AIC_ISR			0x108

#define AT91_AIC_IPR			0x10c
#define AT91_AIC_IMR			0x110
#define AT91_AIC_CISR			0x114

#define AT91_AIC_IECR			0x120
#define AT91_AIC_IDCR			0x124
#define AT91_AIC_ICCR			0x128
#define AT91_AIC_ISCR			0x12c
#define AT91_AIC_EOICR			0x130
#define AT91_AIC_SPU			0x134
#define AT91_AIC_DCR			0x138

static struct irq_domain *aic_domain;

static asmlinkage void __exception_irq_entry
aic_handle(struct pt_regs *regs)
{
	struct irq_domain_chip_generic *dgc = aic_domain->gc;
	struct irq_chip_generic *gc = dgc->gc[0];
	u32 irqnr;
	u32 irqstat;

	irqnr = irq_reg_readl(gc, AT91_AIC_IVR);
	irqstat = irq_reg_readl(gc, AT91_AIC_ISR);

	if (!irqstat)
		irq_reg_writel(gc, 0, AT91_AIC_EOICR);
	else
		handle_domain_irq(aic_domain, irqnr, regs);
}

static void __init aic_hw_init(struct irq_domain *domain)
{
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(domain, 0);
	int i;

	/*
	 * Perform 8 End Of Interrupt Command to make sure AIC
	 * will not Lock out nIRQ
	 */
	for (i = 0; i < 8; i++)
		irq_reg_writel(gc, 0, AT91_AIC_EOICR);

	/*
	 * Spurious Interrupt ID in Spurious Vector Register.
	 * When there is no current interrupt, the IRQ Vector Register
	 * reads the value stored in AIC_SPU
	 */
	irq_reg_writel(gc, 0xffffffff, AT91_AIC_SPU);

	/* No debugging in AIC: Debug (Protect) Control Register */
	irq_reg_writel(gc, 0, AT91_AIC_DCR);

	/* Disable and clear all interrupts initially */
	irq_reg_writel(gc, 0xffffffff, AT91_AIC_IDCR);
	irq_reg_writel(gc, 0xffffffff, AT91_AIC_ICCR);

	for (i = 0; i < NR_AIC_IRQS; i++)
		irq_reg_writel(gc, i, AT91_AIC_SVR(i));
}

static int __init aic_of_init(struct device_node *node,
			      struct device_node *parent)
{
	struct irq_domain *domain;

	if (aic_domain)
		return -EEXIST;

	domain = aic_common_of_init(node, "atmel-aic", NR_AIC_IRQS);
	if (IS_ERR(domain))
		return PTR_ERR(domain);

	aic_domain = domain;
	aic_hw_init(domain);
	set_handle_irq(aic_handle);

	return 0;
}
IRQCHIP_DECLARE(at91rm9200_aic, "atmel,at91rm9200-aic", aic_of_init);
