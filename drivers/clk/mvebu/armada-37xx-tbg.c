/*
 * Marvell Armada 37xx SoC Time Base Generator clocks
 *
 * Copyright (C) 2016 Marvell
 *
 * Gregory CLEMENT <gregory.clement@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define NUM_TBG	    4

#define TBG_CTRL0		0x4
#define TBG_CTRL1		0x8
#define TBG_CTRL7		0x20
#define TBG_CTRL8		0x30

#define TBG_DIV_MASK		0x1FF

#define TBG_A_REFDIV		0
#define TBG_B_REFDIV		16

#define TBG_A_FBDIV		2
#define TBG_B_FBDIV		18

#define TBG_A_VCODIV_SE		0
#define TBG_B_VCODIV_SE		16

#define TBG_A_VCODIV_DIFF	1
#define TBG_B_VCODIV_DIFF	17

struct tbg_def {
	char *name;
	u32 refdiv_offset;
	u32 fbdiv_offset;
	u32 vcodiv_reg;
	u32 vcodiv_offset;
};

struct tbg_def tbg[NUM_TBG] = {
	{"TBG-A-P", TBG_A_REFDIV, TBG_A_FBDIV, TBG_CTRL8, TBG_A_VCODIV_DIFF},
	{"TBG-B-P", TBG_B_REFDIV, TBG_B_FBDIV, TBG_CTRL8, TBG_B_VCODIV_DIFF},
	{"TBG-A-S", TBG_A_REFDIV, TBG_A_FBDIV, TBG_CTRL1, TBG_A_VCODIV_SE},
	{"TBG-B-S", TBG_B_REFDIV, TBG_B_FBDIV, TBG_CTRL1, TBG_B_VCODIV_SE},
};

static struct clk_onecell_data clk_tbg_data;

unsigned int tbg_get_mult(void __iomem *reg, struct tbg_def *ptbg)
{
	u32 val;

	val = readl(reg + TBG_CTRL0);

	return ((val >> ptbg->fbdiv_offset) & TBG_DIV_MASK) << 2;
}

unsigned int tbg_get_div(void __iomem *reg, struct tbg_def *ptbg)
{
	u32 val;
	unsigned int div;

	val = readl(reg + TBG_CTRL7);

	div = (val >> ptbg->refdiv_offset) & TBG_DIV_MASK;
	if (div == 0)
		div = 1;
	val = readl(reg + ptbg->vcodiv_reg);

	div *= 1 << ((val >>  ptbg->vcodiv_offset) & TBG_DIV_MASK);

	return div;
}


static int armada_3700_tbg_clock_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	const char *parent_name;
	struct resource *res;
	struct clk *parent;
	void __iomem *reg;
	int i, ret;

	parent = of_clk_get(np, 0);
	if (IS_ERR(parent)) {
		dev_err(dev, "Could get the clock parent\n");
		return -EINVAL;
	}
	parent_name = __clk_get_name(parent);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	reg = devm_ioremap_resource(dev, res);
	if (IS_ERR(reg)) {
		dev_err(dev, "Could not map the tbg clock registers\n");
		ret = PTR_ERR(reg);
		goto put_clk;
	}

	clk_tbg_data.clk_num = NUM_TBG;
	clk_tbg_data.clks = devm_kcalloc(dev, clk_tbg_data.clk_num,
					 sizeof(struct clk *), GFP_KERNEL);
	if (!clk_tbg_data.clks) {
		ret = -ENOMEM;
		goto put_clk;
	}

	for (i = 0; i < NUM_TBG; i++) {
		const char *name;
		unsigned int mult, div;

		name = tbg[i].name;
		mult = tbg_get_mult(reg, &tbg[i]);
		div = tbg_get_div(reg, &tbg[i]);
		clk_tbg_data.clks[i] = clk_register_fixed_factor(NULL, name,
						 parent_name, 0, mult, div);
		if (IS_ERR(clk_tbg_data.clks[i]))
			dev_err(dev, "Can't register TBG clock %s\n", name);
	}

	of_clk_add_provider(np, of_clk_src_onecell_get, &clk_tbg_data);

	return 0;

put_clk:
	clk_put(parent);
	return ret;
}

static int armada_3700_tbg_clock_remove(struct platform_device *pdev)
{
	int i;

	of_clk_del_provider(pdev->dev.of_node);
	for (i = 0; i < clk_tbg_data.clk_num; i++)
		clk_unregister_fixed_factor(clk_tbg_data.clks[i]);

	return 0;
}

static const struct of_device_id armada_3700_tbg_clock_of_match[] = {
	{ .compatible = "marvell,armada-3700-tbg-clock", },
	{ }
};

MODULE_DEVICE_TABLE(of, armada_3700_tbg_clock_of_match);

static struct platform_driver armada_3700_tbg_clock_driver = {
	.probe = armada_3700_tbg_clock_probe,
	.remove = armada_3700_tbg_clock_remove,
	.driver		= {
		.name	= "marvell-armada-3700-tbg-clock",
		.of_match_table = armada_3700_tbg_clock_of_match,
	},
};

module_platform_driver(armada_3700_tbg_clock_driver);

MODULE_AUTHOR("Gregory CLEMENT <gregory.clement@free-electrons.com>");
MODULE_DESCRIPTION("Marvell Armada 37xx SoC Time Base Generator driver");
MODULE_LICENSE("GPL v2");
