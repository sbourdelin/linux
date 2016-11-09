/*
 * Copyright (c) 2015-2016 Quantenna Communications, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/if_vlan.h>
#include <linux/if_ether.h>

#include "core.h"
#include "bus.h"

#define QTNF_DMP_MAX_LEN 48

struct qtnf_frame_meta_info {
	u8 magic_s;
	u8 ifidx;
	u8 macid;
	u8 magic_e;
} __packed;

static inline int qtnf_is_frame_meta_magic_valid(struct qtnf_frame_meta_info *m)
{
	return m->magic_s == 0xAB && m->magic_e == 0xBA;
}

struct qtnf_wmac *qtnf_core_get_mac(const struct qtnf_bus *bus, u8 macid)
{
	struct qtnf_wmac *mac = NULL;

	if (unlikely(macid >= QTNF_MAX_MAC)) {
		pr_err("received invalid mac(%u)\n", macid);
		return NULL;
	}

	mac = bus->mac[macid];

	if (unlikely(!mac) || unlikely(!mac->mac_started)) {
		pr_err("mac(%u) not initialized\n", macid);
		return NULL;
	}

	return mac;
}

struct net_device *qtnf_classify_skb(struct qtnf_bus *bus, struct sk_buff *skb)
{
	struct qtnf_frame_meta_info *meta;
	struct net_device *ndev = NULL;
	struct qtnf_wmac *mac;
	struct qtnf_vif *vif;

	meta = (struct qtnf_frame_meta_info *)
		(skb_tail_pointer(skb) - sizeof(*meta));

	if (unlikely(!qtnf_is_frame_meta_magic_valid(meta))) {
		pr_err_ratelimited("invalid magic 0x%x:0x%x\n",
				   meta->magic_s, meta->magic_e);
		goto out;
	}

	if (unlikely(meta->macid >= QTNF_MAX_MAC)) {
		pr_err_ratelimited("invalid mac(%u)\n", meta->macid);
		goto out;
	}

	if (unlikely(meta->ifidx >= QTNF_MAX_INTF)) {
		pr_err_ratelimited("invalid vif(%u)\n", meta->ifidx);
		goto out;
	}

	mac = bus->mac[meta->macid];

	if (unlikely(!mac)) {
		pr_err_ratelimited("mac(%d) does not exist\n", meta->macid);
		goto out;
	}

	vif = &mac->iflist[meta->ifidx];

	if (unlikely(vif->wdev.iftype == NL80211_IFTYPE_UNSPECIFIED)) {
		pr_err_ratelimited("vif(%u) does not exists\n", meta->ifidx);
		goto out;
	}

	ndev = vif->netdev;

	if (unlikely(!ndev)) {
		pr_err_ratelimited("netdev for wlan%u.%u does not exists\n",
				   meta->macid, meta->ifidx);
		goto out;
	}

	__skb_trim(skb, skb->len - sizeof(*meta));

	pr_debug("packet received from mac/vif = %d/%d\n",
		 meta->macid, meta->ifidx);

out:
	return ndev;
}
EXPORT_SYMBOL_GPL(qtnf_classify_skb);

/* Netdev handler for open.
 */
static int qtnf_netdev_open(struct net_device *ndev)
{
	netif_carrier_off(ndev);
	qtnf_netdev_updown(ndev, 1);
	return 0;
}

/* Netdev handler for close.
 */
static int qtnf_netdev_close(struct net_device *ndev)
{
	netif_carrier_off(ndev);
	qtnf_virtual_intf_cleanup(ndev);
	qtnf_netdev_updown(ndev, 0);
	return 0;
}

/* Netdev handler for data transmission.
 */
static int
qtnf_netdev_hard_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct qtnf_vif *vif;
	struct qtnf_wmac *mac;

	vif = qtnf_netdev_get_priv(ndev);

	if (unlikely(skb->dev != ndev)) {
		pr_err_ratelimited("invalid skb->dev");
		dev_kfree_skb_any(skb);
		return 0;
	}

	if (unlikely(vif->wdev.iftype == NL80211_IFTYPE_UNSPECIFIED)) {
		pr_err_ratelimited("unsupported vif type (%d)\n",
				   vif->wdev.iftype);
		dev_kfree_skb_any(skb);
		return 0;
	}

	mac = vif->mac;
	if (unlikely(!mac)) {
		pr_err_ratelimited("NULL mac pointer");
		dev_kfree_skb_any(skb);
		return 0;
	}

	if (!skb->len || (skb->len > ETH_FRAME_LEN)) {
		pr_err_ratelimited("invalid skb len %d\n", skb->len);
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return 0;
	}

	/* tx path is enabled: reset vif timeout */
	vif->cons_tx_timeout_cnt = 0;

	return qtnf_bus_data_tx(mac->bus, skb);
}

/* Netdev handler for getting stats.
 */
static struct net_device_stats *qtnf_netdev_get_stats(struct net_device *dev)
{
	return &dev->stats;
}

/* Netdev handler for transmission timeout.
 */
static void qtnf_netdev_tx_timeout(struct net_device *ndev)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(ndev);
	struct qtnf_wmac *mac;
	struct qtnf_bus *bus;

	if (unlikely(!vif || !vif->mac || !vif->mac->bus))
		return;

	mac = vif->mac;
	bus = mac->bus;

	pr_warn("Tx timeout- %lu, mac/vif = %d/%d\n",
		jiffies, mac->macid, vif->vifid);

	qtnf_bus_data_tx_timeout(bus, ndev);
	ndev->stats.tx_errors++;

	if (++vif->cons_tx_timeout_cnt > QTNF_TX_TIMEOUT_TRSHLD) {
		pr_err("Tx timeout threshold exceeded !\n");
		pr_err("schedule interface %s reset !\n", netdev_name(ndev));
		queue_work(bus->workqueue, &vif->reset_work);
	}
}

/* Network device ops handlers */
const struct net_device_ops qtnf_netdev_ops = {
	.ndo_open = qtnf_netdev_open,
	.ndo_stop = qtnf_netdev_close,
	.ndo_start_xmit = qtnf_netdev_hard_start_xmit,
	.ndo_tx_timeout = qtnf_netdev_tx_timeout,
	.ndo_get_stats = qtnf_netdev_get_stats,
};

static int __init qtnf_module_init(void)
{
	return 0;
}

static void __exit qtnf_module_exit(void)
{
}

module_init(qtnf_module_init);
module_exit(qtnf_module_exit);

MODULE_AUTHOR("Quantenna Communications");
MODULE_DESCRIPTION("Quantenna 802.11 wireless LAN FullMAC driver.");
MODULE_LICENSE("GPL");
