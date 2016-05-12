/*
 * Driver for Broadcom BCM2835 soc sdram controller
 *
 * Copyright (C) 2016 Martin Sperl
 *
 * inspired by: atmel-sdramc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

struct bcm2835_sdram_data {
	void __iomem *regs[2];
	struct dentry *debugfsdir;
	struct clk *clk;
};

#define R(n, o) { .name = n, .offset = o }
static const struct debugfs_reg32 bcm2835_sdram_regs[] = {
	R("c",			0x00),
	R("s",			0x04),
	R("src0",		0x08),
	R("src1",		0x0c),
	R("mask0",		0x10),
	R("mask1",		0x14),
	R("mask2",		0x18),
	R("mask3",		0x1c),
	R("mask4",		0x20),
	R("mask5",		0x24),
	R("mask6",		0x28),
	R("mask7",		0x2c),
	R("vaddr",		0x30),
	R("wakeup",		0x34),
	R("profile",		0x38),
	/* 0x3c is not defined */
	R("force0",		0x40),
	R("force1",		0x44),
	/* 0x48 to 0x54 are write only */
};

static void bcm2835_sdram_debugfs(struct platform_device *pdev)
{
	struct bcm2835_sdram_data *data = platform_get_drvdata(pdev);
	struct debugfs_regset32 *regset;
	char *name;
	int i;

	data->debugfsdir = debugfs_create_dir("bcm2835_sdram", NULL);
	if (!data->debugfsdir)
		return;

	/* create the regsets */
	for (i = 0; i < 2; i++) {
		name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
				      "regset%d", i);
		if (!name)
			return;

		regset = devm_kzalloc(&pdev->dev, sizeof(*regset),
				      GFP_KERNEL);
		if (!regset)
			return;

		regset->regs = bcm2835_sdram_regs;
		regset->nregs = ARRAY_SIZE(bcm2835_sdram_regs);
		regset->base = data->regs[i];

		debugfs_create_regset32(name, S_IRUGO,
					data->debugfsdir, regset);
	}
}

static int bcm2835_sdram_probe(struct platform_device *pdev)
{
	struct bcm2835_sdram_data *data;
	struct resource *res;
	int err, i;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	platform_set_drvdata(pdev, data);

	/* get registers */
	for (i = 0; i < 2; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		data->regs[i] = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(data->regs[i])) {
			err = PTR_ERR(data->regs[i]);
			dev_err(&pdev->dev,
				"Could not get register set %d: %d\n",
				i, err);
			return err;
		}
	}
	/* get clock */
	data->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(data->clk))
		return PTR_ERR(data->clk);
	clk_prepare_enable(data->clk);

	bcm2835_sdram_debugfs(pdev);

	return 0;
}

static int bcm2835_sdram_remove(struct platform_device *pdev)
{
	struct bcm2835_sdram_data *data = platform_get_drvdata(pdev);

	debugfs_remove_recursive(data->debugfsdir);

	return 0;
}

static const struct of_device_id bcm2835_sdram_of_match_table[] = {
	{ .compatible = "brcm,bcm2835-sdram", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_sdram_of_match_table);

static struct platform_driver bcm2835_sdram_driver = {
	.probe = bcm2835_sdram_probe,
	.remove = bcm2835_sdram_remove,
	.driver = {
		.name = "bcm2835_sdram",
		.of_match_table = bcm2835_sdram_of_match_table,
	},
};
module_platform_driver(bcm2835_sdram_driver);

MODULE_AUTHOR("Martin Sperl");
MODULE_DESCRIPTION("sdram driver for bcm2835 chip");
MODULE_LICENSE("GPL");
