/* Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/usb/chipidea.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include <linux/extcon.h>
#include <linux/of.h>

#include "ci.h"

#define HS_PHY_AHB_MODE			0x0098
#define HS_PHY_SEC_CTRL			0x0278
# define HS_PHY_DIG_CLAMP_N		BIT(16)

#define HS_PHY_GENCONFIG		0x009c
# define HS_PHY_TXFIFO_IDLE_FORCE_DIS	BIT(4)

#define HS_PHY_GENCONFIG_2		0x00a0
# define HS_PHY_SESS_VLD_CTRL_EN	BIT(7)
# define HS_PHY_ULPI_TX_PKT_EN_CLR_FIX	BIT(19)

#define HSPHY_SESS_VLD_CTRL		BIT(25)

struct ci_hdrc_msm {
	struct platform_device *ci;
	struct clk *core_clk;
	struct clk *iface_clk;
	struct extcon_dev *vbus_edev;
	struct ci_hdrc_platform_data pdata;
	bool secondary_phy;
	bool hsic;
	void __iomem *base;
};

static void ci_hdrc_msm_notify_event(struct ci_hdrc *ci, unsigned event)
{
	struct device *dev = ci->dev->parent;
	struct ci_hdrc_msm *msm_ci = dev_get_drvdata(dev);

	switch (event) {
	case CI_HDRC_CONTROLLER_RESET_EVENT:
		dev_dbg(dev, "CI_HDRC_CONTROLLER_RESET_EVENT received\n");
		/* use AHB transactor, allow posted data writes */
		hw_write_id_reg(ci, HS_PHY_AHB_MODE, 0xffffffff, 0x8);
		/* workaround for rx buffer collision issue */
		hw_write_id_reg(ci, HS_PHY_GENCONFIG,
				HS_PHY_TXFIFO_IDLE_FORCE_DIS, 0);

		if (msm_ci->secondary_phy)
			hw_write_id_reg(ci, HS_PHY_SEC_CTRL, HS_PHY_DIG_CLAMP_N,
					HS_PHY_DIG_CLAMP_N);

		if (!msm_ci->hsic)
			hw_write_id_reg(ci, HS_PHY_GENCONFIG_2,
					HS_PHY_ULPI_TX_PKT_EN_CLR_FIX, 0);

		if (msm_ci->vbus_edev) {
			hw_write_id_reg(ci, HS_PHY_GENCONFIG_2,
					HS_PHY_SESS_VLD_CTRL_EN,
					HS_PHY_SESS_VLD_CTRL_EN);
			hw_write(ci, OP_USBCMD, HSPHY_SESS_VLD_CTRL,
				 HSPHY_SESS_VLD_CTRL);

		}
		break;
	default:
		dev_dbg(dev, "unknown ci_hdrc event\n");
		break;
	}
}

static int ci_hdrc_msm_mux_phy(struct ci_hdrc_msm *ci,
			       struct platform_device *pdev)
{
	struct regmap *regmap;
	struct device_node *syscon;
	struct device *dev = &pdev->dev;
	u32 off, val;
	int ret;

	syscon = of_parse_phandle(dev->of_node, "phy-select", 0);
	if (!syscon)
		return 0;

	regmap = syscon_node_to_regmap(syscon);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = of_property_read_u32_index(dev->of_node, "phy-select", 1, &off);
	if (ret < 0) {
		dev_err(dev, "no offset in syscon\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_index(dev->of_node, "phy-select", 2, &val);
	if (ret < 0) {
		dev_err(dev, "no value in syscon\n");
		return -EINVAL;
	}

	ret = regmap_write(regmap, off, val);
	if (ret)
		return ret;

	ci->secondary_phy = !!val;
	if (ci->secondary_phy) {
		val = readl_relaxed(ci->base + HS_PHY_SEC_CTRL);
		val |= HS_PHY_DIG_CLAMP_N;
		writel_relaxed(val, ci->base + HS_PHY_SEC_CTRL);
	}

	return 0;
}

static int ci_hdrc_msm_probe(struct platform_device *pdev)
{
	struct ci_hdrc_msm *ci;
	struct platform_device *plat_ci;
	struct clk *clk;
	struct reset_control *reset;
	struct resource *res;
	void __iomem *base;
	resource_size_t size;
	int ret;
	struct device_node *ulpi_node, *phy_node;

	dev_dbg(&pdev->dev, "ci_hdrc_msm_probe\n");

	ci = devm_kzalloc(&pdev->dev, sizeof(*ci), GFP_KERNEL);
	if (!ci)
		return -ENOMEM;
	platform_set_drvdata(pdev, ci);

	ci->pdata.name = "ci_hdrc_msm";
	ci->pdata.capoffset = DEF_CAPOFFSET;
	ci->pdata.flags	= CI_HDRC_REGS_SHARED | CI_HDRC_DISABLE_STREAMING |
			  CI_HDRC_OVERRIDE_AHB_BURST;
	ci->pdata.notify_event = ci_hdrc_msm_notify_event;

	reset = devm_reset_control_get(&pdev->dev, "core");
	if (IS_ERR(reset))
		return PTR_ERR(reset);

	ci->core_clk = clk = devm_clk_get(&pdev->dev, "core");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	ci->iface_clk = clk = devm_clk_get(&pdev->dev, "iface");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	size = resource_size(res);
	ci->base = base = devm_ioremap(&pdev->dev, res->start, size);
	if (!base)
		return -ENOMEM;

	ci->vbus_edev = extcon_get_edev_by_phandle(&pdev->dev, 0);
	if (IS_ERR(ci->vbus_edev)) {
		if (PTR_ERR(ci->vbus_edev) != -ENODEV)
			return PTR_ERR(ci->vbus_edev);
		ci->vbus_edev = NULL;
	}

	reset_control_assert(reset);
	usleep_range(10000, 12000);
	reset_control_deassert(reset);

	ret = clk_prepare_enable(ci->core_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(ci->iface_clk);
	if (ret)
		goto err_iface;

	ret = ci_hdrc_msm_mux_phy(ci, pdev);
	if (ret)
		goto err_mux;

	ulpi_node = of_find_node_by_name(pdev->dev.of_node, "ulpi");
	if (ulpi_node) {
		phy_node = of_get_next_available_child(ulpi_node, NULL);
		ci->hsic = of_device_is_compatible(phy_node, "qcom,usb-hsic-phy");
		of_node_put(phy_node);
	}
	of_node_put(ulpi_node);

	plat_ci = ci_hdrc_add_device(&pdev->dev, pdev->resource,
				     pdev->num_resources, &ci->pdata);
	if (IS_ERR(plat_ci)) {
		dev_err(&pdev->dev, "ci_hdrc_add_device failed!\n");
		ret = PTR_ERR(plat_ci);
		goto err_mux;
	}

	ci->ci = plat_ci;

	pm_runtime_no_callbacks(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get(&pdev->dev);

	return 0;

err_mux:
	clk_disable_unprepare(ci->iface_clk);
err_iface:
	clk_disable_unprepare(ci->core_clk);
	return ret;
}

static int ci_hdrc_msm_remove(struct platform_device *pdev)
{
	struct ci_hdrc_msm *ci = platform_get_drvdata(pdev);

	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	ci_hdrc_remove_device(ci->ci);
	clk_disable_unprepare(ci->iface_clk);
	clk_disable_unprepare(ci->core_clk);

	return 0;
}

static const struct of_device_id msm_ci_dt_match[] = {
	{ .compatible = "qcom,ci-hdrc", },
	{ }
};
MODULE_DEVICE_TABLE(of, msm_ci_dt_match);

static struct platform_driver ci_hdrc_msm_driver = {
	.probe = ci_hdrc_msm_probe,
	.remove = ci_hdrc_msm_remove,
	.driver = {
		.name = "msm_hsusb",
		.of_match_table = msm_ci_dt_match,
	},
};

module_platform_driver(ci_hdrc_msm_driver);

MODULE_ALIAS("platform:msm_hsusb");
MODULE_ALIAS("platform:ci13xxx_msm");
MODULE_LICENSE("GPL v2");
