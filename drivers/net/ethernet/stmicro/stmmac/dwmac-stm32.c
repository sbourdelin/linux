/*
 * dwmac-stm32.c - DWMAC Specific Glue layer for STM32 MCU
 *
 * Copyright (C) Alexandre Torgue 2015
 * Author:  Alexandre Torgue <alexandre.torgue@gmail.com>
 * License terms:  GNU General Public License (GPL), version 2
 *
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/stmmac.h>
#include <linux/phy.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_net.h>

#include "stmmac_platform.h"

#define MII_PHY_SEL_MASK	BIT(23)

struct stm32_dwmac {
	int interface;		/* MII interface */
	struct clk *clk_tx;
	struct clk *clk_rx;
	u32 mode_reg;		/* MAC glue-logic mode register */
	struct device *dev;
	struct regmap *regmap;
	u32 speed;
};

static int stm32_dwmac_init(struct platform_device *pdev, void *priv)
{
	struct stm32_dwmac *dwmac = priv;
	struct regmap *regmap = dwmac->regmap;
	int ret, iface = dwmac->interface;
	u32 reg = dwmac->mode_reg;
	u32 val;

	if (dwmac->clk_tx)
		ret = clk_prepare_enable(dwmac->clk_tx);
		if (ret)
			goto out;

	if (dwmac->clk_rx)
		ret = clk_prepare_enable(dwmac->clk_rx);
		if (ret)
			goto out_disable_clk_tx;

	val = (iface == PHY_INTERFACE_MODE_MII) ? 0 : 1;
	ret = regmap_update_bits(regmap, reg, MII_PHY_SEL_MASK, val);
	if (ret)
		goto out_disable_clk_tx_rx;

	return 0;

out_disable_clk_tx_rx:
	clk_disable_unprepare(dwmac->clk_rx);
out_disable_clk_tx:
	clk_disable_unprepare(dwmac->clk_tx);
out:
	return ret;
}

static void stm32_dwmac_exit(struct platform_device *pdev, void *priv)
{
	struct stm32_dwmac *dwmac = priv;

	if (dwmac->clk_tx)
		clk_disable_unprepare(dwmac->clk_tx);
	if (dwmac->clk_rx)
		clk_disable_unprepare(dwmac->clk_rx);
}

static int stm32_dwmac_parse_data(struct stm32_dwmac *dwmac,
				  struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct regmap *regmap;
	int err;

	if (!np)
		return -EINVAL;

	/*  Get TX/RX clocks */
	dwmac->clk_tx = devm_clk_get(dev, "tx-clk");
	if (IS_ERR(dwmac->clk_tx)) {
		dev_warn(dev, "No tx clock provided...\n");
		dwmac->clk_tx = NULL;
	}
	dwmac->clk_rx = devm_clk_get(dev, "rx-clk");
	if (IS_ERR(dwmac->clk_rx)) {
		dev_warn(dev, "No rx clock provided...\n");
		dwmac->clk_rx = NULL;
	}

	/* Get mode register */
	regmap = syscon_regmap_lookup_by_phandle(np, "st,syscon");
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	err = of_property_read_u32_index(np, "st,syscon", 1, &dwmac->mode_reg);
	if (err) {
		dev_err(dev, "Can't get sysconfig mode offset (%d)\n", err);
		return err;
	}

	dwmac->dev = dev;
	dwmac->interface = of_get_phy_mode(np);
	dwmac->regmap = regmap;

	return 0;
}

static int stm32_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct stm32_dwmac *dwmac;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = stmmac_probe_config_dt(pdev, &stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	dwmac = devm_kzalloc(&pdev->dev, sizeof(*dwmac), GFP_KERNEL);
	if (!dwmac)
		return -ENOMEM;

	ret = stm32_dwmac_parse_data(dwmac, pdev);
	if (ret) {
		dev_err(&pdev->dev, "Unable to parse OF data\n");
		return ret;
	}

	plat_dat->bsp_priv = dwmac;
	plat_dat->init = stm32_dwmac_init;
	plat_dat->exit = stm32_dwmac_exit;

	ret = stm32_dwmac_init(pdev, plat_dat->bsp_priv);
	if (ret)
		return ret;

	return stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
}

static const struct of_device_id stm32_dwmac_match[] = {
	{ .compatible = "st,stm32-dwmac"},
	{ }
};
MODULE_DEVICE_TABLE(of, stm32_dwmac_match);

static struct platform_driver stm32_dwmac_driver = {
	.probe  = stm32_dwmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "stm32-dwmac",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = stm32_dwmac_match,
	},
};
module_platform_driver(stm32_dwmac_driver);

MODULE_AUTHOR("Alexandre Torgue <alexandre.torgue@gmail.com>");
MODULE_DESCRIPTION("STMicroelectronics MCU DWMAC Specific Glue layer");
MODULE_LICENSE("GPL");

