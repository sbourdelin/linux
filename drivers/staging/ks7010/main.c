/*
 * Driver for KeyStream wireless LAN cards.
 *
 * Copyright (C) 2005-2008 KeyStream Corp.
 * Copyright (C) 2009 Renesas Technology Corp.
 * Copyright (C) 2017 Tobin C. Harding.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/firmware.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio_func.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/inetdevice.h>

#include "ks7010.h"
#include "sdio.h"
#include "cfg80211.h"

/**
 * ks7010_is_asleep() - True if the device is asleep.
 * @ks: The ks7010 device.
 */
bool ks7010_is_asleep(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
	return false;
}

/**
 * ks7010_request_wakeup() - Request the device to enter active mode.
 * @ks: The ks7010 device.
 */
void ks7010_request_wakeup(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

/**
 * ks7010_request_sleep() - Request the device to enter sleep mode.
 * @ks: The ks7010 device.
 */
void ks7010_request_sleep(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

static int ks7010_open(struct net_device *ndev)
{
	struct ks7010_vif *vif = netdev_priv(ndev);

	set_bit(WLAN_ENABLED, &vif->flags);

	if (test_bit(CONNECTED, &vif->flags)) {
		netif_carrier_on(ndev);
		netif_wake_queue(ndev);
	} else {
		netif_carrier_off(ndev);
	}

	return 0;
}

static int ks7010_close(struct net_device *ndev)
{
	struct ks7010_vif *vif = netdev_priv(ndev);

	netif_stop_queue(ndev);
	netif_carrier_off(vif->ndev);

	ks7010_cfg80211_stop(vif);

	clear_bit(WLAN_ENABLED, &vif->flags);

	return 0;
}

static int
ks7010_set_features(struct net_device *dev, netdev_features_t features)
{
	ks_debug("not implemented yet");
	return 0;
}

static void ks7010_set_multicast_list(struct net_device *dev)
{
	ks_debug("not implemented yet");
}

static const struct net_device_ops ks7010_netdev_ops = {
	.ndo_open               = ks7010_open,
	.ndo_stop               = ks7010_close,
	.ndo_start_xmit         = ks7010_tx_start,
	.ndo_set_features       = ks7010_set_features,
	.ndo_set_rx_mode	= ks7010_set_multicast_list,
};

static const unsigned char dummy_addr[] = {
	0x00, 0x0b, 0xe3, 0x00, 0x00, 0x00
};

#define KS7010_TX_TIMEOUT (3 * HZ)

void ks7010_init_netdev(struct net_device *ndev)
{
	struct ks7010_vif *vif = netdev_priv(ndev);
	struct ks7010 *ks = vif->ks;

	ndev->netdev_ops = &ks7010_netdev_ops;
	ndev->destructor = free_netdev;
	ndev->watchdog_timeo = KS7010_TX_TIMEOUT;

	ks->mac_addr_valid = false;
	ether_addr_copy(ks->mac_addr, dummy_addr);
	ether_addr_copy(ndev->dev_addr, dummy_addr);
}

static int ks7010_fetch_fw(struct ks7010 *ks)
{
	const struct firmware *fw_entry = NULL;
	const u8 *data;
	size_t size;
	int ret;

	if (ks->fw)
		return 0;

	if (!ks->dev) {
		ks_debug("no valid pointer to dev");
		return -ENODEV;
	}

	ret = request_firmware(&fw_entry, KS7010_ROM_FILE, ks->dev);
	if (ret) {
		ks_debug("request_firmware() failed");
		return ret;
	}

	data = fw_entry->data;
	size = fw_entry->size;

	/* TODO firmware sanity checks */

	ks->fw = kmemdup(data, size, GFP_KERNEL);
	if (!ks->fw) {
		ret = -ENOMEM;
		goto release_firmware;
	}
	ks->fw_size = size;

	ret = 0;

release_firmware:
	release_firmware(fw_entry);

	return ret;
}

enum fw_check_type {
	FW_CHECK_RUNNING_SINGLE,
	FW_CHECK_RUNNING_REPEAT
};

#define FW_CHECK_NUM_REPEATS	50
#define FW_CHECK_DELAY		10

static bool
ks7010_fw_is_running(struct ks7010 *ks, enum fw_check_type check_type)
{
	int nchecks;
	int i;

	nchecks = 1;
	if (check_type == FW_CHECK_RUNNING_REPEAT)
		nchecks = FW_CHECK_NUM_REPEATS;

	for (i = 0; i < nchecks; i++) {
		if (ks7010_sdio_fw_is_running(ks))
			return true;

		if (i < nchecks - 1)
			mdelay(FW_CHECK_DELAY);
	}

	return false;
}

static int _upload_fw(struct ks7010 *ks)
{
	int ret;

	if (ks7010_fw_is_running(ks, FW_CHECK_RUNNING_SINGLE)) {
		ks_debug("firmware already running");
		return 0;
	}

	if (!ks->fw) {
		ret = ks7010_fetch_fw(ks);
		if (ret) {
			ks_debug("failed to fetch firmware");
			return ret;
		}
	}

	ret = ks7010_sdio_upload_fw(ks, ks->fw, ks->fw_size);
	if (ret) {
		ks_debug("failed to upload firmware");
		goto err_free_fw;
	}

	if (!ks7010_fw_is_running(ks, FW_CHECK_RUNNING_REPEAT)) {
		ks_debug("firmware failed to start");
		ret = -EIO;
		goto err_free_fw;
	}

	return 0;

err_free_fw:
	kfree(ks->fw);

	return ret;
}

int ks7010_init(struct ks7010 *ks)
{
	struct wireless_dev *wdev;
	int ret;

	spin_lock_init(&ks->stats_lock);

	ret = ks7010_tx_init(ks);
	if (ret) {
		ks_err("failed to tx init");
		return ret;
	}

	ret = ks7010_rx_init(ks);
	if (ret) {
		ks_err("failed to rx init");
		goto err_tx_cleanup;
	}

	ret = ks7010_cfg80211_init(ks);
	if (ret) {
		ks_err("failed to configure cfg80211");
		goto err_rx_cleanup;
	}

	ret = _upload_fw(ks);
	if (ret) {
		ks_err("failed to upload firmware: %d", ret);
		goto err_cfg80211_cleanup;
	}

	rtnl_lock();
	wdev = ks7010_cfg80211_add_interface(ks, "wlan%d", NET_NAME_ENUM,
					     INFRA_NETWORK);
	rtnl_unlock();
	if (IS_ERR(wdev)) {
		ret = PTR_ERR(wdev);
		ks_err("failed to add interface\n");
		goto err_free_fw;
	}

	ks_debug("%s: name=%s dev=0x%p, ks=0x%p\n",
		 __func__, wdev->netdev->name, wdev->netdev, ks);

	return 0;

err_free_fw:
	kfree(ks->fw);
err_cfg80211_cleanup:
	ks7010_cfg80211_cleanup(ks);
err_rx_cleanup:
	ks7010_rx_cleanup(ks);
err_tx_cleanup:
	ks7010_tx_cleanup(ks);

	return ret;
}

/**
 * ks7010_cleanup() - Undoes ks7010_init()
 * @ks: The ks7010 device.
 */
void ks7010_cleanup(struct ks7010 *ks)
{
	rtnl_lock();
	ks7010_cfg80211_rm_interface(ks);
	rtnl_unlock();

	kfree(ks->fw);

	ks7010_cfg80211_cleanup(ks);
	ks7010_rx_cleanup(ks);
	ks7010_tx_cleanup(ks);
}

/* FIXME what about the device embedded in the net_device? */

/**
 * ks7010 *ks7010_create() - Create the ks7010 device.
 * @dev: The device embedded within the SDIO function.
 */
struct ks7010 *ks7010_create(struct device *dev)
{
	struct ks7010 *ks;

	ks = ks7010_cfg80211_create();
	if (!ks)
		return NULL;

	ks->dev = dev;
	ks->state = KS7010_STATE_OFF;

	return ks;
}

/**
 * ks7010_destroy() - Destroy the ks7010 device.
 * @ks: The ks7010 device.
 */
void ks7010_destroy(struct ks7010 *ks)
{
	ks->dev = NULL;
	ks7010_cfg80211_destroy(ks);
}
