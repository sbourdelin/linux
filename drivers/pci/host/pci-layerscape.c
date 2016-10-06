/*
 * PCIe host controller driver for Freescale Layerscape SoCs
 *
 * Copyright (C) 2014 Freescale Semiconductor.
 *
 * Author: Minghuan Lian <Minghuan.Lian@freescale.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#include "pcie-designware.h"

/* PEX1/2 Misc Ports Status Register */
#define SCFG_PEXMSCPORTSR(pex_idx)	(0x94 + (pex_idx) * 4)
#define LTSSM_STATE_SHIFT	20
#define LTSSM_STATE_MASK	0x3f
#define LTSSM_PCIE_L0		0x11 /* L0 state */

/* PEX Internal Configuration Registers */
#define PCIE_STRFMR1		0x71c /* Symbol Timer & Filter Mask Register1 */
#define PCIE_DBI_RO_WR_EN	0x8bc /* DBI Read-Only Write Enable Register */

/* PEX LUT registers */
#define PCIE_LUT_DBG		0x7FC /* PEX LUT Debug Register */

struct ls_pcie_drvdata {
	u32 lut_offset;
	u32 ltssm_shift;
	struct pcie_host_ops *ops;
};

struct ls_pcie {
	void __iomem *dbi;
	void __iomem *lut;
	struct regmap *scfg;
	struct pcie_port pp;
	const struct ls_pcie_drvdata *drvdata;
	int index;
};

#define to_ls_pcie(x)	container_of(x, struct ls_pcie, pp)

static bool ls_pcie_is_bridge(struct ls_pcie *ls_pcie)
{
	u32 header_type;

	header_type = ioread8(ls_pcie->dbi + PCI_HEADER_TYPE);
	header_type &= 0x7f;

	return header_type == PCI_HEADER_TYPE_BRIDGE;
}

/* Clear multi-function bit */
static void ls_pcie_clear_multifunction(struct ls_pcie *ls_pcie)
{
	iowrite8(PCI_HEADER_TYPE_BRIDGE, ls_pcie->dbi + PCI_HEADER_TYPE);
}

/* Fix class value */
static void ls_pcie_fix_class(struct ls_pcie *ls_pcie)
{
	iowrite16(PCI_CLASS_BRIDGE_PCI, ls_pcie->dbi + PCI_CLASS_DEVICE);
}

/* Drop MSG TLP except for Vendor MSG */
static void ls_pcie_drop_msg_tlp(struct ls_pcie *ls_pcie)
{
	u32 val;

	val = ioread32(ls_pcie->dbi + PCIE_STRFMR1);
	val &= 0xDFFFFFFF;
	iowrite32(val, ls_pcie->dbi + PCIE_STRFMR1);
}

static int ls1021_pcie_link_up(struct pcie_port *pp)
{
	struct ls_pcie *ls_pcie = to_ls_pcie(pp);
	u32 state;

	if (!ls_pcie->scfg)
		return 0;

	regmap_read(ls_pcie->scfg, SCFG_PEXMSCPORTSR(ls_pcie->index), &state);
	state = (state >> LTSSM_STATE_SHIFT) & LTSSM_STATE_MASK;

	if (state < LTSSM_PCIE_L0)
		return 0;

	return 1;
}

static void ls1021_pcie_host_init(struct pcie_port *pp)
{
	struct ls_pcie *ls_pcie = to_ls_pcie(pp);
	u32 index[2];

	ls_pcie->scfg = syscon_regmap_lookup_by_phandle(pp->dev->of_node,
						   "fsl,pcie-scfg");
	if (IS_ERR(ls_pcie->scfg)) {
		dev_err(pp->dev, "No syscfg phandle specified\n");
		ls_pcie->scfg = NULL;
		return;
	}

	if (of_property_read_u32_array(pp->dev->of_node,
				       "fsl,pcie-scfg", index, 2)) {
		ls_pcie->scfg = NULL;
		return;
	}
	ls_pcie->index = index[1];

	dw_pcie_setup_rc(pp);

	ls_pcie_drop_msg_tlp(ls_pcie);
}

static int ls_pcie_link_up(struct pcie_port *pp)
{
	struct ls_pcie *ls_pcie = to_ls_pcie(pp);
	u32 state;

	state = (ioread32(ls_pcie->lut + PCIE_LUT_DBG) >>
		 ls_pcie->drvdata->ltssm_shift) &
		 LTSSM_STATE_MASK;

	if (state < LTSSM_PCIE_L0)
		return 0;

	return 1;
}

static void ls_pcie_host_init(struct pcie_port *pp)
{
	struct ls_pcie *ls_pcie = to_ls_pcie(pp);

	iowrite32(1, ls_pcie->dbi + PCIE_DBI_RO_WR_EN);
	ls_pcie_fix_class(ls_pcie);
	ls_pcie_clear_multifunction(ls_pcie);
	ls_pcie_drop_msg_tlp(ls_pcie);
	iowrite32(0, ls_pcie->dbi + PCIE_DBI_RO_WR_EN);
}

static int ls_pcie_msi_host_init(struct pcie_port *pp,
				 struct msi_controller *chip)
{
	struct device_node *msi_node;
	struct device_node *np = pp->dev->of_node;

	/*
	 * The MSI domain is set by the generic of_msi_configure().  This
	 * .msi_host_init() function keeps us from doing the default MSI
	 * domain setup in dw_pcie_host_init() and also enforces the
	 * requirement that "msi-parent" exists.
	 */
	msi_node = of_parse_phandle(np, "msi-parent", 0);
	if (!msi_node) {
		dev_err(pp->dev, "failed to find msi-parent\n");
		return -EINVAL;
	}

	return 0;
}

static struct pcie_host_ops ls1021_pcie_host_ops = {
	.link_up = ls1021_pcie_link_up,
	.host_init = ls1021_pcie_host_init,
	.msi_host_init = ls_pcie_msi_host_init,
};

static struct pcie_host_ops ls_pcie_host_ops = {
	.link_up = ls_pcie_link_up,
	.host_init = ls_pcie_host_init,
	.msi_host_init = ls_pcie_msi_host_init,
};

static struct ls_pcie_drvdata ls1021_drvdata = {
	.ops = &ls1021_pcie_host_ops,
};

static struct ls_pcie_drvdata ls1043_drvdata = {
	.lut_offset = 0x10000,
	.ltssm_shift = 24,
	.ops = &ls_pcie_host_ops,
};

static struct ls_pcie_drvdata ls2080_drvdata = {
	.lut_offset = 0x80000,
	.ltssm_shift = 0,
	.ops = &ls_pcie_host_ops,
};

static const struct of_device_id ls_pcie_of_match[] = {
	{ .compatible = "fsl,ls1021a-pcie", .data = &ls1021_drvdata },
	{ .compatible = "fsl,ls1043a-pcie", .data = &ls1043_drvdata },
	{ .compatible = "fsl,ls2080a-pcie", .data = &ls2080_drvdata },
	{ .compatible = "fsl,ls2085a-pcie", .data = &ls2080_drvdata },
	{ },
};

static int __init ls_add_pcie_port(struct ls_pcie *ls_pcie,
				   struct platform_device *pdev)
{
	struct pcie_port *pp = &ls->pp;
	int ret;

	pp->dev = &pdev->dev;
	pp->dbi_base = ls_pcie->dbi;
	pp->ops = ls_pcie->drvdata->ops;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(pp->dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

static int __init ls_pcie_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct ls_pcie *ls_pcie;
	struct resource *dbi_base;
	int ret;

	match = of_match_device(ls_pcie_of_match, &pdev->dev);
	if (!match)
		return -ENODEV;

	ls_pcie = devm_kzalloc(&pdev->dev, sizeof(*ls_pcie), GFP_KERNEL);
	if (!ls_pcie)
		return -ENOMEM;

	dbi_base = platform_get_resource_byname(pdev, IORESOURCE_MEM, "regs");
	ls_pcie->dbi = devm_ioremap_resource(&pdev->dev, dbi_base);
	if (IS_ERR(ls_pcie->dbi)) {
		dev_err(&pdev->dev, "missing *regs* space\n");
		return PTR_ERR(ls_pcie->dbi);
	}

	ls_pcie->drvdata = match->data;
	ls_pcie->lut = ls_pcie->dbi + ls_pcie->drvdata->lut_offset;

	if (!ls_pcie_is_bridge(ls_pcie))
		return -ENODEV;

	ret = ls_add_pcie_port(ls_pcie, pdev);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, ls_pcie);

	return 0;
}

static struct platform_driver ls_pcie_driver = {
	.driver = {
		.name = "layerscape-pcie",
		.of_match_table = ls_pcie_of_match,
	},
};
builtin_platform_driver_probe(ls_pcie_driver, ls_pcie_probe);
