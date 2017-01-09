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

static void qtnf_vif_reset_handler(struct work_struct *work)
{
	struct qtnf_vif *vif = container_of(work, struct qtnf_vif, reset_work);

	rtnl_lock();
	qtnf_virtual_intf_local_reset(vif->netdev);
	rtnl_unlock();
}

static int qtnf_add_default_intf(struct qtnf_wmac *mac)
{
	struct qtnf_vif *vif;

	vif = qtnf_get_free_vif(mac);
	if (!vif) {
		pr_err("could not get free vif structure\n");
		return -EFAULT;
	}

	vif->wdev.iftype = NL80211_IFTYPE_AP;
	vif->bss_priority = QTNF_DEF_BSS_PRIORITY;
	vif->wdev.wiphy = priv_to_wiphy(mac);
	INIT_WORK(&vif->reset_work, qtnf_vif_reset_handler);
	vif->cons_tx_timeout_cnt = 0;

	return 0;
}

static struct qtnf_wmac *qtnf_mac_init(struct qtnf_bus *bus, int macid)
{
	struct wiphy *wiphy;
	struct qtnf_wmac *mac;
	unsigned int i;

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
		pr_err("failed to create primary interface for mac(%d)\n",
		       macid);
		wiphy_free(wiphy);
		return NULL;
	}

	mac->mac_started = 1;
	bus->mac[macid] = mac;
	return mac;
}

static int qtnf_mac_init_single_band(struct wiphy *wiphy,
				     struct qtnf_wmac *mac,
				     enum nl80211_band band)
{
	int ret;

	wiphy->bands[band] = kzalloc(sizeof(*wiphy->bands[band]), GFP_KERNEL);
	if (!wiphy->bands[band])
		return -ENOMEM;

	wiphy->bands[band]->band = band;

	ret = qtnf_cmd_get_mac_chan_info(mac, wiphy->bands[band]);
	if (ret) {
		pr_err("failed to get chans info for band %u\n",
		       band);
		return ret;
	}

	qtnf_band_init_rates(wiphy->bands[band]);
	qtnf_band_setup_htvht_caps(&mac->macinfo, wiphy->bands[band]);

	return 0;
}

static int qtnf_mac_init_bands(struct qtnf_wmac *mac)
{
	struct wiphy *wiphy = priv_to_wiphy(mac);
	int ret = 0;

	if (mac->macinfo.bands_cap & QLINK_BAND_2GHZ) {
		ret = qtnf_mac_init_single_band(wiphy, mac, NL80211_BAND_2GHZ);
		if (ret)
			goto out;
	}

	if (mac->macinfo.bands_cap & QLINK_BAND_5GHZ) {
		ret = qtnf_mac_init_single_band(wiphy, mac, NL80211_BAND_5GHZ);
		if (ret)
			goto out;
	}

	if (mac->macinfo.bands_cap & QLINK_BAND_60GHZ)
		ret = qtnf_mac_init_single_band(wiphy, mac, NL80211_BAND_60GHZ);

out:
	return ret;
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
		pr_err("failed to allocate net_device\n");
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
	dev->tx_queue_len = 100;

	qdev_vif = netdev_priv(dev);
	*((void **)qdev_vif) = vif;

	SET_NETDEV_DEV(dev, mac->bus->dev);

	if (register_netdevice(dev)) {
		pr_err("failed to register virtual network device\n");
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

	pr_debug("starting mac(%d) init\n", macid);

	if (!(bus->hw_info.mac_bitmap & BIT(macid))) {
		pr_info("mac(%d) is not available for host operations\n",
			macid);
		return 0;
	}

	mac = qtnf_mac_init(bus, macid);
	if (!mac) {
		pr_err("failed to initialize mac(%d)\n", macid);
		return -1;
	}

	if (qtnf_cmd_get_mac_info(mac)) {
		pr_err("failed to get mac(%d) info\n", macid);
		return -1;
	}

	vif = qtnf_get_base_vif(mac);
	if (!vif) {
		pr_err("could not get valid vif pointer\n");
		return -1;
	}

	if (qtnf_cmd_send_add_intf(vif, NL80211_IFTYPE_AP, vif->mac_addr)) {
		pr_err("could not add primary vif for mac(%d)\n", macid);
		return -1;
	}

	if (qtnf_cmd_send_get_phy_params(mac)) {
		pr_err("could not get phy thresholds for mac(%d)\n", macid);
		return -1;
	}

	if (qtnf_mac_init_bands(mac)) {
		pr_err("could not get channel info for mac(%d)\n", macid);
		return -1;
	}

	if (qtnf_register_wiphy(bus, mac)) {
		pr_err("wiphy registration failed for mac(%d)\n", macid);
		return -1;
	}

	mac->wiphy_registered = 1;

	/* add primary networking interface */
	rtnl_lock();
	if (qtnf_net_attach(mac, vif, "wlan%d", NET_NAME_ENUM,
			    NL80211_IFTYPE_AP)) {
		pr_err("could not attach primary interface for mac(%d)\n",
		       macid);
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

	bus->fw_state = QTNF_FW_STATE_BOOT_DONE;
	qtnf_bus_data_rx_start(bus);

	bus->workqueue = alloc_ordered_workqueue("QTNF_BUS", 0);
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
		pr_err("failed to get HW info\n");
		return -1;
	}

	if (bus->hw_info.ql_proto_ver != QLINK_PROTO_VER) {
		pr_err("qlink protocol version mismatch\n");
		return -1;
	}

	if (bus->hw_info.num_mac > QTNF_MAX_MAC) {
		pr_err("FW reported invalid mac count: %d\n",
		       bus->hw_info.num_mac);
		return -1;
	}

	for (i = 0; i < bus->hw_info.num_mac; i++) {
		if (qtnf_core_mac_init(bus, i)) {
			pr_err("mac(%d) init failed\n", i);
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
	enum nl80211_band band;

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

		for (band = NL80211_BAND_2GHZ;
		     band < NUM_NL80211_BANDS; ++band) {
			if (!wiphy->bands[band])
				continue;

			kfree(wiphy->bands[band]->channels);
			wiphy->bands[band]->n_channels = 0;

			kfree(wiphy->bands[band]);
			wiphy->bands[band] = NULL;
		}

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
