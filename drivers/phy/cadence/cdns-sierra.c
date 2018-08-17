// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence Sierra PHY Driver
 *
 * Copyright (c) 2018 Cadence Design Systems
 * Author: Alan Douglas <adouglas@cadence.com>
 *
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <dt-bindings/phy/phy.h>

#define CDNS_PHY_PLL_CFG		(0xc00e << 2)
#define CDNS_DET_STANDEC_A		(0x4000 << 2)
#define CDNS_DET_STANDEC_B		(0x4001 << 2)
#define CDNS_DET_STANDEC_C		(0x4002 << 2)
#define CDNS_DET_STANDEC_D		(0x4003 << 2)
#define CDNS_DET_STANDEC_E		(0x4004 << 2)
#define CDNS_PSM_LANECAL		(0x4008 << 2)
#define CDNS_PSM_DIAG			(0x4015 << 2)
#define CDNS_PSC_TX_A0			(0x4028 << 2)
#define CDNS_PSC_TX_A1			(0x4029 << 2)
#define CDNS_PSC_TX_A2			(0x402A << 2)
#define CDNS_PSC_TX_A3			(0x402B << 2)
#define CDNS_PSC_RX_A0			(0x4030 << 2)
#define CDNS_PSC_RX_A1			(0x4031 << 2)
#define CDNS_PSC_RX_A2			(0x4032 << 2)
#define CDNS_PSC_RX_A3			(0x4033 << 2)
#define CDNS_PLLCTRL_SUBRATE		(0x403A << 2)
#define CDNS_PLLCTRL_GEN_D		(0x403E << 2)
#define CDNS_DRVCTRL_ATTEN		(0x406A << 2)
#define CDNS_CLKPATHCTRL_TMR		(0x4081 << 2)
#define CDNS_RX_CREQ_FLTR_A_MODE1	(0x4087 << 2)
#define CDNS_RX_CREQ_FLTR_A_MODE0	(0x4088 << 2)
#define CDNS_CREQ_CCLKDET_MODE01	(0x408E << 2)
#define CDNS_RX_CTLE_MAINTENANCE	(0x4091 << 2)
#define CDNS_CREQ_FSMCLK_SEL		(0x4092 << 2)
#define CDNS_CTLELUT_CTRL		(0x4098 << 2)
#define CDNS_DFE_ECMP_RATESEL		(0x40C0 << 2)
#define CDNS_DFE_SMP_RATESEL		(0x40C1 << 2)
#define CDNS_DEQ_VGATUNE_CTRL		(0x40E1 << 2)
#define CDNS_TMRVAL_MODE3		(0x416E << 2)
#define CDNS_TMRVAL_MODE2		(0x416F << 2)
#define CDNS_TMRVAL_MODE1		(0x4170 << 2)
#define CDNS_TMRVAL_MODE0		(0x4171 << 2)
#define CDNS_PICNT_MODE1		(0x4174 << 2)
#define CDNS_CPI_OUTBUF_RATESEL		(0x417C << 2)
#define CDNS_LFPSFILT_NS		(0x418A << 2)
#define CDNS_LFPSFILT_RD		(0x418B << 2)
#define CDNS_LFPSFILT_MP		(0x418C << 2)
#define CDNS_SDFILT_H2L_A		(0x4191 << 2)

#define SIERRA_PHYS_NUM 2

struct cdns_phy_instance {
	struct phy *phy;
	u32 phy_type;
	u32 nlanes;
	void __iomem *base;
};

struct cdns_sierra_phy {
	struct device *dev;
	void __iomem *base;
	u32 *init_data;
	struct cdns_phy_instance phys[SIERRA_PHYS_NUM];
	struct reset_control *reset;
	struct reset_control *apb_reset;
	struct clk *clk;
	struct phy *pcie_phy;
};

static void cdns_sierra_pcie_on(struct cdns_phy_instance *ins)
{
	int i;

	for (i = 0; i < (ins->nlanes << 11); i += (1 << 11)) {
		writel(0x891f, ins->base + CDNS_DET_STANDEC_D + i);
		writel(0x0053, ins->base + CDNS_DET_STANDEC_E + i);
		writel(0x0400, ins->base + CDNS_TMRVAL_MODE2 + i);
		writel(0x0200, ins->base + CDNS_TMRVAL_MODE3 + i);
	}
}

static void cdns_sierra_usb_on(struct cdns_phy_instance *ins)
{
	int i;

	writel(0x0000, ins->base + CDNS_PHY_PLL_CFG);
	for (i = 0; i < (ins->nlanes << 11); i += (1 << 11)) {
		writel(0xFE0A, ins->base + CDNS_DET_STANDEC_A + i);
		writel(0x000F, ins->base + CDNS_DET_STANDEC_B + i);
		writel(0x55A5, ins->base + CDNS_DET_STANDEC_C + i);
		writel(0x69AD, ins->base + CDNS_DET_STANDEC_D + i);
		writel(0x0241, ins->base + CDNS_DET_STANDEC_E + i);
		writel(0x0110, ins->base + CDNS_PSM_LANECAL + i);
		writel(0xCF00, ins->base + CDNS_PSM_DIAG + i);
		writel(0x001F, ins->base + CDNS_PSC_TX_A0 + i);
		writel(0x0007, ins->base + CDNS_PSC_TX_A1 + i);
		writel(0x0003, ins->base + CDNS_PSC_TX_A2 + i);
		writel(0x0003, ins->base + CDNS_PSC_TX_A3 + i);
		writel(0x0FFF, ins->base + CDNS_PSC_RX_A0 + i);
		writel(0x0003, ins->base + CDNS_PSC_RX_A1 + i);
		writel(0x0003, ins->base + CDNS_PSC_RX_A2 + i);
		writel(0x0001, ins->base + CDNS_PSC_RX_A3 + i);
		writel(0x0001, ins->base + CDNS_PLLCTRL_SUBRATE + i);
		writel(0x0406, ins->base + CDNS_PLLCTRL_GEN_D + i);
		writel(0x0000, ins->base + CDNS_DRVCTRL_ATTEN + i);
		writel(0x823E, ins->base + CDNS_CLKPATHCTRL_TMR + i);
		writel(0x078F, ins->base + CDNS_RX_CREQ_FLTR_A_MODE1 + i);
		writel(0x078F, ins->base + CDNS_RX_CREQ_FLTR_A_MODE0 + i);
		writel(0x7B3C, ins->base + CDNS_CREQ_CCLKDET_MODE01 + i);
		writel(0x023C, ins->base + CDNS_RX_CTLE_MAINTENANCE + i);
		writel(0x3232, ins->base + CDNS_CREQ_FSMCLK_SEL + i);
		writel(0x8452, ins->base + CDNS_CTLELUT_CTRL + i);
		writel(0x4121, ins->base + CDNS_DFE_ECMP_RATESEL + i);
		writel(0x4121, ins->base + CDNS_DFE_SMP_RATESEL + i);
		writel(0x9999, ins->base + CDNS_DEQ_VGATUNE_CTRL + i);
		writel(0x0330, ins->base + CDNS_TMRVAL_MODE0 + i);
		writel(0x01FF, ins->base + CDNS_PICNT_MODE1 + i);
		writel(0x0009, ins->base + CDNS_CPI_OUTBUF_RATESEL + i);
		writel(0x000F, ins->base + CDNS_LFPSFILT_NS + i);
		writel(0x0009, ins->base + CDNS_LFPSFILT_RD + i);
		writel(0x0001, ins->base + CDNS_LFPSFILT_MP + i);
		writel(0x8013, ins->base + CDNS_SDFILT_H2L_A + i);
		writel(0x0400, ins->base + CDNS_TMRVAL_MODE1 + i);
	}
}

static int cdns_sierra_phy_on(struct phy *x)
{
	struct cdns_phy_instance *ins = phy_get_drvdata(x);
	struct cdns_sierra_phy *phy = dev_get_drvdata(x->dev.parent);
	int ret;

	ret = clk_prepare_enable(phy->clk);
	if (ret)
		return ret;

	/* Ensure the PHY is in reset */
	reset_control_assert(phy->reset);

	/* Enable APB */
	reset_control_deassert(phy->apb_reset);

	/* Program the PHY registers*/
	if (ins->phy_type == PHY_TYPE_PCIE)
		cdns_sierra_pcie_on(ins);
	else
		cdns_sierra_usb_on(ins);

	/* Take the PHY out of reset */
	ret = reset_control_assert(phy->reset);

	if (ret) {
		clk_disable_unprepare(phy->clk);
		return ret;
	}

	return ret;
}

static int cdns_sierra_phy_off(struct phy *x)
{
	struct cdns_sierra_phy *phy = dev_get_drvdata(x->dev.parent);
	int ret = 0;

	reset_control_assert(phy->reset);
	reset_control_assert(phy->apb_reset);
	clk_disable_unprepare(phy->clk);
	dev_info(phy->dev, "sierra PHY OFF\n");

	return ret;
}

static const struct phy_ops ops = {
	.power_on	= cdns_sierra_phy_on,
	.power_off	= cdns_sierra_phy_off,
	.owner		= THIS_MODULE,
};

static const struct of_device_id cdns_sierra_id_table[];
static struct phy *cdns_sierra_xlate(struct device *dev,
				     struct of_phandle_args *args)
{
	struct cdns_sierra_phy *sphy = dev_get_drvdata(dev);
	struct cdns_phy_instance *ins = NULL;
	int i;

	if (args->args_count != 2) {
		dev_err(dev, "invalid number of cells in 'phy' property\n");
		return ERR_PTR(-EINVAL);
	}

	if (WARN_ON(args->args[1] == 0 || args->args[1] > 4))
		return ERR_PTR(-ENODEV);

	for (i = 0; i < SIERRA_PHYS_NUM; i++) {
		if (sphy->phys[i].phy_type == args->args[0])
			ins = &sphy->phys[i];
	}

	if (!ins) {
		dev_err(dev, "failed to find appropriate phy\n");
		return ERR_PTR(-EINVAL);
	}

	ins->nlanes = args->args[1];

	return ins->phy;
}

static int cdns_sierra_phy_probe(struct platform_device *pdev)
{
	struct cdns_sierra_phy *sphy;
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	struct resource *res;
	int i;

	sphy = devm_kzalloc(dev, sizeof(*sphy), GFP_KERNEL);
	if (!sphy)
		return -ENOMEM;

	sphy->dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "reg");
	sphy->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(sphy->base)) {
		dev_err(dev, "missing \"reg\"\n");
		return PTR_ERR(sphy->base);
	}

	/* Get init data for this PHY */
	match = of_match_device(cdns_sierra_id_table, dev);
	if (!match)
		return -EINVAL;
	sphy->init_data = (u32 *)match->data;

	/* Check that PHY is present */
	if  (sphy->init_data[0] != readl(sphy->base))
		return -EINVAL;

	platform_set_drvdata(pdev, sphy);

	sphy->clk = devm_clk_get(dev, "phy_clk");
	if (IS_ERR(sphy->clk)) {
		dev_err(dev, "failed to get clock phy_clk\n");
		return PTR_ERR(sphy->clk);
	}

	sphy->reset = devm_reset_control_get(dev, "sierra_reset");
	if (IS_ERR(sphy->reset)) {
		dev_err(dev, "failed to get reset\n");
		return PTR_ERR(sphy->reset);
	}

	sphy->apb_reset = devm_reset_control_get(dev, "sierra_apb");
	if (IS_ERR(sphy->apb_reset)) {
		dev_err(dev, "failed to get apb reset\n");
		return PTR_ERR(sphy->apb_reset);
	}

	sphy->phys[0].phy_type = PHY_TYPE_PCIE;
	sphy->phys[1].phy_type = PHY_TYPE_USB3;
	for (i = 0; i < SIERRA_PHYS_NUM; i++) {
		struct phy *gphy = devm_phy_create(dev, NULL, &ops);

		if (IS_ERR(gphy))
			return PTR_ERR(gphy);
		sphy->phys[i].phy = gphy;
		sphy->phys[i].base = sphy->base;
		phy_set_drvdata(gphy, &sphy->phys[i]);
	}

	pm_runtime_enable(dev);
	phy_provider = devm_of_phy_provider_register(dev, cdns_sierra_xlate);
	return PTR_ERR_OR_ZERO(phy_provider);
}

static int cdns_sierra_phy_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static int cdns_map_sierra[] = {
	0x00007364,
};

static const struct of_device_id cdns_sierra_id_table[] = {
	{
		.compatible = "cdns,sierra-phy",
		.data = cdns_map_sierra,
	},
	{}
};
MODULE_DEVICE_TABLE(of, cdns_sierra_id_table);

static struct platform_driver cdns_sierra_driver = {
	.probe		= cdns_sierra_phy_probe,
	.remove		= cdns_sierra_phy_remove,
	.driver		= {
		.name	= "cdns-sierra-phy",
		.of_match_table = cdns_sierra_id_table,
	},
};
module_platform_driver(cdns_sierra_driver);

MODULE_ALIAS("platform:cdns_sierra");
MODULE_AUTHOR("Cadence Design Systems");
MODULE_DESCRIPTION("CDNS sierra phy driver");
MODULE_LICENSE("GPL v2");
