/*
 * clk-max77686.c - Clock driver for Maxim 77686/MAX77802
 *
 * Copyright (C) 2012 Samsung Electornics
 * Jonghwa Lee <jonghwa3.lee@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mfd/max77686.h>
#include <linux/mfd/max77686-private.h>
#include <linux/clk-provider.h>
#include <linux/mutex.h>
#include <linux/clkdev.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/mutex.h>

#include <dt-bindings/clock/maxim,max77686.h>
#include <dt-bindings/clock/maxim,max77802.h>

#define MAX77802_CLOCK_LOW_JITTER_SHIFT 0x3

enum chip_name {
	CHIP_MAX77686,
	CHIP_MAX77802,
};

struct max_gen_hw_clk_data {
	const char *name;
	u32 reg;
	u32 mask;
	u32 flags;
};

struct max_gen_clk_data {
	struct regmap *regmap;
	struct clk_init_data clk_idata;
	struct clk_hw hw;
	u32 reg;
	u32 mask;
};

struct max_gen_clk_driver_info {
	enum chip_name chip;
	struct clk **clks;
	struct max_gen_clk_data *max_clk_data;
	struct clk_onecell_data of_data;
};

static struct max_gen_hw_clk_data max77686_hw_clks_info[MAX77686_CLKS_NUM] = {
	[MAX77686_CLK_AP] = {
		.name = "32khz_ap",
		.reg = MAX77686_REG_32KHZ,
		.mask = BIT(MAX77686_CLK_AP),
	},
	[MAX77686_CLK_CP] = {
		.name = "32khz_cp",
		.reg = MAX77686_REG_32KHZ,
		.mask = BIT(MAX77686_CLK_CP),
	},
	[MAX77686_CLK_PMIC] = {
		.name = "32khz_pmic",
		.reg = MAX77686_REG_32KHZ,
		.mask = BIT(MAX77686_CLK_PMIC),
	},
};

static struct max_gen_hw_clk_data max77802_hw_clks_info[MAX77802_CLKS_NUM] = {
	[MAX77802_CLK_32K_AP] = {
		.name = "32khz_ap",
		.reg = MAX77802_REG_32KHZ,
		.mask = BIT(MAX77802_CLK_32K_AP),
	},
	[MAX77802_CLK_32K_CP] = {
		.name = "32khz_cp",
		.reg = MAX77802_REG_32KHZ,
		.mask = BIT(MAX77802_CLK_32K_CP),
	},
};

static struct max_gen_clk_data *to_max_gen_clk_data(struct clk_hw *hw)
{
	return container_of(hw, struct max_gen_clk_data, hw);
}

static int max_gen_clk_prepare(struct clk_hw *hw)
{
	struct max_gen_clk_data *max_gen = to_max_gen_clk_data(hw);

	return regmap_update_bits(max_gen->regmap, max_gen->reg,
				  max_gen->mask, max_gen->mask);
}

static void max_gen_clk_unprepare(struct clk_hw *hw)
{
	struct max_gen_clk_data *max_gen = to_max_gen_clk_data(hw);

	regmap_update_bits(max_gen->regmap, max_gen->reg,
			   max_gen->mask, ~max_gen->mask);
}

static int max_gen_clk_is_prepared(struct clk_hw *hw)
{
	struct max_gen_clk_data *max_gen = to_max_gen_clk_data(hw);
	int ret;
	u32 val;

	ret = regmap_read(max_gen->regmap, max_gen->reg, &val);

	if (ret < 0)
		return -EINVAL;

	return val & max_gen->mask;
}

static unsigned long max_gen_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	return 32768;
}

static struct clk_ops max_gen_clk_ops = {
	.prepare	= max_gen_clk_prepare,
	.unprepare	= max_gen_clk_unprepare,
	.is_prepared	= max_gen_clk_is_prepared,
	.recalc_rate	= max_gen_recalc_rate,
};

static int max77686_clk_probe(struct platform_device *pdev)
{
	struct device *parent = pdev->dev.parent;
	struct device *dev = &pdev->dev;
	const struct platform_device_id *id = platform_get_device_id(pdev);
	struct max_gen_clk_driver_info *drv_info;
	struct max_gen_clk_data *max_clk_data;
	const struct max_gen_hw_clk_data *hw_clks;
	struct regmap *regmap;
	int i, ret, num_clks;

	drv_info = devm_kzalloc(dev, sizeof(*drv_info), GFP_KERNEL);
	if (!drv_info)
		return -ENOMEM;

	regmap = dev_get_regmap(parent, NULL);
	if (!regmap) {
		dev_err(dev, "Failed to get rtc regmap\n");
		return -ENODEV;
	}

	drv_info->chip = id->driver_data;

	switch (drv_info->chip) {
	case CHIP_MAX77686:
		num_clks = MAX77686_CLKS_NUM;
		hw_clks = max77686_hw_clks_info;
		break;
	case CHIP_MAX77802:
		num_clks = MAX77802_CLKS_NUM;
		hw_clks = max77802_hw_clks_info;
		break;
	default:
		dev_err(dev, "Unknown Chip ID\n");
		return -EINVAL;
	};

	drv_info->max_clk_data = devm_kcalloc(dev, num_clks,
					      sizeof(*drv_info->max_clk_data),
					      GFP_KERNEL);
	if (!drv_info->max_clk_data)
		return -ENOMEM;

	drv_info->clks = devm_kcalloc(dev, num_clks,
				      sizeof(*drv_info->clks), GFP_KERNEL);
	if (!drv_info->clks)
		return -ENOMEM;

	for (i = 0; i < num_clks; i++) {
		struct clk *clk;
		const char *clk_name;

		max_clk_data = &drv_info->max_clk_data[i];

		max_clk_data->regmap = regmap;
		max_clk_data->mask = hw_clks[i].mask;
		max_clk_data->reg = hw_clks[i].reg;
		max_clk_data->clk_idata.flags = hw_clks[i].flags;
		max_clk_data->clk_idata.ops = &max_gen_clk_ops;

		if (parent->of_node &&
		    !of_property_read_string_index(parent->of_node,
						   "clock-output-names",
						   i, &clk_name))
			max_clk_data->clk_idata.name = clk_name;
		else
			max_clk_data->clk_idata.name = hw_clks[i].name;

		max_clk_data->hw.init = &max_clk_data->clk_idata;

		clk = devm_clk_register(dev, &max_clk_data->hw);
		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			dev_err(dev, "Failed to clock register: %d\n", ret);
			return ret;
		}

		ret = clk_register_clkdev(clk, max_clk_data->clk_idata.name,
					  NULL);
		if (ret < 0) {
			dev_err(dev, "Failed to clkdev register: %d\n", ret);
			return ret;
		}
		drv_info->clks[i] = clk;
	}

	platform_set_drvdata(pdev, drv_info);

	if (parent->of_node) {
		drv_info->of_data.clks = drv_info->clks;
		drv_info->of_data.clk_num = num_clks;
		ret = of_clk_add_provider(parent->of_node,
					  of_clk_src_onecell_get,
					  &drv_info->of_data);

		if (ret < 0) {
			dev_err(dev, "Failed to register OF clock provider: %d\n",
				ret);
			return ret;
		}
	}

	/* MAX77802: Enable low-jitter mode on the 32khz clocks. */
	if (drv_info->chip == CHIP_MAX77802) {
		ret = regmap_update_bits(regmap, MAX77802_REG_32KHZ,
					 1 << MAX77802_CLOCK_LOW_JITTER_SHIFT,
					 1 << MAX77802_CLOCK_LOW_JITTER_SHIFT);
		if (ret < 0) {
			dev_err(dev, "Failed to set low-jitter mode: %d\n",
				ret);
			return ret;
		}
	}

	return 0;
}

static int max77686_clk_remove(struct platform_device *pdev)
{
	struct device *parent = pdev->dev.parent;

	if (parent->of_node)
		of_clk_del_provider(parent->of_node);

	return 0;
}

static const struct platform_device_id max77686_clk_id[] = {
	{ "max77686-clk", .driver_data = (kernel_ulong_t)CHIP_MAX77686, },
	{ "max77802-clk", .driver_data = (kernel_ulong_t)CHIP_MAX77802, },
	{},
};
MODULE_DEVICE_TABLE(platform, max77686_clk_id);

static struct platform_driver max77686_clk_driver = {
	.driver = {
		.name  = "max77686-clk",
	},
	.probe = max77686_clk_probe,
	.remove = max77686_clk_remove,
	.id_table = max77686_clk_id,
};

module_platform_driver(max77686_clk_driver);

MODULE_DESCRIPTION("MAXIM 77686 Clock Driver");
MODULE_AUTHOR("Jonghwa Lee <jonghwa3.lee@samsung.com>");
MODULE_LICENSE("GPL");
