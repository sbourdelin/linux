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
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/if_vlan.h>
#include <linux/semaphore.h>
#include <linux/workqueue.h>
#include <net/ip.h>
#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/delay.h>

#include "hinic_pci_id_tbl.h"
#include "hinic_hw_qp.h"
#include "hinic_hw_dev.h"
#include "hinic_port.h"
#include "hinic_tx.h"
#include "hinic_rx.h"
#include "hinic_dev.h"

MODULE_AUTHOR("Huawei Technologies CO., Ltd");
MODULE_DESCRIPTION("Huawei Intelligent NIC driver");
MODULE_VERSION(HINIC_DRV_VERSION);
MODULE_LICENSE("GPL");

static unsigned int tx_weight = 64;
module_param(tx_weight, uint, 0644);
MODULE_PARM_DESC(tx_weight, "Number Tx packets for NAPI budget (default=64)");

static unsigned int rx_weight = 64;
module_param(rx_weight, uint, 0644);
MODULE_PARM_DESC(rx_weight, "Number Rx packets for NAPI budget (default=64)");

#define HINIC_WQ_NAME			"hinic_dev"

#define MSG_ENABLE_DEFAULT		(NETIF_MSG_DRV | NETIF_MSG_PROBE | \
					 NETIF_MSG_IFUP |		   \
					 NETIF_MSG_TX_ERR | NETIF_MSG_RX_ERR)

#define VLAN_BITMAP_SIZE(nic_dev)	(ALIGN(VLAN_N_VID, 8) / 8)

#define work_to_rx_mode_work(work)	\
		container_of(work, struct hinic_rx_mode_work, work)

#define rx_mode_work_to_nic_dev(rx_mode_work) \
		container_of(rx_mode_work, struct hinic_dev, rx_mode_work)

static int change_mac_addr(struct net_device *netdev, const u8 *addr);

static int hinic_get_link_ksettings(struct net_device *netdev,
				    struct ethtool_link_ksettings
				    *link_ksettings)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	struct hinic_port_cap port_cap;
	enum hinic_autoneg_cap autoneg_cap;
	enum hinic_autoneg_state autoneg_state;
	enum hinic_port_link_state link_state;
	int err;

	ethtool_link_ksettings_zero_link_mode(link_ksettings, advertising);
	ethtool_link_ksettings_add_link_mode(link_ksettings, supported,
					     Autoneg);

	link_ksettings->base.speed = SPEED_UNKNOWN;
	link_ksettings->base.autoneg = AUTONEG_DISABLE;
	link_ksettings->base.duplex = DUPLEX_UNKNOWN;

	err = hinic_port_get_cap(nic_dev, &port_cap);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to get port capabilities\n");
		return err;
	}

	err = hinic_port_link_state(nic_dev, &link_state);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to get port link state\n");
		return err;
	}

	if (link_state != HINIC_LINK_STATE_UP) {
		netif_info(nic_dev, drv, netdev, "No link\n");
		return err;
	}

	switch (port_cap.speed) {
	case HINIC_SPEED_10MB_LINK:
		link_ksettings->base.speed = SPEED_10;
		break;

	case HINIC_SPEED_100MB_LINK:
		link_ksettings->base.speed = SPEED_100;
		break;

	case HINIC_SPEED_1000MB_LINK:
		link_ksettings->base.speed = SPEED_1000;
		break;

	case HINIC_SPEED_10GB_LINK:
		link_ksettings->base.speed = SPEED_10000;
		break;

	case HINIC_SPEED_25GB_LINK:
		link_ksettings->base.speed = SPEED_25000;
		break;

	case HINIC_SPEED_40GB_LINK:
		link_ksettings->base.speed = SPEED_40000;
		break;

	case HINIC_SPEED_100GB_LINK:
		link_ksettings->base.speed = SPEED_100000;
		break;

	default:
		link_ksettings->base.speed = SPEED_UNKNOWN;
		break;
	}

	autoneg_cap = port_cap.autoneg_cap;
	autoneg_state = port_cap.autoneg_state;

	if (!!(autoneg_cap & HINIC_AUTONEG_SUPPORTED))
		ethtool_link_ksettings_add_link_mode(link_ksettings,
						     advertising, Autoneg);

	link_ksettings->base.autoneg = (autoneg_state == HINIC_AUTONEG_ACTIVE) ?
				       AUTONEG_ENABLE : AUTONEG_DISABLE;
	link_ksettings->base.duplex = (port_cap.duplex == HINIC_DUPLEX_FULL) ?
				      DUPLEX_FULL : DUPLEX_HALF;

	return 0;
}

static void hinic_get_drvinfo(struct net_device *netdev,
			      struct ethtool_drvinfo *info)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	struct hinic_hwif *hwif = hwdev->hwif;
	struct pci_dev *pdev = hwif->pdev;

	strlcpy(info->driver, HINIC_DRV_NAME, sizeof(info->driver));
	strlcpy(info->version, HINIC_DRV_VERSION, sizeof(info->driver));
	strlcpy(info->bus_info, pci_name(pdev), sizeof(info->bus_info));
}

static void hinic_get_ringparam(struct net_device *netdev,
				struct ethtool_ringparam *ring)
{
	ring->rx_max_pending = HINIC_RQ_DEPTH;
	ring->tx_max_pending = HINIC_SQ_DEPTH;
	ring->rx_pending = HINIC_RQ_DEPTH;
	ring->tx_pending = HINIC_SQ_DEPTH;
}

static void hinic_get_channels(struct net_device *netdev,
			       struct ethtool_channels *channels)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	struct hinic_hwdev *hwdev = nic_dev->hwdev;

	channels->max_rx = hwdev->nic_cap.max_qps;
	channels->max_tx = hwdev->nic_cap.max_qps;
	channels->max_other = 0;
	channels->max_combined = 0;
	channels->rx_count = hinic_hwdev_num_qps(hwdev);
	channels->tx_count = hinic_hwdev_num_qps(hwdev);
	channels->other_count = 0;
	channels->combined_count = 0;
}

static const struct ethtool_ops hinic_ethtool_ops = {
	.get_link_ksettings = hinic_get_link_ksettings,
	.get_drvinfo = hinic_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_ringparam = hinic_get_ringparam,
	.get_channels = hinic_get_channels,
};

static void update_nic_stats(struct hinic_dev *nic_dev)
{
	struct hinic_rxq_stats *nic_rx_stats = &nic_dev->rx_stats;
	struct hinic_txq_stats *nic_tx_stats = &nic_dev->tx_stats;
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	int i, num_qps = hinic_hwdev_num_qps(hwdev);
	struct hinic_rxq_stats rx_stats;
	struct hinic_txq_stats tx_stats;

	u64_stats_init(&tx_stats.syncp);
	u64_stats_init(&rx_stats.syncp);

	for (i = 0; i < num_qps; i++) {
		struct hinic_rxq *rxq = &nic_dev->rxqs[i];

		hinic_rxq_get_stats(rxq, &rx_stats);

		u64_stats_update_begin(&nic_rx_stats->syncp);
		nic_rx_stats->bytes += rx_stats.bytes;
		nic_rx_stats->pkts += rx_stats.pkts;
		u64_stats_update_end(&nic_rx_stats->syncp);

		hinic_rxq_clean_stats(rxq);
	}

	for (i = 0; i < num_qps; i++) {
		struct hinic_txq *txq = &nic_dev->txqs[i];

		hinic_txq_get_stats(txq, &tx_stats);

		u64_stats_update_begin(&nic_tx_stats->syncp);
		nic_tx_stats->bytes += tx_stats.bytes;
		nic_tx_stats->pkts += tx_stats.pkts;
		nic_tx_stats->tx_busy += tx_stats.tx_busy;
		nic_tx_stats->tx_wake += tx_stats.tx_wake;
		nic_tx_stats->tx_dropped += tx_stats.tx_dropped;
		u64_stats_update_end(&nic_tx_stats->syncp);

		hinic_txq_clean_stats(txq);
	}
}

/**
 * create_txqs - Create the Logical Tx Queues of specific NIC device
 * @nic_dev: the specific NIC device
 *
 * Return 0 - Success, negative - Failure
 **/
static int create_txqs(struct hinic_dev *nic_dev)
{
	struct net_device *netdev = nic_dev->netdev;
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	size_t txq_size;
	int err, i, j, num_txqs = hinic_hwdev_num_qps(hwdev);

	if (nic_dev->txqs)
		return -EINVAL;

	txq_size = num_txqs * sizeof(*nic_dev->txqs);
	nic_dev->txqs = kzalloc(txq_size, GFP_KERNEL);
	if (!nic_dev->txqs)
		return -ENOMEM;

	for (i = 0; i < num_txqs; i++) {
		struct hinic_sq *sq = hinic_hwdev_get_sq(hwdev, i);

		err = hinic_init_txq(&nic_dev->txqs[i], sq, nic_dev->netdev);
		if (err) {
			netif_err(nic_dev, drv, netdev, "Failed to init Txq\n");
			goto init_txq_err;
		}
	}

	return 0;

init_txq_err:
	for (j = 0; j < i; j++)
		hinic_clean_txq(&nic_dev->txqs[j]);

	kfree(nic_dev->txqs);
	return err;
}

/**
 * free_txqs - Free the Logical Tx Queues of specific NIC device
 * @nic_dev: the specific NIC device
 **/
static void free_txqs(struct hinic_dev *nic_dev)
{
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	int i, num_txqs = hinic_hwdev_num_qps(hwdev);

	if (!nic_dev->txqs)
		return;

	for (i = 0; i < num_txqs; i++)
		hinic_clean_txq(&nic_dev->txqs[i]);

	kfree(nic_dev->txqs);
	nic_dev->txqs = NULL;
}

/**
 * create_txqs - Create the Logical Rx Queues of specific NIC device
 * @nic_dev: the specific NIC device
 *
 * Return 0 - Success, negative - Failure
 **/
static int create_rxqs(struct hinic_dev *nic_dev)
{
	struct net_device *netdev = nic_dev->netdev;
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	size_t rxq_size;
	int err, i, j, num_rxqs = hinic_hwdev_num_qps(hwdev);

	if (nic_dev->rxqs)
		return -EINVAL;

	rxq_size = num_rxqs * sizeof(*nic_dev->rxqs);
	nic_dev->rxqs = kzalloc(rxq_size, GFP_KERNEL);
	if (!nic_dev->rxqs)
		return -ENOMEM;

	for (i = 0; i < num_rxqs; i++) {
		struct hinic_rq *rq = hinic_hwdev_get_rq(hwdev, i);

		err = hinic_init_rxq(&nic_dev->rxqs[i], rq, nic_dev->netdev);
		if (err) {
			netif_err(nic_dev, drv, netdev, "Failed to init rxq\n");
			goto init_rxq_err;
		}
	}

	return 0;

init_rxq_err:
	for (j = 0; j < i; j++)
		hinic_clean_rxq(&nic_dev->rxqs[j]);

	kfree(nic_dev->rxqs);
	return err;
}

/**
 * free_txqs - Free the Logical Rx Queues of specific NIC device
 * @nic_dev: the specific NIC device
 **/
static void free_rxqs(struct hinic_dev *nic_dev)
{
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	int i, num_rxqs = hinic_hwdev_num_qps(hwdev);

	if (!nic_dev->rxqs)
		return;

	for (i = 0; i < num_rxqs; i++)
		hinic_clean_rxq(&nic_dev->rxqs[i]);

	kfree(nic_dev->rxqs);
	nic_dev->rxqs = NULL;
}

static int hinic_open(struct net_device *netdev)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	enum hinic_port_link_state link_state;
	int err, ret, num_qps = hinic_hwdev_num_qps(hwdev);

	if (!(nic_dev->flags & HINIC_INTF_UP)) {
		err = hinic_hwdev_ifup(hwdev);
		if (err) {
			netif_err(nic_dev, drv, netdev, "Failed - NIC HW if up\n");
			return err;
		}
	}

	err = create_txqs(nic_dev);
	if (err) {
		netif_err(nic_dev, drv, netdev,
			  "Failed to create Tx queues\n");
		goto create_txqs_err;
	}

	err = create_rxqs(nic_dev);
	if (err) {
		netif_err(nic_dev, drv, netdev,
			  "Failed to create Rx queues\n");
		goto create_rxqs_err;
	}

	netif_set_real_num_tx_queues(netdev, num_qps);
	netif_set_real_num_rx_queues(netdev, num_qps);

	err = hinic_port_set_state(nic_dev, HINIC_PORT_ENABLE);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to set port state\n");
		goto port_state_err;
	}

	err = hinic_port_set_func_state(nic_dev, HINIC_FUNC_PORT_ENABLE);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to set func port state\n");
		goto func_port_state_err;
	}

	/* Wait up to 3 sec between port enable to link state */
	msleep(3000);

	err = hinic_port_link_state(nic_dev, &link_state);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to get link state\n");
		goto port_link_err;
	}

	down(&nic_dev->mgmt_lock);

	if (link_state == HINIC_LINK_STATE_UP)
		nic_dev->flags |= HINIC_LINK_UP;

	nic_dev->flags |= HINIC_INTF_UP;

	if ((nic_dev->flags & (HINIC_LINK_UP | HINIC_INTF_UP)) ==
	    (HINIC_LINK_UP | HINIC_INTF_UP)) {
		netif_info(nic_dev, drv, netdev, "link + intf UP\n");
		netif_carrier_on(netdev);
		netif_tx_wake_all_queues(netdev);
	}

	up(&nic_dev->mgmt_lock);

	netif_info(nic_dev, drv, netdev, "HINIC_INTF is UP\n");

	return 0;

port_link_err:
	ret = hinic_port_set_func_state(nic_dev, HINIC_FUNC_PORT_DISABLE);
	if (ret)
		netif_warn(nic_dev, drv, netdev, "Failed to revert func port state\n");

func_port_state_err:
	ret = hinic_port_set_state(nic_dev, HINIC_PORT_DISABLE);
	if (ret)
		netif_warn(nic_dev, drv, netdev, "Failed to revert port state\n");

port_state_err:
	free_rxqs(nic_dev);

create_rxqs_err:
	free_txqs(nic_dev);

create_txqs_err:
	if (!(nic_dev->flags & HINIC_INTF_UP))
		hinic_hwdev_ifdown(hwdev);
	return err;
}

static int hinic_close(struct net_device *netdev)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	unsigned int flags;
	int err;

	down(&nic_dev->mgmt_lock);

	flags = nic_dev->flags;
	nic_dev->flags &= ~HINIC_INTF_UP;

	netif_carrier_off(netdev);
	netif_tx_disable(netdev);

	update_nic_stats(nic_dev);

	up(&nic_dev->mgmt_lock);

	err = hinic_port_set_func_state(nic_dev, HINIC_FUNC_PORT_DISABLE);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to set func port state\n");
		nic_dev->flags |= (flags & HINIC_INTF_UP);
		return err;
	}

	err = hinic_port_set_state(nic_dev, HINIC_PORT_DISABLE);
	if (err) {
		netif_err(nic_dev, drv, netdev, "Failed to set port state\n");
		nic_dev->flags |= (flags & HINIC_INTF_UP);
		return err;
	}

	free_rxqs(nic_dev);
	free_txqs(nic_dev);

	if (flags & HINIC_INTF_UP)
		hinic_hwdev_ifdown(hwdev);

	netif_info(nic_dev, drv, netdev, "HINIC_INTF is DOWN\n");

	return 0;
}

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

/**
 * set_mac_addr - adding mac address to network device
 * @netdev: network device
 * @addr: mac address to add
 *
 * Return 0 - Success, negative - Failure
 **/
static int set_mac_addr(struct net_device *netdev, const u8 *addr)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	unsigned long *vlan_bitmap = nic_dev->vlan_bitmap;
	u16 vid = 0;
	int err;

	if (!is_valid_ether_addr(addr))
		return -EADDRNOTAVAIL;

	netif_info(nic_dev, drv, netdev, "set mac addr = %02x %02x %02x %02x %02x %02x\n",
		   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

	down(&nic_dev->mgmt_lock);

	do {
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

/**
 * remove_mac_addr - remove mac address from network device
 * @netdev: network device
 * @addr: mac address to remove
 *
 * Return 0 - Success, negative - Failure
 **/
static int remove_mac_addr(struct net_device *netdev, const u8 *addr)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	unsigned long *vlan_bitmap = nic_dev->vlan_bitmap;
	u16 vid = 0;
	int err;

	if (!is_valid_ether_addr(addr))
		return -EADDRNOTAVAIL;

	netif_info(nic_dev, drv, netdev, "remove mac addr = %02x %02x %02x %02x %02x %02x\n",
		   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

	down(&nic_dev->mgmt_lock);

	do {
		err = hinic_port_del_mac(nic_dev, addr, vid);
		if (err) {
			netif_err(nic_dev, drv, netdev,
				  "Failed to delete mac\n");
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

static void set_rx_mode(struct work_struct *work)
{
	struct hinic_rx_mode_work *rx_mode_work = work_to_rx_mode_work(work);
	struct hinic_dev *nic_dev = rx_mode_work_to_nic_dev(rx_mode_work);
	struct net_device *netdev = nic_dev->netdev;

	netif_info(nic_dev, drv, netdev, "set rx mode work\n");

	hinic_port_set_rx_mode(nic_dev, rx_mode_work->rx_mode);

	__dev_uc_sync(netdev, set_mac_addr, remove_mac_addr);

	__dev_mc_sync(netdev, set_mac_addr, remove_mac_addr);
}

static void hinic_set_rx_mode(struct net_device *netdev)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	struct hinic_rx_mode_work *rx_mode_work = &nic_dev->rx_mode_work;
	u32 rx_mode =	HINIC_RX_MODE_UC |
			HINIC_RX_MODE_MC |
			HINIC_RX_MODE_BC;

	if (netdev->flags & IFF_PROMISC)
		rx_mode |= HINIC_RX_MODE_PROMISC;
	else if (netdev->flags & IFF_ALLMULTI)
		rx_mode |= HINIC_RX_MODE_MC_ALL;

	rx_mode_work->rx_mode = rx_mode;

	queue_work(nic_dev->workq, &rx_mode_work->work);
}

static u16 hinic_select_queue(struct net_device *netdev, struct sk_buff *skb,
			      void *accel_priv,
			      select_queue_fallback_t fallback)
{
	u16 qid;

	if (skb_rx_queue_recorded(skb))
		qid = skb_get_rx_queue(skb);
	else
		qid = fallback(netdev, skb);

	return qid;
}

static void hinic_get_stats64(struct net_device *netdev,
			      struct rtnl_link_stats64 *stats)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	struct hinic_rxq_stats *nic_rx_stats = &nic_dev->rx_stats;
	struct hinic_txq_stats *nic_tx_stats = &nic_dev->tx_stats;

	stats->rx_bytes = nic_rx_stats->bytes;
	stats->rx_packets = nic_rx_stats->pkts;

	stats->tx_bytes = nic_tx_stats->bytes;
	stats->tx_packets = nic_tx_stats->pkts;
	stats->tx_errors = nic_tx_stats->tx_dropped;

	down(&nic_dev->mgmt_lock);

	if (!(nic_dev->flags & HINIC_INTF_UP)) {
		up(&nic_dev->mgmt_lock);
		return;
	}

	update_nic_stats(nic_dev);

	up(&nic_dev->mgmt_lock);

	stats->rx_bytes = nic_rx_stats->bytes;
	stats->rx_packets = nic_rx_stats->pkts;

	stats->tx_bytes = nic_tx_stats->bytes;
	stats->tx_packets = nic_tx_stats->pkts;
	stats->tx_errors = nic_tx_stats->tx_dropped;
}

static void hinic_tx_timeout(struct net_device *netdev)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);

	netif_err(nic_dev, drv, netdev, "Tx timeout\n");
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void hinic_netpoll(struct net_device *netdev)
{
	struct hinic_dev *nic_dev = netdev_priv(netdev);
	struct hinic_hwdev *hwdev = nic_dev->hwdev;
	int i, num_qps = hinic_hwdev_num_qps(hwdev);

	for (i = 0; i < num_qps; i++) {
		struct hinic_txq *txq = &nic_dev->txqs[i];
		struct hinic_rxq *rxq = &nic_dev->rxqs[i];

		napi_schedule(&txq->napi);
		napi_schedule(&rxq->napi);
	}
}
#endif

static const struct net_device_ops hinic_netdev_ops = {
	.ndo_open = hinic_open,
	.ndo_stop = hinic_close,
	.ndo_change_mtu = hinic_change_mtu,
	.ndo_set_mac_address = hinic_set_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_vlan_rx_add_vid = hinic_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid = hinic_vlan_rx_kill_vid,
	.ndo_set_rx_mode = hinic_set_rx_mode,
	.ndo_start_xmit = hinic_xmit_frame,
	.ndo_select_queue = hinic_select_queue,
	.ndo_get_stats64 = hinic_get_stats64,
	.ndo_tx_timeout = hinic_tx_timeout,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller = hinic_netpoll,
#endif
};

static void netdev_features_init(struct net_device *netdev)
{
	netdev->hw_features = NETIF_F_SG | NETIF_F_HIGHDMA;

	netdev->vlan_features = netdev->hw_features;

	netdev->features = netdev->hw_features | NETIF_F_HW_VLAN_CTAG_FILTER;
}

/**
 * link_status_event_handler - link event handler
 * @handle: nic device for the handler
 * @buf_in: input buffer
 * @in_size: input size
 * @buf_in: output buffer
 * @out_size: returned output size
 *
 * Return 0 - Success, negative - Failure
 **/
static void link_status_event_handler(void *handle, void *buf_in, u16 in_size,
				      void *buf_out, u16 *out_size)
{
	struct hinic_dev *nic_dev = (struct hinic_dev *)handle;
	struct net_device *netdev = nic_dev->netdev;
	struct hinic_port_link_status *link_status, *ret_link_status;

	link_status = buf_in;

	if (link_status->link == HINIC_LINK_STATE_UP) {
		down(&nic_dev->mgmt_lock);

		nic_dev->flags |= HINIC_LINK_UP;

		if ((nic_dev->flags & (HINIC_LINK_UP | HINIC_INTF_UP)) ==
		    (HINIC_LINK_UP | HINIC_INTF_UP)) {
			netif_carrier_on(netdev);
			netif_tx_wake_all_queues(netdev);
		}

		up(&nic_dev->mgmt_lock);

		netif_info(nic_dev, drv, netdev, "HINIC_Link is UP\n");
	} else {
		down(&nic_dev->mgmt_lock);

		nic_dev->flags &= ~HINIC_LINK_UP;

		netif_carrier_off(netdev);
		netif_tx_disable(netdev);

		up(&nic_dev->mgmt_lock);

		netif_info(nic_dev, drv, netdev, "HINIC_Link is DOWN\n");
	}

	ret_link_status = buf_out;
	ret_link_status->status = 0;

	*out_size = sizeof(*ret_link_status);
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
	struct hinic_txq_stats *tx_stats;
	struct hinic_rxq_stats *rx_stats;
	struct hinic_rx_mode_work *rx_mode_work;
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
	netdev->ethtool_ops = &hinic_ethtool_ops;

	nic_dev = (struct hinic_dev *)netdev_priv(netdev);
	nic_dev->hwdev = hwdev;
	nic_dev->netdev = netdev;
	nic_dev->msg_enable = MSG_ENABLE_DEFAULT;
	nic_dev->flags = 0;
	nic_dev->txqs = NULL;
	nic_dev->rxqs = NULL;
	nic_dev->tx_weight = tx_weight;
	nic_dev->rx_weight = rx_weight;

	sema_init(&nic_dev->mgmt_lock, 1);

	tx_stats = &nic_dev->tx_stats;
	rx_stats = &nic_dev->rx_stats;

	u64_stats_init(&tx_stats->syncp);
	u64_stats_init(&rx_stats->syncp);

	nic_dev->vlan_bitmap = kzalloc(VLAN_BITMAP_SIZE(nic_dev), GFP_KERNEL);
	if (!nic_dev->vlan_bitmap) {
		err = -ENOMEM;
		goto vlan_bitmap_err;
	}

	nic_dev->workq = create_singlethread_workqueue(HINIC_WQ_NAME);
	if (!nic_dev->workq) {
		err = -ENOMEM;
		goto workq_err;
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

	rx_mode_work = &nic_dev->rx_mode_work;
	INIT_WORK(&rx_mode_work->work, set_rx_mode);

	netdev_features_init(netdev);

	netif_carrier_off(netdev);

	hinic_hwdev_cb_register(nic_dev->hwdev, HINIC_MGMT_MSG_CMD_LINK_STATUS,
				nic_dev, link_status_event_handler);

	err = register_netdev(netdev);
	if (err) {
		netif_err(nic_dev, probe, netdev, "Failed to register netdev\n");
		goto reg_netdev_err;
	}

	return 0;

reg_netdev_err:
	hinic_hwdev_cb_unregister(nic_dev->hwdev,
				  HINIC_MGMT_MSG_CMD_LINK_STATUS);
	cancel_work_sync(&rx_mode_work->work);

set_mtu_err:
add_mac_err:
	pci_set_drvdata(pdev, NULL);
	destroy_workqueue(nic_dev->workq);

workq_err:
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
	struct hinic_rx_mode_work *rx_mode_work;

	if (!netdev)
		return;

	unregister_netdev(netdev);

	nic_dev = netdev_priv(netdev);

	hinic_hwdev_cb_unregister(nic_dev->hwdev,
				  HINIC_MGMT_MSG_CMD_LINK_STATUS);

	rx_mode_work = &nic_dev->rx_mode_work;
	cancel_work_sync(&rx_mode_work->work);

	pci_set_drvdata(pdev, NULL);

	destroy_workqueue(nic_dev->workq);

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
