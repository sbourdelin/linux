/*
 * Copyright (C) 2016 Broadcom
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>

#define NS2_USB3_PHY_MAX			0x02

#define NS2_USB3_PHY_CONFIG_CTRL_REG		0x10
#define NS2_USB3_PHY_CONFIG_CTRL_MASK		(BIT(3) | BIT(4) | BIT(5))
#define NS2_USB3_PHY_CONFIG_CTRL_PLL_SEQ_START	BIT(6)

#define NS2_USB3_PHY_P0CTL_REG			0x14
#define NS2_USB3_PHY_P1CTL_REG			0x18
#define NS2_USB3_PHY_PXCTL_I_BIT		BIT(1)

#define NS2_USB3_PHY_MISC_STATUS_REG		0x20

#define NS2_IDM_RST_CTRL_P0_OFFSET		0x800
#define NS2_IDM_RST_CTRL_P1_OFFSET		0x1800
#define NS2_IDM_RESET_CONTROL_BIT		BIT(0)

#define NS2_IDM_IO_CTRL_P0_OFFSET		0x408
#define NS2_IDM_IO_CTRL_P1_OFFSET		0x1408
/* Bit 23 for PPC Polarity, Bit 24 for PPC NANDNOR select */
#define NS2_IDM_IO_CTRL_PPC_CFG			(BIT(23) | BIT(24))

#define NS2_PHY_RESET_BIT			BIT(5)
#define NS2_PHY_PLL_RESET_BIT			BIT(6)

/* NS2 USB3 MDIO */
#define NS2_USB3_MDIO_PLL30_ADDR		0x8000
#define NS2_USB3_MDIO_BLK_ACCESS		0x1F
#define NS2_USB3_MDIO_PLL30_ANAPLL_CTRL		0x14
#define NS2_USB3_MDIO_PLL30_ANAPLL_CTRL_VAL	0x23
#define NS2_USB3_MDIO_PLL30_GEN_PLL		0xF
#define NS2_USB3_MDIO_PLL30_GEN_PLL_PCLK_SEL	BIT(11)
#define NS2_USB3_MDIO_P0_AFE30_ADDR		0x8080
#define NS2_USB3_MDIO_P1_AFE30_ADDR		0x9080
#define NS2_USB3_MDIO_AFE30_RX_SIG_DETECT	0x5
#define NS2_USB3_MDIO_AFE30_RX_SIG_DETECT_VAL	0xAC0D

#define NS2_USB3_MDIO_P0_PIPE_BLK_ADDR		0x8060
#define NS2_USB3_MDIO_P1_PIPE_BLK_ADDR		0x9060
#define NS2_USB3_MDIO_PIPE_BLK_REG_1_OFFSET	0x1
#define NS2_USB3_MDIO_PIPE_BLK_REG_1_VAL	0x207

#define NS2_USB3_MDIO_P0_AEQ_BLK_ADDR		0x80E0
#define NS2_USB3_MDIO_P1_AEQ_BLK_ADDR		0x90E0
#define NS2_USB3_MDIO_AEQ_BLK_REG_1_OFFSET	0x1
#define NS2_USB3_MDIO_AEQ_BLK_REG_1_VAL		0x3000

enum ns2_phy_block {
	PHY_RESET,
	PHY_PLL_RESET,
	PHY_SOFT_RESET,
	PHY_PIPE_RESET,
	PHY_REF_CLOCK,
	PHY_PLL_SEQ_START,
	PHY_PLL_STATUS,
	PHY_VBUS_PPC,
};

enum ns2_reg_base {
	NS2_USB3_CTRL = 1,
	NS2_USB3_PHY_CFG,
	NS2_USB3_RST_CTRL,
	NS2_USB3_REG_BASE_MAX
};

struct ns2_usb3_phy {
	void __iomem *reg_base[NS2_USB3_REG_BASE_MAX];
	struct ns2_usb3_phy_master *mphy;
	struct phy *phy;
	int port_no;
};

struct ns2_usb3_phy_master {
	struct ns2_usb3_phy iphys[NS2_USB3_PHY_MAX];
	struct mdio_device *mdiodev;
	struct mutex phy_mutex;
	int init_count; /* PHY is dual port phy, so init once*/
};

static int iproc_ns2_phy_action(struct ns2_usb3_phy *iphy,
				enum ns2_phy_block block, bool assert)
{
	void __iomem *addr;
	u32  data, count;
	u32 offset = 0;
	int ret = 0;

	switch (block) {
	case PHY_RESET:
		addr = iphy->reg_base[NS2_USB3_CTRL];

		data = readl(addr + offset);

		if (assert)
			data &= ~NS2_PHY_RESET_BIT;
		else
			data |= NS2_PHY_RESET_BIT;

		writel(data, addr + offset);
		break;

	case PHY_PLL_RESET:
		addr = iphy->reg_base[NS2_USB3_CTRL];

		data = readl(addr + offset);

		if (assert)
			data &= ~NS2_PHY_PLL_RESET_BIT;
		else
			data |= NS2_PHY_PLL_RESET_BIT;

		writel(data, addr + offset);
		break;

	case PHY_SOFT_RESET:
		addr = iphy->reg_base[NS2_USB3_PHY_CFG];
		offset = NS2_USB3_PHY_P0CTL_REG;

		data = readl(addr + offset);

		if (assert)
			data &= ~NS2_USB3_PHY_PXCTL_I_BIT;
		else
			data |= NS2_USB3_PHY_PXCTL_I_BIT;

		writel(data, addr + offset);

		offset = NS2_USB3_PHY_P1CTL_REG;

		data = readl(addr + offset);

		if (assert)
			data &= ~NS2_USB3_PHY_PXCTL_I_BIT;
		else
			data |= NS2_USB3_PHY_PXCTL_I_BIT;

		writel(data, addr + offset);
		break;

	case PHY_PIPE_RESET:
		addr = iphy->reg_base[NS2_USB3_RST_CTRL];
		offset = NS2_IDM_RST_CTRL_P0_OFFSET;

		data = readl(addr + offset);

		if (assert)
			data |= NS2_IDM_RESET_CONTROL_BIT;
		else
			data &= ~NS2_IDM_RESET_CONTROL_BIT;

		writel(data, addr + offset);

		offset = NS2_IDM_RST_CTRL_P1_OFFSET;
		data = readl(addr + offset);

		if (assert)
			data |= NS2_IDM_RESET_CONTROL_BIT;
		else
			data &= ~NS2_IDM_RESET_CONTROL_BIT;

		writel(data, addr + offset);
		break;

	case PHY_VBUS_PPC:
		addr = iphy->reg_base[NS2_USB3_RST_CTRL];
		offset = NS2_IDM_IO_CTRL_P0_OFFSET;

		data = readl(addr + offset);

		if (assert)
			data |= NS2_IDM_IO_CTRL_PPC_CFG;
		else
			data &= ~NS2_IDM_IO_CTRL_PPC_CFG;

		writel(data, addr + offset);

		offset = NS2_IDM_IO_CTRL_P1_OFFSET;
		data = readl(addr + offset);

		if (assert)
			data |= NS2_IDM_IO_CTRL_PPC_CFG;
		else
			data &= ~NS2_IDM_IO_CTRL_PPC_CFG;

		writel(data, addr + offset);
		break;

	case PHY_REF_CLOCK:
		addr = iphy->reg_base[NS2_USB3_PHY_CFG];
		offset = NS2_USB3_PHY_CONFIG_CTRL_REG;

		data = readl(addr + offset);

		data &= ~NS2_USB3_PHY_CONFIG_CTRL_MASK;

		writel(data, addr + offset);
		break;

	case PHY_PLL_SEQ_START:
		addr = iphy->reg_base[NS2_USB3_PHY_CFG];
		offset = NS2_USB3_PHY_CONFIG_CTRL_REG;

		data = readl(addr + offset);

		data |= NS2_USB3_PHY_CONFIG_CTRL_PLL_SEQ_START;

		writel(data, addr + offset);
		break;

	case PHY_PLL_STATUS:
		count = 2000;
		addr = iphy->reg_base[NS2_USB3_PHY_CFG];
		offset = NS2_USB3_PHY_MISC_STATUS_REG;

		do {
			udelay(1);

			data = readl(addr + offset);
			if (data == 1)
				break;
		} while (--count);

		if (!count)
			ret = -ETIMEDOUT;
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int ns2_usb3_phy_exit(struct phy *phy)
{
	struct ns2_usb3_phy *iphy = phy_get_drvdata(phy);
	int rc = 0;

	mutex_lock(&iphy->mphy->phy_mutex);

	if (iphy->mphy->init_count <= 0) {
		mutex_unlock(&iphy->mphy->phy_mutex);
		return 0;
	} else if (iphy->mphy->init_count == 1) {
		/* Only put in to reset for last port to exit */
		rc = iproc_ns2_phy_action(iphy, PHY_PLL_RESET, true);
		if (rc)
			goto out;

		rc = iproc_ns2_phy_action(iphy, PHY_SOFT_RESET, true);
		if (rc)
			goto out;

		rc = iproc_ns2_phy_action(iphy, PHY_RESET, true);
		if (rc)
			goto out;

		rc = iproc_ns2_phy_action(iphy, PHY_PIPE_RESET, true);
		if (rc)
			goto out;
	}

out:
	iphy->mphy->init_count--;
	mutex_unlock(&iphy->mphy->phy_mutex);

	return rc;
}

static int ns2_usb3_phy_init(struct phy *phy)
{
	struct ns2_usb3_phy *iphy = phy_get_drvdata(phy);
	u16 addr;
	u16 reg_val;
	int rc;

	mutex_lock(&iphy->mphy->phy_mutex);

	if (iphy->mphy->init_count) {
		/* Use count to identify last port to call phy_exit. */
		iphy->mphy->init_count++;
		mutex_unlock(&iphy->mphy->phy_mutex);
		return 0;
	}

	rc = iproc_ns2_phy_action(iphy, PHY_RESET, false);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_SOFT_RESET, true);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_PIPE_RESET, true);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_REF_CLOCK, true);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_PLL_RESET, true);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_RESET, true);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_RESET, false);
	if (rc)
		goto out;

	/* PLL programming */
	/* PHY PLL30 Block */
	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_BLK_ACCESS,
				NS2_USB3_MDIO_PLL30_ADDR);
	if (rc)
		goto out;

	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_PLL30_ANAPLL_CTRL,
				NS2_USB3_MDIO_PLL30_ANAPLL_CTRL_VAL);
	if (rc)
		goto out;

	reg_val = (u16) mdiobus_read(iphy->mphy->mdiodev->bus,
				iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_PLL30_GEN_PLL);
	reg_val |= NS2_USB3_MDIO_PLL30_GEN_PLL_PCLK_SEL;

	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_PLL30_GEN_PLL, reg_val);
	if (rc)
		goto out;

	/* PHY AFE30 Block */
	addr = NS2_USB3_MDIO_P0_AFE30_ADDR;
	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_BLK_ACCESS, addr);
	if (rc)
		goto out;

	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_AFE30_RX_SIG_DETECT,
				NS2_USB3_MDIO_AFE30_RX_SIG_DETECT_VAL);
	if (rc)
		goto out;

	addr = NS2_USB3_MDIO_P1_AFE30_ADDR;
	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_BLK_ACCESS, addr);
	if (rc)
		goto out;

	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_AFE30_RX_SIG_DETECT,
				NS2_USB3_MDIO_AFE30_RX_SIG_DETECT_VAL);
	if (rc)
		goto out;

	/* PHY PIPE Block */
	addr = NS2_USB3_MDIO_P0_PIPE_BLK_ADDR;
	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_BLK_ACCESS, addr);
	if (rc)
		goto out;

	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_PIPE_BLK_REG_1_OFFSET,
				NS2_USB3_MDIO_PIPE_BLK_REG_1_VAL);
	if (rc)
		goto out;

	addr = NS2_USB3_MDIO_P1_PIPE_BLK_ADDR;
	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_BLK_ACCESS, addr);
	if (rc)
		goto out;

	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_PIPE_BLK_REG_1_OFFSET,
				NS2_USB3_MDIO_PIPE_BLK_REG_1_VAL);
	if (rc)
		goto out;

	/* AEQ Block */
	addr = NS2_USB3_MDIO_P0_AEQ_BLK_ADDR;
	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_BLK_ACCESS, addr);
	if (rc)
		goto out;

	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_AEQ_BLK_REG_1_OFFSET,
				NS2_USB3_MDIO_AEQ_BLK_REG_1_VAL);
	if (rc)
		goto out;

	/* PHY PORT_1 */
	addr = NS2_USB3_MDIO_P1_AEQ_BLK_ADDR;
	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_BLK_ACCESS, addr);
	if (rc)
		goto out;

	rc = mdiobus_write(iphy->mphy->mdiodev->bus, iphy->mphy->mdiodev->addr,
				NS2_USB3_MDIO_AEQ_BLK_REG_1_OFFSET,
				NS2_USB3_MDIO_AEQ_BLK_REG_1_VAL);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_PLL_SEQ_START, true);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_PIPE_RESET, false);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_SOFT_RESET, false);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_PLL_RESET, false);
	if (rc)
		goto out;

	rc = iproc_ns2_phy_action(iphy, PHY_PLL_STATUS, true);
	if (rc)
		goto out;

	/* Set USB3H VBUS PPC Polarity and NandNor select */
	rc = iproc_ns2_phy_action(iphy, PHY_VBUS_PPC, true);

out:
	iphy->mphy->init_count++;
	mutex_unlock(&iphy->mphy->phy_mutex);

	return rc;
}

static struct phy_ops ns2_usb3_phy_ops = {
	.init = ns2_usb3_phy_init,
	.exit = ns2_usb3_phy_exit,
	.owner = THIS_MODULE,
};

static int ns2_usb3_phy_map_by_name(struct ns2_usb3_phy *iphy,
				    enum ns2_reg_base reg, const char *compat)
{
	struct device_node *temp;
	void __iomem *base;
	struct device *dev = &iphy->mphy->mdiodev->dev;

	temp = of_find_compatible_node(NULL, NULL, compat);
	if (!temp) {
		dev_err(dev, "Unable to find %s in Device Tree\n", compat);
		return -EINVAL;
	}

	base = of_iomap(temp, 0);
	if (!base) {
		dev_err(dev, "Unable to map %s\n", compat);
		return -EINVAL;
	}
	iphy->reg_base[reg] = base;

	return 0;
}

static int ns2_usb3_phy_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &mdiodev->dev;
	struct device_node *dn = dev->of_node, *child;
	struct ns2_usb3_phy_master *mphy;
	struct phy_provider *provider;
	int cnt;

	mphy = devm_kzalloc(dev, sizeof(*mphy), GFP_KERNEL);
	if (!mphy)
		return -ENOMEM;
	mphy->mdiodev = mdiodev;
	mutex_init(&mphy->phy_mutex);
	mphy->init_count = 0;

	cnt = 0;
	for_each_available_child_of_node(dn, child) {
		struct ns2_usb3_phy *iphy;
		unsigned int val;
		int rc;

		iphy = &mphy->iphys[cnt];
		if (of_property_read_u32(child, "reg", &val)) {
			dev_err(dev, "missing reg property in node %s\n",
					child->name);
			return -EINVAL;
		}
		iphy->port_no = val;
		iphy->mphy = mphy;

		rc = ns2_usb3_phy_map_by_name(iphy, NS2_USB3_CTRL,
					      "brcm,cdru-usb3-ctrl");
		if (rc)
			return rc;

		rc = ns2_usb3_phy_map_by_name(iphy, NS2_USB3_PHY_CFG,
					      "brcm,icfg-usb");
		if (rc)
			return rc;

		rc = ns2_usb3_phy_map_by_name(iphy, NS2_USB3_RST_CTRL,
					      "brcm,usb3-idm");
		if (rc)
			return rc;

		iphy->phy = devm_phy_create(dev, child, &ns2_usb3_phy_ops);
		if (IS_ERR(iphy->phy)) {
			dev_err(dev, "failed to create PHY\n");
			return PTR_ERR(iphy->phy);
		}

		phy_set_drvdata(iphy->phy, iphy);
		cnt++;
	}

	dev_set_drvdata(dev, mphy);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider)) {
		dev_err(dev, "could not register PHY provider\n");
		return PTR_ERR(provider);
	}

	dev_info(dev, "registered %d phy(s)\n", cnt);
	return 0;
}

static const struct of_device_id ns2_usb3_phy_of_match[] = {
	{.compatible = "brcm,ns2-usb3-phy",},
	{ /* sentinel */ }
};

static struct mdio_driver ns2_usb3_phy_driver = {
	.mdiodrv = {
		.driver = {
			.name = "ns2-usb3-phy",
			.of_match_table = ns2_usb3_phy_of_match,
		},
	},
	.probe = ns2_usb3_phy_probe,
};
mdio_module_driver(ns2_usb3_phy_driver);

MODULE_DESCRIPTION("Broadcom NS2 USB3 PHY driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Broadcom");
