/*
 * Copyright (C) 2017 Marvell
 *
 * Hanna Hawa <hannah@marvell.com>
 * Thomas Petazzoni <thomas.petazzoni@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>
#include <dt-bindings/interrupt-controller/mvebu-icu.h>

#include "irq-mvebu-gicp.h"

/* ICU registers */
#define ICU_SETSPI_NSR_AL	0x10
#define ICU_SETSPI_NSR_AH	0x14
#define ICU_CLRSPI_NSR_AL	0x18
#define ICU_CLRSPI_NSR_AH	0x1c
#define ICU_INT_CFG(x)          (0x100 + 4 * (x))
#define   ICU_INT_ENABLE	BIT(24)
#define   ICU_IS_EDGE		BIT(28)
#define   ICU_GROUP_SHIFT	29

/* ICU definitions */
#define ICU_MAX_IRQS		207
#define ICU_SATA0_ICU_ID	109
#define ICU_SATA1_ICU_ID	107

struct mvebu_icu {
	struct irq_chip irq_chip;
	void __iomem *base;
	struct irq_domain *domain;
	struct device *dev;
	struct mvebu_gicp *gicp;
};

static int
mvebu_icu_irq_parent_domain_alloc(struct irq_domain *domain,
				  unsigned int virq, unsigned int type,
				  int *irq_msg_num)
{
	struct mvebu_icu *icu = domain->host_data;
	struct irq_fwspec fwspec;
	int gicp_idx, ret;

	gicp_idx = mvebu_gicp_alloc(icu->gicp);
	if (gicp_idx < 0) {
		dev_err(icu->dev, "Cannot allocate GICP interrupt\n");
		return gicp_idx;
	}

	fwspec.fwnode = domain->parent->fwnode;
	fwspec.param_count = 3;
	fwspec.param[0] = GIC_SPI;
	fwspec.param[1] = mvebu_gicp_idx_to_spi(icu->gicp, gicp_idx) - 32;
	fwspec.param[2] = type;

	/* Allocate the IRQ in the parent */
	ret = irq_domain_alloc_irqs_parent(domain, virq, 1, &fwspec);
	if (ret) {
		mvebu_gicp_free(icu->gicp, gicp_idx);
		return ret;
	}

	*irq_msg_num = gicp_idx;

	return 0;
}

static void
mvebu_icu_irq_parent_domain_free(struct irq_domain *domain,
				 unsigned int virq,
				 int irq_msg_num)
{
	struct mvebu_icu *icu = domain->host_data;

	irq_domain_free_irqs_parent(domain, virq, 1);
	mvebu_gicp_free(icu->gicp, irq_msg_num);
}

static int
mvebu_icu_irq_domain_translate(struct irq_domain *d, struct irq_fwspec *fwspec,
			       unsigned long *hwirq, unsigned int *type)
{
	struct mvebu_icu *icu = d->host_data;
	unsigned int icu_group;

	/* Check the count of the parameters in dt */
	if (WARN_ON(fwspec->param_count < 3)) {
		dev_err(icu->dev, "wrong ICU parameter count %d\n",
			fwspec->param_count);
		return -EINVAL;
	}

	/* Only ICU group type is handled */
	icu_group = fwspec->param[0];
	if (icu_group != ICU_GRP_NSR && icu_group != ICU_GRP_SR &&
	    icu_group != ICU_GRP_SEI && icu_group != ICU_GRP_REI) {
		dev_err(icu->dev, "wrong ICU group type %x\n", icu_group);
		return -EINVAL;
	}

	*hwirq = fwspec->param[1];
	if (*hwirq < 0 || *hwirq >= ICU_MAX_IRQS) {
		dev_err(icu->dev, "invalid interrupt number %ld\n", *hwirq);
		return -EINVAL;
	}

	/* Mask the type to prevent wrong DT configuration */
	*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;

	return 0;
}

static int
mvebu_icu_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
			   unsigned int nr_irqs, void *args)
{
	int err = 0, irq_msg_num = 0;
	unsigned long hwirq;
	unsigned int type = 0;
	unsigned int icu_group, icu_int;
	struct irq_fwspec *fwspec = args;
	struct mvebu_icu *icu = domain->host_data;

	err = mvebu_icu_irq_domain_translate(domain, fwspec, &hwirq, &type);
	if (err) {
		dev_err(icu->dev, "failed to translate ICU parameters\n");
		return err;
	}

	icu_group = fwspec->param[0];

	err = mvebu_icu_irq_parent_domain_alloc(domain, virq, type,
						&irq_msg_num);
	if (err) {
		dev_err(icu->dev, "failed to allocate ICU interrupt in parent domain\n");
		return err;
	}

	/* Configure the ICU with irq number & type */
	icu_int = irq_msg_num | ICU_INT_ENABLE;
	if (type & IRQ_TYPE_EDGE_RISING)
		icu_int |= ICU_IS_EDGE;
	icu_int |= icu_group << ICU_GROUP_SHIFT;
	writel_relaxed(icu_int, icu->base + ICU_INT_CFG(hwirq));

	/*
	 * The SATA unit has 2 ports, and a dedicated ICU entry per
	 * port. The ahci sata driver supports only one irq interrupt
	 * per SATA unit. To solve this conflict, we configure the 2
	 * SATA wired interrupts in the south bridge into 1 GIC
	 * interrupt in the north bridge. Even if only a single port
	 * is enabled, if sata node is enabled, both interrupts are
	 * configured (regardless of which port is actually in use).
	 */
	if (hwirq == ICU_SATA0_ICU_ID || hwirq == ICU_SATA1_ICU_ID) {
		writel_relaxed(icu_int,
			       icu->base + ICU_INT_CFG(ICU_SATA0_ICU_ID));
		writel_relaxed(icu_int,
			       icu->base + ICU_INT_CFG(ICU_SATA1_ICU_ID));
	}

	/* Make sure there is no interrupt left pending by the firmware */
	err = irq_set_irqchip_state(virq, IRQCHIP_STATE_PENDING, false);
	if (err) {
		mvebu_icu_irq_parent_domain_free(domain, virq, irq_msg_num);
		return err;
	}

	err = irq_domain_set_hwirq_and_chip(domain, virq, hwirq,
					    &icu->irq_chip, icu);
	if (err) {
		dev_err(icu->dev, "failed to set the data to IRQ domain\n");
		mvebu_icu_irq_parent_domain_free(domain, virq, irq_msg_num);
		return err;
	}

	return 0;
}

static void
mvebu_icu_irq_domain_free(struct irq_domain *domain, unsigned int virq,
			  unsigned int nr_irqs)
{
	struct mvebu_icu *icu = domain->host_data;
	struct irq_data *irq = irq_get_irq_data(virq);
	struct irq_data *irq_parent = irq->parent_data;
	int irq_msg_num;

	irq_msg_num = mvebu_gicp_spi_to_idx(icu->gicp,
					    irqd_to_hwirq(irq_parent));

	WARN_ON(nr_irqs != 1);

	writel_relaxed(0, icu->base + ICU_INT_CFG(irqd_to_hwirq(irq)));

	mvebu_icu_irq_parent_domain_free(domain, virq, irq_msg_num);
}

static const struct irq_domain_ops mvebu_icu_domain_ops = {
	.translate = mvebu_icu_irq_domain_translate,
	.alloc     = mvebu_icu_irq_domain_alloc,
	.free      = mvebu_icu_irq_domain_free,
};

static int mvebu_icu_probe(struct platform_device *pdev)
{
	struct mvebu_icu *icu;
	struct irq_domain *parent_domain;
	struct device_node *node = pdev->dev.of_node;
	struct platform_device *gicp_pdev;
	struct device_node *parent_irq_dn;
	struct device_node *gicp_dn;
	struct resource *res;
	struct resource *gicp_res;
	phys_addr_t setspi, clrspi;
	u32 i, icu_int;

	icu = devm_kzalloc(&pdev->dev, sizeof(struct mvebu_icu),
			   GFP_KERNEL);
	if (!icu)
		return -ENOMEM;

	icu->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	icu->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(icu->base)) {
		dev_err(&pdev->dev, "Failed to map icu base address.\n");
		return PTR_ERR(icu->base);
	}

	icu->irq_chip.name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					    "ICU.%x",
					    (unsigned int)res->start);
	if (!icu->irq_chip.name)
		return -ENOMEM;

	icu->irq_chip.irq_mask = irq_chip_mask_parent;
	icu->irq_chip.irq_unmask = irq_chip_unmask_parent;
	icu->irq_chip.irq_eoi = irq_chip_eoi_parent;
	icu->irq_chip.irq_set_type = irq_chip_set_type_parent;
#ifdef CONFIG_SMP
	icu->irq_chip.irq_set_affinity = irq_chip_set_affinity_parent;
#endif

	gicp_dn = of_parse_phandle(node, "marvell,gicp", 0);
	if (!gicp_dn) {
		dev_err(&pdev->dev, "Missing marvell,gicp property.\n");
		return -ENODEV;
	}

	gicp_pdev = of_find_device_by_node(gicp_dn);
	if (!gicp_pdev) {
		dev_err(&pdev->dev, "Cannot find gicp device.\n");
		return -ENODEV;
	}

	icu->gicp = platform_get_drvdata(gicp_pdev);

	gicp_res = platform_get_resource(gicp_pdev, IORESOURCE_MEM, 0);
	if (!gicp_res) {
		dev_err(&pdev->dev, "Failed to get gicp resource\n");
		return -ENODEV;
	}

	parent_irq_dn = of_irq_find_parent(node);
	if (!parent_irq_dn) {
		dev_err(&pdev->dev, "failed to find parent IRQ node\n");
		return -ENODEV;
	}

	parent_domain = irq_find_host(parent_irq_dn);
	if (!parent_domain) {
		dev_err(&pdev->dev, "Unable to locate ICU parent domain\n");
		return -ENODEV;
	}

	get_device(&gicp_pdev->dev);

	/*
	 * We need the GICP to be probed() before us
	 */
	if (!device_is_bound(&gicp_pdev->dev))
		return -EPROBE_DEFER;

	/* Set Clear/Set ICU SPI message address in AP */
	setspi = mvebu_gicp_setspi_phys_addr(icu->gicp);
	writel_relaxed(upper_32_bits(setspi), icu->base + ICU_SETSPI_NSR_AH);
	writel_relaxed(lower_32_bits(setspi), icu->base + ICU_SETSPI_NSR_AL);
	clrspi = mvebu_gicp_clrspi_phys_addr(icu->gicp);
	writel_relaxed(upper_32_bits(clrspi), icu->base + ICU_CLRSPI_NSR_AH);
	writel_relaxed(lower_32_bits(clrspi), icu->base + ICU_CLRSPI_NSR_AL);

	/*
	 * Clean all ICU interrupts with type SPI_NSR, required to
	 * avoid unpredictable SPI assignments done by firmware.
	 */
	for (i = 0 ; i < ICU_MAX_IRQS ; i++) {
		icu_int = readl(icu->base + ICU_INT_CFG(i));
		if ((icu_int >> ICU_GROUP_SHIFT) == ICU_GRP_NSR)
			writel_relaxed(0x0, icu->base + ICU_INT_CFG(i));
	}

	icu->domain =
		irq_domain_add_hierarchy(parent_domain, 0,
					 mvebu_gicp_spi_count(icu->gicp),
					 node, &mvebu_icu_domain_ops, icu);
	if (!icu->domain) {
		dev_err(&pdev->dev, "Failed to create ICU domain\n");
		put_device(&gicp_pdev->dev);
		return -ENOMEM;
	}

	return 0;
}

static const struct of_device_id mvebu_icu_of_match[] = {
	{ .compatible = "marvell,cp110-icu", },
	{},
};

static struct platform_driver mvebu_icu_driver = {
	.probe  = mvebu_icu_probe,
	.driver = {
		.name = "mvebu-icu",
		.of_match_table = mvebu_icu_of_match,
	},
};
builtin_platform_driver(mvebu_icu_driver);
