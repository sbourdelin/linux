/*
 * NVIDIA Tegra XUSB MFD driver
 *
 * Copyright (C) 2015 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/core.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <soc/tegra/xusb.h>

static const struct of_device_id tegra_xusb_of_match[] = {
	{ .compatible = "nvidia,tegra124-xusb", },
	{},
};
MODULE_DEVICE_TABLE(of, tegra_xusb_of_match);

static struct regmap_config tegra_fpci_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

struct tegra_xusb_priv {
	struct platform_device *mbox_pdev;
	struct platform_device *xhci_pdev;
};

static struct platform_device *tegra_xusb_add_device(struct device *parent,
	const char *name, int id, const struct resource *res,
	unsigned int num_res, const void *data, size_t size_data)
{
	int ret = -ENOMEM;
	struct platform_device *pdev;

	pdev = platform_device_alloc(name, id);
	if (!pdev)
		goto err_alloc;

	pdev->dev.parent = parent;
	pdev->dev.dma_mask = parent->dma_mask;
	pdev->dev.dma_parms = parent->dma_parms;
	pdev->dev.coherent_dma_mask = parent->coherent_dma_mask;
	pdev->dev.of_node = parent->of_node;

	ret = platform_device_add_resources(pdev,
			res, num_res);
	if (ret)
		goto err;

	ret = platform_device_add_data(pdev,
			data, size_data);
	if (ret)
		goto err;

	ret = platform_device_add(pdev);
	if (ret)
		goto err;

	return pdev;

err:
	kfree(pdev->dev.dma_mask);

err_alloc:
	platform_device_put(pdev);
	return ERR_PTR(ret);
}

static int tegra_xusb_probe(struct platform_device *pdev)
{
	struct resource *res;
	void __iomem *fpci_base;
	int ret;
	struct tegra_xusb_shared_regs *sregs;
	struct tegra_xusb_priv *priv;

	sregs = devm_kzalloc(&pdev->dev, sizeof(*sregs), GFP_KERNEL);
	if (!sregs)
		return -ENOMEM;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/*
	  The registers are a bit jumbled up:

	  xhci uses:    0x70098000 - 0x700980cf
	  mailbox uses: 0x700980e0 - 0x700980f3
	  xhci uses:    0x7009841c - 0x7009841f - Undocumented paging register
	  mailbox uses: 0x70098428 - 0x7009842b
	  xhci uses:    0x70098800 - 0x700989ff - Undocumented paging window

	  Use a regmap to cover this area and pass it to child nodes.
	*/
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	fpci_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(fpci_base)) {
		ret = PTR_ERR(fpci_base);
		dev_err(&pdev->dev, "Failed to get shared resource: %d\n", ret);
		return ret;
	}

	tegra_fpci_regmap_config.max_register = res->end - res->start - 3;
	sregs->fpci_regs = devm_regmap_init_mmio(&pdev->dev, fpci_base,
			&tegra_fpci_regmap_config);
	if (IS_ERR(sregs->fpci_regs)) {
		ret = PTR_ERR(sregs->fpci_regs);
		dev_err(&pdev->dev, "Failed to init regmap: %d\n", ret);
		return ret;
	}

	priv->mbox_pdev = tegra_xusb_add_device(&pdev->dev,
			"tegra-xusb-mbox", PLATFORM_DEVID_NONE, NULL, 0,
			sregs, sizeof(sregs));
	if (IS_ERR(priv->mbox_pdev)) {
		dev_err(&pdev->dev, "Failed to add mailbox subdevice\n");
		return PTR_ERR(priv->mbox_pdev);
	}

	priv->xhci_pdev = tegra_xusb_add_device(&pdev->dev,
			"tegra-xhci", PLATFORM_DEVID_NONE, NULL, 0, sregs,
			sizeof(sregs));
	if (IS_ERR(priv->xhci_pdev)) {
		dev_err(&pdev->dev, "Failed to add xhci subdevice\n");
		return PTR_ERR(priv->xhci_pdev);
	}

	platform_set_drvdata(pdev, priv);

	return 0;
}

static int tegra_xusb_remove(struct platform_device *pdev)
{
	struct tegra_xusb_priv *priv;

	priv = platform_get_drvdata(pdev);

	platform_device_unregister(priv->xhci_pdev);

	platform_device_unregister(priv->mbox_pdev);

	return 0;
}

static struct platform_driver tegra_xusb_driver = {
	.probe = tegra_xusb_probe,
	.remove = tegra_xusb_remove,
	.driver = {
		.name = "tegra-xusb",
		.of_match_table = tegra_xusb_of_match,
	},
};
module_platform_driver(tegra_xusb_driver);

MODULE_DESCRIPTION("NVIDIA Tegra XUSB MFD");
MODULE_AUTHOR("Andrew Bresticker <abrestic@chromium.org>");
MODULE_LICENSE("GPL v2");
