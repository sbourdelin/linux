/*
 * Copyright (C) 2016 Socionext Inc.
 *   Author: Masahiro Yamada <yamada.masahiro@socionext.com>
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

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "clk-uniphier.h"

static struct clk_hw *uniphier_clk_register(struct device *dev,
					    struct regmap *regmap,
					const struct uniphier_clk_data *data)
{
	switch (data->type) {
	case UNIPHIER_CLK_TYPE_FIXED_FACTOR:
		return uniphier_clk_register_fixed_factor(dev, data->name,
							  &data->data.factor);
	case UNIPHIER_CLK_TYPE_FIXED_RATE:
		return uniphier_clk_register_fixed_rate(dev, data->name,
							&data->data.rate);
	case UNIPHIER_CLK_TYPE_GATE:
		return uniphier_clk_register_gate(dev, regmap, data->name,
						  &data->data.gate);
	case UNIPHIER_CLK_TYPE_MUX:
		return uniphier_clk_register_mux(dev, regmap, data->name,
						 &data->data.mux);
	default:
		dev_err(dev, "unsupported clock type\n");
		return ERR_PTR(-EINVAL);
	}
}

static const struct of_device_id uniphier_clk_match[] = {
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, uniphier_clk_match);

int uniphier_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	struct clk_hw_onecell_data *hw_data;
	struct device_node *parent;
	struct regmap *regmap;
	const struct uniphier_clk_data *p;
	int clk_num = 0;

	match = of_match_node(uniphier_clk_match, dev->of_node);
	if (!match)
		return -ENODEV;

	parent = of_get_parent(dev->of_node); /* parent should be syscon node */
	regmap = syscon_node_to_regmap(parent);
	of_node_put(parent);
	if (IS_ERR(regmap)) {
		dev_err(dev, "failed to get regmap (error %ld)\n",
			PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	for (p = match->data; p->name; p++)
		clk_num = max(clk_num, p->output_index + 1);

	hw_data = devm_kzalloc(dev,
			sizeof(*hw_data) + clk_num * sizeof(struct clk_hw *),
			GFP_KERNEL);
	if (!hw_data)
		return -ENOMEM;

	hw_data->num = clk_num;

	for (p = match->data; p->name; p++) {
		struct clk_hw *hw;

		dev_dbg(dev, "register %s (index=%d)\n", p->name,
			p->output_index);
		hw = uniphier_clk_register(dev, regmap, p);
		if (IS_ERR(hw)) {
			dev_err(dev, "failed to register %s (error %ld)\n",
				p->name, PTR_ERR(hw));
			return PTR_ERR(hw);
		}

		if (p->output_index >= 0)
			hw_data->hws[p->output_index] = hw;
	}

	return of_clk_add_hw_provider(dev->of_node, of_clk_hw_onecell_get,
				      hw_data);
}

int uniphier_clk_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);

	return 0;
}

static struct platform_driver uniphier_clk_driver = {
	.probe = uniphier_clk_probe,
	.remove = uniphier_clk_remove,
	.driver = {
		.name = "uniphier-clk",
		.of_match_table = uniphier_clk_match,
	},
};
module_platform_driver(uniphier_clk_driver);

MODULE_AUTHOR("Masahiro Yamada <yamada.masahiro@socionext.com>");
MODULE_DESCRIPTION("UniPhier Clock Driver");
MODULE_LICENSE("GPL");
