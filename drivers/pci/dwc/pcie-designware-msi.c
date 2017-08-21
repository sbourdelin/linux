/*
 * Copyright (C) 2017 Linaro Limited <ard.biesheuvel@linaro.org>
 *
 * Based on code posted for the tango platform by
 *                            Marc Gonzalez <marc_gonzalez@sigmadesigns.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/msi.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>

#include "pcie-designware.h"

struct dw_pcie_msi {
	void __iomem		*regbase;
	int			irq;
	struct irq_domain	*irqd;
	struct irq_domain	*msid;
	DECLARE_BITMAP(		used_msi, MAX_MSI_IRQS);
	spinlock_t		used_msi_lock;
	u32			doorbell;
};

static void dw_pcie_msi_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct dw_pcie_msi *dw_msi = irq_desc_get_handler_data(desc);
	unsigned long status, base, virq, idx, pos;

	chained_irq_enter(chip, desc);
	spin_lock(&dw_msi->used_msi_lock);

	for (pos = 0; pos < MAX_MSI_IRQS;
	     pos = find_next_bit(dw_msi->used_msi, MAX_MSI_IRQS, pos)) {
		base = round_down(pos, 32);
		status = readl_relaxed(dw_msi->regbase + PCIE_MSI_INTR0_STATUS +
				       (base / 32) * 12);
		for_each_set_bit(idx, &status, 32) {
			virq = irq_find_mapping(dw_msi->irqd, base + idx);
			generic_handle_irq(virq);
		}
		pos = base + 32;
	}

	spin_unlock(&dw_msi->used_msi_lock);
	chained_irq_exit(chip, desc);
}

static void dw_pcie_ack(struct irq_data *d)
{
	struct dw_pcie_msi *dw_msi = d->chip_data;
	u32 offset = (d->hwirq / 32) * 12;
	u32 bit = BIT(d->hwirq % 32);

	writel_relaxed(bit, dw_msi->regbase + PCIE_MSI_INTR0_STATUS + offset);
}

static void dw_pcie_update_msi_enable(struct irq_data *d, bool unmask)
{
	unsigned long flags;
	struct dw_pcie_msi *dw_msi = d->chip_data;
	u32 offset = (d->hwirq / 32) * 12;
	u32 bit = BIT(d->hwirq % 32);
	u32 val;

	spin_lock_irqsave(&dw_msi->used_msi_lock, flags);
	val = readl_relaxed(dw_msi->regbase + PCIE_MSI_INTR0_ENABLE + offset);
	val = unmask ? (val | bit) : (val & ~bit);
	writel_relaxed(val, dw_msi->regbase + PCIE_MSI_INTR0_ENABLE + offset);
	spin_unlock_irqrestore(&dw_msi->used_msi_lock, flags);
}

static void dw_pcie_mask(struct irq_data *d)
{
	dw_pcie_update_msi_enable(d, false);
}

static void dw_pcie_unmask(struct irq_data *d)
{
	dw_pcie_update_msi_enable(d, true);
}

static int dw_pcie_set_affinity(struct irq_data *d,
				    const struct cpumask *mask, bool force)
{
	return -EINVAL;
}

static void dw_pcie_compose_msi_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct dw_pcie_msi *dw_msi = d->chip_data;

	msg->address_lo = lower_32_bits(virt_to_phys(&dw_msi->doorbell));
	msg->address_hi = upper_32_bits(virt_to_phys(&dw_msi->doorbell));
	msg->data = d->hwirq;
}

static struct irq_chip dw_pcie_chip = {
	.irq_ack		= dw_pcie_ack,
	.irq_mask		= dw_pcie_mask,
	.irq_unmask		= dw_pcie_unmask,
	.irq_set_affinity	= dw_pcie_set_affinity,
	.irq_compose_msi_msg	= dw_pcie_compose_msi_msg,
};

static void dw_pcie_msi_ack(struct irq_data *d)
{
	irq_chip_ack_parent(d);
}

static void dw_pcie_msi_mask(struct irq_data *d)
{
	pci_msi_mask_irq(d);
	irq_chip_mask_parent(d);
}

static void dw_pcie_msi_unmask(struct irq_data *d)
{
	pci_msi_unmask_irq(d);
	irq_chip_unmask_parent(d);
}

static struct irq_chip dw_pcie_msi_chip = {
	.name			= "DW-MSI",
	.irq_ack		= dw_pcie_msi_ack,
	.irq_mask		= dw_pcie_msi_mask,
	.irq_unmask		= dw_pcie_msi_unmask,
};

static struct msi_domain_info dw_pcie_msi_dom_info = {
	.flags	= MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS,
	.chip	= &dw_pcie_msi_chip,
};

static int dw_pcie_msi_irq_domain_alloc(struct irq_domain *dom,
					unsigned int virq,
					unsigned int nr_irqs, void *args)
{
	struct dw_pcie_msi *dw_msi = dom->host_data;
	unsigned long flags;
	int pos;

	spin_lock_irqsave(&dw_msi->used_msi_lock, flags);
	pos = find_first_zero_bit(dw_msi->used_msi, MAX_MSI_IRQS);
	if (pos >= MAX_MSI_IRQS) {
		spin_unlock_irqrestore(&dw_msi->used_msi_lock, flags);
		return -ENOSPC;
	}
	__set_bit(pos, dw_msi->used_msi);
	spin_unlock_irqrestore(&dw_msi->used_msi_lock, flags);
	irq_domain_set_info(dom, virq, pos, &dw_pcie_chip, dw_msi,
			    handle_edge_irq, NULL, NULL);

	return 0;
}

static void dw_pcie_msi_irq_domain_free(struct irq_domain *dom,
					unsigned int virq,
					unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(dom, virq);
	struct dw_pcie_msi *dw_msi = d->chip_data;
	unsigned long flags;

	spin_lock_irqsave(&dw_msi->used_msi_lock, flags);
	__clear_bit(d->hwirq, dw_msi->used_msi);
	spin_unlock_irqrestore(&dw_msi->used_msi_lock, flags);
}

static const struct irq_domain_ops irq_dom_ops = {
	.alloc	= dw_pcie_msi_irq_domain_alloc,
	.free	= dw_pcie_msi_irq_domain_free,
};

static int dw_pcie_msi_probe(struct platform_device *pdev)
{
	struct fwnode_handle *fwnode = of_node_to_fwnode(pdev->dev.of_node);
	struct device *dev = &pdev->dev;
	struct dw_pcie_msi *dw_msi;
	struct resource *res;

	dw_msi = devm_kzalloc(dev, sizeof(*dw_msi), GFP_KERNEL);
	if (!dw_msi)
		return -ENOMEM;

	/* get the control register and map it */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dw_msi->regbase = devm_ioremap_resource(dev, res);
	if (IS_ERR(dw_msi->regbase))
		return PTR_ERR(dw_msi->regbase);

	/* get the wired interrupt that gets raised when we receive an MSI */
	dw_msi->irq = platform_get_irq(pdev, 0);
	if (dw_msi->irq <= 0) {
		pr_err("Failed to map IRQ\n");
		return -ENXIO;
	}

	dw_msi->irqd = irq_domain_create_linear(fwnode, MAX_MSI_IRQS,
						&irq_dom_ops, dw_msi);
	if (!dw_msi->irqd) {
		dev_err(dev, "Failed to create IRQ domain\n");
		return -ENOMEM;
	}

	dw_msi->msid = pci_msi_create_irq_domain(fwnode, &dw_pcie_msi_dom_info,
						 dw_msi->irqd);
	if (!dw_msi->msid) {
		dev_err(dev, "Failed to create MSI domain\n");
		irq_domain_remove(dw_msi->irqd);
		return -ENOMEM;
	}

	irq_set_chained_handler_and_data(dw_msi->irq, dw_pcie_msi_isr, dw_msi);
	platform_set_drvdata(pdev, dw_msi);

	/* program the msi_data */
	writel_relaxed(lower_32_bits(virt_to_phys(&dw_msi->doorbell)),
		       dw_msi->regbase + PCIE_MSI_ADDR_LO);
	writel_relaxed(upper_32_bits(virt_to_phys(&dw_msi->doorbell)),
		       dw_msi->regbase + PCIE_MSI_ADDR_HI);

	return 0;
}

static int dw_pcie_msi_remove(struct platform_device *pdev)
{
	struct dw_pcie_msi *dw_msi = platform_get_drvdata(pdev);

	irq_set_chained_handler_and_data(dw_msi->irq, NULL, NULL);
	irq_domain_remove(dw_msi->msid);
	irq_domain_remove(dw_msi->irqd);

	return 0;
}

static const struct of_device_id dw_pcie_dw_msi_of_match[] = {
	{ .compatible = "snps,dw-pcie-msi" },
	{ },
};

static struct platform_driver pci_dw_msi_driver = {
	.driver.name			= "pcie-designware-msi",
	.driver.of_match_table		= dw_pcie_dw_msi_of_match,
	.probe				= dw_pcie_msi_probe,
	.remove				= dw_pcie_msi_remove,
};
builtin_platform_driver(pci_dw_msi_driver);
