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
	__le16 reserve;
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
	u8 devad;

	if (!bus)
		return -EINVAL;

	devad = ((regnum >> 16) & 0x1f);

	dev_dbg(&bus->dev, "phy id=%d, devad=%d\n", phy_id, devad);

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_MDIO_CONFIG, false);

	mdio_cmd = (struct hclge_mdio_cfg_cmd *)desc.data;

	mdio_cmd->prtad = phy_id & HCLGE_MDIO_CTRL_PRTAD_MSK;
	mdio_cmd->data_wr = cpu_to_le16(data);
	mdio_cmd->devad = devad & HCLGE_MDIO_CTRL_DEVAD_MSK;

	/* Write reg and data */
	mdio_cmd->ctrl_bit = HCLGE_MDIO_IS_C22(1);
	mdio_cmd->ctrl_bit |= HCLGE_MDIO_CTRL_OP(HCLGE_MDIO_C22_WRITE);
	mdio_cmd->ctrl_bit |= HCLGE_MDIO_CTRL_START_BIT;

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
	u8 devad;

	if (!bus)
		return -EINVAL;

	devad = ((regnum >> 16) & GENMASK(4, 0));

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_MDIO_CONFIG, true);

	mdio_cmd = (struct hclge_mdio_cfg_cmd *)desc.data;

	dev_dbg(&bus->dev, "phy id=%d, devad=%d\n", phy_id, devad);

	mdio_cmd->prtad = phy_id & HCLGE_MDIO_CTRL_PRTAD_MSK;
	mdio_cmd->devad = devad & HCLGE_MDIO_CTRL_DEVAD_MSK;

	/* Write reg and data */
	mdio_cmd->ctrl_bit = HCLGE_MDIO_IS_C22(1);
	mdio_cmd->ctrl_bit |= HCLGE_MDIO_CTRL_OP(HCLGE_MDIO_C22_WRITE);
	mdio_cmd->ctrl_bit |= HCLGE_MDIO_CTRL_START_BIT;

	/* Read out phy data */
	status = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (status) {
		dev_err(&hdev->pdev->dev,
			"mdio read fail when get data, status is %d.\n",
			status);
		return status;
	}

	if (HCLGE_MDIO_STA_VAL(mdio_cmd->sta)) {
		dev_err(&hdev->pdev->dev, "mdio read data error\n");
		return -EIO;
	}

	return le16_to_cpu(mdio_cmd->data_rd);
}

int hclge_mac_mdio_config(struct hclge_dev *hdev)
{
	struct hclge_mac *mac = &hdev->hw.mac;
	struct net_device *ndev = &mac->ndev;
	struct phy_device *phy_dev;
	struct mii_bus *mdio_bus;
	int ret;

	if (hdev->hw.mac.phy_addr >= PHY_MAX_ADDR)
		return 0;

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
	mdio_bus->phy_mask = ~(1 << mac->phy_addr);
	ret = mdiobus_register(mdio_bus);
	if (ret) {
		dev_err(mdio_bus->parent,
			"Failed to register MDIO bus ret = %#x\n", ret);
		goto err_mdio_register;
	}

	phy_dev = mdiobus_get_phy(mdio_bus, mac->phy_addr);
	if (!phy_dev || IS_ERR(phy_dev)) {
		dev_err(mdio_bus->parent, "Failed to get phy device\n");
		ret = -EIO;
		goto err_mdio_register;
	}

	phy_dev->irq = mdio_bus->irq[mac->phy_addr];
	mac->phy_dev = phy_dev;

	return 0;

err_mdio_register:
	mdiobus_unregister(mdio_bus);
	mdiobus_free(mdio_bus);
err_miibus_alloc:
	return ret;
}

static void hclge_mac_adjust_link(struct net_device *net_dev)
{
	struct hclge_mac *hw_mac;
	struct hclge_dev *hdev;
	struct hclge_hw *hw;
	int duplex;
	int speed;

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
	struct net_device *ndev = &mac->ndev;
	int ret;

	if (!phy_dev)
		return 0;

	phy_dev->dev_flags = 0;

	ret = phy_connect_direct(ndev, phy_dev,
				 hclge_mac_adjust_link,
				 PHY_INTERFACE_MODE_SGMII);
	if (unlikely(ret)) {
		pr_info("phy_connect_direct err");
		return -ENODEV;
	}

	phy_dev->supported = SUPPORTED_10baseT_Half |
			SUPPORTED_10baseT_Full |
			SUPPORTED_100baseT_Half |
			SUPPORTED_100baseT_Full |
			SUPPORTED_Autoneg |
			SUPPORTED_1000baseT_Full;

	phy_start(mac->phy_dev);

	return 0;
}

void hclge_mac_stop_phy(struct hclge_dev *hdev)
{
	if (!hdev->hw.mac.phy_dev)
		return;

	phy_disconnect(hdev->hw.mac.phy_dev);
	phy_stop(hdev->hw.mac.phy_dev);
}
