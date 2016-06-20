/**
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
 **/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/if_vlan.h>
#include <linux/if_ether.h>

#include "core.h"
#include "pcie.h"
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
		pr_err("%s: invalid macid received: %u\n", __func__,
		       macid);
		return NULL;
	}

	mac = bus->mac[macid];

	if (unlikely(!mac) || unlikely(!mac->mac_started)) {
		pr_err("%s: mac %u not initialized\n", __func__,
		       macid);
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
		pr_err_ratelimited("%s: invalid magic(0x%x:0x%x)\n",
				   __func__, meta->magic_s, meta->magic_e);
		goto out;
	}

	if (unlikely(meta->macid >= QTNF_MAX_MAC)) {
		pr_err_ratelimited("%s: invalid macid(%u)\n", __func__,
				   meta->macid);
		goto out;
	}

	if (unlikely(meta->ifidx >= QTNF_MAX_INTF)) {
		pr_err_ratelimited("%s: invalid vifid(%u)\n", __func__,
				   meta->ifidx);
		goto out;
	}

	mac = bus->mac[meta->macid];

	if (unlikely(!mac)) {
		pr_err_ratelimited("%s: mac(%d) does not exist\n", __func__,
				   meta->macid);
		goto out;
	}

	vif = &mac->iflist[meta->ifidx];

	if (unlikely(vif->wdev.iftype == NL80211_IFTYPE_UNSPECIFIED)) {
		pr_err_ratelimited("%s: vif(%u) does not exists\n", __func__,
				   meta->ifidx);
		goto out;
	}

	ndev = vif->netdev;

	if (unlikely(!ndev)) {
		pr_err_ratelimited("%s: netdev for wlan%u.%u does not exists\n",
				   __func__, meta->macid, meta->ifidx);
		goto out;
	}

	__skb_trim(skb, skb->len - sizeof(*meta));

	pr_debug("RCVD MAC(%d) VIF(%d)\n", meta->macid, meta->ifidx);

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

	if (unlikely(vif->wdev.iftype == NL80211_IFTYPE_UNSPECIFIED)) {
		dev_kfree_skb_any(skb);
		return 0;
	}

	mac = vif->mac;
	if (unlikely(!mac)) {
		pr_warn("start_xmit: NULL mac pointer");
		dev_kfree_skb_any(skb);
		return 0;
	}

	if (!skb->len || (skb->len > ETH_FRAME_LEN)) {
		pr_err("start_xmit: invalid skb len %d\n", skb->len);
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return 0;
	}

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

	if (unlikely(!vif || !vif->mac))
		return;

	pr_warn("qtnf: Tx timeout- %lu, mac/vif num = %d/%d\n", jiffies,
		vif->mac->macid, vif->vifid);

	netif_carrier_off(ndev);
	qtnf_virtual_intf_cleanup(ndev);
	qtnf_netdev_updown(ndev, 0);
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
MODULE_LICENSE("Dual BSD/GPL");
