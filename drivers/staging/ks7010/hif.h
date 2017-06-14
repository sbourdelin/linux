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

#ifndef _KS7010_HIF_H
#define _KS7010_HIF_H

#include <linux/compiler.h>
#include <linux/skbuff.h>
#include <linux/ieee80211.h>

#include "common.h"

#define HIF_MAX_CHANNELS 14
#define HIF_SSID_MAX_LEN 32

#define AP_INFO_RATE_MAX_SIZE	8
#define RATE_SET_MAX_SIZE	16

#define BASIC_RATE	0x80
#define RATE_MASK	0x7F
#define TX_RATE_AUTO	0xff

#define PTK_IDX 0
#define GTK1_IDX 1
#define GTK2_IDX 2

/**
 * enum hif_network_type - Network type.
 * @INFRA_NETWORK: Infrastructure network.
 * @ADHOC_NETWORK: Not implemented.
 */
enum hif_network_type {
	INFRA_NETWORK = 0x01,
	ADHOC_NETWORK = 0x02,
};

/**
 * enum hif_dot11_auth_type - 802.11 Authentication.
 * @DOT11_AUTH_OPEN: Open system authentication.
 * @DOT11_AUTH_SHARED: Shared key authentication.
 */
enum hif_dot11_auth_mode {
	DOT11_AUTH_OPEN		= 0x01,
	DOT11_AUTH_SHARED	= 0x02,
};

/**
 * enum hif_auth_mode - Authentication modes.
 * @AUTH_NONE: Used for WEP and no authentication
 * @AUTH_WPA: Wi-Fi Protected Access version 1.
 * @AUTH_WPA2: Wi-Fi Protected Access version 2.
 * @AUTH_WPA_PSK: Wi-Fi Protected Access version 1 pre-shared key.
 * @AUTH_WPA2_PSK: Wi-Fi Protected Access version 2 pre-shared key.
 */
enum hif_auth_mode {
	AUTH_NONE		= 0x01,
	AUTH_WPA		= 0x02,
	AUTH_WPA2		= 0x04,
	AUTH_WPA_PSK		= 0x08,
	AUTH_WPA2_PSK		= 0x10,
};

/**
 * enum hif_crypt_type - Cryptography protocol.
 * @CRYPT_NONE: No cryptography used.
 * @CRYPT_WEP: Wired Equivalent Protocol.
 * @CRYPT_TKIP: Temporal Key Integrity Protocol (WPA).
 * @CRYPT_AES: Advanced Encryption Standard (RSN).
 */
enum hif_crypt_type {
	CRYPT_NONE          = 0x01,
	CRYPT_WEP           = 0x02,
	CRYPT_TKIP          = 0x04,
	CRYPT_AES           = 0x08,
};

/**
 * enum hif_preamble_type - Used by PHY to synchronize transmiter and receiver.
 * @PREAMBLE_LONG: Long preamble.
 * @PREAMBLE_SHORT: Short preamble.
 */
enum hif_preamble_type {
	PREAMBLE_LONG,
	PREAMBLE_SHORT
};

/**
 * enum hif_bss_scan_type - Scan type.
 * @BSS_SCAN_ACTIVE: Use probe request frames it identify networks.
 * @BSS_SCAN_PASSIVE: Identify networks by listening for beacons.
 */
enum hif_bss_scan_type {
	BSS_SCAN_ACTIVE = 0,
	BSS_SCAN_PASSIVE
};

/**
 * enum hif_nw_phy_type - set_request
 *  (pseudo_adhoc, adhoc, and infrastructure)
 * @PHY_MODE_11B_ONLY: 802.11b
 * @PHY_MODE_11G_ONLY: 802.11g
 *
 * FIXME remove this (802.11g is backward compatible with b)?
 * @PHY_MODE_11BG_COMPATIBLE_MODE:
 */
enum hif_nw_phy_type {
	PHY_MODE_11B_ONLY = 0,
	PHY_MODE_11G_ONLY,
	PHY_MODE_11BG_COMPATIBLE
};

/**
 * enum hif_nw_cts_mode - Clear to send mode.
 * @CTS_MODE_FALSE: false.
 * @CTS_MODE_TRUE: true.
 */
enum hif_nw_cts_mode {
	CTS_MODE_FALSE = 0,
	CTS_MODE_TRUE
};

/**
 * struct hif_channels - Channel list.
 * @list: List of channels, each channel is one octet.
 * @size: The size of the list.
 */
struct hif_channels {
	u8 list[HIF_MAX_CHANNELS];
	size_t size;
};

/**
 * struct hif_ssid - Service set identifier.
 * @buf: Buffer holding the SSID.
 * @size: Size of SSID.
 */
struct hif_ssid {
	char buf[HIF_SSID_MAX_LEN];
	size_t size;
};

/**
 * enum hif_power_mgmt_type
 * @POWER_MGMT_ATTIVE: Initiate request to activate device.
 * @POWER_MGMT_DEEP_SLEEP: Initiate sleep request, do not receive DTIM's.
 * @POWER_MGMT_SLEEP: Initiate sleep request, receive DTIM's.
 */
enum hif_power_mgmt_type {
	POWER_MGMT_ACTIVE,
	POWER_MGMT_DEEP_SLEEP,
	POWER_MGMT_SLEEP
};

struct tx_data;			/* used as return argument */

int ks7010_hif_tx_start(struct ks7010 *ks, struct sk_buff *skb,
			struct tx_data *txd);
int ks7010_hif_tx(struct ks7010 *ks, u8 *data, size_t data_size);
void ks7010_hif_rx(struct ks7010 *ks, u8 *data, size_t data_size);

void ks7010_hif_get_mac_addr(struct ks7010 *ks);
void ks7010_hif_get_fw_version(struct ks7010 *ks);

void ks7010_hif_set_rts_thresh(struct ks7010 *ks, u32 thresh);
void ks7010_hif_set_frag_thresh(struct ks7010 *ks, u32 thresh);

int ks7010_hif_connect(struct ks7010 *ks);
int ks7010_hif_reconnect(struct ks7010 *ks);
int ks7010_hif_disconnect(struct ks7010 *ks);

int ks7010_hif_add_wep_key(struct ks7010 *ks, int idx);
int ks7010_hif_add_wpa_key(struct ks7010 *ks, int key_index);
int ks7010_hif_set_default_key(struct ks7010 *ks, int idx);

void ks7010_hif_scan(struct ks7010 *ks, enum hif_bss_scan_type type,
		     struct hif_channels *channels, struct hif_ssid *ssid);

void ks7010_hif_set_power_mgmt_active(struct ks7010 *ks);
void ks7010_hif_set_power_mgmt_sleep(struct ks7010 *ks);
void ks7010_hif_set_power_mgmt_deep_sleep(struct ks7010 *ks);

void ks7010_hif_init(struct ks7010 *ks);
void ks7010_hif_cleanup(struct ks7010 *ks);

void ks7010_hif_create(struct ks7010 *ks);
void ks7010_hif_destroy(struct ks7010 *ks);

#endif	/* _KS7010_HIF_H */
