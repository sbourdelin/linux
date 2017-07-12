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

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>

#include "hinic_pci_id_tbl.h"
#include "hinic_hw_dev.h"
#include "hinic_dev.h"

MODULE_AUTHOR("Huawei Technologies CO., Ltd");
MODULE_DESCRIPTION("Huawei Intelligent NIC driver");
MODULE_VERSION(HINIC_DRV_VERSION);
MODULE_LICENSE("GPL");

#define MSG_ENABLE_DEFAULT		(NETIF_MSG_DRV | NETIF_MSG_PROBE | \
					 NETIF_MSG_IFUP |		   \
					 NETIF_MSG_TX_ERR | NETIF_MSG_RX_ERR)

static const struct net_device_ops hinic_netdev_ops = {
	/* Operations are empty, should be filled */
};

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

	pci_set_drvdata(pdev, netdev);

	netif_carrier_off(netdev);

	err = register_netdev(netdev);
	if (err) {
		netif_err(nic_dev, probe, netdev, "Failed to register netdev\n");
		goto reg_netdev_err;
	}

	return 0;

reg_netdev_err:
	pci_set_drvdata(pdev, NULL);
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
