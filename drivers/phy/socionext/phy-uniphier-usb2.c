// SPDX-License-Identifier: GPL-2.0
/*
 * phy-uniphier-usb2.c - PHY driver for UniPhier USB2 controller
 * Copyright 2015-2018 Socionext Inc.
 * Author:
 *	Kunihiko Hayashi <hayashi.kunihiko@socionext.com>
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define PHY_PARAMS	2

struct uniphier_u2phy_soc_data {
	struct {
		u32 addr;
		u32 val;
	} param[PHY_PARAMS];
};

struct uniphier_u2phy_priv {
	struct regmap *regmap;
	struct phy *phy;
	const struct uniphier_u2phy_soc_data *data;
	struct uniphier_u2phy_priv *next;
};

static int uniphier_u2phy_init(struct phy *phy)
{
	struct uniphier_u2phy_priv *priv = phy_get_drvdata(phy);
	int i;

	if (!priv->data)
		return 0;

	for (i = 0; i < PHY_PARAMS; i++)
		regmap_write(priv->regmap,
			     priv->data->param[i].addr,
			     priv->data->param[i].val);

	return 0;
}

static struct phy *uniphier_u2phy_xlate(struct device *dev,
					struct of_phandle_args *args)
{
	struct uniphier_u2phy_priv *priv = dev_get_drvdata(dev);

	while (priv && args->np != priv->phy->dev.of_node)
		priv = priv->next;

	if (!priv) {
		dev_err(dev, "Failed to find appropriate phy\n");
		return ERR_PTR(-EINVAL);
	}

	return priv->phy;
}

static const struct phy_ops uniphier_u2phy_ops = {
	.init  = uniphier_u2phy_init,
	.owner = THIS_MODULE,
};

static int uniphier_u2phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *parent, *child;
	struct uniphier_u2phy_priv *priv = NULL, *next = NULL;
	struct phy_provider *phy_provider;
	struct regmap *regmap;
	struct phy *phy;
	const struct uniphier_u2phy_soc_data *data;
	int ret, data_idx, ndatas;

	data = of_device_get_match_data(dev);
	if (WARN_ON(!data))
		return -EINVAL;

	/* get number of data */
	for (ndatas = 0; data[ndatas].param[0].addr; ndatas++)
		;

	parent = of_get_parent(dev->of_node);
	regmap = syscon_node_to_regmap(parent);
	of_node_put(parent);
	if (IS_ERR(regmap)) {
		dev_err(dev, "Failed to get regmap\n");
		return PTR_ERR(regmap);
	}

	for_each_child_of_node(dev->of_node, child) {
		priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
		if (!priv) {
			ret = -ENOMEM;
			goto out_put_child;
		}
		priv->regmap = regmap;

		phy = devm_phy_create(dev, child, &uniphier_u2phy_ops);
		if (IS_ERR(phy)) {
			dev_err(dev, "Failed to create phy\n");
			ret = PTR_ERR(phy);
			goto out_put_child;
		}
		priv->phy = phy;

		ret = of_property_read_u32(child, "reg", &data_idx);
		if (ret) {
			dev_err(dev, "Failed to get reg property\n");
			goto out_put_child;
		}

		if (data_idx < ndatas)
			priv->data = &data[data_idx];
		else
			dev_warn(dev, "No phy configuration: %s\n",
				 child->full_name);

		phy_set_drvdata(phy, priv);
		priv->next = next;
		next = priv;
	}

	dev_set_drvdata(dev, priv);
	phy_provider = devm_of_phy_provider_register(dev,
						     uniphier_u2phy_xlate);
	if (IS_ERR(phy_provider))
		return PTR_ERR(phy_provider);

	return 0;

out_put_child:
	of_node_put(child);

	return ret;
}

static const struct uniphier_u2phy_soc_data uniphier_pro4_data[] = {
	{
		.param = {
			{ 0x500, 0x05142400 },
			{ 0x50c, 0x00010010 },
		},
	},
	{
		.param = {
			{ 0x508, 0x05142400 },
			{ 0x50c, 0x00010010 },
		},
	},
	{
		.param = {
			{ 0x510, 0x05142400 },
			{ 0x51c, 0x00010010 },
		},
	},
	{
		.param = {
			{ 0x518, 0x05142400 },
			{ 0x51c, 0x00010010 },
		},
	},
	{ /* sentinel */ }
};

static const struct uniphier_u2phy_soc_data uniphier_ld11_data[] = {
	{
		.param = {
			{ 0x500, 0x82280000 },
			{ 0x504, 0x00000106 },
		},
	},
	{
		.param = {
			{ 0x508, 0x82280000 },
			{ 0x50c, 0x00000106 },
		},
	},
	{
		.param = {
			{ 0x510, 0x82280000 },
			{ 0x514, 0x00000106 },
		},
	},
	{ /* sentinel */ }
};

static const struct of_device_id uniphier_u2phy_match[] = {
	{
		.compatible = "socionext,uniphier-pro4-usb2-phy",
		.data = &uniphier_pro4_data,
	},
	{
		.compatible = "socionext,uniphier-ld11-usb2-phy",
		.data = &uniphier_ld11_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, uniphier_u2phy_match);

static struct platform_driver uniphier_u2phy_driver = {
	.probe = uniphier_u2phy_probe,
	.driver = {
		.name = "uniphier-usb2-phy",
		.of_match_table = uniphier_u2phy_match,
	},
};
module_platform_driver(uniphier_u2phy_driver);

MODULE_AUTHOR("Kunihiko Hayashi <hayashi.kunihiko@socionext.com>");
MODULE_DESCRIPTION("UniPhier PHY driver for USB2 controller");
MODULE_LICENSE("GPL v2");
