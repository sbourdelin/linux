/*
 * Atmel AT91 common AIC (Advanced Interrupt Controller) code shared by
 * irq-atmel-aic and irq-atmel-aic5 drivers
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

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#include <asm/exception.h>
#include <asm/mach/irq.h>

#include "irq-atmel-aic-common.h"

#define NR_AIC_IRQS			32

#define AT91_AIC_SMR_BASE		0
#define AT91_AIC_SVR_BASE		0x80
#define AT91_AIC_IVR			0x100
#define AT91_AIC_ISR			0x108
#define AT91_AIC_IECR			0x120
#define AT91_AIC_IDCR			0x124
#define AT91_AIC_ICCR			0x128
#define AT91_AIC_ISCR			0x12c
#define AT91_AIC_EOICR			0x130
#define AT91_AIC_SPU			0x134
#define AT91_AIC_DCR			0x138
#define AT91_INVALID_OFFSET		(-1)

#define AT91_AIC5_SSR			0x0
#define AT91_AIC5_SMR			0x4
#define AT91_AIC5_SVR			0x8
#define AT91_AIC5_IVR			0x10
#define AT91_AIC5_ISR			0x18
#define AT91_AIC5_EOICR			0x38
#define AT91_AIC5_SPU			0x3c
#define AT91_AIC5_IECR			0x40
#define AT91_AIC5_IDCR			0x44
#define AT91_AIC5_ICCR			0x48
#define AT91_AIC5_ISCR			0x4c
#define AT91_AIC5_DCR			0x6c

#define AT91_AIC_PRIOR			GENMASK(2, 0)
#define AT91_AIC_IRQ_MIN_PRIORITY	0
#define AT91_AIC_IRQ_MAX_PRIORITY	7

#define AT91_AIC_SRCTYPE		GENMASK(6, 5)
#define AT91_AIC_SRCTYPE_LOW		(0 << 5)
#define AT91_AIC_SRCTYPE_FALLING	(1 << 5)
#define AT91_AIC_SRCTYPE_HIGH		(2 << 5)
#define AT91_AIC_SRCTYPE_RISING		(3 << 5)

struct aic_chip_data {
	u32 ext_irqs;
};

/**
 * struct aic_reg_offset
 *
 * @eoi:	End of interrupt command register
 * @smr:	Source mode register
 * @ssr:	Source select register
 * @iscr:	Interrupt set command register
 * @idcr:	Interrupt disable command register
 * @iccr:	Interrupt clear command register
 * @iecr:	Interrupt enable command register
 * @spu:	Spurious interrupt vector register
 * @dcr:	Debug control register
 * @svr:	Source vector register
 * @ivr:	Interrupt vector register
 * @isr:	Interrupt status register
 *
 * Each value means register offset.
 */
struct aic_reg_offset {
	int eoi;
	int smr;
	int ssr;
	int iscr;
	int idcr;
	int iccr;
	int iecr;
	int spu;
	int dcr;
	int svr;
	int ivr;
	int isr;
};

static const struct aic_reg_offset aic_regs = {
	.eoi	= AT91_AIC_EOICR,
	.smr	= AT91_AIC_SMR_BASE,
	.ssr	= AT91_INVALID_OFFSET,	/* No SSR exists */
	.iscr	= AT91_AIC_ISCR,
	.idcr	= AT91_AIC_IDCR,
	.iccr	= AT91_AIC_ICCR,
	.iecr	= AT91_AIC_IECR,
	.spu	= AT91_AIC_SPU,
	.dcr	= AT91_AIC_DCR,
	.svr	= AT91_AIC_SVR_BASE,
	.ivr	= AT91_AIC_IVR,
	.isr	= AT91_AIC_ISR,
};

static const struct aic_reg_offset aic5_regs = {
	.eoi	= AT91_AIC5_EOICR,
	.smr	= AT91_AIC5_SMR,
	.ssr	= AT91_AIC5_SSR,
	.iscr	= AT91_AIC5_ISCR,
	.idcr	= AT91_AIC5_IDCR,
	.iccr	= AT91_AIC5_ICCR,
	.iecr	= AT91_AIC5_IECR,
	.spu	= AT91_AIC5_SPU,
	.dcr	= AT91_AIC5_DCR,
	.svr	= AT91_AIC5_SVR,
	.ivr	= AT91_AIC5_IVR,
	.isr	= AT91_AIC5_ISR,
};

static struct irq_domain *aic_domain;
static const struct aic_reg_offset *aic_reg_data;

static asmlinkage void __exception_irq_entry
aic_handle(struct pt_regs *regs)
{
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(aic_domain,
								  0);
	u32 hwirq = irq_reg_readl(gc, aic_reg_data->ivr);
	u32 status = irq_reg_readl(gc, aic_reg_data->isr);

	if (!status)
		irq_reg_writel(gc, 0, aic_reg_data->eoi);
	else
		handle_domain_irq(aic_domain, hwirq, regs);
}

static inline bool aic_is_ssr_used(void)
{
	return aic_reg_data->ssr != AT91_INVALID_OFFSET;
}

static void aic_update_smr(struct irq_chip_generic *gc, int hwirq,
			   u32 mask, u32 val)
{
	int reg = aic_reg_data->smr;
	u32 tmp = 0;

	if (aic_is_ssr_used())
		irq_reg_writel(gc, hwirq, aic_reg_data->ssr);
	else
		reg += hwirq * 4;

	tmp = irq_reg_readl(gc, reg);
	tmp &= mask;
	tmp |= val;

	irq_reg_writel(gc, tmp, reg);
}

static int aic_irq_domain_xlate(struct irq_domain *d, struct device_node *node,
				const u32 *intspec, unsigned int intsize,
				irq_hw_number_t *out_hwirq,
				unsigned int *out_type)
{
	struct irq_chip_generic *gc = irq_get_domain_generic_chip(d, 0);
	bool condition = (intsize < 3) ||
			 (intspec[2] < AT91_AIC_IRQ_MIN_PRIORITY) ||
			 (intspec[2] > AT91_AIC_IRQ_MAX_PRIORITY);

	if (!gc || WARN_ON(condition))
		return -EINVAL;

	/*
	 * intspec[0]: HW IRQ number
	 * intspec[1]: IRQ flag
	 * intspec[2]: IRQ priority
	 */

	*out_hwirq = intspec[0];
	*out_type = intspec[1] & IRQ_TYPE_SENSE_MASK;

	irq_gc_lock(gc);
	aic_update_smr(gc, *out_hwirq, ~AT91_AIC_PRIOR, intspec[2]);
	irq_gc_unlock(gc);

	return 0;
}

static const struct irq_domain_ops aic_irq_ops = {
	.map	= irq_map_generic_chip,
	.xlate	= aic_irq_domain_xlate,
};

static void aic_irq_shutdown(struct irq_data *d)
{
	struct irq_chip_type *ct = irq_data_get_chip_type(d);

	ct->chip.irq_mask(d);
}

static void aic_mask(struct irq_data *d)
{
	struct irq_domain *domain = d->domain;
	struct irq_chip_generic *bgc = irq_get_domain_generic_chip(domain, 0);
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	u32 mask = d->mask;

	/*
	 * Disable interrupt. We always take the lock of the
	 * first irq chip as all chips share the same registers.
	 */
	irq_gc_lock(bgc);

	if (aic_is_ssr_used()) {
		irq_reg_writel(gc, d->hwirq, aic_reg_data->ssr);
		irq_reg_writel(gc, 1, aic_reg_data->idcr);
	} else {
		irq_reg_writel(gc, mask, aic_reg_data->idcr);
	}

	gc->mask_cache &= ~mask;

	irq_gc_unlock(bgc);
}

static void aic_unmask(struct irq_data *d)
{
	struct irq_domain *domain = d->domain;
	struct irq_chip_generic *bgc = irq_get_domain_generic_chip(domain, 0);
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	u32 mask = d->mask;

	/*
	 * Enable interrupt. We always take the lock of the
	 * first irq chip as all chips share the same registers.
	 */
	irq_gc_lock(bgc);

	if (aic_is_ssr_used()) {
		irq_reg_writel(gc, d->hwirq, aic_reg_data->ssr);
		irq_reg_writel(gc, 1, aic_reg_data->iecr);
	} else {
		irq_reg_writel(gc, mask, aic_reg_data->iecr);
	}

	gc->mask_cache |= mask;

	irq_gc_unlock(bgc);
}

static int aic_retrigger(struct irq_data *d)
{
	struct irq_domain *domain = d->domain;
	struct irq_chip_generic *bgc = irq_get_domain_generic_chip(domain, 0);

	/* Set interrupt */
	irq_gc_lock(bgc);

	if (aic_is_ssr_used()) {
		irq_reg_writel(bgc, d->hwirq, aic_reg_data->ssr);
		irq_reg_writel(bgc, 1, aic_reg_data->iscr);
	} else {
		irq_reg_writel(bgc, d->mask, aic_reg_data->iscr);
	}

	irq_gc_unlock(bgc);

	return 0;
}

static int aic_set_type(struct irq_data *d, unsigned int type)
{
	struct irq_domain *domain = d->domain;
	struct irq_chip_generic *bgc = irq_get_domain_generic_chip(domain, 0);
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct aic_chip_data *aic = gc->private;
	u32 val;

	switch (type) {
	case IRQ_TYPE_LEVEL_HIGH:
		val = AT91_AIC_SRCTYPE_HIGH;
		break;
	case IRQ_TYPE_EDGE_RISING:
		val = AT91_AIC_SRCTYPE_RISING;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		if (!(d->mask & aic->ext_irqs))
			return -EINVAL;

		val = AT91_AIC_SRCTYPE_LOW;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		if (!(d->mask & aic->ext_irqs))
			return -EINVAL;

		val = AT91_AIC_SRCTYPE_FALLING;
		break;
	default:
		return -EINVAL;
	}

	irq_gc_lock(bgc);
	aic_update_smr(bgc, d->hwirq, ~AT91_AIC_SRCTYPE, val);
	irq_gc_unlock(bgc);

	return 0;
}

#ifdef CONFIG_PM

enum aic_pm_mode {
	AIC_PM_SUSPEND,
	AIC_PM_RESUME,
};

static void aic_pm_ctrl_ssr(struct irq_data *d, enum aic_pm_mode mode)
{
	struct irq_domain *domain = d->domain;
	struct irq_chip_generic *bgc = irq_get_domain_generic_chip(domain, 0);
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	u32 mask;
	u32 which;
	int i;

	if (mode == AIC_PM_SUSPEND)
		which = gc->wake_active;
	else
		which = gc->mask_cache;

	irq_gc_lock(bgc);

	for (i = 0; i < AIC_IRQS_PER_CHIP; i++) {
		mask = 1 << i;
		if ((mask & gc->mask_cache) == (mask & gc->wake_active))
			continue;

		irq_reg_writel(bgc, i + gc->irq_base, aic_reg_data->ssr);

		if (mask & which)
			irq_reg_writel(bgc, 1, aic_reg_data->iecr);
		else
			irq_reg_writel(bgc, 1, aic_reg_data->idcr);
	}

	irq_gc_unlock(bgc);
}

static void aic_pm_ctrl(struct irq_data *d, enum aic_pm_mode mode)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	u32 mask_idcr;
	u32 mask_iecr;

	if (mode == AIC_PM_SUSPEND) {
		mask_idcr = gc->mask_cache;
		mask_iecr = gc->wake_active;
	} else {
		mask_idcr = gc->wake_active;
		mask_iecr = gc->mask_cache;
	}

	irq_gc_lock(gc);
	irq_reg_writel(gc, mask_idcr, aic_reg_data->idcr);
	irq_reg_writel(gc, mask_iecr, aic_reg_data->iecr);
	irq_gc_unlock(gc);
}

static void aic_suspend(struct irq_data *d)
{
	if (aic_is_ssr_used())
		aic_pm_ctrl_ssr(d, AIC_PM_SUSPEND);
	else
		aic_pm_ctrl(d, AIC_PM_SUSPEND);
}

static void aic_resume(struct irq_data *d)
{
	if (aic_is_ssr_used())
		aic_pm_ctrl_ssr(d, AIC_PM_RESUME);
	else
		aic_pm_ctrl(d, AIC_PM_RESUME);
}

static void aic_pm_shutdown(struct irq_data *d)
{
	struct irq_domain *domain = d->domain;
	struct irq_chip_generic *bgc = irq_get_domain_generic_chip(domain, 0);
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	int i;

	if (aic_is_ssr_used()) {
		irq_gc_lock(bgc);
		for (i = 0; i < AIC_IRQS_PER_CHIP; i++) {
			irq_reg_writel(bgc, i + gc->irq_base, aic_reg_data->ssr);
			irq_reg_writel(bgc, 1, aic_reg_data->idcr);
			irq_reg_writel(bgc, 1, aic_reg_data->iccr);
		}
		irq_gc_unlock(bgc);
	} else {
		irq_gc_lock(gc);
		irq_reg_writel(gc, 0xffffffff, aic_reg_data->idcr);
		irq_reg_writel(gc, 0xffffffff, aic_reg_data->iccr);
		irq_gc_unlock(gc);
	}
}
#else
#define aic_suspend	NULL
#define aic_resume	NULL
#define aic_pm_shutdown	NULL
#endif /* CONFIG_PM */

static void __init aic_common_ext_irq_of_init(struct irq_domain *domain)
{
	struct device_node *node = irq_domain_get_of_node(domain);
	struct irq_chip_generic *gc;
	struct aic_chip_data *aic;
	struct property *prop;
	const __be32 *p;
	u32 hwirq;

	gc = irq_get_domain_generic_chip(domain, 0);

	aic = gc->private;
	aic->ext_irqs |= 1;

	of_property_for_each_u32(node, "atmel,external-irqs", prop, p, hwirq) {
		gc = irq_get_domain_generic_chip(domain, hwirq);
		if (!gc) {
			pr_warn("AIC: external irq %d >= %d skip it\n",
				hwirq, domain->revmap_size);
			continue;
		}

		aic = gc->private;
		aic->ext_irqs |= (1 << (hwirq % AIC_IRQS_PER_CHIP));
	}
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
		irq_reg_writel(gc, 0, aic_reg_data->eoi);

	/*
	 * Spurious Interrupt ID in Spurious Vector Register.
	 * When there is no current interrupt, the IRQ Vector Register
	 * reads the value stored in AIC_SPU
	 */
	irq_reg_writel(gc, 0xffffffff, aic_reg_data->spu);

	/* No debugging in AIC: Debug (Protect) Control Register */
	irq_reg_writel(gc, 0, aic_reg_data->dcr);

	/* Disable and clear all interrupts initially */
	if (aic_is_ssr_used()) {
		for (i = 0; i < domain->revmap_size; i++) {
			irq_reg_writel(gc, i, aic_reg_data->ssr);
			irq_reg_writel(gc, i, aic_reg_data->svr);
			irq_reg_writel(gc, 1, aic_reg_data->idcr);
			irq_reg_writel(gc, 1, aic_reg_data->iccr);
		}
	} else {
		irq_reg_writel(gc, 0xffffffff, aic_reg_data->idcr);
		irq_reg_writel(gc, 0xffffffff, aic_reg_data->iccr);

		for (i = 0; i < NR_AIC_IRQS; i++)
			irq_reg_writel(gc, i, aic_reg_data->svr + (i * 4));
	}
}

struct irq_domain *__init aic_common_of_init(struct device_node *node,
					     const char *name, int nirqs)
{
	struct irq_chip_generic *gc;
	struct irq_domain *domain;
	struct aic_chip_data *aic;
	void __iomem *reg_base;
	int nchips;
	int ret;
	int i;

	if (aic_domain)
		return ERR_PTR(-EEXIST);

	nchips = DIV_ROUND_UP(nirqs, AIC_IRQS_PER_CHIP);

	reg_base = of_iomap(node, 0);
	if (!reg_base)
		return ERR_PTR(-ENOMEM);

	aic = kcalloc(nchips, sizeof(*aic), GFP_KERNEL);
	if (!aic) {
		ret = -ENOMEM;
		goto err_iounmap;
	}

	domain = irq_domain_add_linear(node, nchips * AIC_IRQS_PER_CHIP,
				       &aic_irq_ops, aic);
	if (!domain) {
		ret = -ENOMEM;
		goto err_free_aic;
	}

	ret = irq_alloc_domain_generic_chips(domain, AIC_IRQS_PER_CHIP, 1, name,
					     handle_fasteoi_irq,
					     IRQ_NOREQUEST | IRQ_NOPROBE |
					     IRQ_NOAUTOEN, 0, 0);
	if (ret)
		goto err_domain_remove;

	for (i = 0; i < nchips; i++) {
		gc = irq_get_domain_generic_chip(domain, i * AIC_IRQS_PER_CHIP);

		gc->reg_base = reg_base;
		gc->unused = 0;
		gc->wake_enabled = ~0;

		gc->chip_types[0].type = IRQ_TYPE_SENSE_MASK;
		gc->chip_types[0].regs.eoi = aic_reg_data->eoi;
		gc->chip_types[0].chip.irq_eoi = irq_gc_eoi;
		gc->chip_types[0].chip.irq_set_wake = irq_gc_set_wake;
		gc->chip_types[0].chip.irq_shutdown = aic_irq_shutdown;
		gc->chip_types[0].chip.irq_mask = aic_mask;
		gc->chip_types[0].chip.irq_unmask = aic_unmask;
		gc->chip_types[0].chip.irq_retrigger = aic_retrigger;
		gc->chip_types[0].chip.irq_set_type = aic_set_type;
		gc->chip_types[0].chip.irq_suspend = aic_suspend;
		gc->chip_types[0].chip.irq_resume = aic_resume;
		gc->chip_types[0].chip.irq_pm_shutdown = aic_pm_shutdown;

		gc->private = &aic[i];
	}

	aic_domain = domain;
	aic_common_ext_irq_of_init(domain);
	aic_hw_init(domain);
	set_handle_irq(aic_handle);

	return domain;

err_domain_remove:
	irq_domain_remove(domain);

err_free_aic:
	kfree(aic);

err_iounmap:
	iounmap(reg_base);

	return ERR_PTR(ret);
}
