// SPDX-License-Identifier: GPL-2.0
/*
 * reset-uniphier-usb3.c - USB3 reset driver for UniPhier
 * Copyright 2018 Socionext Inc.
 * Author: Kunihiko Hayashi <hayashi.kunihiko@socionext.com>
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "reset-simple.h"

#define MAX_CLKS	2
#define MAX_RSTS	2

struct uniphier_usb3_reset_soc_data {
	const char *clock_names[MAX_CLKS];
	const char *reset_names[MAX_RSTS];
};

struct uniphier_usb3_reset_priv {
	int nclks;
	struct clk *clk[MAX_CLKS];
	int nrsts;
	struct reset_control *rst[MAX_RSTS];
	const struct uniphier_usb3_reset_soc_data *data;
};

static int uniphier_usb3_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uniphier_usb3_reset_priv *priv;
	struct reset_simple_data *rst_data;
	struct resource *res;
	resource_size_t size;
	const char *name;
	int i, ret, nc, nr;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->data = of_device_get_match_data(dev);
	if (WARN_ON(!priv->data))
		return -EINVAL;

	rst_data = devm_kzalloc(dev, sizeof(*rst_data), GFP_KERNEL);
	if (!rst_data)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	size = resource_size(res);
	rst_data->membase = devm_ioremap_resource(dev, res);
	if (IS_ERR(rst_data->membase))
		return PTR_ERR(rst_data->membase);

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

	spin_lock_init(&rst_data->lock);
	rst_data->rcdev.owner = THIS_MODULE;
	rst_data->rcdev.nr_resets = size * BITS_PER_BYTE;
	rst_data->rcdev.ops = &reset_simple_ops;
	rst_data->rcdev.of_node = dev->of_node;
	rst_data->active_low = true;

	platform_set_drvdata(pdev, priv);

	ret = devm_reset_controller_register(dev, &rst_data->rcdev);
	if (ret)
		goto out_rst_assert;

	return 0;

out_rst_assert:
	while (nr--)
		reset_control_assert(priv->rst[nr]);
out_clk_disable:
	while (nc--)
		clk_disable_unprepare(priv->clk[nc]);

	return ret;
}

static int uniphier_usb3_reset_remove(struct platform_device *pdev)
{
	struct uniphier_usb3_reset_priv *priv = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < priv->nrsts; i++)
		reset_control_assert(priv->rst[i]);
	for (i = 0; i < priv->nclks; i++)
		clk_disable_unprepare(priv->clk[i]);

	return 0;
}

static const struct uniphier_usb3_reset_soc_data uniphier_pro4_data = {
	.clock_names = { "gio", "link", },
	.reset_names = { "gio", "link", },
};

static const struct uniphier_usb3_reset_soc_data uniphier_pxs2_data = {
	.clock_names = { "link", },
	.reset_names = { "link", },
};

static const struct uniphier_usb3_reset_soc_data uniphier_ld20_data = {
	.clock_names = { "link", },
	.reset_names = { "link", },
};

static const struct uniphier_usb3_reset_soc_data uniphier_pxs3_data = {
	.clock_names = { "link", },
	.reset_names = { "link", },
};

static const struct of_device_id uniphier_usb3_reset_match[] = {
	{
		.compatible = "socionext,uniphier-pro4-usb3-reset",
		.data = &uniphier_pro4_data,
	},
	{
		.compatible = "socionext,uniphier-pxs2-usb3-reset",
		.data = &uniphier_pxs2_data,
	},
	{
		.compatible = "socionext,uniphier-ld20-usb3-reset",
		.data = &uniphier_ld20_data,
	},
	{
		.compatible = "socionext,uniphier-pxs3-usb3-reset",
		.data = &uniphier_pxs3_data,
	},
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, uniphier_usb3_reset_match);

static struct platform_driver uniphier_usb3_reset_driver = {
	.probe = uniphier_usb3_reset_probe,
	.remove = uniphier_usb3_reset_remove,
	.driver = {
		.name = "uniphier-usb3-reset",
		.of_match_table = uniphier_usb3_reset_match,
	},
};
module_platform_driver(uniphier_usb3_reset_driver);

MODULE_AUTHOR("Kunihiko Hayashi <hayashi.kunihiko@socionext.com>");
MODULE_DESCRIPTION("UniPhier USB3 Reset Driver");
MODULE_LICENSE("GPL");
