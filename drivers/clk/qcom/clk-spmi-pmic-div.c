/* Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/types.h>

#define REG_DIV_CTL1			0x43
#define DIV_CTL1_DIV_FACTOR_MASK	GENMASK(2, 0)

#define REG_EN_CTL			0x46
#define REG_EN_MASK			BIT(7)

#define ENABLE_DELAY_NS(cxo_ns, div)	((2 + 3 * div) * cxo_ns)
#define DISABLE_DELAY_NS(cxo_ns, div)	((3 * div) * cxo_ns)

#define CLK_SPMI_PMIC_DIV_OFFSET	1

#define CLKDIV_XO_DIV_1_0		0
#define CLKDIV_XO_DIV_1			1
#define CLKDIV_XO_DIV_2			2
#define CLKDIV_XO_DIV_4			3
#define CLKDIV_XO_DIV_8			4
#define CLKDIV_XO_DIV_16		5
#define CLKDIV_XO_DIV_32		6
#define CLKDIV_XO_DIV_64		7
#define CLKDIV_MAX_ALLOWED		8

struct clkdiv {
	struct regmap		*regmap;
	u16			base;
	spinlock_t		lock;

	/* clock properties */
	struct clk_hw		hw;
	unsigned int		div_factor;
	unsigned int		cxo_period_ns;
};

static inline struct clkdiv *to_clkdiv(struct clk_hw *hw)
{
	return container_of(hw, struct clkdiv, hw);
}

static inline unsigned int div_factor_to_div(unsigned int div_factor)
{
	if (div_factor == CLKDIV_XO_DIV_1_0)
		return 1;

	return 1 << (div_factor - CLK_SPMI_PMIC_DIV_OFFSET);
}

static inline unsigned int div_to_div_factor(unsigned int div)
{
	return min(ilog2(div) + CLK_SPMI_PMIC_DIV_OFFSET,
		   CLKDIV_MAX_ALLOWED - 1);
}

static bool is_spmi_pmic_clkdiv_enabled(struct clkdiv *clkdiv)
{
	unsigned int val = 0;

	regmap_read(clkdiv->regmap, clkdiv->base + REG_EN_CTL,
			 &val);
	return (val & REG_EN_MASK) ? true : false;
}

static int spmi_pmic_clkdiv_set_enable_state(struct clkdiv *clkdiv,
			bool enable_state)
{
	int rc;

	rc = regmap_update_bits(clkdiv->regmap, clkdiv->base + REG_EN_CTL,
				REG_EN_MASK,
				(enable_state == true) ? REG_EN_MASK : 0);
	if (rc)
		return rc;

	if (enable_state == true)
		ndelay(ENABLE_DELAY_NS(clkdiv->cxo_period_ns,
				div_factor_to_div(clkdiv->div_factor)));
	else
		ndelay(DISABLE_DELAY_NS(clkdiv->cxo_period_ns,
				div_factor_to_div(clkdiv->div_factor)));

	return rc;
}

static int spmi_pmic_clkdiv_config_freq_div(struct clkdiv *clkdiv,
			unsigned int div)
{
	unsigned int div_factor;
	unsigned long flags;
	bool enabled;
	int rc;

	div_factor = div_to_div_factor(div);

	spin_lock_irqsave(&clkdiv->lock, flags);

	enabled = is_spmi_pmic_clkdiv_enabled(clkdiv);
	if (enabled) {
		rc = spmi_pmic_clkdiv_set_enable_state(clkdiv, false);
		if (rc)
			goto fail;
	}

	rc = regmap_update_bits(clkdiv->regmap,
				clkdiv->base + REG_DIV_CTL1,
				DIV_CTL1_DIV_FACTOR_MASK, div_factor);
	if (rc)
		goto fail;

	clkdiv->div_factor = div_factor;

	if (enabled)
		rc = spmi_pmic_clkdiv_set_enable_state(clkdiv, true);

fail:
	spin_unlock_irqrestore(&clkdiv->lock, flags);
	return rc;
}

static int clk_spmi_pmic_div_enable(struct clk_hw *hw)
{
	struct clkdiv *clkdiv = to_clkdiv(hw);
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&clkdiv->lock, flags);
	rc = spmi_pmic_clkdiv_set_enable_state(clkdiv, true);
	spin_unlock_irqrestore(&clkdiv->lock, flags);

	return rc;
}

static void clk_spmi_pmic_div_disable(struct clk_hw *hw)
{
	struct clkdiv *clkdiv = to_clkdiv(hw);
	unsigned long flags;

	spin_lock_irqsave(&clkdiv->lock, flags);
	spmi_pmic_clkdiv_set_enable_state(clkdiv, false);
	spin_unlock_irqrestore(&clkdiv->lock, flags);
}

static long clk_spmi_pmic_div_round_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long *parent_rate)
{
	unsigned long new_rate;
	unsigned int div, div_factor;

	div = DIV_ROUND_UP(*parent_rate, rate);
	div_factor = div_to_div_factor(div);
	new_rate = *parent_rate / div_factor_to_div(div_factor);

	return new_rate;
}

static unsigned long clk_spmi_pmic_div_recalc_rate(struct clk_hw *hw,
			unsigned long parent_rate)
{
	struct clkdiv *clkdiv = to_clkdiv(hw);
	unsigned long rate;

	rate = parent_rate / div_factor_to_div(clkdiv->div_factor);

	return rate;
}

static int clk_spmi_pmic_div_set_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long parent_rate)
{
	struct clkdiv *clkdiv = to_clkdiv(hw);

	return spmi_pmic_clkdiv_config_freq_div(clkdiv, parent_rate / rate);
}

static const struct clk_ops clk_spmi_pmic_div_ops = {
	.enable = clk_spmi_pmic_div_enable,
	.disable = clk_spmi_pmic_div_disable,
	.set_rate = clk_spmi_pmic_div_set_rate,
	.recalc_rate = clk_spmi_pmic_div_recalc_rate,
	.round_rate = clk_spmi_pmic_div_round_rate,
};

struct spmi_pmic_div_clk_cc {
	struct clk_hw	**div_clks;
	int		nclks;
};

#define SPMI_PMIC_CLKDIV_MIN_INDEX	1

static struct clk_hw *spmi_pmic_div_clk_hw_get(struct of_phandle_args *clkspec,
				      void *data)
{
	struct spmi_pmic_div_clk_cc *clk_cc = data;
	unsigned int idx = (clkspec->args[0] - SPMI_PMIC_CLKDIV_MIN_INDEX);

	if (idx < 0 || idx >= clk_cc->nclks) {
		pr_err("%s: index value %u is invalid; allowed range: [%d, %d]\n",
		       __func__, SPMI_PMIC_CLKDIV_MIN_INDEX, clk_cc->nclks);
		return ERR_PTR(-EINVAL);
	}

	return clk_cc->div_clks[idx];
}

#define SPMI_PMIC_DIV_CLK_SIZE		0x100

static const struct of_device_id spmi_pmic_clkdiv_match_table[] = {
	{
		.compatible = "qcom,spmi-clkdiv",
		.data =  (void *)(uintptr_t)1,		/* Generic */
	},
	{
		.compatible = "qcom,pm8998-clkdiv",
		.data =  (void *)(uintptr_t)3,		/* 3 div_clks */
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, spmi_pmic_clkdiv_match_table);

static int spmi_pmic_clkdiv_probe(struct platform_device *pdev)
{
	struct spmi_pmic_div_clk_cc *clk_cc;
	struct clk_init_data init = {};
	struct clkdiv *clkdiv;
	struct clk *cxo;
	struct regmap *regmap;
	struct device *dev = &pdev->dev;
	const char *parent_name;
	int nclks, i, rc, cxo_hz;
	u32 start;

	rc = of_property_read_u32(dev->of_node, "reg", &start);
	if (rc < 0) {
		dev_err(dev, "reg property reading failed\n");
		return rc;
	}

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap) {
		dev_err(dev, "Couldn't get parent's regmap\n");
		return -EINVAL;
	}

	nclks = (uintptr_t)of_match_node(spmi_pmic_clkdiv_match_table,
					 dev->of_node)->data;

	clkdiv = devm_kcalloc(dev, nclks, sizeof(*clkdiv), GFP_KERNEL);
	if (!clkdiv)
		return -ENOMEM;

	clk_cc = devm_kzalloc(&pdev->dev, sizeof(*clk_cc), GFP_KERNEL);
	if (!clk_cc)
		return -ENOMEM;

	clk_cc->div_clks = devm_kcalloc(&pdev->dev, nclks,
					sizeof(*clk_cc->div_clks), GFP_KERNEL);
	if (!clk_cc->div_clks)
		return -ENOMEM;

	cxo = clk_get(dev, "xo");
	if (IS_ERR(cxo)) {
		rc = PTR_ERR(cxo);
		if (rc != -EPROBE_DEFER)
			dev_err(dev, "failed to get xo clock");
		return rc;
	}
	cxo_hz = clk_get_rate(cxo);
	clk_put(cxo);

	parent_name = of_clk_get_parent_name(dev->of_node, 0);
	if (!parent_name) {
		dev_err(dev, "missing parent clock\n");
		return -ENODEV;
	}

	init.parent_names = &parent_name;
	init.num_parents = parent_name ? 1 : 0;
	init.ops = &clk_spmi_pmic_div_ops;
	init.flags = 0;

	for (i = 0; i < nclks; i++) {
		spin_lock_init(&clkdiv[i].lock);
		clkdiv[i].base = start + i * SPMI_PMIC_DIV_CLK_SIZE;
		clkdiv[i].regmap = regmap;
		clkdiv[i].cxo_period_ns = NSEC_PER_SEC / cxo_hz;
		init.name = kasprintf(GFP_KERNEL, "div_clk%d", i + 1);
		clkdiv[i].hw.init = &init;
		rc = devm_clk_hw_register(dev, &clkdiv[i].hw);
		kfree(init.name); /* clock framework made a copy of the name */
		if (rc)
			return rc;

		clk_cc->div_clks[i] = &clkdiv[i].hw;
	}

	clk_cc->nclks = nclks;
	rc = of_clk_add_hw_provider(pdev->dev.of_node, spmi_pmic_div_clk_hw_get,
				    clk_cc);
	return rc;
}

static int spmi_pmic_clkdiv_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);

	return 0;
}

static struct platform_driver spmi_pmic_clkdiv_driver = {
	.driver		= {
		.name	= "qcom,spmi-pmic-clkdiv",
		.of_match_table = spmi_pmic_clkdiv_match_table,
	},
	.probe		= spmi_pmic_clkdiv_probe,
	.remove		= spmi_pmic_clkdiv_remove,
};
module_platform_driver(spmi_pmic_clkdiv_driver);

MODULE_DESCRIPTION("QCOM SPMI PMIC clkdiv driver");
MODULE_LICENSE("GPL v2");
