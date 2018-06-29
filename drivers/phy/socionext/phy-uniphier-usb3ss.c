// SPDX-License-Identifier: GPL-2.0
/*
 * phy-uniphier-usb3ss.c - SS-PHY driver for Socionext UniPhier USB3 controller
 * Copyright 2015-2018 Socionext Inc.
 * Author:
 *	Kunihiko Hayashi <hayashi.kunihiko@socionext.com>
 * Contributors:
 *      Motoya Tanigawa <tanigawa.motoya@socionext.com>
 *      Masami Hiramatsu <masami.hiramatsu@linaro.org>
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#define SSPHY_TESTI		0x0
#define SSPHY_TESTO		0x4
#define TESTI_DAT_MASK		GENMASK(13, 6)
#define TESTI_ADR_MASK		GENMASK(5, 1)
#define TESTI_WR_EN		BIT(0)

#define MAX_CLKS	3
#define MAX_RSTS	2
#define MAX_PHY_PARAMS	7

struct uniphier_u3ssphy_param {
	u32 addr;
	u32 mask;
	u32 val;
};

struct uniphier_u3ssphy_priv {
	struct device *dev;
	void __iomem *base;
	int nclks;
	struct clk *clk[MAX_CLKS], *clk_phy, *clk_phy_ext;
	int nrsts;
	struct reset_control *rst[MAX_RSTS], *rst_phy;
	const struct uniphier_u3ssphy_soc_data *data;
};

struct uniphier_u3ssphy_soc_data {
	const char *clock_names[MAX_CLKS];
	const char *reset_names[MAX_RSTS];
	int nparams;
	const struct uniphier_u3ssphy_param param[MAX_PHY_PARAMS];
	bool is_legacy;
};

static void uniphier_u3ssphy_testio_write(struct uniphier_u3ssphy_priv *priv,
					  u32 data)
{
	/* need to read TESTO twice after accessing TESTI */
	writel(data, priv->base + SSPHY_TESTI);
	readl(priv->base + SSPHY_TESTI);
	readl(priv->base + SSPHY_TESTI);
}

static void uniphier_u3ssphy_set_param(struct uniphier_u3ssphy_priv *priv,
				       const struct uniphier_u3ssphy_param *p)
{
	u32 val, val_prev;

	/* read previous data */
	val  = FIELD_PREP(TESTI_DAT_MASK, 1);
	val |= FIELD_PREP(TESTI_ADR_MASK, p->addr);
	uniphier_u3ssphy_testio_write(priv, val);
	val_prev = readl(priv->base + SSPHY_TESTO);

	/* update value */
	val  = FIELD_PREP(TESTI_DAT_MASK,
			  (val_prev & ~p->mask) | (p->val & p->mask));
	val |= FIELD_PREP(TESTI_ADR_MASK, p->addr);
	uniphier_u3ssphy_testio_write(priv, val);
	uniphier_u3ssphy_testio_write(priv, val | TESTI_WR_EN);
	uniphier_u3ssphy_testio_write(priv, val);

	/* read current data as dummy */
	val  = FIELD_PREP(TESTI_DAT_MASK, 1);
	val |= FIELD_PREP(TESTI_ADR_MASK, p->addr);
	uniphier_u3ssphy_testio_write(priv, val);
	readl(priv->base + SSPHY_TESTO);
}

static void
uniphier_u3ssphy_legacy_testio_write(struct uniphier_u3ssphy_priv *priv,
				     u32 data)
{
	int i;

	/* need to read TESTO 10 times after accessing TESTI */
	writel(data, priv->base + SSPHY_TESTI);
	for (i = 0; i < 10; i++)
		readl(priv->base + SSPHY_TESTO);
}

static void
uniphier_u3ssphy_legacy_set_param(struct uniphier_u3ssphy_priv *priv,
				  const struct uniphier_u3ssphy_param *p)
{
	u32 val;

	val  = FIELD_PREP(TESTI_DAT_MASK, p->val & p->mask);
	val |= FIELD_PREP(TESTI_ADR_MASK, p->addr);
	uniphier_u3ssphy_legacy_testio_write(priv, val);
	uniphier_u3ssphy_legacy_testio_write(priv, val | TESTI_WR_EN);
	uniphier_u3ssphy_legacy_testio_write(priv, val);
}

static int uniphier_u3ssphy_init(struct phy *phy)
{
	struct uniphier_u3ssphy_priv *priv = phy_get_drvdata(phy);
	int i, ret;

	ret = clk_prepare_enable(priv->clk_phy_ext);
	if (ret)
		return ret;

	ret = clk_prepare_enable(priv->clk_phy);
	if (ret)
		goto out_clk_ext_disable;

	ret = reset_control_deassert(priv->rst_phy);
	if (ret)
		goto out_clk_disable;

	for (i = 0; i < priv->data->nparams; i++)
		if (priv->data->is_legacy)
			uniphier_u3ssphy_legacy_set_param(priv,
							&priv->data->param[i]);
		else
			uniphier_u3ssphy_set_param(priv,
						   &priv->data->param[i]);

	return 0;

out_clk_disable:
	clk_disable_unprepare(priv->clk_phy);
out_clk_ext_disable:
	clk_disable_unprepare(priv->clk_phy_ext);

	return ret;
}

static int uniphier_u3ssphy_exit(struct phy *phy)
{
	struct uniphier_u3ssphy_priv *priv = phy_get_drvdata(phy);

	reset_control_assert(priv->rst_phy);
	clk_disable_unprepare(priv->clk_phy);
	clk_disable_unprepare(priv->clk_phy_ext);

	return 0;
}

static const struct phy_ops uniphier_u3ssphy_ops = {
	.init           = uniphier_u3ssphy_init,
	.exit           = uniphier_u3ssphy_exit,
	.owner          = THIS_MODULE,
};

static int uniphier_u3ssphy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uniphier_u3ssphy_priv *priv;
	struct phy_provider *phy_provider;
	struct resource *res;
	struct phy *phy;
	struct clk *clk;
	struct reset_control *rst;
	const char *name;
	int i, ret, nc, nr;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->data = of_device_get_match_data(dev);
	if (WARN_ON(!priv->data ||
		    priv->data->nparams > MAX_PHY_PARAMS))
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	for (i = 0; i < MAX_CLKS; i++) {
		name = priv->data->clock_names[i];
		if (!name)
			break;
		clk = devm_clk_get(dev, name);
		/* "phy-ext" is optional */
		if (!strcmp(name, "phy-ext")) {
			if (PTR_ERR(clk) == -ENOENT)
				clk = NULL;
			priv->clk_phy_ext = clk;
		} else if (!strcmp(name, "phy")) {
			priv->clk_phy = clk;
		} else {
			priv->clk[priv->nclks++] = clk;
		}
		if (IS_ERR(clk))
			return PTR_ERR(clk);
	}

	for (i = 0; i < MAX_RSTS; i++) {
		name = priv->data->reset_names[i];
		if (!name)
			break;
		rst = devm_reset_control_get_shared(dev, name);
		if (IS_ERR(rst))
			return PTR_ERR(rst);

		if (!strcmp(name, "phy"))
			priv->rst_phy = rst;
		else
			priv->rst[priv->nrsts++] = rst;
	}

	phy = devm_phy_create(dev, dev->of_node, &uniphier_u3ssphy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

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

	platform_set_drvdata(pdev, priv);
	phy_set_drvdata(phy, priv);
	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider)) {
		ret = PTR_ERR(phy_provider);
		goto out_rst_assert;
	}

	return 0;

out_rst_assert:
	while (nr--)
		reset_control_assert(priv->rst[nr]);
out_clk_disable:
	while (nc--)
		clk_disable_unprepare(priv->clk[nc]);

	return ret;
}

static int uniphier_u3ssphy_remove(struct platform_device *pdev)
{
	struct uniphier_u3ssphy_priv *priv = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < priv->nrsts; i++)
		reset_control_assert(priv->rst[i]);
	for (i = 0; i < priv->nclks; i++)
		clk_disable_unprepare(priv->clk[i]);

	return 0;
}

static const struct uniphier_u3ssphy_soc_data uniphier_pro4_data = {
	.clock_names = { "gio", "link", },
	.reset_names = { "gio", "link", },
	.nparams = 7,
	.param = {
		{  0, 0x0f, 0x04 },
		{  3, 0x0f, 0x08 },
		{  5, 0x0f, 0x08 },
		{  6, 0x0f, 0x07 },
		{  7, 0x0f, 0x02 },
		{ 28, 0x0f, 0x0a },
		{ 30, 0x0f, 0x09 },
	},
	.is_legacy = true,
};

static const struct uniphier_u3ssphy_soc_data uniphier_pxs2_data = {
	.clock_names = { "link", "phy", },
	.reset_names = { "link", "phy", },
	.nparams = 7,
	.param = {
		{  7, 0x0f, 0x0a },
		{  8, 0x0f, 0x03 },
		{  9, 0x0f, 0x05 },
		{ 11, 0x0f, 0x09 },
		{ 13, 0x60, 0x40 },
		{ 27, 0x07, 0x07 },
		{ 28, 0x03, 0x01 },
	},
	.is_legacy = false,
};

static const struct uniphier_u3ssphy_soc_data uniphier_ld20_data = {
	.clock_names = { "link", "phy", },
	.reset_names = { "link", "phy", },
	.nparams = 3,
	.param = {
		{  7, 0x0f, 0x06 },
		{ 13, 0xff, 0xcc },
		{ 26, 0xf0, 0x50 },
	},
	.is_legacy = false,
};

static const struct uniphier_u3ssphy_soc_data uniphier_pxs3_data = {
	.clock_names = { "link", "phy", "phy-ext", },
	.reset_names = { "link", "phy", },
	.nparams = 3,
	.param = {
		{  7, 0x0f, 0x06 },
		{ 13, 0xff, 0xcc },
		{ 26, 0xf0, 0x50 },
	},
	.is_legacy = false,
};

static const struct of_device_id uniphier_u3ssphy_match[] = {
	{
		.compatible = "socionext,uniphier-pro4-usb3-ssphy",
		.data = &uniphier_pro4_data,
	},
	{
		.compatible = "socionext,uniphier-pxs2-usb3-ssphy",
		.data = &uniphier_pxs2_data,
	},
	{
		.compatible = "socionext,uniphier-ld20-usb3-ssphy",
		.data = &uniphier_ld20_data,
	},
	{
		.compatible = "socionext,uniphier-pxs3-usb3-ssphy",
		.data = &uniphier_pxs3_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, uniphier_u3ssphy_match);

static struct platform_driver uniphier_u3ssphy_driver = {
	.probe = uniphier_u3ssphy_probe,
	.remove = uniphier_u3ssphy_remove,
	.driver	= {
		.name = "uniphier-usb3-ssphy",
		.of_match_table	= uniphier_u3ssphy_match,
	},
};

module_platform_driver(uniphier_u3ssphy_driver);

MODULE_AUTHOR("Kunihiko Hayashi <hayashi.kunihiko@socionext.com>");
MODULE_DESCRIPTION("UniPhier SS-PHY driver for USB3 controller");
MODULE_LICENSE("GPL v2");
