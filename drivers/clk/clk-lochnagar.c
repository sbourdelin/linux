// SPDX-License-Identifier: GPL-2.0
/*
 * Lochnagar clock control
 *
 * Copyright 2017-2018 Cirrus Logic Inc.
 *
 * Author: Charles Keepax <ckeepax@opensource.cirrus.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <linux/mfd/lochnagar.h>
#include <dt-bindings/clk/lochnagar.h>

#define LOCHNAGAR_NUM_CLOCKS	(LOCHNAGAR_SPDIF_CLKOUT + 1)

enum lochnagar_clk_type {
	LOCHNAGAR_CLK_TYPE_UNUSED,
	LOCHNAGAR_CLK_TYPE_FIXED,
	LOCHNAGAR_CLK_TYPE_REGMAP,
};

struct lochnagar_regmap_clk {
	unsigned int cfg_reg;
	unsigned int ena_mask;
	unsigned int dir_mask;

	unsigned int src_reg;
	unsigned int src_mask;
};

struct lochnagar_fixed_clk {
	unsigned int rate;
};

struct lochnagar_clk {
	struct lochnagar_clk_priv *priv;
	struct clk_hw hw;

	const char * const name;

	enum lochnagar_clk_type type;
	union {
		struct lochnagar_fixed_clk fixed;
		struct lochnagar_regmap_clk regmap;
	};
};

struct lochnagar_clk_priv {
	struct device *dev;
	struct lochnagar *lochnagar;

	const char **parents;
	unsigned int nparents;

	struct lochnagar_clk lclks[LOCHNAGAR_NUM_CLOCKS];

	struct clk *clks[LOCHNAGAR_NUM_CLOCKS];
	struct clk_onecell_data of_clks;
};

static const char * const lochnagar1_clk_parents[] = {
	"ln-none",
	"ln-spdif-mclk",
	"ln-psia1-mclk",
	"ln-psia2-mclk",
	"ln-cdc-clkout",
	"ln-dsp-clkout",
	"ln-pmic-32k",
	"ln-gf-mclk1",
	"ln-gf-mclk3",
	"ln-gf-mclk2",
	"ln-gf-mclk4",
};

static const char * const lochnagar2_clk_parents[] = {
	"ln-none",
	"ln-cdc-clkout",
	"ln-dsp-clkout",
	"ln-pmic-32k",
	"ln-spdif-mclk",
	"ln-clk-12m",
	"ln-clk-11m",
	"ln-clk-24m",
	"ln-clk-22m",
	"ln-reserved",
	"ln-usb-clk-24m",
	"ln-gf-mclk1",
	"ln-gf-mclk3",
	"ln-gf-mclk2",
	"ln-psia1-mclk",
	"ln-psia2-mclk",
	"ln-spdif-clkout",
	"ln-adat-clkout",
	"ln-usb-clk-12m",
};

#define LN_CLK_FIXED(ID, NAME, RATE) \
	[LOCHNAGAR_##ID] = { \
		.name = NAME, .type = LOCHNAGAR_CLK_TYPE_FIXED, \
		{ .fixed.rate = RATE, }, \
	}

#define LN1_CLK_REGMAP(ID, NAME, REG, ...) \
	[LOCHNAGAR_##ID] = { \
		.name = NAME, .type = LOCHNAGAR_CLK_TYPE_REGMAP, \
		{ .regmap = { \
			__VA_ARGS__ \
			.cfg_reg = LOCHNAGAR1_##REG, \
			.ena_mask = LOCHNAGAR1_##ID##_ENA_MASK, \
			.src_reg = LOCHNAGAR1_##ID##_SEL, \
			.src_mask = LOCHNAGAR1_SRC_MASK, \
		}, }, \
	}

#define LN2_CLK_REGMAP(ID, NAME) \
	[LOCHNAGAR_##ID] = { \
		.name = NAME, .type = LOCHNAGAR_CLK_TYPE_REGMAP, \
		{ .regmap = { \
			.cfg_reg = LOCHNAGAR2_##ID##_CTRL, \
			.src_reg = LOCHNAGAR2_##ID##_CTRL, \
			.ena_mask = LOCHNAGAR2_CLK_ENA_MASK, \
			.dir_mask = LOCHNAGAR2_CLK_DIR_MASK, \
			.src_mask = LOCHNAGAR2_CLK_SRC_MASK, \
		}, }, \
	}

static const struct lochnagar_clk lochnagar1_clks[LOCHNAGAR_NUM_CLOCKS] = {
	LN1_CLK_REGMAP(CDC_MCLK1,      "ln-cdc-mclk1",  CDC_AIF_CTRL2),
	LN1_CLK_REGMAP(CDC_MCLK2,      "ln-cdc-mclk2",  CDC_AIF_CTRL2),
	LN1_CLK_REGMAP(DSP_CLKIN,      "ln-dsp-clkin",  DSP_AIF),
	LN1_CLK_REGMAP(GF_CLKOUT1,     "ln-gf-clkout1", GF_AIF1),

	LN_CLK_FIXED(PMIC_32K,         "ln-pmic-32k",   32768),
};

static const struct lochnagar_clk lochnagar2_clks[LOCHNAGAR_NUM_CLOCKS] = {
	LN2_CLK_REGMAP(CDC_MCLK1,      "ln-cdc-mclk1"),
	LN2_CLK_REGMAP(CDC_MCLK2,      "ln-cdc-mclk2"),
	LN2_CLK_REGMAP(DSP_CLKIN,      "ln-dsp-clkin"),
	LN2_CLK_REGMAP(GF_CLKOUT1,     "ln-gf-clkout1"),
	LN2_CLK_REGMAP(GF_CLKOUT2,     "ln-gf-clkout2"),
	LN2_CLK_REGMAP(PSIA1_MCLK,     "ln-psia1-mclk"),
	LN2_CLK_REGMAP(PSIA2_MCLK,     "ln-psia2-mclk"),
	LN2_CLK_REGMAP(SPDIF_MCLK,     "ln-spdif-mclk"),
	LN2_CLK_REGMAP(ADAT_MCLK,      "ln-adat-mclk"),
	LN2_CLK_REGMAP(SOUNDCARD_MCLK, "ln-soundcard-mclk"),

	LN_CLK_FIXED(PMIC_32K,         "ln-pmic-32k",    32768),
	LN_CLK_FIXED(CLK_12M,          "ln-clk-12m",     12288000),
	LN_CLK_FIXED(CLK_11M,          "ln-clk-11m",     11298600),
	LN_CLK_FIXED(CLK_24M,          "ln-clk-24m",     24576000),
	LN_CLK_FIXED(CLK_22M,          "ln-clk-22m",     22579200),
	LN_CLK_FIXED(USB_CLK_24M,      "ln-usb-clk-24m", 24000000),
	LN_CLK_FIXED(USB_CLK_12M,      "ln-usb-clk-12m", 12000000),
};

static inline struct lochnagar_clk *lochnagar_hw_to_lclk(struct clk_hw *hw)
{
	return container_of(hw, struct lochnagar_clk, hw);
}

static int lochnagar_regmap_prepare(struct clk_hw *hw)
{
	struct lochnagar_clk *lclk = lochnagar_hw_to_lclk(hw);
	struct lochnagar_clk_priv *priv = lclk->priv;
	struct regmap *regmap = priv->lochnagar->regmap;
	int ret;

	dev_dbg(priv->dev, "Prepare %s\n", lclk->name);

	if (!lclk->regmap.ena_mask)
		return 0;

	ret = regmap_update_bits(regmap, lclk->regmap.cfg_reg,
				 lclk->regmap.ena_mask,
				 lclk->regmap.ena_mask);
	if (ret < 0)
		dev_err(priv->dev, "Failed to prepare %s: %d\n",
			lclk->name, ret);

	return ret;
}

static void lochnagar_regmap_unprepare(struct clk_hw *hw)
{
	struct lochnagar_clk *lclk = lochnagar_hw_to_lclk(hw);
	struct lochnagar_clk_priv *priv = lclk->priv;
	struct regmap *regmap = priv->lochnagar->regmap;
	int ret;

	dev_dbg(priv->dev, "Unprepare %s\n", lclk->name);

	if (!lclk->regmap.ena_mask)
		return;

	ret = regmap_update_bits(regmap, lclk->regmap.cfg_reg,
				 lclk->regmap.ena_mask, 0);
	if (ret < 0)
		dev_err(priv->dev, "Failed to unprepare %s: %d\n",
			lclk->name, ret);
}

static int lochnagar_regmap_set_parent(struct clk_hw *hw, u8 index)
{
	struct lochnagar_clk *lclk = lochnagar_hw_to_lclk(hw);
	struct lochnagar_clk_priv *priv = lclk->priv;
	struct regmap *regmap = priv->lochnagar->regmap;
	int ret;

	dev_dbg(priv->dev, "Reparent %s to %s\n",
		lclk->name, priv->parents[index]);

	if (lclk->regmap.dir_mask) {
		ret = regmap_update_bits(regmap, lclk->regmap.cfg_reg,
					 lclk->regmap.dir_mask,
					 lclk->regmap.dir_mask);
		if (ret < 0) {
			dev_err(priv->dev, "Failed to set %s direction: %d\n",
				lclk->name, ret);
			return ret;
		}
	}

	ret = regmap_update_bits(regmap, lclk->regmap.src_reg,
				 lclk->regmap.src_mask, index);
	if (ret < 0)
		dev_err(priv->dev, "Failed to reparent %s: %d\n",
			lclk->name, ret);

	return ret;
}

static u8 lochnagar_regmap_get_parent(struct clk_hw *hw)
{
	struct lochnagar_clk *lclk = lochnagar_hw_to_lclk(hw);
	struct lochnagar_clk_priv *priv = lclk->priv;
	struct regmap *regmap = priv->lochnagar->regmap;
	unsigned int val;
	int ret;

	ret = regmap_read(regmap, lclk->regmap.src_reg, &val);
	if (ret < 0) {
		dev_err(priv->dev, "Failed to read parent of %s: %d\n",
			lclk->name, ret);
		return 0;
	}

	val &= lclk->regmap.src_mask;

	dev_dbg(priv->dev, "Parent of %s is %s\n",
		lclk->name, priv->parents[val]);

	return val;
}

static const struct clk_ops lochnagar_clk_regmap_ops = {
	.prepare = lochnagar_regmap_prepare,
	.unprepare = lochnagar_regmap_unprepare,
	.set_parent = lochnagar_regmap_set_parent,
	.get_parent = lochnagar_regmap_get_parent,
};

static int lochnagar_init_parents(struct lochnagar_clk_priv *priv)
{
	struct device_node *np = priv->lochnagar->dev->of_node;
	enum lochnagar_type type = priv->lochnagar->type;
	int i, j;

	switch (type) {
	case LOCHNAGAR1:
		memcpy(priv->lclks, lochnagar1_clks, sizeof(lochnagar1_clks));

		priv->nparents = ARRAY_SIZE(lochnagar1_clk_parents);
		priv->parents = devm_kmemdup(priv->dev, lochnagar1_clk_parents,
					     sizeof(lochnagar1_clk_parents),
					     GFP_KERNEL);
		break;
	case LOCHNAGAR2:
		memcpy(priv->lclks, lochnagar2_clks, sizeof(lochnagar2_clks));

		priv->nparents = ARRAY_SIZE(lochnagar2_clk_parents);
		priv->parents = devm_kmemdup(priv->dev, lochnagar2_clk_parents,
					     sizeof(lochnagar2_clk_parents),
					     GFP_KERNEL);
		break;
	default:
		dev_err(priv->dev, "Unknown Lochnagar type: %d\n", type);
		return -EINVAL;
	}

	if (!priv->parents)
		return -ENOMEM;

	for (i = 0; i < priv->nparents; i++) {
		j = of_property_match_string(np, "clock-names",
					     priv->parents[i]);
		if (j >= 0) {
			const char * const name = of_clk_get_parent_name(np, j);

			dev_dbg(priv->dev, "Set parent %s to %s\n",
				priv->parents[i], name);

			priv->parents[i] = name;
		}
	}

	return 0;
}

static int lochnagar_init_clks(struct lochnagar_clk_priv *priv)
{
	struct clk_init_data clk_init = {
		.ops = &lochnagar_clk_regmap_ops,
		.parent_names = priv->parents,
		.num_parents = priv->nparents,
	};
	struct lochnagar_clk *lclk;
	struct clk *clk;
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(priv->lclks); i++) {
		lclk = &priv->lclks[i];

		lclk->priv = priv;

		switch (lclk->type) {
		case LOCHNAGAR_CLK_TYPE_FIXED:
			clk = clk_register_fixed_rate(priv->dev, lclk->name,
						      NULL, 0,
						      lclk->fixed.rate);
			break;
		case LOCHNAGAR_CLK_TYPE_REGMAP:
			clk_init.name = lclk->name;
			lclk->hw.init = &clk_init;

			clk = devm_clk_register(priv->dev, &lclk->hw);
			break;
		default:
			continue;
		}

		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			dev_err(priv->dev, "Failed to register %s: %d\n",
				lclk->name, ret);
			return ret;
		}

		dev_dbg(priv->dev, "Registered %s\n", lclk->name);

		priv->clks[i] = clk;
	}

	return 0;
}

static int lochnagar_init_of_providers(struct lochnagar_clk_priv *priv)
{
	struct device *dev = priv->dev;
	int ret;

	priv->of_clks.clks = priv->clks;
	priv->of_clks.clk_num = ARRAY_SIZE(priv->clks);

	ret = of_clk_add_provider(priv->lochnagar->dev->of_node,
				  of_clk_src_onecell_get,
				  &priv->of_clks);
	if (ret < 0) {
		dev_err(dev, "Failed to register clock provider: %d\n", ret);
		return ret;
	}

	return 0;
}

static int lochnagar_clk_probe(struct platform_device *pdev)
{
	struct lochnagar *lochnagar = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct lochnagar_clk_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->lochnagar = lochnagar;

	ret = lochnagar_init_parents(priv);
	if (ret)
		return ret;

	ret = lochnagar_init_clks(priv);
	if (ret)
		return ret;

	ret = lochnagar_init_of_providers(priv);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	return 0;
}

static int lochnagar_clk_remove(struct platform_device *pdev)
{
	struct lochnagar_clk_priv *priv = platform_get_drvdata(pdev);
	int i;

	of_clk_del_provider(priv->lochnagar->dev->of_node);

	for (i = 0; i < ARRAY_SIZE(priv->lclks); i++) {
		switch (priv->lclks[i].type) {
		case LOCHNAGAR_CLK_TYPE_FIXED:
			clk_unregister_fixed_rate(priv->clks[i]);
			break;
		default:
			break;
		}
	}

	return 0;
}

static struct platform_driver lochnagar_clk_driver = {
	.driver = {
		.name = "lochnagar-clk",
	},

	.probe = lochnagar_clk_probe,
	.remove = lochnagar_clk_remove,
};
module_platform_driver(lochnagar_clk_driver);

MODULE_AUTHOR("Charles Keepax <ckeepax@opensource.cirrus.com>");
MODULE_DESCRIPTION("Clock driver for Cirrus Logic Lochnagar Board");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:lochnagar-clk");
