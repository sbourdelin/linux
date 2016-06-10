/*
 * Marvell Armada 37xx SoC xtal clocks
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
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define NB_GPIO1_LATCH	0xC
#define XTAL_MODE	    BIT(31)

static struct clk *xtal_clk;

static int armada_3700_xtal_clock_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	const char *xtal_name = "xtal";
	struct device_node *parent;
	struct regmap *regmap;
	unsigned int rate;
	u32 reg;
	int ret;

	parent = np->parent;
	if (!parent) {
		dev_err(&pdev->dev, "no parrent\n");
		return -ENODEV;
	}

	regmap = syscon_node_to_regmap(parent);
	if (IS_ERR(regmap)) {
		dev_err(&pdev->dev, "cannot get regmap\n");
		return PTR_ERR(regmap);
	}

	ret = regmap_read(regmap, NB_GPIO1_LATCH, &reg);
	if (ret) {
		dev_err(&pdev->dev, "cannot read from regmap\n");
		return ret;
	}

	if (reg & XTAL_MODE)
		rate = 40000000;
	else
		rate = 25000000;

	of_property_read_string_index(np, "clock-output-names", 0, &xtal_name);
	xtal_clk = clk_register_fixed_rate(NULL, xtal_name, NULL, 0, rate);
	if (IS_ERR(xtal_clk))
		return PTR_ERR(xtal_clk);
	of_clk_add_provider(np, of_clk_src_simple_get, xtal_clk);

	return 0;
}

static int armada_3700_xtal_clock_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);
	clk_unregister_fixed_rate(xtal_clk);

	return 0;
}

static const struct of_device_id armada_3700_xtal_clock_of_match[] = {
	{ .compatible = "marvell,armada-3700-xtal-clock", },
	{ }
};
MODULE_DEVICE_TABLE(of, armada_3700_xtal_clock_of_match);

static struct platform_driver armada_3700_xtal_clock_driver = {
	.probe = armada_3700_xtal_clock_probe,
	.remove = armada_3700_xtal_clock_remove,
	.driver		= {
		.name	= "marvell-armada-3700-xtal-clock",
		.of_match_table = armada_3700_xtal_clock_of_match,
	},
};

module_platform_driver(armada_3700_xtal_clock_driver);

MODULE_AUTHOR("Gregory CLEMENT <gregory.clement@free-electrons.com>");
MODULE_DESCRIPTION("Marvell Armada 37xx SoC xtal clocks driver");
MODULE_LICENSE("GPL v2");
