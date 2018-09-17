// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of_net.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "mtk-gmac.h"

/* Infra configuration register */
#define TOP_DCMCTL		0x10

/* Infra configuration register bits */
#define INFRA_DCM_ENABLE	BIT(0)

/* Peri configuration register */
#define PERI_PHY_INTF_SEL	0x418
#define PERI_PHY_DLY		0x428

/* Peri configuration register bits and bitmasks */
#define DLY_GTXC_ENABLE		BIT(5)
#define DLY_GTXC_INV		BIT(6)
#define DLY_GTXC_STAGES		GENMASK(4, 0)
#define DLY_RXC_ENABLE		BIT(12)
#define DLY_RXC_INV		BIT(13)
#define DLY_RXC_STAGES		GENMASK(11, 7)
#define DLY_TXC_ENABLE		BIT(19)
#define DLY_TXC_INV		BIT(20)
#define DLY_TXC_STAGES		GENMASK(18, 14)
#define PHY_INTF_MASK		GENMASK(2, 0)
#define RMII_CLK_SRC_MASK	GENMASK(5, 4)
#define RMII_CLK_SRC_RXC	BIT(4)

/* Peri configuration register value */
#define DLY_VAL_RGMII		0x11a3
#define DLY_VAL_RGMII_ID	0x0
#define DLY_VAL_RGMII_RXID	0x23
#define DLY_VAL_RGMII_TXID	0x1180
#define PHY_INTF_MII_GMII	0x0
#define PHY_INTF_RGMII		0x1
#define PHY_INTF_RMII		0x4

static const char * const gmac_clks_source_name[] = {
	"axi", "apb", "mac_ext", "ptp", "ptp_parent", "ptp_top"
};

static int get_platform_resources(struct platform_device *pdev,
				  struct gmac_resources *gmac_res)
{
	struct resource *res;
	int gpio;

	/* Get irq resource */
	gmac_res->irq = platform_get_irq_byname(pdev, "macirq");
	if (gmac_res->irq < 0) {
		if (gmac_res->irq != -EPROBE_DEFER) {
			dev_err(&pdev->dev,
				"MAC IRQ configuration information not found\n");
		}
		return gmac_res->irq;
	}

	/* Get memory resource */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	gmac_res->base_addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(gmac_res->base_addr)) {
		dev_err(&pdev->dev, "cannot map register memory\n");
		return PTR_ERR(gmac_res->base_addr);
	}

	gmac_res->mac_addr =
		(const char *)of_get_mac_address(pdev->dev.of_node);

	gpio = of_get_named_gpio(pdev->dev.of_node, "reset-gpio", 0);
	if (!gpio_is_valid(gpio)) {
		dev_err(&pdev->dev, "failed to parse phy reset gpio\n");
		return gpio;
	}

	gmac_res->phy_rst = gpio;

	return 0;
}

static int mt2712_gmac_top_regmap_get(struct plat_gmac_data *plat)
{
	plat->infra_regmap =
		syscon_regmap_lookup_by_compatible("mediatek,mt2712-infracfg");
	if (IS_ERR(plat->infra_regmap)) {
		pr_err("Failed to get infracfg syscon\n");
		return PTR_ERR(plat->infra_regmap);
	}

	plat->peri_regmap =
		syscon_regmap_lookup_by_compatible("mediatek,mt2712-pericfg");
	if (IS_ERR(plat->peri_regmap)) {
		pr_err("Failed to get pericfg syscon\n");
		return PTR_ERR(plat->infra_regmap);
	}

	return 0;
}

static int mt2712_gmac_clk_get(struct platform_device *pdev,
			       struct plat_gmac_data *plat)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(plat->clks); i++) {
		plat->clks[i] = devm_clk_get(&pdev->dev,
					     gmac_clks_source_name[i]);
		if (IS_ERR(plat->clks[i])) {
			if (PTR_ERR(plat->clks[i]) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			plat->clks[i] = NULL;
		}
	}

	return 0;
}

static int mt2712_gmac_clk_enable(struct plat_gmac_data *plat)
{
	int clk, ret;

	for (clk = 0; clk < GMAC_CLK_MAX ; clk++) {
		ret = clk_prepare_enable(plat->clks[clk]);
		if (ret)
			goto err_disable_clks;
	}

	ret = clk_set_parent(plat->clks[GMAC_CLK_PTP],
			     plat->clks[GMAC_CLK_PTP_PARENT]);
	if (ret)
		goto err_disable_clks;

	return 0;

err_disable_clks:
	while (--clk >= 0)
		clk_disable_unprepare(plat->clks[clk]);

	return ret;
}

static void mt2712_gmac_clk_disable(struct plat_gmac_data *plat)
{
	int clk;

	for (clk = GMAC_CLK_MAX - 1; clk >= 0; clk--)
		clk_disable_unprepare(plat->clks[clk]);
}

static int platform_data_get(struct platform_device *pdev,
			     struct plat_gmac_data *plat)
{
	int ret;

	ret = mt2712_gmac_top_regmap_get(plat);
	if (ret)
		return ret;

	/* Get clock resource */
	ret = mt2712_gmac_clk_get(pdev, plat);
	if (ret)
		return ret;

	return 0;
}

static void mt2712_gmac_set_interface(struct plat_gmac_data *plat)
{
	/* bus clock initialzation */
	regmap_update_bits(plat->infra_regmap, TOP_DCMCTL,
			   INFRA_DCM_ENABLE, INFRA_DCM_ENABLE);

	regmap_write(plat->peri_regmap, PERI_PHY_DLY, 0);

	/* select phy interface in top control domain */
	switch (plat->phy_mode) {
	case PHY_INTERFACE_MODE_MII:
	case PHY_INTERFACE_MODE_GMII:
		regmap_update_bits(plat->peri_regmap,
				   PERI_PHY_INTF_SEL,
				   PHY_INTF_MASK,
				   PHY_INTF_MII_GMII);
		break;
	case PHY_INTERFACE_MODE_RMII:
		regmap_update_bits(plat->peri_regmap,
				   PERI_PHY_INTF_SEL,
				   PHY_INTF_MASK,
				   PHY_INTF_RMII);
		/* bit[5:4] = 1 ref_clk connect to rxc pad */
		regmap_update_bits(plat->peri_regmap,
				   PERI_PHY_INTF_SEL,
				   RMII_CLK_SRC_MASK,
				   RMII_CLK_SRC_RXC);
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		regmap_update_bits(plat->peri_regmap,
				   PERI_PHY_INTF_SEL,
				   PHY_INTF_MASK,
				   PHY_INTF_RGMII);
		break;
	default:
		pr_err("phy interface not support\n");
	}
}

static void mt2712_gmac_set_delay(struct plat_gmac_data *plat)
{
	switch (plat->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
		regmap_write(plat->peri_regmap, PERI_PHY_DLY, DLY_VAL_RGMII);
		break;
	case PHY_INTERFACE_MODE_RGMII_ID:
		regmap_write(plat->peri_regmap, PERI_PHY_DLY, DLY_VAL_RGMII_ID);
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		regmap_write(plat->peri_regmap, PERI_PHY_DLY, DLY_VAL_RGMII_RXID);
		break;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		regmap_write(plat->peri_regmap, PERI_PHY_DLY, DLY_VAL_RGMII_TXID);
		break;
	}
}

static int mt2712_gmac_probe(struct platform_device *pdev)
{
	struct plat_gmac_data *plat;
	struct gmac_resources gmac_res;
	int ret = 0;

	plat = devm_kzalloc(&pdev->dev, sizeof(*plat), GFP_KERNEL);
	if (!plat)
		return -ENOMEM;

	plat->np = pdev->dev.of_node;
	plat->phy_mode = of_get_phy_mode(plat->np);
	plat->gmac_clk_enable = mt2712_gmac_clk_enable;
	plat->gmac_clk_disable = mt2712_gmac_clk_disable;
	plat->gmac_set_interface = mt2712_gmac_set_interface;
	plat->gmac_set_delay = mt2712_gmac_set_delay;

	ret = get_platform_resources(pdev, &gmac_res);
	if (ret)
		return ret;

	ret = platform_data_get(pdev, plat);
	if (ret)
		return ret;

	return gmac_drv_probe(&pdev->dev, plat, &gmac_res);
}

int mt2712_gmac_remove(struct platform_device *pdev)
{
	return gmac_drv_remove(&pdev->dev);
}

static const struct of_device_id of_mt2712_gmac_match[] = {
	{ .compatible = "mediatek,mt2712-eth"},
	{}
};

MODULE_DEVICE_TABLE(of, of_mt2712_gmac_match);

static struct platform_driver mt2712_gmac_driver = {
	.probe = mt2712_gmac_probe,
	.remove = mt2712_gmac_remove,
	.driver = {
		.name = "mt2712_gmac_eth",
		.of_match_table = of_mt2712_gmac_match,
	},
};

module_platform_driver(mt2712_gmac_driver);

MODULE_LICENSE("GPL");
