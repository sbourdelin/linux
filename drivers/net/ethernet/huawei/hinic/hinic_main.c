/*
 * Huawei HiNIC PCI Express Linux driver
 * Copyright(c) 2017 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/if_vlan.h>
#include <linux/semaphore.h>
#include <net/ip.h>
#include <linux/bitops.h>
#include <linux/bitmap.h>

#include "hinic_pci_id_tbl.h"
#include "hinic_hw_dev.h"
#include "hinic_port.h"
#include "hinic_dev.h"

MODULE_AUTHOR("Huawei Technologies CO., Ltd");
MODULE_DESCRIPTION("Huawei Intelligent NIC driver");
MODULE_VERSION(HINIC_DRV_VERSION);
MODULE_LICENSE("GPL");

#define MSG_ENABLE_DEFAULT		(NETIF_MSG_DRV | NETIF_MSG_PROBE | \
					 NETIF_MSG_IFUP |		   \
					 NETIF_MSG_TX_ERR | NETIF_MSG_RX_ERR)

#define VLAN_BITMAP_SIZE(nic_dev)	(ALIGN(VLAN_N_VID, 8) / 8)

static int change_mac_addr(struct net_device *netdev, const u8 *addr);

static int hinic_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	int err;

	netif_info(nic_dev, drv, netdev, "set_mtu mtu = %d\n", new_mtu);

	err = hinic_port_set_mtu(nic_dev, new_mtu);
	if (err)
		netif_err(nic_dev, drv, netdev, "Failed to set port mtu\n");
	else
		netdev->mtu = new_mtu;

	return err;
}

static int hinic_set_mac_addr(struct net_device *netdev, void *addr)
{
	struct sockaddr *saddr = addr;
	unsigned char new_mac[ETH_ALEN];
	int err;

	memcpy(new_mac, saddr->sa_data, ETH_ALEN);

	err = change_mac_addr(netdev, new_mac);
	if (!err)
		memcpy(netdev->dev_addr, new_mac, ETH_ALEN);

	return err;
}

/**
 * change_mac_addr - change the main mac address of network device
 * @netdev: network device
 * @addr: mac address to set
 *
 * Return 0 - Success, negative - Failure
 **/
static int change_mac_addr(struct net_device *netdev, const u8 *addr)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	unsigned long *vlan_bitmap = nic_dev->vlan_bitmap;
	u16 vid = 0;
	int err;

	if (!is_valid_ether_addr(addr))
		return -EADDRNOTAVAIL;

	netif_info(nic_dev, drv, netdev, "change mac addr = %02x %02x %02x %02x %02x %02x\n",
		   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

	down(&nic_dev->mgmt_lock);

	do {
		err = hinic_port_del_mac(nic_dev, netdev->dev_addr, vid);
		if (err) {
			netif_err(nic_dev, drv, netdev,
				  "Failed to delete mac\n");
			break;
		}

		err = hinic_port_add_mac(nic_dev, addr, vid);
		if (err) {
			netif_err(nic_dev, drv, netdev,
				  "Failed to add mac\n");
			break;
		}

		vid = find_next_bit(vlan_bitmap, VLAN_N_VID, vid + 1);
	} while (vid != VLAN_N_VID);

	up(&nic_dev->mgmt_lock);

	return err;
}

static int hinic_vlan_rx_add_vid(struct net_device *netdev,
				 __always_unused __be16 proto, u16 vid)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	unsigned long *vlan_bitmap = nic_dev->vlan_bitmap;
	int ret, err;

	netif_info(nic_dev, drv, netdev, "add vid = %d\n", vid);

	down(&nic_dev->mgmt_lock);

	err = hinic_port_add_vlan(nic_dev, vid);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to add vlan\n");
		goto vlan_add_err;
	}

	err = hinic_port_add_mac(nic_dev, netdev->dev_addr, vid);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to set mac\n");
		goto add_mac_err;
	}

	bitmap_set(vlan_bitmap, vid, 1);

	up(&nic_dev->mgmt_lock);

	return 0;

add_mac_err:
	ret = hinic_port_del_vlan(nic_dev, vid);
	if (ret)
		netif_err(nic_dev, drv, netdev,
			  "Failed to revert by removing vlan\n");

vlan_add_err:
	up(&nic_dev->mgmt_lock);
	return err;
}

static int hinic_vlan_rx_kill_vid(struct net_device *netdev,
				  __always_unused __be16 proto, u16 vid)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	unsigned long *vlan_bitmap = nic_dev->vlan_bitmap;
	int err;

	netif_info(nic_dev, drv, netdev, "remove vid = %d\n", vid);

	down(&nic_dev->mgmt_lock);

	err = hinic_port_del_vlan(nic_dev, vid);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to delete vlan\n");
		goto del_vlan_err;
	}

	bitmap_clear(vlan_bitmap, vid, 1);

	up(&nic_dev->mgmt_lock);

	return 0;

del_vlan_err:
	up(&nic_dev->mgmt_lock);
	return err;
}

static const struct net_device_ops hinic_netdev_ops = {
	.ndo_change_mtu = hinic_change_mtu,
	.ndo_set_mac_address = hinic_set_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_vlan_rx_add_vid = hinic_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid = hinic_vlan_rx_kill_vid,
	/* more operations should be filled */
};

static void netdev_features_init(struct net_device *netdev)
{
	netdev->hw_features = NETIF_F_SG | NETIF_F_HIGHDMA;

	netdev->vlan_features = netdev->hw_features;

	netdev->features = netdev->hw_features | NETIF_F_HW_VLAN_CTAG_FILTER;
}

/**
 * nic_dev_init - Initialize the NIC device
 * @pdev: the NIC pci device
 *
 * Return 0 - Success, negative - Failure
 **/
static int nic_dev_init(struct pci_dev *pdev)
{
	struct hinic_dev *nic_dev;
	struct net_device *netdev;
	struct hinic_hwdev *hwdev;
	int err, num_qps;

	err = hinic_init_hwdev(&hwdev, pdev);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize HW device\n");
		return err;
	}

	num_qps = hinic_hwdev_num_qps(hwdev);
	if (num_qps <= 0) {
		dev_err(&pdev->dev, "Invalid number of QPS\n");
		err = -EINVAL;
		goto num_qps_err;
	}

	netdev = alloc_etherdev_mq(sizeof(*nic_dev), num_qps);
	if (!netdev) {
		pr_err("Failed to allocate Ethernet device\n");
		err = -ENOMEM;
		goto alloc_etherdev_err;
	}

	netdev->netdev_ops = &hinic_netdev_ops;

	nic_dev = (struct hinic_dev *)netdev_priv(netdev);
	nic_dev->hwdev = hwdev;
	nic_dev->netdev = netdev;
	nic_dev->msg_enable = MSG_ENABLE_DEFAULT;

	sema_init(&nic_dev->mgmt_lock, 1);

	nic_dev->vlan_bitmap = kzalloc(VLAN_BITMAP_SIZE(nic_dev), GFP_KERNEL);
	if (!nic_dev->vlan_bitmap) {
		err = -ENOMEM;
		goto vlan_bitmap_err;
	}

	pci_set_drvdata(pdev, netdev);

	err = hinic_port_get_mac(nic_dev, netdev->dev_addr);
	if (err)
		netif_warn(nic_dev, drv, netdev, "Failed to get mac address\n");

	err = hinic_port_add_mac(nic_dev, netdev->dev_addr, 0);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to add mac\n");
		goto add_mac_err;
	}

	err = hinic_port_set_mtu(nic_dev, netdev->mtu);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to set mtu\n");
		goto set_mtu_err;
	}

	netdev_features_init(netdev);

	netif_carrier_off(netdev);

	err = register_netdev(netdev);
	if (err) {
		netif_err(nic_dev, probe, netdev, "Failed to register netdev\n");
		goto reg_netdev_err;
	}

	return 0;

reg_netdev_err:
set_mtu_err:
add_mac_err:
	pci_set_drvdata(pdev, NULL);
	kfree(nic_dev->vlan_bitmap);

vlan_bitmap_err:
	free_netdev(netdev);

alloc_etherdev_err:
num_qps_err:
	hinic_free_hwdev(hwdev);
	return err;
}

static int hinic_probe(struct pci_dev *pdev,
		       const struct pci_device_id *id)
{
	int err = pci_enable_device(pdev);

	if (err) {
		dev_err(&pdev->dev, "Failed to enable PCI device\n");
		return err;
	}

	err = pci_request_regions(pdev, HINIC_DRV_NAME);
	if (err) {
		dev_err(&pdev->dev, "Failed to request PCI regions\n");
		goto pci_regions_err;
	}

	pci_set_master(pdev);

	err = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
	if (err) {
		dev_warn(&pdev->dev, "Couldn't set 64-bit DMA mask\n");
		err = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev, "Failed to set DMA mask\n");
			goto dma_mask_err;
		}
	}

	err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
	if (err) {
		dev_warn(&pdev->dev,
			 "Couldn't set 64-bit consistent DMA mask\n");
		err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev,
				"Failed to set consistent DMA mask\n");
			goto dma_consistent_mask_err;
		}
	}

	err = nic_dev_init(pdev);
	if (err) {
		dev_err(&pdev->dev, "Failed to initialize NIC device\n");
		goto nic_dev_init_err;
	}

	pr_info("HiNIC driver - probed\n");
	return 0;

nic_dev_init_err:
dma_consistent_mask_err:
dma_mask_err:
	pci_release_regions(pdev);

pci_regions_err:
	pci_disable_device(pdev);
	return err;
}

static void hinic_remove(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct hinic_dev *nic_dev;

	if (!netdev)
		return;

	unregister_netdev(netdev);

	pci_set_drvdata(pdev, NULL);

	nic_dev = netdev_priv(netdev);

	kfree(nic_dev->vlan_bitmap);

	hinic_free_hwdev(nic_dev->hwdev);

	free_netdev(netdev);

	pci_release_regions(pdev);
	pci_disable_device(pdev);

	pr_info("HiNIC driver - removed\n");
}

static const struct pci_device_id hinic_pci_table[] = {
	{ PCI_VDEVICE(HUAWEI, PCI_DEVICE_ID_HI1822_PF), 0},
	{ 0, 0}
};
MODULE_DEVICE_TABLE(pci, hinic_pci_table);

static struct pci_driver hinic_driver = {
	.name		= HINIC_DRV_NAME,
	.id_table	= hinic_pci_table,
	.probe		= hinic_probe,
	.remove		= hinic_remove,
};

static int __init hinic_init(void)
{
	return pci_register_driver(&hinic_driver);
}

static void __exit hinic_exit(void)
{
	pci_unregister_driver(&hinic_driver);
}

module_init(hinic_init);
module_exit(hinic_exit);
