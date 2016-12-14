/*
 * Eberspächer Flexcard PMC II Carrier Board PCI Driver
 *
 * Copyright (c) 2014 - 2016, Linutronix GmbH
 * Author: Benedikt Spranger <b.spranger@linutronix.de>
 *         Holger Dengler <dengler@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include <linux/mfd/core.h>
#include <linux/mfd/flexcard.h>

#define FLEXCARD_CAN_OFFSET	0x2000
#define FLEXCARD_CAN_SIZE	0x2000

#define FLEXCARD_FR_OFFSET	0x4000
#define FLEXCARD_FR_SIZE	0x2000

#define FLEXCARD_CONF_START	0x000
#define FLEXCARD_CONF_SIZE	0x13F
#define FLEXCARD_CLKRST_START	0x144
#define FLEXCARD_CLKRST_SIZE	0x3
#define FLEXCARD_NF_START	0x170
#define FLEXCARD_NF_SIZE	0x7
#define FLEXCARD_CLK_START	0x700
#define FLEXCARD_CLK_SIZE	0x13

static DEFINE_IDA(flexcard_ida);

static struct resource flexcard_clk_res[] = {
	DEFINE_RES_MEM_NAMED(FLEXCARD_CLK_START,
			     FLEXCARD_CLK_SIZE,
			     "flexcard-clock"),
	DEFINE_RES_MEM_NAMED(FLEXCARD_CLKRST_START,
			     FLEXCARD_CLKRST_SIZE,
			     "flexcard-clock-reset"),
};

static struct mfd_cell flexcard_clk_dev[] = {
	{
		.name = "flexcard-clock",
		.num_resources = ARRAY_SIZE(flexcard_clk_res),
		.resources = flexcard_clk_res,
	},
};

static struct resource flexcard_misc_res[] = {
	DEFINE_RES_MEM_NAMED(FLEXCARD_CONF_START,
			     FLEXCARD_CONF_SIZE,
			     "flexcard-conf"),
	DEFINE_RES_MEM_NAMED(FLEXCARD_NF_START,
			     FLEXCARD_NF_SIZE,
			     "flexcard-nf"),
};

static struct mfd_cell flexcard_misc_dev[] = {
	{
		.name = "flexcard-misc",
		.num_resources = ARRAY_SIZE(flexcard_misc_res),
		.resources = flexcard_misc_res,
	},
};

enum flexcard_cell_id {
	FLEXCARD_CELL_CAN,
	FLEXCARD_CELL_FLEXRAY,
};

static int flexcard_clk_setup(struct flexcard_device *priv)
{
	struct pci_dev *pdev = priv->pdev;

	flexcard_clk_dev[0].id = priv->cardnr;
	flexcard_clk_res[0].parent = &pdev->resource[0];
	flexcard_clk_res[1].parent = &pdev->resource[0];

	return mfd_add_devices(&pdev->dev, 0, flexcard_clk_dev,
			       ARRAY_SIZE(flexcard_clk_dev),
			       &pdev->resource[0], 0, NULL);
}

static int flexcard_misc_setup(struct flexcard_device *priv)
{
	struct pci_dev *pdev = priv->pdev;

	flexcard_misc_dev[0].id = priv->cardnr;
	flexcard_misc_res[0].parent = &pdev->resource[0];
	flexcard_misc_res[1].parent = &pdev->resource[0];

	return mfd_add_devices(&pdev->dev, 0, flexcard_misc_dev,
			       ARRAY_SIZE(flexcard_misc_dev),
			       &pdev->resource[0], 0, NULL);
}

static int flexcard_tiny_can(struct flexcard_device *priv,
			     int idx, int id, u32 offset)
{
	struct mfd_cell *cell = &priv->cells[idx];
	struct resource *res = &priv->res[idx];
	struct pci_dev *pci = priv->pdev;

	cell->name = "flexcard-dcan";
	cell->resources = res;
	cell->num_resources = 1;
	cell->id = id;

	res->name = "flexcard-dcan";
	res->flags = IORESOURCE_MEM;
	res->parent = &pci->resource[1];
	res->start = pci->resource[1].start + offset;
	res->end = res->start + FLEXCARD_CAN_SIZE - 1;

	if (res->end > pci->resource[1].end)
		return -EINVAL;

	return 0;
}

static int flexcard_tiny_flexray(struct flexcard_device *priv,
				 int idx, int id, u32 offset)
{
	struct mfd_cell *cell = &priv->cells[idx];
	struct resource *res = &priv->res[idx];
	struct pci_dev *pci = priv->pdev;

	cell->name = "flexcard-eray";
	cell->resources = res;
	cell->num_resources = 1;
	cell->id = id;

	res->name = "flexcard-eray";
	res->flags = IORESOURCE_MEM;
	res->parent = &pci->resource[1];
	res->start = pci->resource[1].start + offset;
	res->end = res->start + FLEXCARD_FR_SIZE - 1;

	if (res->end > pci->resource[1].end)
		return -EINVAL;

	return 0;
}

static int flexcard_tiny_probe(struct flexcard_device *priv)
{
	struct pci_dev *pdev = priv->pdev;
	u32 fc_slic0, offset = 0;
	u8 nr_can, nr_fr, nr;
	int i, ret;

	/*
	 * Reading FC_LIC[0] register to determine the number of CAN and
	 * FlexRay Devices
	 */
	fc_slic0 = readl(&priv->bar0->conf.fc_slic[0]);
	nr_can = (fc_slic0 >> 4) & 0xf;
	nr_fr = fc_slic0 & 0xf;
	nr = nr_can + nr_fr;

	dev_info(&pdev->dev, "tinys: CAN: %d FR: %d", nr_can, nr_fr);

	priv->cells = devm_kzalloc(&pdev->dev, nr * sizeof(struct mfd_cell),
				   GFP_KERNEL);
	if (!priv->cells)
		return -ENOMEM;

	priv->res = devm_kzalloc(&pdev->dev, nr * sizeof(struct resource),
				 GFP_KERNEL);
	if (!priv->res)
		return -ENOMEM;

	for (i = 0; i < nr_fr; i++) {
		ret = flexcard_tiny_flexray(priv, i, i, offset);
		if (ret)
			return ret;
		offset += FLEXCARD_FR_OFFSET;
	}

	for (i = 0; i < nr_can; i++) {
		ret = flexcard_tiny_can(priv, nr_fr + i, i, offset);
		if (ret)
			return ret;
		offset += FLEXCARD_CAN_OFFSET;
	}

	return mfd_add_devices(&pdev->dev, 0, priv->cells, nr, NULL,
			       0, priv->irq_domain);
}

static int flexcard_probe(struct pci_dev *pdev,
			  const struct pci_device_id *id)
{
	struct flexcard_device *priv;
	union {
		struct fc_version ver;
		u32 reg;
	} fw_ver, hw_ver;

	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	pci_set_drvdata(pdev, priv);
	priv->pdev = pdev;

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "unable to enable device: %d\n", ret);
		return ret;
	}

	pci_set_master(pdev);
	ret = pci_request_regions(pdev, "flexcard");
	if (ret) {
		dev_err(&pdev->dev, "unable to request regions: %d\n", ret);
		goto out_disable;
	}

	priv->bar0 = pci_ioremap_bar(pdev, 0);
	if (!priv->bar0) {
		dev_err(&pdev->dev, "unable to remap bar0 regs\n");
		ret = -ENOMEM;
		goto out_release;
	}
	fw_ver.reg = readl(&priv->bar0->conf.fc_fw_ver);
	hw_ver.reg = readl(&priv->bar0->conf.fc_hw_ver);

	ret = ida_simple_get(&flexcard_ida, 0, 0, GFP_KERNEL);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not get new Flexcard id:%d\n", ret);
		goto out_unmap;
	}
	priv->cardnr = ret;

	ret = flexcard_setup_irq(pdev);
	if (ret) {
		dev_err(&pdev->dev, "unable to setup irq controller: %d", ret);
		goto out_ida;
	}

	ret = flexcard_tiny_probe(priv);
	if (ret) {
		dev_err(&pdev->dev, "unable to probe tinys: %d", ret);
		goto out_remove_irq;
	}

	ret = flexcard_misc_setup(priv);
	if (ret) {
		dev_err(&pdev->dev, "unable to probe tinys: %d", ret);
		goto out_mfd_dev_remove;
	}

	ret = flexcard_clk_setup(priv);
	if (ret) {
		dev_err(&pdev->dev, "unable to register clksrc: %d\n", ret);
		goto out_mfd_dev_remove;
	}

	dev_info(&pdev->dev, "HW %02x.%02x.%02x FW %02x.%02x.%02x\n",
		 hw_ver.ver.maj, hw_ver.ver.min, hw_ver.ver.dev,
		 fw_ver.ver.maj, fw_ver.ver.min, fw_ver.ver.dev);

	return 0;

out_mfd_dev_remove:
	mfd_remove_devices(&pdev->dev);
out_remove_irq:
	flexcard_remove_irq(pdev);
out_ida:
	ida_simple_remove(&flexcard_ida, priv->cardnr);
out_unmap:
	iounmap(priv->bar0);
out_release:
	pci_release_regions(pdev);
out_disable:
	pci_disable_device(pdev);

	return ret;
}

static void flexcard_remove(struct pci_dev *pdev)
{
	struct flexcard_device *priv = pci_get_drvdata(pdev);

	mfd_remove_devices(&pdev->dev);
	flexcard_remove_irq(pdev);
	ida_simple_remove(&flexcard_ida, priv->cardnr);
	iounmap(priv->bar0);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

#define PCI_VENDOR_ID_EBEL	0x1974

static const struct pci_device_id flexcard_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_EBEL, 0x0009), },
	{ }
};
MODULE_DEVICE_TABLE(pci, flexcard_pci_ids);

static struct pci_driver flexcard_driver = {
	.name     = "flexcard",
	.id_table = flexcard_pci_ids,
	.probe    = flexcard_probe,
	.remove   = flexcard_remove,
};

module_pci_driver(flexcard_driver);

MODULE_AUTHOR("Holger Dengler <dengler@linutronix.de>");
MODULE_AUTHOR("Benedikt Spranger <b.spranger@linutronix.de>");
MODULE_DESCRIPTION("Eberspaecher Flexcard PMC II Carrier Board Driver");
MODULE_LICENSE("GPL v2");
