// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 ROHM Semiconductors
// bd71837.c  -- ROHM BD71837MWV clock driver

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mfd/bd71837.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>


struct bd71837_clk {
	struct clk_hw hw;
	uint8_t reg;
	uint8_t mask;
	unsigned long rate;
	struct platform_device *pdev;
	struct bd71837 *mfd;
};

static int bd71837_clk_set(struct clk_hw *hw, int status)
{
	struct bd71837_clk *c = container_of(hw, struct bd71837_clk, hw);

	return bd71837_update_bits(c->mfd, c->reg, c->mask, status);
}

static void bd71837_clk_disable(struct clk_hw *hw)
{
	int rv;
	struct bd71837_clk *c = container_of(hw, struct bd71837_clk, hw);

	rv = bd71837_clk_set(hw, 0);
	if (rv)
		dev_dbg(&c->pdev->dev, "Failed to disable 32K clk (%d)\n", rv);
}

static int bd71837_clk_enable(struct clk_hw *hw)
{
	return bd71837_clk_set(hw, 1);
}

static int bd71837_clk_is_enabled(struct clk_hw *hw)
{
	struct bd71837_clk *c = container_of(hw, struct bd71837_clk, hw);

	return c->mask & bd71837_reg_read(c->mfd, c->reg);
}

static unsigned long bd71837_clk_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct bd71837_clk *c = container_of(hw, struct bd71837_clk, hw);

	return c->rate;
}

static const struct clk_ops bd71837_clk_ops = {
	.recalc_rate = &bd71837_clk_recalc_rate,
	.prepare = &bd71837_clk_enable,
	.unprepare = &bd71837_clk_disable,
	.is_prepared = &bd71837_clk_is_enabled,
};

static int bd71837_clk_probe(struct platform_device *pdev)
{
	struct bd71837_clk *c;
	int rval = -ENOMEM;
	struct bd71837 *mfd = dev_get_drvdata(pdev->dev.parent);
	struct clk_init_data init = {
		.name = "bd71837-32k-out",
		.ops = &bd71837_clk_ops,
	};

	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		goto err_out;

	c->reg = BD71837_REG_OUT32K;
	c->mask = BD71837_OUT32K_EN;
	c->rate = BD71837_CLK_RATE;
	c->mfd = mfd;
	c->pdev = pdev;

	of_property_read_string_index(pdev->dev.parent->of_node,
					      "clock-output-names", 0,
					      &init.name);

	c->hw.init = &init;

	rval = devm_clk_hw_register(&pdev->dev, &c->hw);
	if (rval) {
		dev_err(&pdev->dev, "failed to register 32K clk");
		goto err_out;
	}

	if (pdev->dev.parent->of_node) {
		rval = of_clk_add_hw_provider(pdev->dev.parent->of_node,
					     of_clk_hw_simple_get,
					     &c->hw);
		if (rval) {
			dev_err(&pdev->dev, "adding clk provider failed\n");
			goto err_out;
		}
	}

	rval = clk_hw_register_clkdev(&c->hw, init.name, NULL);
	if (rval) {
		dev_err(&pdev->dev, "failed to register clkdev for bd71837");
		goto err_clean_provider;
	}

	platform_set_drvdata(pdev, c);

	return 0;

err_clean_provider:
	of_clk_del_provider(pdev->dev.parent->of_node);
err_out:
	return rval;
}

static int bd71837_clk_remove(struct platform_device *pdev)
{
	if (pdev->dev.parent->of_node)
		of_clk_del_provider(pdev->dev.parent->of_node);
	return 0;
}

static struct platform_driver bd71837_clk = {
	.driver = {
		.name = "bd71837-clk",
	},
	.probe = bd71837_clk_probe,
	.remove = bd71837_clk_remove,
};

module_platform_driver(bd71837_clk);

MODULE_AUTHOR("Matti Vaittinen <matti.vaittinen@fi.rohmeurope.com>");
MODULE_DESCRIPTION("BD71837 chip clk driver");
MODULE_LICENSE("GPL");
