/*
 * HiSilicon INNO USB2 PHY Driver.
 *
 * Copyright (c) 2016-2017 HiSilicon Technologies Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define	MAX_PORTS	4
#define REF_CLK_STABLE_TIME	100	/*unit:us*/
#define UTMI_CLK_STABLE_TIME	200	/*unit:us*/
#define UTMI_RST_COMPLETE_TIME	200	/*unit:us*/
#define PORT_RST_COMPLETE_TIME	2	/*unit:ms*/
#define TEST_RST_COMPLETE_TIME	100	/*unit:us*/
#define POR_RST_COMPLETE_TIME	300	/*unit:us*/


struct  hisi_inno_phy_port {
	struct clk *utmi_clk;
	struct reset_control *port_rst;
	struct reset_control *utmi_rst;
};

struct hisi_inno_phy_priv {
	struct regmap *reg_peri;
	struct clk *ref_clk;
	struct reset_control *test_rst;
	struct reset_control *por_rst;
	const struct reg_sequence *reg_seq;
	u32	reg_num;
	struct  hisi_inno_phy_port *ports;
	u8	port_num;
};

#define HI3798CV200_PERI_USB0	0x120
static const struct reg_sequence hi3798cv200_reg_seq[] = {
	{ HI3798CV200_PERI_USB0, 0x00a00604, },
	{ HI3798CV200_PERI_USB0, 0x00e00604, },
	{ HI3798CV200_PERI_USB0, 0x00a00604, 1000 },
};

static int hisi_inno_phy_setup(struct hisi_inno_phy_priv *priv)
{
	return regmap_multi_reg_write_bypassed(priv->reg_peri,
				priv->reg_seq, priv->reg_num);
}

static int hisi_inno_port_init(struct hisi_inno_phy_port *port)
{
	int ret;

	reset_control_deassert(port->port_rst);
	msleep(PORT_RST_COMPLETE_TIME);

	ret = clk_prepare_enable(port->utmi_clk);
	if (ret)
		return ret;
	udelay(UTMI_CLK_STABLE_TIME);

	reset_control_deassert(port->utmi_rst);
	udelay(UTMI_RST_COMPLETE_TIME);

	return 0;
}

static int hisi_inno_phy_init(struct phy *phy)
{
	struct hisi_inno_phy_priv *priv = phy_get_drvdata(phy);
	int ret, port;

	ret = clk_prepare_enable(priv->ref_clk);
	if (ret)
		return ret;
	udelay(REF_CLK_STABLE_TIME);

	if (priv->test_rst) {
		reset_control_deassert(priv->test_rst);
		udelay(TEST_RST_COMPLETE_TIME);
	}

	reset_control_deassert(priv->por_rst);
	udelay(POR_RST_COMPLETE_TIME);

	/* config phy clk and phy eye diagram */
	ret = hisi_inno_phy_setup(priv);
	if (ret)
		goto err_disable_ref_clk;

	for (port = 0; port < priv->port_num; port++) {
		ret = hisi_inno_port_init(&priv->ports[port]);
		if (ret)
			goto err_disable_clks;
	}

	return 0;

err_disable_clks:
	while (--port >= 0)
		clk_disable_unprepare(priv->ports[port].utmi_clk);
err_disable_ref_clk:
	clk_disable_unprepare(priv->ref_clk);

	return ret;
}

static void hisi_inno_phy_disable(struct phy *phy)
{
	struct hisi_inno_phy_priv *priv = phy_get_drvdata(phy);
	int i;

	for (i = 0; i < priv->port_num; i++)
		clk_disable_unprepare(priv->ports[i].utmi_clk);

	clk_disable_unprepare(priv->ref_clk);
}

static int hisi_inno_phy_of_get_ports(struct device *dev,
					struct  hisi_inno_phy_priv *priv)
{
	struct device_node *node = dev->of_node;
	struct device_node *child;
	int port = 0;
	int ret;

	priv->port_num = of_get_child_count(node);
	if (priv->port_num > MAX_PORTS) {
		dev_err(dev, "too many ports : %d (max = %d)\n",
				priv->port_num, MAX_PORTS);
		return -EINVAL;
	}

	priv->ports = devm_kcalloc(dev, priv->port_num,
				sizeof(struct hisi_inno_phy_port), GFP_KERNEL);
	if (!priv->ports)
		return -ENOMEM;

	for_each_child_of_node(node, child) {
		struct hisi_inno_phy_port *phy_port = &priv->ports[port];

		phy_port->utmi_clk = devm_get_clk_from_child(dev, child, NULL);
		if (IS_ERR(phy_port->utmi_clk)) {
			ret = PTR_ERR(phy_port->utmi_clk);
			goto fail;
		}

		phy_port->port_rst = of_reset_control_get_exclusive(child, "port_rst");
		if (IS_ERR(phy_port->port_rst)) {
			ret = PTR_ERR(phy_port->port_rst);
			goto fail;
		}

		phy_port->utmi_rst = of_reset_control_get_exclusive(child, "utmi_rst");
		if (IS_ERR(phy_port->utmi_rst)) {
			ret = PTR_ERR(phy_port->utmi_rst);
			reset_control_put(phy_port->port_rst);
			goto fail;
		}
		port++;
	}

	return 0;

fail:
	while (--port >= 0) {
		struct hisi_inno_phy_port *phy_port = &priv->ports[port];

		reset_control_put(phy_port->utmi_rst);
		reset_control_put(phy_port->port_rst);
		clk_put(phy_port->utmi_clk);
	}
	of_node_put(child);

	return ret;
}

static int hisi_inno_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy *phy;
	struct hisi_inno_phy_priv *priv;
	struct device_node *node = dev->of_node;
	int ret = 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (of_device_is_compatible(node, "hisilicon,hi3798cv200-usb2-phy")) {
		priv->reg_seq = hi3798cv200_reg_seq;
		priv->reg_num = sizeof(hi3798cv200_reg_seq)
				/ sizeof(struct reg_sequence);
	}

	priv->reg_peri = syscon_regmap_lookup_by_phandle(node,
			"hisilicon,peripheral-syscon");
	if (IS_ERR(priv->reg_peri)) {
		dev_err(dev, "no hisilicon,peripheral-syscon\n");
		return PTR_ERR(priv->reg_peri);
	}

	priv->ref_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->ref_clk))
		return PTR_ERR(priv->ref_clk);

	priv->por_rst = devm_reset_control_get_exclusive(dev, "por_rst");
	if (IS_ERR(priv->por_rst))
		return PTR_ERR(priv->por_rst);

	priv->test_rst = devm_reset_control_get_optional_exclusive(dev, "test_rst");
	if (IS_ERR(priv->test_rst))
		return PTR_ERR(priv->test_rst);

	ret = hisi_inno_phy_of_get_ports(dev, priv);
	if (ret)
		return ret;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	platform_set_drvdata(pdev, phy);
	phy_set_drvdata(phy, priv);

	return hisi_inno_phy_init(phy);
}

#ifdef CONFIG_PM_SLEEP
static int hisi_inno_phy_suspend(struct device *dev)
{
	struct phy *phy = dev_get_drvdata(dev);

	hisi_inno_phy_disable(phy);

	return 0;
}

static int hisi_inno_phy_resume(struct device *dev)
{
	struct phy *phy = dev_get_drvdata(dev);

	return hisi_inno_phy_init(phy);
}
#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops hisi_inno_phy_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(hisi_inno_phy_suspend, hisi_inno_phy_resume)
};

static const struct of_device_id hisi_inno_phy_of_match[] = {
	{.compatible = "hisilicon,inno-usb2-phy",},
	{.compatible = "hisilicon,hi3798cv200-usb2-phy",},
	{ },
};
MODULE_DEVICE_TABLE(of, hisi_inno_phy_of_match);

static struct platform_driver hisi_inno_phy_driver = {
	.probe	= hisi_inno_phy_probe,
	.driver = {
		.name	= "hisi-inno-phy",
		.of_match_table	= hisi_inno_phy_of_match,
		.pm    = &hisi_inno_phy_pm_ops,
	}
};
module_platform_driver(hisi_inno_phy_driver);

MODULE_DESCRIPTION("HiSilicon INNO USB2 PHY Driver");
MODULE_LICENSE("GPL v2");
