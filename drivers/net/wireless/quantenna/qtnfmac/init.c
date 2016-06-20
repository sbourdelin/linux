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

#include <linux/types.h>
#include <linux/export.h>
#include <net/mac80211.h>

#include "core.h"
#include "bus.h"
#include "commands.h"
#include "event.h"
#include "cfg80211.h"
#include "util.h"

struct qtnf_vif *qtnf_get_free_vif(struct qtnf_wmac *mac)
{
	struct qtnf_vif *vif;
	int i;

	for (i = 0; i < QTNF_MAX_INTF; i++) {
		vif = &mac->iflist[i];
		if (vif->wdev.iftype == NL80211_IFTYPE_UNSPECIFIED)
			return vif;
	}

	return NULL;
}

struct qtnf_vif *qtnf_get_base_vif(struct qtnf_wmac *mac)
{
	struct qtnf_vif *vif;

	vif = &mac->iflist[0];

	if (vif->wdev.iftype == NL80211_IFTYPE_UNSPECIFIED)
		return NULL;

	return vif;
}

static int qtnf_add_default_intf(struct qtnf_wmac *mac)
{
	struct qtnf_vif *vif;

	vif = (void *)qtnf_get_free_vif(mac);
	if (!vif) {
		pr_err("qtnfmac: %s:could not get free vif structure\n",
		       __func__);
		return -EFAULT;
	}

	vif->wdev.iftype = NL80211_IFTYPE_AP;
	vif->bss_priority = QTNF_DEF_BSS_PRIORITY;
	vif->wdev.wiphy = priv_to_wiphy(mac);

	return 0;
}

static struct qtnf_wmac *qtnf_init_mac(struct qtnf_bus *bus, int macid)
{
	struct wiphy *wiphy;
	struct qtnf_wmac *mac;
	u8 i;

	wiphy = qtnf_allocate_wiphy(bus);
	if (!wiphy)
		return ERR_PTR(-ENOMEM);

	mac = wiphy_priv(wiphy);

	mac->macid = macid;
	mac->bus = bus;

	for (i = 0; i < QTNF_MAX_INTF; i++) {
		memset(&mac->iflist[i], 0, sizeof(struct qtnf_vif));
		mac->iflist[i].wdev.iftype = NL80211_IFTYPE_UNSPECIFIED;
		mac->iflist[i].mac = mac;
		mac->iflist[i].vifid = i;
		qtnf_sta_list_init(&mac->iflist[i].sta_list);
	}

	if (qtnf_add_default_intf(mac)) {
		pr_err("failed to create default interface for mac=%d\n",
		       macid);
		wiphy_free(wiphy);
		return NULL;
	}

	mac->mac_started = 1;
	bus->mac[macid] = mac;
	return mac;
}

int qtnf_net_attach(struct qtnf_wmac *mac, struct qtnf_vif *vif,
		    const char *name, unsigned char name_assign_type,
		    enum nl80211_iftype iftype)
{
	struct wiphy *wiphy = priv_to_wiphy(mac);
	struct net_device *dev;
	void *qdev_vif;

	dev = alloc_netdev_mqs(sizeof(struct qtnf_vif *), name,
			       name_assign_type, ether_setup, 1, 1);
	if (!dev) {
		pr_err("no memory available for netdevice\n");
		memset(&vif->wdev, 0, sizeof(vif->wdev));
		vif->wdev.iftype = NL80211_IFTYPE_UNSPECIFIED;
		return -ENOMEM;
	}

	vif->netdev = dev;

	dev->netdev_ops = &qtnf_netdev_ops;
	dev->destructor = free_netdev;
	dev_net_set(dev, wiphy_net(wiphy));
	dev->ieee80211_ptr = &vif->wdev;
	dev->ieee80211_ptr->iftype = iftype;
	ether_addr_copy(dev->dev_addr, vif->mac_addr);
	SET_NETDEV_DEV(dev, wiphy_dev(wiphy));
	dev->flags |= IFF_BROADCAST | IFF_MULTICAST;
	dev->watchdog_timeo = QTNF_DEF_WDOG_TIMEOUT;
	dev->tx_queue_len = QTNF_MAX_TX_QLEN;

	qdev_vif = netdev_priv(dev);
	*((unsigned long *)qdev_vif) = (unsigned long)vif;

	SET_NETDEV_DEV(dev, mac->bus->dev);

	if (register_netdevice(dev)) {
		pr_err("qtnfmac: cannot register virtual network device\n");
		free_netdev(dev);
		vif->wdev.iftype = NL80211_IFTYPE_UNSPECIFIED;
		return -EFAULT;
	}

	return 0;
}

static int qtnf_core_mac_init(struct qtnf_bus *bus, int macid)
{
	struct qtnf_wmac *mac;
	struct qtnf_vif *vif;

	pr_info("%s: macid=%d\n", __func__, macid);

	if (!(bus->hw_info.mac_bitmap & BIT(macid))) {
		pr_warn("WMAC with id=%d is not available for RC operation\n",
			macid);
		return 0;
	}

	mac = qtnf_init_mac(bus, macid);
	if (!mac) {
		pr_err("%s: failed to initialize mac; macid=%d\n", __func__,
		       macid);
		return -1;
	}

	if (qtnf_cmd_get_mac_info(mac)) {
		pr_err("failed to get MAC information\n");
		return -1;
	}

	vif = qtnf_get_base_vif(mac);
	if (!vif) {
		pr_err("core_attach: could not get valid vif pointer\n");
		return -1;
	}

	if (qtnf_cmd_send_add_intf(vif, NL80211_IFTYPE_AP, vif->mac_addr)) {
		pr_err("core_attach: could not add default vif\n");
		return -1;
	}

	if (qtnf_cmd_send_get_phy_params(mac)) {
		pr_err("core_attach: could not get phy thresholds for mac\n");
		return -1;
	}

	if (qtnf_cmd_get_mac_chan_info(mac)) {
		pr_err("core_attach: could not get channel information for mac\n");
		return -1;
	}

	if (qtnf_register_wiphy(bus, mac)) {
		pr_err("core_attach: wiphy registartion failed\n");
		return -1;
	}

	mac->wiphy_registered = 1;

	/* add primary networking interface */
	rtnl_lock();
	if (qtnf_net_attach(mac, vif, "wlan%d", NET_NAME_ENUM,
			    NL80211_IFTYPE_AP)) {
		pr_err("could not attach netdev\n");
		vif->wdev.iftype = NL80211_IFTYPE_UNSPECIFIED;
		vif->netdev = NULL;
		rtnl_unlock();
		return -1;
	}
	rtnl_unlock();

	return 0;
}

int qtnf_core_attach(struct qtnf_bus *bus)
{
	int i;

	qtnf_trans_init(bus);

	if (qtnf_bus_poll_state(bus, QTN_BUS_DEVICE, QTN_EP_FW_QLINK_DONE,
				QTN_FW_QLINK_TIMEOUT_MS)) {
		pr_err("Qlink server init timeout\n");
		return -1;
	}

	bus->fw_state = QTNF_FW_STATE_BOOT_DONE;
	qtnf_bus_data_rx_start(bus);

	bus->workqueue = alloc_ordered_workqueue("QTNF", 0);
	if (!bus->workqueue) {
		pr_err("failed to alloc main workqueue\n");
		return -1;
	}

	INIT_WORK(&bus->event_work, qtnf_event_work_handler);

	if (qtnf_cmd_send_init_fw(bus)) {
		pr_err("failed to send FW init commands\n");
		return -1;
	}

	bus->fw_state = QTNF_FW_STATE_ACTIVE;

	if (qtnf_cmd_get_hw_info(bus)) {
		pr_err("failed to get HW information\n");
		return -1;
	}

	if (bus->hw_info.ql_proto_ver != QLINK_PROTO_VER) {
		pr_err("qlink protocol version mismatch\n");
		return -1;
	}

	if (bus->hw_info.num_mac > QTNF_MAX_MAC) {
		pr_err("invalid supported mac number from EP\n");
		return -1;
	}

	for (i = 0; i < bus->hw_info.num_mac; i++) {
		if (qtnf_core_mac_init(bus, i)) {
			pr_err("init failed for mac interface; macid=%d", i);
			return -1;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(qtnf_core_attach);

void qtnf_core_detach(struct qtnf_bus *bus)
{
	struct wiphy *wiphy;
	struct qtnf_wmac *mac;
	struct qtnf_vif *vif;
	int i, cnt;

	for (cnt = 0; cnt < QTNF_MAX_MAC; cnt++) {
		mac = bus->mac[cnt];

		if (!mac || !mac->mac_started)
			continue;

		wiphy = priv_to_wiphy(mac);

		for (i = 0; i < QTNF_MAX_INTF; i++) {
			vif = &mac->iflist[i];
			rtnl_lock();
			if (vif->netdev &&
			    vif->wdev.iftype != NL80211_IFTYPE_UNSPECIFIED) {
				qtnf_virtual_intf_cleanup(vif->netdev);
				qtnf_del_virtual_intf(wiphy, &vif->wdev);
			}
			rtnl_unlock();
			qtnf_sta_list_free(&vif->sta_list);
		}

		if (mac->wiphy_registered)
			wiphy_unregister(wiphy);

		kfree(mac->macinfo.channels);
		kfree(mac->macinfo.limits);
		kfree(wiphy->iface_combinations);
		wiphy_free(wiphy);
		bus->mac[cnt] = NULL;
	}

	if (bus->workqueue) {
		flush_workqueue(bus->workqueue);
		destroy_workqueue(bus->workqueue);
	}

	qtnf_trans_free(bus);
}
EXPORT_SYMBOL_GPL(qtnf_core_detach);
