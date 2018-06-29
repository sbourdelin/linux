// SPDX-License-Identifier: GPL-2.0
/*
 * Regulator controller driver for UniPhier SoC
 * Copyright 2018 Socionext Inc.
 * Author: Kunihiko Hayashi <hayashi.kunihiko@socionext.com>
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/reset.h>

#define MAX_CLKS	2
#define MAX_RSTS	2

struct uniphier_regulator_soc_data {
	const char *clock_names[MAX_CLKS];
	const char *reset_names[MAX_RSTS];
	const struct regulator_desc *desc;
};

struct uniphier_regulator_priv {
	void __iomem *base;
	int nclks;
	struct clk *clk[MAX_CLKS];
	int nrsts;
	struct reset_control *rst[MAX_RSTS];
	const struct uniphier_regulator_soc_data *data;
};

static int uniphier_regulator_enable(struct regulator_dev *rdev)
{
	struct uniphier_regulator_priv *priv = rdev_get_drvdata(rdev);
	u32 val;

	val = readl_relaxed(priv->base + rdev->desc->enable_reg);
	val &= ~rdev->desc->enable_mask;
	val |= rdev->desc->enable_val;
	writel_relaxed(val, priv->base + rdev->desc->enable_reg);

	return 0;
}

static int uniphier_regulator_disable(struct regulator_dev *rdev)
{
	struct uniphier_regulator_priv *priv = rdev_get_drvdata(rdev);
	u32 val;

	val = readl_relaxed(priv->base + rdev->desc->enable_reg);
	val &= ~rdev->desc->enable_mask;
	val |= rdev->desc->disable_val;
	writel_relaxed(val, priv->base + rdev->desc->enable_reg);

	return 0;
}

static int uniphier_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct uniphier_regulator_priv *priv = rdev_get_drvdata(rdev);
	u32 val;
	int ret = -EINVAL;

	val = readl(priv->base + rdev->desc->enable_reg);
	val &= rdev->desc->enable_mask;

	if (val == rdev->desc->enable_val)
		ret = 1;
	else if (val == rdev->desc->disable_val)
		ret = 0;

	return ret;
}

static struct regulator_ops uniphier_regulator_ops = {
	.enable     = uniphier_regulator_enable,
	.disable    = uniphier_regulator_disable,
	.is_enabled = uniphier_regulator_is_enabled,
};

static int uniphier_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uniphier_regulator_priv *priv;
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	struct resource *res;
	const char *name;
	int i, ret, nc, nr;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->data = of_device_get_match_data(dev);
	if (WARN_ON(!priv->data))
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	for (i = 0; i < MAX_CLKS; i++) {
		name = priv->data->clock_names[i];
		if (!name)
			break;
		priv->clk[i] = devm_clk_get(dev, name);
		if (IS_ERR(priv->clk[i]))
			return PTR_ERR(priv->clk[i]);
		priv->nclks++;
	}

	for (i = 0; i < MAX_RSTS; i++) {
		name = priv->data->reset_names[i];
		if (!name)
			break;
		priv->rst[i] = devm_reset_control_get_shared(dev, name);
		if (IS_ERR(priv->rst[i]))
			return PTR_ERR(priv->rst[i]);
		priv->nrsts++;
	}

	for (nc = 0; nc < priv->nclks; nc++) {
		ret = clk_prepare_enable(priv->clk[nc]);
		if (ret)
			goto out_clk_disable;
	}

	for (nr = 0; nr < priv->nrsts; nr++) {
		ret = reset_control_deassert(priv->rst[nr]);
		if (ret)
			goto out_rst_assert;
	}

	/* Register UniPhier regulator */
	config.dev = dev;
	config.driver_data = priv;
	config.of_node = dev->of_node;
	config.init_data = of_get_regulator_init_data(dev, dev->of_node,
						      priv->data->desc);
	rdev = devm_regulator_register(dev, priv->data->desc, &config);
	if (IS_ERR(rdev)) {
		ret = PTR_ERR(rdev);
		goto out_rst_assert;
	}

	platform_set_drvdata(pdev, priv);

	return 0;

out_rst_assert:
	while (nr--)
		reset_control_assert(priv->rst[nr]);
out_clk_disable:
	while (nc--)
		clk_disable_unprepare(priv->clk[nc]);

	return ret;
}

static int uniphier_regulator_remove(struct platform_device *pdev)
{
	struct uniphier_regulator_priv *priv = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < priv->nrsts; i++)
		reset_control_assert(priv->rst[i]);
	for (i = 0; i < priv->nclks; i++)
		clk_disable_unprepare(priv->clk[i]);

	return 0;
}

/* USB3 controller data */
#define USB3VBUS_OFFSET		0x0
#define USB3VBUS_REG		BIT(4)
#define USB3VBUS_REG_EN		BIT(3)
static const struct regulator_desc uniphier_usb3_regulator_desc = {
	.name = "vbus",
	.of_match = of_match_ptr("vbus"),
	.ops = &uniphier_regulator_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.enable_reg  = USB3VBUS_OFFSET,
	.enable_mask = USB3VBUS_REG_EN | USB3VBUS_REG,
	.enable_val  = USB3VBUS_REG_EN | USB3VBUS_REG,
	.disable_val = USB3VBUS_REG_EN,
};

static const struct uniphier_regulator_soc_data uniphier_pro4_usb3_data = {
	.clock_names = { "gio", "link", },
	.reset_names = { "gio", "link", },
	.desc = &uniphier_usb3_regulator_desc,
};

static const struct uniphier_regulator_soc_data uniphier_pxs2_usb3_data = {
	.clock_names = { "link", },
	.reset_names = { "link", },
	.desc = &uniphier_usb3_regulator_desc,
};

static const struct uniphier_regulator_soc_data uniphier_ld20_usb3_data = {
	.clock_names = { "link", },
	.reset_names = { "link", },
	.desc = &uniphier_usb3_regulator_desc,
};

static const struct uniphier_regulator_soc_data uniphier_pxs3_usb3_data = {
	.clock_names = { "link", },
	.reset_names = { "link", },
	.desc = &uniphier_usb3_regulator_desc,
};

static const struct of_device_id uniphier_regulator_match[] = {
	/* USB VBUS */
	{
		.compatible = "socionext,uniphier-pro4-usb3-regulator",
		.data = &uniphier_pro4_usb3_data,
	},
	{
		.compatible = "socionext,uniphier-pxs2-usb3-regulator",
		.data = &uniphier_pxs2_usb3_data,
	},
	{
		.compatible = "socionext,uniphier-ld20-usb3-regulator",
		.data = &uniphier_ld20_usb3_data,
	},
	{
		.compatible = "socionext,uniphier-pxs3-usb3-regulator",
		.data = &uniphier_pxs3_usb3_data,
	},
	{ /* Sentinel */ },
};

static struct platform_driver uniphier_regulator_driver = {
	.probe = uniphier_regulator_probe,
	.remove = uniphier_regulator_remove,
	.driver = {
		.name  = "uniphier-regulator",
		.of_match_table = uniphier_regulator_match,
	},
};
module_platform_driver(uniphier_regulator_driver);

MODULE_AUTHOR("Kunihiko Hayashi <hayashi.kunihiko@socionext.com>");
MODULE_DESCRIPTION("UniPhier Regulator Controller Driver");
MODULE_LICENSE("GPL");
