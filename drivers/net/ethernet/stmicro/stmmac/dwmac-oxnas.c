/*
 * Oxford Semiconductor OXNAS DWMAC glue layer
 *
 * Copyright (C) 2016 Neil Armstrong <narmstrong@baylibre.com>
 * Copyright (C) 2014 Daniel Golle <daniel@makrotopia.org>
 * Copyright (C) 2013 Ma Haijun <mahaijuns@gmail.com>
 * Copyright (C) 2012 John Crispin <blogic@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/stmmac.h>

#include "stmmac_platform.h"

/* System Control regmap offsets */
#define OXNAS_DWMAC_CTRL_REGOFFSET	0x78
#define OXNAS_DWMAC_DELAY_REGOFFSET	0x100

/* Control Register */
#define DWMAC_CKEN_RX_IN        14
#define DWMAC_CKEN_RXN_OUT      13
#define DWMAC_CKEN_RX_OUT       12
#define DWMAC_CKEN_TX_IN        10
#define DWMAC_CKEN_TXN_OUT      9
#define DWMAC_CKEN_TX_OUT       8
#define DWMAC_RX_SOURCE         7
#define DWMAC_TX_SOURCE         6
#define DWMAC_LOW_TX_SOURCE     4
#define DWMAC_AUTO_TX_SOURCE    3
#define DWMAC_RGMII             2
#define DWMAC_SIMPLE_MUX        1
#define DWMAC_CKEN_GTX          0

/* Delay register */
#define DWMAC_TX_VARDELAY_SHIFT		0
#define DWMAC_TXN_VARDELAY_SHIFT	8
#define DWMAC_RX_VARDELAY_SHIFT		16
#define DWMAC_RXN_VARDELAY_SHIFT	24
#define DWMAC_TX_VARDELAY(d)		((d) << DWMAC_TX_VARDELAY_SHIFT)
#define DWMAC_TXN_VARDELAY(d)		((d) << DWMAC_TXN_VARDELAY_SHIFT)
#define DWMAC_RX_VARDELAY(d)		((d) << DWMAC_RX_VARDELAY_SHIFT)
#define DWMAC_RXN_VARDELAY(d)		((d) << DWMAC_RXN_VARDELAY_SHIFT)

struct oxnas_dwmac {
	struct clk	*clk;
	struct regmap	*regmap;
};

static int oxnas_dwmac_init(struct platform_device *pdev, void *priv)
{
	struct oxnas_dwmac *dwmac = priv;
	unsigned int value;
	int ret;

	/* Reset HW here before changing the glue configuration */
	ret = device_reset(&pdev->dev);
	if (ret)
		return ret;

	clk_prepare_enable(dwmac->clk);

	ret = regmap_read(dwmac->regmap, OXNAS_DWMAC_CTRL_REGOFFSET, &value);
	if (ret < 0)
		return ret;

	/* Enable GMII_GTXCLK to follow GMII_REFCLK, required for gigabit PHY */
	value |= BIT(DWMAC_CKEN_GTX);
	/* Use simple mux for 25/125 Mhz clock switching */
	value |= BIT(DWMAC_SIMPLE_MUX);
	/* set auto switch tx clock source */
	value |= BIT(DWMAC_AUTO_TX_SOURCE);
	/* enable tx & rx vardelay */
	value |= BIT(DWMAC_CKEN_TX_OUT);
	value |= BIT(DWMAC_CKEN_TXN_OUT);
	value |= BIT(DWMAC_CKEN_TX_IN);
	value |= BIT(DWMAC_CKEN_RX_OUT);
	value |= BIT(DWMAC_CKEN_RXN_OUT);
	value |= BIT(DWMAC_CKEN_RX_IN);
	regmap_write(dwmac->regmap, OXNAS_DWMAC_CTRL_REGOFFSET, value);

	/* set tx & rx vardelay */
	value = DWMAC_TX_VARDELAY(4);
	value |= DWMAC_TXN_VARDELAY(2);
	value |= DWMAC_RX_VARDELAY(10);
	value |= DWMAC_RXN_VARDELAY(8);
	regmap_write(dwmac->regmap, OXNAS_DWMAC_DELAY_REGOFFSET, value);

	return 0;
}

static void oxnas_dwmac_exit(struct platform_device *pdev, void *priv)
{
	struct oxnas_dwmac *dwmac = priv;

	clk_disable_unprepare(dwmac->clk);
}

static int oxnas_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device_node *sysctrl;
	struct oxnas_dwmac *dwmac;
	int ret;

	sysctrl = of_parse_phandle(pdev->dev.of_node, "oxsemi,sys-ctrl", 0);
	if (!sysctrl) {
		dev_err(&pdev->dev, "failed to get sys-ctrl node\n");
		return -EINVAL;
	}

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = stmmac_probe_config_dt(pdev, &stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	dwmac = devm_kzalloc(&pdev->dev, sizeof(*dwmac), GFP_KERNEL);
	if (!dwmac)
		return -ENOMEM;

	dwmac->regmap = syscon_node_to_regmap(sysctrl);
	if (IS_ERR(dwmac->regmap)) {
		dev_err(&pdev->dev, "failed to have sysctrl regmap\n");
		return PTR_ERR(dwmac->regmap);
	}

	dwmac->clk = devm_clk_get(&pdev->dev, "gmac");
	if (IS_ERR(dwmac->clk))
		return PTR_ERR(dwmac->clk);

	plat_dat->bsp_priv = dwmac;
	plat_dat->init = oxnas_dwmac_init;
	plat_dat->exit = oxnas_dwmac_exit;

	return stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
}

static const struct of_device_id oxnas_dwmac_match[] = {
	{ .compatible = "oxsemi,ox820-dwmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, oxnas_dwmac_match);

static struct platform_driver oxnas_dwmac_driver = {
	.probe  = oxnas_dwmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "oxnas-dwmac",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = oxnas_dwmac_match,
	},
};
module_platform_driver(oxnas_dwmac_driver);

MODULE_AUTHOR("Neil Armstrong <narmstrong@baylibre.com>");
MODULE_DESCRIPTION("Oxford Semiconductor OXNAS DWMAC glue layer");
MODULE_LICENSE("GPL v2");
