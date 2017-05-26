/*
 * Copyright 2017 IBM Corperation
 *
 * Joel Stanley <joel@jms.id.au>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/reset-controller.h>

#define to_basic_reset_priv(p)		\
	container_of((p), struct basic_reset_priv, rcdev)

struct basic_reset_priv {
	struct regmap *regmap;
	struct reset_controller_dev rcdev;
	u32 reg;
};

static int basic_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct basic_reset_priv *priv = to_basic_reset_priv(rcdev);
	u32 mask = BIT(id);

	regmap_update_bits(priv->regmap, priv->reg, mask, mask);

	return 0;
}

static int basic_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	struct basic_reset_priv *priv = to_basic_reset_priv(rcdev);
	u32 mask = BIT(id);

	regmap_update_bits(priv->regmap, priv->reg, mask, 0);

	return 0;
}

static int basic_reset_status(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct basic_reset_priv *priv = to_basic_reset_priv(rcdev);
	u32 mask = BIT(id);
	u32 val;

	regmap_read(priv->regmap, priv->reg, &val);

	return !!(val & mask);
}

static const struct reset_control_ops basic_reset_ops = {
	.assert = basic_reset_assert,
	.deassert = basic_reset_deassert,
	.status = basic_reset_status,
};

static int basic_reset_probe(struct platform_device *pdev)
{
	struct device_node *parent_np = of_get_parent(pdev->dev.of_node);
	struct basic_reset_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = syscon_node_to_regmap(parent_np);
	of_node_put(parent_np);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	ret = of_property_read_u32(pdev->dev.of_node, "reg", &priv->reg);
	if (ret)
		return ret;

	priv->rcdev.owner = THIS_MODULE;
	priv->rcdev.ops = &basic_reset_ops;
	priv->rcdev.of_node = pdev->dev.of_node;
	priv->rcdev.nr_resets = 32;

	return reset_controller_register(&priv->rcdev);
}

static const struct of_device_id basic_reset_dt_match[] = {
	{ .compatible = "reset-basic" },
	{ },
};

static struct platform_driver basic_reset_driver = {
	.probe	= basic_reset_probe,
	.driver	= {
		.name = "basic-reset",
		.of_match_table = basic_reset_dt_match,
	},
};
builtin_platform_driver(basic_reset_driver);
