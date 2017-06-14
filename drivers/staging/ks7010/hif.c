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

#include <linux/kernel.h>
#include <crypto/hash.h>
#include <uapi/linux/wireless.h>

#include "ks7010.h"
#include "hif.h"
#include "fil.h"
#include "cfg80211.h"

/**
 * DOC: Host Interface Layer - Provides abstraction layer on top of
 * Firmware Interface Layer. When interfacing with the device FIL
 * provides the mechanism, HIF provides the policy.
 */

static bool _ptk_available(struct ks7010 *ks)
{
	return ks->vif->wpa_keys[PTK_IDX].key_size > 0;
}

static bool _gtks_available(struct ks7010 *ks)
{
	if (ks->vif->wpa_keys[GTK1_IDX].key_size > 0 &&
	    ks->vif->wpa_keys[GTK2_IDX].key_size > 0)
		return true;

	return false;
}

/**
 * ks7010_hif_tx_start() - HIF initiate transmission.
 * @ks: The ks7010 device.
 * @skb: sk_buff from networking stack.
 * @txd: Passed to FIL as return argument.
 */
int
ks7010_hif_tx_start(struct ks7010 *ks, struct sk_buff *skb, struct tx_data *txd)
{
	struct ethhdr *eth;
	u16 proto;
	enum fil_tx_type type = TX_TYPE_DATA;
	bool have_ptk;
	bool have_gtks;
	bool wpa;
	int ret;

	eth = (struct ethhdr *)skb->data;
	proto = ntohs(eth->h_proto);

	have_ptk = _ptk_available(ks);
	have_gtks = _gtks_available(ks);

	wpa = ks->vif->wpa_enabled && have_ptk;
	if (wpa) {
		if (proto == ETH_P_PAE && !have_gtks)
			type = TX_TYPE_AUTH;
		else
			;/* TODO handle TKIP and CCMP */

	} else if (proto == ETH_P_PAE) {
		type = TX_TYPE_AUTH;
	} else {
		type = TX_TYPE_DATA;
	}

	ret = ks7010_fil_tx(ks, skb, type, txd);
	if (ret)
		return ret;

	return 0;
}

/**
 * ks7010_hif_tx() - Transmit a frame from FIL.
 * @ks: The ks7010 device.
 * @data: The tx data.
 * @data_size: Size of data.
 *
 * Called by the FIL to send a tx frame to the device.
 */
int ks7010_hif_tx(struct ks7010 *ks, u8 *data, size_t data_size)
{
	return ks7010_tx_enqueue(ks, data, data_size);
}

/**
 * ks7010_hif_rx() - HIF response to an rx event.
 * @ks: The ks7010 device.
 * @data: The rx data.
 * @data_size: Size of data.
 */
void ks7010_hif_rx(struct ks7010 *ks, u8 *data, size_t data_size)
{
	ks7010_fil_rx(ks, data, data_size);
}

/**
 * ks7010_hif_get_mac_addr() - Get the MAC address.
 * @ks: The ks7010 device.
 */
void ks7010_hif_get_mac_addr(struct ks7010 *ks)
{
	ks7010_fil_get_mac_addr(ks);
}

static void hif_get_mac_addr_conf(struct ks7010 *ks, u8 *data, u16 size)
{
	if (size != ETH_ALEN) {
		ks_debug("MAC address size error");
		return;
	}

	ether_addr_copy(ks->mac_addr, data);
	ks->mac_addr_valid = true;
}

/**
 * ks7010_hif_get_fw_version() - Get the firmware version.
 * @ks: The ks7010 device.
 */
void ks7010_hif_get_fw_version(struct ks7010 *ks)
{
	ks7010_fil_get_fw_version(ks);
}

static void hif_get_fw_version_conf(struct ks7010 *ks, u8 *data, u16 size)
{
	if (size > ETHTOOL_FWVERS_LEN) {
		ks_debug("firmware version too big");
		return;
	}

	memcpy(ks->fw_version, data, size);
	ks->fw_version_len = size;
}

/**
 * ks7010_hif_set_rts_thresh() - Set the RTS threshold.
 * @ks: The ks7010 device.
 * @thresh: The new request to send threshold.
 */
void ks7010_hif_set_rts_thresh(struct ks7010 *ks, u32 thresh)
{
	if (thresh > IEEE80211_MAX_RTS_THRESHOLD) {
		ks_debug("threshold maximum exceeded, not setting threshold");
		return;
	}

	ks7010_fil_set_rts_thresh(ks, thresh);
}

static void hif_get_rts_thresh_conf(struct ks7010 *ks, u8 *data, u16 size)
{
	/* TODO convert data to threshold value */
	ks_debug("firmware returned %d bytes", size);
}

/**
 * ks7010_hif_set_frag_thresh() - Set the fragmentation threshold.
 * @ks: The ks7010 device.
 * @thresh: The new fragmentation threshold.
 */
void ks7010_hif_set_frag_thresh(struct ks7010 *ks, u32 thresh)
{
	if (thresh > IEEE80211_MAX_FRAG_THRESHOLD) {
		ks_debug("threshold maximum exceeded, not setting threshold");
		return;
	}

	ks7010_fil_set_frag_thresh(ks, thresh);
}

static void hif_get_frag_thresh_conf(struct ks7010 *ks, u8 *data, u16 size)
{
	/* TODO convert data to threshold value */
	ks_debug("firmware returned %d bytes", size);
}

/**
 * ks7010_hif_connect() - Initiate network connection.
 * @ks: The ks7010 device.
 */
int ks7010_hif_connect(struct ks7010 *ks)
{
	/* TODO interface connect with firmware */
	return 0;
}

/**
 * ks7010_hif_reconnect() - Initiate network re-connection.
 * @ks: The ks7010 device.
 */
int ks7010_hif_reconnect(struct ks7010 *ks)
{
	/* TODO interface re-connect with firmware */
	return 0;
}

/**
 * ks7010_hif_disconnect() - Initiate network disconnection.
 * @ks: The ks7010 device.
 */
int ks7010_hif_disconnect(struct ks7010 *ks)
{
	struct ks7010_vif *vif = ks->vif;

	if (!(test_bit(CONNECTED, &vif->flags) ||
	      test_bit(CONNECT_PEND, &vif->flags)))
		return -ENOTCONN;

	/* TODO interface disconnect with firmware */

	/* The connected flag will be cleared in
	 * disconnect event notification.
	 */
	clear_bit(CONNECT_PEND, &vif->flags);

	return 0;
}

/**
 * hif_conn_ind() - Network connection indication.
 * @ks: The ks7010 device.
 * @ind: Data returned by firmware for connection event.
 */
static void hif_conn_ind(struct ks7010 *ks, struct fil_conn_ind *ind)
{
	struct ks7010_vif *vif = ks->vif;

	if (ind->code == CONN_CODE_DISCONNECT) {
		ks_debug("connection event: disconnected");

		clear_bit(CONNECTED, &vif->flags);
		return;
	}
	ks_debug("connection event: connected");

	/* TODO handle connection event */

	spin_lock(&vif->if_lock);
	set_bit(CONNECTED, &vif->flags);
	spin_unlock(&vif->if_lock);
}

static void _add_key(struct ks7010 *ks, int idx, u8 *key_val, size_t key_size)
{
	void (*fil_set_key_fn)(struct ks7010 *ks, u8 *key, size_t key_size);

	switch (idx) {
	case 0:
		fil_set_key_fn = ks7010_fil_set_key_1;
		break;

	case 1:
		fil_set_key_fn = ks7010_fil_set_key_2;
		break;

	case 2:
		fil_set_key_fn = ks7010_fil_set_key_3;
		break;

	case 3:
		fil_set_key_fn = ks7010_fil_set_key_4;
		break;

	default:
		ks_debug("key index out of range: %d", idx);
		return;
	}

	fil_set_key_fn(ks, key_val, key_size);
}

/**
 * ks7010_hif_add_wep_key() - Add WEP key to device.
 * @ks: The ks7010 device.
 * @key_index: Index of key to add.
 */
int ks7010_hif_add_wep_key(struct ks7010 *ks, int key_index)
{
	struct ks7010_vif *vif = ks->vif;
	struct ks7010_wep_key *key;

	if (key_index > KS7010_MAX_WEP_KEY_INDEX) {
		ks_debug("key index %d out of bounds\n", key_index);
		return -EINVAL;
	}

	key = &vif->wep_keys[key_index];
	_add_key(ks, key_index, key->key_val, key->key_size);

	return 0;
}

/**
 * ks7010_hif_add_wpa_key() - Add WPA key to device.
 * @ks: The ks7010 device.
 * @idx: Index of key to add.
 */
int ks7010_hif_add_wpa_key(struct ks7010 *ks, int key_index)
{
	/* TODO interface add_wpa_key with the firmware */
	return 0;
}

/**
 * ks7010_hif_set_default_key() - Set the default key index to use.
 * @ks: The ks7010 device.
 * @idx: New default index.
 */
int ks7010_hif_set_default_key(struct ks7010 *ks, int idx)
{
	struct ks7010_vif *vif = ks->vif;
	int max_idx;

	max_idx = max(KS7010_MAX_WEP_KEY_INDEX, KS7010_MAX_WPA_KEY_INDEX);
	if (idx > max_idx) {
		ks_debug("key index out of bounds: %d", idx);
		return -EINVAL;
	}

	/* FIXME same variable for WEP index and WPA default tx key? */
	vif->def_txkey_index = idx;
	ks7010_fil_set_default_key_index(ks, idx);

	return 0;
}

static enum fil_scan_type hif_to_fil_scan_type(enum hif_bss_scan_type type)
{
	if (type == BSS_SCAN_ACTIVE)
		return FIL_SCAN_TYPE_ACTIVE;

	return FIL_SCAN_TYPE_PASSIVE;
}

/**
 * ks7010_hif_scan() - Initiate a network scan.
 * @ks: The ks7010 device.
 * @type: Scan type, &enum hif_bss_scan_type.
 * @channels: Channels to scan, &struct hif_channels.
 * @ssid: SSID to scan, &struct hif_ssid.
 */
void ks7010_hif_scan(struct ks7010 *ks, enum hif_bss_scan_type type,
		     struct hif_channels *channels, struct hif_ssid *ssid)
{
	struct fil_scan req;

	memset(&req, 0, sizeof(req));

	req.scan_type = hif_to_fil_scan_type(type);
	req.ssid = ssid->buf;
	req.ssid_size = ssid->size;
	req.channels = channels->list;
	req.channels_size = channels->size;

	ks7010_fil_scan(ks, &req);
}

/**
 * ks7010_hif_set_power_mgmt_active() - Disable power save.
 * @ks: The ks7010 device.
 */
void ks7010_hif_set_power_mgmt_active(struct ks7010 *ks)
{
	struct fil_power_mgmt req;

	req.ps_enable = false;
	req.wake_up = true;
	req.receive_dtims = true;

	ks7010_fil_set_power_mgmt(ks, &req);
}

/**
 * ks7010_hif_set_power_mgmt_sleep() - Enable power save, sleep.
 * @ks: The ks7010 device.
 *
 * Power save sleep mode. Wake periodically to receive DTIM's.
 */
void ks7010_hif_set_power_mgmt_sleep(struct ks7010 *ks)
{
	struct fil_power_mgmt req;

	req.ps_enable = true;
	req.wake_up = false;
	req.receive_dtims = true;

	ks7010_fil_set_power_mgmt(ks, &req);
}

/**
 * ks7010_hif_set_power_mgmt_deep_sleep() - Enable power save, deep sleep.
 * @ks: The ks7010 device.
 *
 * Power save deep sleep mode. Do not wake to receive DTIM's.
 */
void ks7010_hif_set_power_mgmt_deep_sleep(struct ks7010 *ks)
{
	struct fil_power_mgmt req;

	req.ps_enable = true;
	req.wake_up = false;
	req.receive_dtims = false;

	ks7010_fil_set_power_mgmt(ks, &req);
}

static void
hif_result_debug_msg(const char *fn_name, enum fil_result_code result)
{
	switch (result) {
	case RESULT_SUCCESS:
		ks_debug("%s result 'success'", fn_name);
		break;

	case RESULT_INVALID_PARAMETERS:
		ks_debug("%s result 'invalid parameters'", fn_name);
		break;

	case RESULT_NOT_SUPPORTED:
		ks_debug("%s result 'not supported'", fn_name);
		break;
	}
}

enum scan_event {
	SCAN_ABORTED = true,
	SCAN_COMPLETED = false
};

static void hif_scan_conf(struct ks7010 *ks, enum fil_result_code result)
{
	hif_result_debug_msg("scan conf", result);

	if (result != RESULT_SUCCESS)
		ks7010_cfg80211_scan_aborted(ks);
}

static void hif_scan_ind(struct ks7010 *ks, struct fil_scan_ind *ind)
{
	/* TODO handle scan indication */

	ks7010_cfg80211_scan_complete(ks);
}

static void hif_data_ind(struct ks7010 *ks, int key_index,
			 u8 *data, size_t data_size)
{
	/* TODO handle data indication */
}

/*
 * FIXME currently all the callbacks are running in software interrupt
 * context, called by the rx bottom half tasklet. Is this correct?
 */

static struct fil_ops fil_ops = {
	.get_fw_version_conf = hif_get_fw_version_conf,
	.get_mac_addr_conf = hif_get_mac_addr_conf,
	.get_rts_thresh_conf = hif_get_rts_thresh_conf,
	.get_frag_thresh_conf = hif_get_frag_thresh_conf,
	.scan_conf = hif_scan_conf,
	.scan_ind = hif_scan_ind,
	.data_ind = hif_data_ind,
	.conn_ind = hif_conn_ind,
};

void ks7010_hif_init(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

void ks7010_hif_cleanup(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

void ks7010_hif_create(struct ks7010 *ks)
{
	ks->fil_ops = &fil_ops;
}

void ks7010_hif_destroy(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

