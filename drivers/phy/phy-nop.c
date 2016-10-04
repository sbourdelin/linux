/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

/**
 * struct phy_nop:- structure holding NOP PHY attributes.
 *
 * @dev: pointer to device.
 * @phy: pointer to generic phy.
 * @clk: pointer to phy clock.
 * @refclk: pointer to phy reference clock.
 * @vdd: vdd supply to the phy core block.
 * @rst: pointer to reset controller for the phy block.
 */
struct phy_nop {
	struct device *dev;
	struct phy *phy;
	struct clk *clk;
	struct clk *refclk;
	struct regulator *vdd;
	struct reset_control *rst;
};

static int phy_nop_poweron(struct phy *generic_phy)
{
	struct phy_nop *phy = phy_get_drvdata(generic_phy);
	int ret = 0;

	if (phy->vdd) {
		ret = regulator_enable(phy->vdd);
		if (ret) {
			dev_err(phy->dev, "vdd enable failed: %d\n", ret);
			return ret;
		}
	}

	if (phy->clk) {
		ret = clk_prepare_enable(phy->clk);
		if (ret) {
			dev_err(phy->dev, "main clk enable failed: %d\n", ret);
			goto err_clk;
		}
	}

	if (phy->refclk) {
		ret = clk_prepare_enable(phy->refclk);
		if (ret) {
			dev_err(phy->dev, "ref clk enable failed: %d\n", ret);
			goto err_refclk;
		}
	}

	if (phy->rst) {
		ret = reset_control_deassert(phy->rst);
		if (ret) {
			dev_err(phy->dev, "phy reset deassert failed\n");
			goto err_rst;
		}
	}

	return 0;

err_rst:
	clk_disable_unprepare(phy->refclk);
err_refclk:
	clk_disable_unprepare(phy->clk);
err_clk:
	regulator_disable(phy->vdd);
	return ret;
}

static int phy_nop_poweroff(struct phy *generic_phy)
{
	struct phy_nop *phy = phy_get_drvdata(generic_phy);

	if (phy->rst)
		reset_control_assert(phy->rst);

	if (phy->clk)
		clk_disable_unprepare(phy->clk);
	if (phy->refclk)
		clk_disable_unprepare(phy->refclk);

	if (phy->vdd)
		regulator_disable(phy->vdd);

	return 0;
}

static const struct phy_ops phy_nop_gen_ops = {
	.power_on	= phy_nop_poweron,
	.power_off	= phy_nop_poweroff,
	.owner		= THIS_MODULE,
};

static int phy_nop_probe(struct platform_device *pdev)
{
	struct phy_nop *phy;
	struct phy *generic_phy;
	struct device *dev = &pdev->dev;
	struct phy_provider *phy_provider;
	int ret = 0;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;
	phy->dev = dev;

	/* XXX: Do we need to request any clocks ? */
	phy->clk = devm_clk_get(dev, "main_clk");
	if (IS_ERR(phy->clk)) {
		dev_dbg(dev, "failed to get main_clk: %ld\n",
						PTR_ERR(phy->clk));
		phy->clk = NULL;
	}
	phy->refclk = devm_clk_get(dev, "ref_clk");
	if (IS_ERR(phy->refclk)) {
		dev_dbg(dev, "failed to get ref_clk: %ld\n",
						PTR_ERR(phy->refclk));
		phy->refclk = NULL;
	}

	/* XXX: Do we need to request any regulators ? */
	phy->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(phy->vdd)) {
		dev_dbg(dev, "failed to get vdd for phy: %ld\n",
						PTR_ERR(phy->vdd));
		phy->vdd = NULL;
	}

	/* XXX: Do we need to request any phy-reset ? */
	phy->rst = devm_reset_control_get(dev, "phy");
	if (IS_ERR(phy->rst)) {
		dev_dbg(dev, "failed to get phy core reset: %ld\n",
						PTR_ERR(phy->rst));
		phy->rst = NULL;
	}

	generic_phy = devm_phy_create(dev, NULL, &phy_nop_gen_ops);
	if (IS_ERR(generic_phy)) {
		ret = PTR_ERR(generic_phy);
		dev_err(dev, "failed to create generic phy %d\n", ret);
		return ret;
	}
	phy->phy = generic_phy;
	phy_set_drvdata(generic_phy, phy);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider)) {
		ret = PTR_ERR(phy_provider);
		dev_err(dev, "failed to register phy %d\n", ret);
	}

	return ret;
}

static const struct of_device_id phy_nop_id_table[] = {
	{
		.compatible = "phy-nop",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, phy_nop_id_table);

static struct platform_driver phy_nop_driver = {
	.probe		= phy_nop_probe,
	.driver = {
		.name	= "phy_nop",
		.of_match_table = of_match_ptr(phy_nop_id_table),
	},
};

module_platform_driver(phy_nop_driver);

MODULE_DESCRIPTION("Generic No-op PHY driver");
MODULE_LICENSE("GPL v2");
