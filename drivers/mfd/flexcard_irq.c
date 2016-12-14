/*
 * Eberspächer Flexcard PMC II Carrier Board PCI Driver - Interrupt controller
 *
 * Copyright (c) 2014 - 2016, Linutronix GmbH
 * Author: Benedikt Spranger <b.spranger@linutronix.de>
 *         Holger Dengler <dengler@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/export.h>
#include <linux/flexcard.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>

#include <linux/mfd/core.h>
#include <linux/mfd/flexcard.h>

struct fc_irq_tab {
	u32 mskcache;
	u32 mskoffs;
	u32 msk;
	u32 ackoffs;
	u32 ack;
};

#define to_irq_tab_ack(statbit, cofs, mskofs, mskbit, ackofs, ackbit)	\
	[statbit] = {							\
			.mskcache	= cofs,				\
			.mskoffs	= mskofs,			\
			.msk		= (1U << mskbit),		\
			.ackoffs	= ackofs,			\
			.ack		= (1U << ackbit) }

#define to_irq_tab(statbit, cofs, mskofs, mskbit)			\
	[statbit] = {							\
			.mskcache	= cofs,				\
			.mskoffs	= mskofs,			\
			.msk		= (1U << mskbit) }

#define DEVMSK_OFFS	offsetof(struct fc_bar0, conf.irc)
#define DEVACK_OFFS	offsetof(struct fc_bar0, conf.irs)
#define DEVMSK_CACHE	offsetof(struct flexcard_device, dev_irqmsk)

#define dev_to_irq_tab_ack(s, m, a)				\
		to_irq_tab_ack(s, DEVMSK_CACHE, DEVMSK_OFFS, m,	\
			       DEVACK_OFFS, a)

#define dev_to_irq_tab(s, m)				\
		to_irq_tab(s, DEVMSK_CACHE, DEVMSK_OFFS, m)

static const struct fc_irq_tab flexcard_irq_tab[] = {
	/* Device Interrupts */
	dev_to_irq_tab_ack(28, 28, 0),	/* TIMER  */
	dev_to_irq_tab_ack(29, 29, 1),	/* CC1CYS */
	dev_to_irq_tab_ack(21, 30, 10),	/* CC2CYS */
	dev_to_irq_tab_ack(30, 18, 2),	/* CC3CYS */
	dev_to_irq_tab_ack(25, 19, 6),	/* CC4CYS */
	dev_to_irq_tab_ack(26, 26, 4),	/* WAKE1A */
	dev_to_irq_tab_ack(27, 27, 5),	/* WAKE1B */
	dev_to_irq_tab_ack(23, 24, 8),	/* WAKE2A */
	dev_to_irq_tab_ack(22, 25, 9),	/* WAKE2B */
	dev_to_irq_tab_ack(19, 22, 12),	/* WAKE3A */
	dev_to_irq_tab_ack(18, 23, 13),	/* WAKE3B */
	dev_to_irq_tab_ack(17, 20, 14),	/* WAKE4A */
	dev_to_irq_tab_ack(16, 21, 15),	/* WAKE4B */
	dev_to_irq_tab(31, 15),		/* CC1T0  */
	dev_to_irq_tab(3, 14),		/* CC2T0  */
	dev_to_irq_tab(24, 16),		/* CC3T0  */
	dev_to_irq_tab(20, 17),		/* CC4T0  */
};

#define NR_FLEXCARD_IRQ		ARRAY_SIZE(flexcard_irq_tab)

#define VALID_DEVIRQ_MSK	((1U << 28) | \
				 (1U << 29) | \
				 (1U << 21) | \
				 (1U << 30) | \
				 (1U << 25) | \
				 (1U << 26) | \
				 (1U << 27) | \
				 (1U << 23) | \
				 (1U << 22) | \
				 (1U << 19) | \
				 (1U << 18) | \
				 (1U << 17) | \
				 (1U << 16) | \
				 (1U << 31) | \
				 (1U << 3)  | \
				 (1U << 24) | \
				 (1U << 20))

static irqreturn_t flexcard_demux(int irq, void *data)
{
	struct flexcard_device *priv = data;
	irqreturn_t ret = IRQ_NONE;
	unsigned int slot, cur, stat;

	stat = readl(&priv->bar0->conf.irs) & VALID_DEVIRQ_MSK;
	while (stat) {
		slot = __ffs(stat);
		stat &= (1 << slot);
		cur = irq_linear_revmap(priv->irq_domain, slot);
		generic_handle_irq(cur);
		ret = IRQ_HANDLED;
	}
	return ret;
}

static void flexcard_irq_ack(struct irq_data *d)
{
	struct flexcard_device *priv = irq_data_get_irq_chip_data(d);
	const struct fc_irq_tab *tp = &flexcard_irq_tab[d->hwirq];
	void __iomem *p = (void __iomem *)priv->bar0 + tp->ackoffs;

	writel(tp->ack, p);
}

static void flexcard_irq_mask(struct irq_data *d)
{
	struct flexcard_device *priv = irq_data_get_irq_chip_data(d);
	const struct fc_irq_tab *tp = &flexcard_irq_tab[d->hwirq];
	void __iomem *p = (void __iomem *)priv->bar0 + tp->mskoffs;
	u32 *msk = (void *)priv + tp->mskcache;

	raw_spin_lock(&priv->irq_lock);
	*msk &= ~tp->msk;
	writel(*msk, p);
	raw_spin_unlock(&priv->irq_lock);
}

static void flexcard_irq_unmask(struct irq_data *d)
{
	struct flexcard_device *priv = irq_data_get_irq_chip_data(d);
	const struct fc_irq_tab *tp = &flexcard_irq_tab[d->hwirq];
	void __iomem *p = (void __iomem *)priv->bar0 + tp->mskoffs;
	u32 *msk = (void *)priv + tp->mskcache;

	raw_spin_lock(&priv->irq_lock);
	*msk |= tp->msk;
	writel(*msk, p);
	raw_spin_unlock(&priv->irq_lock);
}

static int flexcard_req_irq(struct pci_dev *pdev)
{
	struct flexcard_device *priv = pci_get_drvdata(pdev);
	int ret;

	ret = pci_enable_msi(pdev);
	if (ret) {
		dev_warn(&pdev->dev, "could not enable MSI\n");
		/* shared PCI irq fallback */
		return request_irq(pdev->irq, flexcard_demux,
				   IRQF_NO_THREAD | IRQF_SHARED,
				   "flexcard", priv);
	}
	dev_info(&pdev->dev, "MSI enabled\n");

	ret = request_irq(pdev->irq, flexcard_demux, IRQF_NO_THREAD,
			  "flexcard", priv);
	if (ret)
		pci_disable_msi(pdev);

	return ret;
}

static struct irq_chip flexcard_irq_chip = {
	.name		= "flexcard_irq",
	.irq_ack	= flexcard_irq_ack,
	.irq_mask	= flexcard_irq_mask,
	.irq_unmask	= flexcard_irq_unmask,
};

static int flexcard_irq_domain_map(struct irq_domain *d, unsigned int irq,
				   irq_hw_number_t hw)
{
	struct flexcard_device *priv = d->host_data;

	irq_set_chip_and_handler_name(irq, &flexcard_irq_chip,
				      handle_level_irq, "flexcard");
	irq_set_chip_data(irq, priv);
	irq_modify_status(irq, IRQ_NOREQUEST | IRQ_NOAUTOEN, IRQ_NOPROBE);

	return 0;
}

static const struct irq_domain_ops flexcard_irq_domain_ops = {
	.map = flexcard_irq_domain_map,
};

int flexcard_setup_irq(struct pci_dev *pdev)
{
	struct flexcard_device *priv = pci_get_drvdata(pdev);
	struct irq_domain *domain;
	int ret;

	/* Make sure none of the subirqs is enabled */
	writel(0, &priv->bar0->conf.irc);
	writel(0, &priv->bar0->dma.dma_irer);

	raw_spin_lock_init(&priv->irq_lock);

	domain = irq_domain_add_linear(NULL, NR_FLEXCARD_IRQ,
				       &flexcard_irq_domain_ops, priv);
	if (!domain) {
		dev_err(&pdev->dev, "could not request irq domain\n");
		return -ENODEV;
	}

	priv->irq_domain = domain;

	ret = flexcard_req_irq(pdev);
	if (ret)
		irq_domain_remove(priv->irq_domain);

	return ret;
}

void flexcard_remove_irq(struct pci_dev *pdev)
{
	struct flexcard_device *priv = pci_get_drvdata(pdev);

	/* Disable all subirqs */
	writel(0, &priv->bar0->conf.irc);
	writel(0, &priv->bar0->dma.dma_irer);

	free_irq(pdev->irq, priv);
	pci_disable_msi(pdev);
	irq_domain_remove(priv->irq_domain);
}
