/*
 * clk-max-gen.c - Generic clock driver for Maxim PMICs clocks
 *
 * Copyright (C) 2014 Google, Inc
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
 *
 * This driver is based on clk-max77686.c
 *
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>
#include <linux/clk-provider.h>
#include <linux/mutex.h>
#include <linux/clkdev.h>
#include <linux/of.h>
#include <linux/export.h>

#include "clk-max-gen.h"

struct max_gen_clk {
	struct regmap *regmap;
	u32 mask;
	u32 reg;
	struct clk_hw hw;
};

struct max_gen_data {
	size_t num;
	struct max_gen_clk clks[];
};

static struct max_gen_clk *to_max_gen_clk(struct clk_hw *hw)
{
	return container_of(hw, struct max_gen_clk, hw);
}

static int max_gen_clk_prepare(struct clk_hw *hw)
{
	struct max_gen_clk *max_gen = to_max_gen_clk(hw);

	return regmap_update_bits(max_gen->regmap, max_gen->reg,
				  max_gen->mask, max_gen->mask);
}

static void max_gen_clk_unprepare(struct clk_hw *hw)
{
	struct max_gen_clk *max_gen = to_max_gen_clk(hw);

	regmap_update_bits(max_gen->regmap, max_gen->reg,
			   max_gen->mask, ~max_gen->mask);
}

static int max_gen_clk_is_prepared(struct clk_hw *hw)
{
	struct max_gen_clk *max_gen = to_max_gen_clk(hw);
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

struct clk_ops max_gen_clk_ops = {
	.prepare	= max_gen_clk_prepare,
	.unprepare	= max_gen_clk_unprepare,
	.is_prepared	= max_gen_clk_is_prepared,
	.recalc_rate	= max_gen_recalc_rate,
};
EXPORT_SYMBOL_GPL(max_gen_clk_ops);

static int max_gen_clk_register(struct device *dev, struct max_gen_clk *max_gen)
{
	struct clk_hw *hw = &max_gen->hw;
	int ret;

	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		return ret;

	return clk_hw_register_clkdev(hw, hw->init->name, NULL);
}

static struct clk_hw *
of_clk_max_gen_get(struct of_phandle_args *clkspec, void *data)
{
	struct max_gen_data *max_gen_data = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= max_gen_data->num) {
		pr_err("%s: invalid index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	return &max_gen_data->clks[idx].hw;
}

int max_gen_clk_probe(struct platform_device *pdev, struct regmap *regmap,
		      u32 reg, struct clk_init_data *clks_init, int num_init)
{
	int i, ret;
	struct max_gen_data *max_gen_data;
	struct max_gen_clk *max_gen_clks;
	struct device *dev = pdev->dev.parent;
	const char *clk_name;
	struct clk_init_data *init;

	max_gen_data = devm_kzalloc(dev, sizeof(*max_gen_data) +
			sizeof(*max_gen_data->clks) * num_init, GFP_KERNEL);
	if (!max_gen_data)
		return -ENOMEM;

	max_gen_clks = max_gen_data->clks;

	for (i = 0; i < num_init; i++) {
		max_gen_clks[i].regmap = regmap;
		max_gen_clks[i].mask = 1 << i;
		max_gen_clks[i].reg = reg;

		init = devm_kzalloc(dev, sizeof(*init), GFP_KERNEL);
		if (!init)
			return -ENOMEM;

		if (dev->of_node &&
		    !of_property_read_string_index(dev->of_node,
						   "clock-output-names",
						   i, &clk_name))
			init->name = clk_name;
		else
			init->name = clks_init[i].name;

		init->ops = clks_init[i].ops;
		init->flags = clks_init[i].flags;

		max_gen_clks[i].hw.init = init;

		ret = max_gen_clk_register(dev, &max_gen_clks[i]);
		if (ret) {
			dev_err(dev, "failed to register %s\n",
				max_gen_clks[i].hw.init->name);
			return ret;
		}
	}

	if (dev->of_node) {
		ret = of_clk_add_hw_provider(dev->of_node, of_clk_max_gen_get,
					     max_gen_clks);
		if (ret) {
			dev_err(dev, "failed to register OF clock provider\n");
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(max_gen_clk_probe);

int max_gen_clk_remove(struct platform_device *pdev, int num_init)
{
	struct device *dev = pdev->dev.parent;

	if (dev->of_node)
		of_clk_del_provider(dev->of_node);

	return 0;
}
EXPORT_SYMBOL_GPL(max_gen_clk_remove);
