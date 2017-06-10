/*
 * Copyright (c) 2016~2017 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/etherdevice.h>
#include <linux/kernel.h>

#include "hclge_cmd.h"
#include "hclge_main.h"

enum hclge_mdio_c22_op_seq {
	HCLGE_MDIO_C22_WRITE = 1,
	HCLGE_MDIO_C22_READ = 2
};

enum hclge_mdio_c45_op_seq {
	HCLGE_MDIO_C45_WRITE_ADDR = 0,
	HCLGE_MDIO_C45_WRITE_DATA,
	HCLGE_MDIO_C45_READ_INCREMENT,
	HCLGE_MDIO_C45_READ
};

#define HCLGE_MDIO_CTRL_START_BIT       BIT(0)
#define HCLGE_MDIO_CTRL_ST_MSK  GENMASK(2, 1)
#define HCLGE_MDIO_CTRL_ST_LSH  1
#define HCLGE_MDIO_IS_C22(c22)  (((c22) << HCLGE_MDIO_CTRL_ST_LSH) & \
	HCLGE_MDIO_CTRL_ST_MSK)

#define HCLGE_MDIO_CTRL_OP_MSK  GENMASK(4, 3)
#define HCLGE_MDIO_CTRL_OP_LSH  3
#define HCLGE_MDIO_CTRL_OP(access) \
	(((access) << HCLGE_MDIO_CTRL_OP_LSH) &	HCLGE_MDIO_CTRL_OP_MSK)
#define HCLGE_MDIO_CTRL_PRTAD_MSK       GENMASK(4, 0)
#define HCLGE_MDIO_CTRL_DEVAD_MSK       GENMASK(4, 0)

#define HCLGE_MDIO_STA_VAL(val)	((val) & BIT(0))

struct hclge_mdio_cfg_cmd {
	u8 ctrl_bit;
	u8 prtad;       /* The external port address */
	u8 devad;       /* The external device address */
	u8 rsvd;
	__le16 addr_c45;/* Only valid for c45 */
	__le16 data_wr;
	__le16 data_rd;
	__le16 sta;
};

static int hclge_mdio_write(struct mii_bus *bus, int phy_id, int regnum,
			    u16 data)
{
	struct hclge_dev *hdev = (struct hclge_dev *)bus->priv;
	struct hclge_mdio_cfg_cmd *mdio_cmd;
	enum hclge_cmd_status status;
	struct hclge_desc desc;
	u8 is_c45, devad;
	u16 reg;

	if (!bus)
		return -EINVAL;

	is_c45 = !!(regnum & MII_ADDR_C45);
	devad = ((regnum >> 16) & 0x1f);
	reg = (u16)(regnum & 0xffff);

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_MDIO_CONFIG, false);

	mdio_cmd = (struct hclge_mdio_cfg_cmd *)desc.data;

	if (!is_c45) {
		/* C22 write reg and data */
		mdio_cmd->ctrl_bit = HCLGE_MDIO_IS_C22(!is_c45);
		mdio_cmd->ctrl_bit |= HCLGE_MDIO_CTRL_OP(HCLGE_MDIO_C22_WRITE);
		mdio_cmd->ctrl_bit |= HCLGE_MDIO_CTRL_START_BIT;
		mdio_cmd->data_wr = cpu_to_le16(data);
		mdio_cmd->devad = devad & HCLGE_MDIO_CTRL_DEVAD_MSK;
		mdio_cmd->prtad = phy_id & HCLGE_MDIO_CTRL_PRTAD_MSK;
	} else {
		/* Set phy addr */
		mdio_cmd->ctrl_bit |= HCLGE_MDIO_CTRL_START_BIT;
		mdio_cmd->addr_c45 = cpu_to_le16(reg);
		mdio_cmd->data_wr = cpu_to_le16(data);
		mdio_cmd->devad = devad & HCLGE_MDIO_CTRL_DEVAD_MSK;
		mdio_cmd->prtad = phy_id & HCLGE_MDIO_CTRL_PRTAD_MSK;
	}

	status = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (status) {
		dev_err(&hdev->pdev->dev,
			"mdio write fail when sending cmd, status is %d.\n",
			status);
		return -EIO;
	}

	return 0;
}

static int hclge_mdio_read(struct mii_bus *bus, int phy_id, int regnum)
{
	struct hclge_dev *hdev = (struct hclge_dev *)bus->priv;
	struct hclge_mdio_cfg_cmd *mdio_cmd;
	enum hclge_cmd_status status;
	struct hclge_desc desc;
	u8 is_c45, devad;
	u16 reg;

	if (!bus)
		return -EINVAL;

	is_c45 = !!(regnum & MII_ADDR_C45);
	devad = ((regnum >> 16) & GENMASK(4, 0));
	reg = (u16)(regnum & GENMASK(15, 0));

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_MDIO_CONFIG, true);

	mdio_cmd = (struct hclge_mdio_cfg_cmd *)desc.data;

	dev_dbg(&bus->dev, "phy id=%d, is_c45=%d, devad=%d, reg=%#x!\n",
		phy_id, is_c45, devad, reg);

	if (!is_c45) {
		/* C22 read reg */
		mdio_cmd->ctrl_bit = HCLGE_MDIO_IS_C22(!is_c45);
		mdio_cmd->ctrl_bit |= HCLGE_MDIO_CTRL_OP(HCLGE_MDIO_C22_READ);
		mdio_cmd->ctrl_bit |= HCLGE_MDIO_CTRL_START_BIT;
		mdio_cmd->devad = reg & HCLGE_MDIO_CTRL_DEVAD_MSK;
		mdio_cmd->prtad = phy_id & HCLGE_MDIO_CTRL_PRTAD_MSK;
	} else {
		/* C45 phy addr */
		mdio_cmd->ctrl_bit |= HCLGE_MDIO_CTRL_START_BIT;

		mdio_cmd->addr_c45 = cpu_to_le16(reg);
		mdio_cmd->devad = devad & HCLGE_MDIO_CTRL_DEVAD_MSK;
		mdio_cmd->prtad = phy_id & HCLGE_MDIO_CTRL_PRTAD_MSK;
	}

	/* Read out phy data */
	status = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (status) {
		dev_err(&hdev->pdev->dev,
			"mdio read fail when get data, status is %d.\n",
			status);
		return -EIO;
	}

	if (le16_to_cpu(HCLGE_MDIO_STA_VAL(mdio_cmd->sta))) {
		dev_err(&hdev->pdev->dev, "mdio read data error\n");
		return -ENOMEM;
	}

	return le16_to_cpu(mdio_cmd->data_rd);
}

int hclge_mac_mdio_config(struct hclge_dev *hdev)
{
	struct hclge_mac *mac = &hdev->hw.mac;
	struct mii_bus *mdio_bus;
	struct net_device *ndev = &mac->ndev;
	struct phy_device *phy;
	bool is_c45;
	int ret;

	if (hdev->hw.mac.phy_addr >= PHY_MAX_ADDR)
		return 0;

	if (hdev->hw.mac.phy_if == PHY_INTERFACE_MODE_NA)
		return 0;
	else if (mac->phy_if == PHY_INTERFACE_MODE_SGMII)
		is_c45 = 0;
	else if (mac->phy_if == PHY_INTERFACE_MODE_XGMII)
		is_c45 = 1;
	else
		return -ENODATA;

	SET_NETDEV_DEV(ndev, &hdev->pdev->dev);

	mdio_bus = devm_mdiobus_alloc(&hdev->pdev->dev);
	if (!mdio_bus) {
		ret = -ENOMEM;
		goto err_miibus_alloc;
	}

	mdio_bus->name = "hisilicon MII bus";
	mdio_bus->read = hclge_mdio_read;
	mdio_bus->write = hclge_mdio_write;
	snprintf(mdio_bus->id, MII_BUS_ID_SIZE, "%s-%s", "mii",
		 dev_name(&hdev->pdev->dev));

	mdio_bus->parent = &hdev->pdev->dev;
	mdio_bus->priv = hdev;
	mdio_bus->phy_mask = ~0;
	ret = mdiobus_register(mdio_bus);
	if (ret) {
		dev_err(mdio_bus->parent,
			"Failed to register MDIO bus ret = %#x\n", ret);
		goto err_mdio_register;
	}

	phy = get_phy_device(mdio_bus, mac->phy_addr, is_c45);
	if (!phy || IS_ERR(phy)) {
		dev_err(mdio_bus->parent, "Failed to get phy device\n");
		ret = -EIO;
		goto err_mdio_register;
	}

	phy->irq = mdio_bus->irq[mac->phy_addr];

	/* All data is now stored in the phy struct;
	 * register it
	 */
	ret = phy_device_register(phy);
	if (ret) {
		ret = -ENODEV;
		goto err_phy_register;
	}

	mac->phy_dev = phy;

	return 0;

err_phy_register:
	phy_device_free(phy);
err_mdio_register:
	mdiobus_unregister(mdio_bus);
	mdiobus_free(mdio_bus);
err_miibus_alloc:
	return ret;
}

static void hclge_mac_adjust_link(struct net_device *net_dev)
{
	int duplex;
	int speed;
	struct hclge_mac *hw_mac;
	struct hclge_hw *hw;
	struct hclge_dev *hdev;

	if (!net_dev)
		return;

	hw_mac = container_of(net_dev, struct hclge_mac, ndev);
	hw = container_of(hw_mac, struct hclge_hw, mac);
	hdev = hw->back;

	speed = hw_mac->phy_dev->speed;
	duplex = hw_mac->phy_dev->duplex;

	/* update antoneg. */
	hw_mac->autoneg = hw_mac->phy_dev->autoneg;

	if ((hw_mac->speed != speed) || (hw_mac->duplex != duplex))
		(void)hclge_cfg_mac_speed_dup(hdev, speed, !!duplex);
}

int hclge_mac_start_phy(struct hclge_dev *hdev)
{
	struct hclge_mac *mac = &hdev->hw.mac;
	struct phy_device *phy_dev = mac->phy_dev;
	int ret;

	if (!phy_dev)
		return 0;

	if (mac->phy_if != PHY_INTERFACE_MODE_XGMII) {
		phy_dev->dev_flags = 0;

		ret = phy_connect_direct(&mac->ndev, phy_dev,
					 hclge_mac_adjust_link,
					 mac->phy_if);
		phy_dev->supported = SUPPORTED_10baseT_Half |
				SUPPORTED_10baseT_Full |
				SUPPORTED_100baseT_Half |
				SUPPORTED_100baseT_Full |
				SUPPORTED_Autoneg |
				SUPPORTED_1000baseT_Full;

		phy_dev->autoneg = false;
	} else {
		ret = phy_attach_direct(&mac->ndev, phy_dev, 0, mac->phy_if);
		phy_dev->supported = SUPPORTED_10000baseR_FEC |
				SUPPORTED_10000baseKR_Full;
	}
	if (unlikely(ret))
		return -ENODEV;

	phy_start(phy_dev);

	return 0;
}

void hclge_mac_stop_phy(struct hclge_dev *hdev)
{
	struct hclge_mac *mac = &hdev->hw.mac;
	struct phy_device *phy_dev = mac->phy_dev;

	if (!phy_dev)
		return;

	phy_stop(phy_dev);

	if (mac->phy_if != PHY_INTERFACE_MODE_XGMII)
		phy_disconnect(phy_dev);
	else
		phy_detach(phy_dev);
}
