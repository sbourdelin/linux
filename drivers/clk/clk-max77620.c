/*
 * Clock driver for Maxim Max77620 device.
 *
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/mfd/max77620.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

struct max77620_clks_info {
	struct device *dev;
	struct regmap *rmap;
	struct clk *clk;
	struct clk_hw hw;
};

static struct max77620_clks_info *to_max77620_clks_info(struct clk_hw *hw)
{
	return container_of(hw, struct max77620_clks_info, hw);
}

static unsigned long max77620_clks_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	return 32768;
}

static int max77620_clks_prepare(struct clk_hw *hw)
{
	struct max77620_clks_info *mci = to_max77620_clks_info(hw);

	return regmap_update_bits(mci->rmap, MAX77620_REG_CNFG1_32K,
				  MAX77620_CNFG1_32K_OUT0_EN,
				  MAX77620_CNFG1_32K_OUT0_EN);
}

static void max77620_clks_unprepare(struct clk_hw *hw)
{
	struct max77620_clks_info *mci = to_max77620_clks_info(hw);

	regmap_update_bits(mci->rmap, MAX77620_REG_CNFG1_32K,
			   MAX77620_CNFG1_32K_OUT0_EN, 0);
}

static int max77620_clks_is_prepared(struct clk_hw *hw)
{
	struct max77620_clks_info *mci = to_max77620_clks_info(hw);
	unsigned int rval;
	int ret;

	ret = regmap_read(mci->rmap, MAX77620_REG_CNFG1_32K, &rval);
	if (ret < 0)
		return ret;

	return !!(rval & MAX77620_CNFG1_32K_OUT0_EN);
}

static struct clk_ops max77620_clks_ops = {
	.prepare	= max77620_clks_prepare,
	.unprepare	= max77620_clks_unprepare,
	.is_prepared	= max77620_clks_is_prepared,
	.recalc_rate	= max77620_clks_recalc_rate,
};

struct clk_init_data max77620_clk_init_data = {
	.name = "clk-32k",
	.ops = &max77620_clks_ops,
	.flags = CLK_IGNORE_UNUSED,
};

static int max77620_clks_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.parent->of_node;
	struct max77620_clks_info *mci;
	struct clk *clk;
	int ret;

	mci = devm_kzalloc(&pdev->dev, sizeof(*mci), GFP_KERNEL);
	if (!mci)
		return -ENOMEM;

	platform_set_drvdata(pdev, mci);

	mci->dev = &pdev->dev;
	mci->rmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!mci->rmap) {
		dev_err(mci->dev, "Failed to get parent regmap\n");
		return -ENODEV;
	}
	mci->hw.init = &max77620_clk_init_data;

	clk = devm_clk_register(&pdev->dev, &mci->hw);
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		dev_err(mci->dev, "Fail to register clock: %d\n", ret);
		return ret;
	}

	mci->clk = clk;
	ret = of_clk_add_provider(np, of_clk_src_simple_get, mci->clk);
	if (ret < 0)
		dev_err(&pdev->dev, "Fail to add clock driver, %d\n", ret);

	return ret;
}

static int max77620_clks_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.parent->of_node);

	return 0;
}

static struct platform_device_id max77620_clks_devtype[] = {
	{ .name = "max77620-clock", },
	{},
};

static struct platform_driver max77620_clks_driver = {
	.driver = {
		.name = "max77620-clock",
	},
	.probe = max77620_clks_probe,
	.remove = max77620_clks_remove,
	.id_table = max77620_clks_devtype,
};

module_platform_driver(max77620_clks_driver);

MODULE_DESCRIPTION("Clock driver for Maxim max77620 PMIC Device");
MODULE_AUTHOR("Laxman Dewangan <ldewangan@nvidia.com>");
MODULE_LICENSE("GPL v2");
