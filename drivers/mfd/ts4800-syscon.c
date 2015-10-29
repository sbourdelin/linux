/*
 * Device driver for TS-4800 FPGA's syscon
 *
 * Copyright (c) 2015 - Savoir-faire Linux
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/ts4800-syscon.h>


static const struct regmap_config ts4800_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 2,
	.val_bits = 16,
};

static int ts4800_syscon_probe(struct platform_device *pdev)
{
	struct ts4800_syscon *syscon;
	struct resource *res;
	void __iomem *base;

	syscon = devm_kzalloc(&pdev->dev, sizeof(*syscon), GFP_KERNEL);
	if (!syscon)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	syscon->regmap = devm_regmap_init_mmio_clk(&pdev->dev, NULL, base,
						   &ts4800_regmap_config);
	if (IS_ERR(syscon->regmap)) {
		dev_err(&pdev->dev,
			"regmap init failed: %ld\n",
			PTR_ERR(syscon->regmap));
		return PTR_ERR(syscon->regmap);
	}

	platform_set_drvdata(pdev, syscon);

	return 0;
}

static int ts4800_syscon_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id ts4800_syscon_of_match[] = {
	{ .compatible = "ts,ts4800-syscon", },
	{ },
};

static struct platform_driver ts4800_syscon_driver = {
	.driver = {
		.name	= "ts4800_syscon",
		.of_match_table = ts4800_syscon_of_match,
	},
	.probe	= ts4800_syscon_probe,
	.remove	= ts4800_syscon_remove,
};
module_platform_driver(ts4800_syscon_driver);

MODULE_AUTHOR("Damien Riegel <damien.riegel@savoirfairelinux.com>");
MODULE_DESCRIPTION("TS-4800 Syscon driver");
MODULE_LICENSE("GPL v2");
