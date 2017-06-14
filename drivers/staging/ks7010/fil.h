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

#ifndef _KS7010_FIL_H
#define _KS7010_FIL_H

#include <linux/compiler.h>
#include <linux/skbuff.h>
#include <linux/ieee80211.h>
#include <linux/skbuff.h>

#include "common.h"

/**
 * fil_nw_type - Network type
 * @NW_TYPE_INFRA: Infrastructure networks.
 * @NW_TYPE_ADHOC: Not implemented.
 */
enum fil_nw_type {
	NW_TYPE_INFRA,
	/* No other network types implemented yet */
	NW_TYPE_ADHOC
};

/**
 * enum fil_wpa_mode - Wi-Fi Protected Access modes.
 * @FIL_WPA_MODE_NONE: WPA not enabled.
 * @FIL_WPA_MODE_WPA: WPA version 1.
 * @FIL_WPA_MODE_RSN: WPA version 2.
 */
enum fil_wpa_mode {
	FIL_WPA_MODE_NONE = 0,
	FIL_WPA_MODE_WPA,
	FIL_WPA_MODE_RSN
};

/**
 * enum fil_scan_type - Scan type.
 * @FIL_SCAN_TYPE_ACTIVE: Use probe request frames it identify networks.
 * @FIL_SCAN_TYPE_PASSIVE: Identify networks by listening for beacons.
 */
enum fil_scan_type {
	FIL_SCAN_TYPE_ACTIVE = 0,
	FIL_SCAN_TYPE_PASSIVE
};

/**
 * struct fil_scan - Data required to initiate a scan.
 * @scan_type: &enum fil_scan_type
 * @ssid: SSID to scan.
 * @ssid_size: Size of SSID.
 * @channels: List of channels to scan.
 * @channels_size: Size (number) of channels in list.
 */
struct fil_scan {
	enum fil_scan_type scan_type;
	u8 *ssid;
	size_t ssid_size;
	u8 *channels;
	size_t channels_size;
};

/* FIXME 802.11g is backward compatible with b? */
enum fil_phy_type {
	FIL_PYH_TYPE_11B_ONLY = 0,
	FIL_PYH_TYPE_11G_ONLY,
	FIL_PYH_TYPE_11BG_COMPATIBLE,
};

/**
 * enum fil_cts_mode - Clear to send mode
 * @FIL_CTS_MODE_FALSE: TODO document this
 * @FIL_CTS_MODE_TRUE: TODO document this
 */
enum fil_cts_mode {
	FIL_CTS_MODE_FALSE = 0,
	FIL_CTS_MODE_TRUE
};

/**
 * enum fil_dot11_auth_type - 802.11 Authentication.
 * @FIL_DOT11_AUTH_TYPE_OPEN_SYSTEM: Open system authentication.
 * @FIL_DOT11_AUTH_TYPE_SHARED_KEY: Shared key authentication.
 */
enum fil_dot11_auth_type {
	FIL_DOT11_AUTH_TYPE_OPEN_SYSTEM = 0,
	FIL_DOT11_AUTH_TYPE_SHARED_KEY
};

/**
 * enum fil_bss_capability_flags - Basic service set capabilities.
 * @BSS_CAP_ESS: Extended service set (mutually exclusive with IBSS).
 * @BSS_CAP_IBSS: Independent service set (mutually exclusive with ESS).
 * @BSS_CAP_CF_POLABLE: Contention free polling bits.
 * @BSS_CAP_CF_POLL_REQ: Contention free polling bits.
 * @BSS_CAP_PRIVACY: Privacy, bit set indicates WEP required.
 * @BSS_CAP_SHORT_PREAMBLE: Bit on for short preamble. 802.11g always
 *	uses short preamble.
 * @BSS_CAP_PBCC: Packet binary convolution coding modulation scheme.
 * @BSS_CAP_CHANNEL_AGILITY: Bit on for channel agility.
 * @BSS_CAP_SHORT_SLOT_TIME: Short slot time (802.11g).
 * @BSS_CAP_DSSS_OFDM: DSSS-OFDM frame construction (802.11g).
 */
enum fil_bss_capability_flags {
	BSS_CAP_ESS             = 0,
	BSS_CAP_IBSS            = 1,
	BSS_CAP_CF_POLABLE      = 2,
	BSS_CAP_CF_POLL_REQ     = 3,
	BSS_CAP_PRIVACY         = 4,
	BSS_CAP_SHORT_PREAMBLE  = 5,
	BSS_CAP_PBCC            = 6,
	BSS_CAP_CHANNEL_AGILITY = 7,
	BSS_CAP_SHORT_SLOT_TIME = 10,
	BSS_CAP_DSSS_OFDM       = 13
};

/**
 * struct fil_set_infra - Data required to set network type to infrastructure.
 * @phy_type: &enum fil_phy_type
 * @cts_mode: &enum fil_cts_mode
 * @scan_type: &enum fil_scan_type
 * @auth_type: &enum fil_dot11_auth_type
 * @capability: Network capability flags, &enum fil_bss_capability_flags.
 * @beacon_lost_count: TODO document this
 * @rates: Operational rates list.
 * @rates_size: Size of rates list.
 * @ssid: Service set identifier.
 * @ssid_size: Size of SSID.
 * @channels: Channel list.
 * @channels_size: Size of channel list.
 * @bssid: Basic service set identifier.
 */
struct fil_set_infra {
	enum fil_phy_type phy_type;
	enum fil_cts_mode cts_mode;
	enum fil_scan_type scan_type;
	enum fil_dot11_auth_type auth_type;

	u16 capability;
	u16 beacon_lost_count;

	u8 *rates;
	size_t rates_size;

	u8 *ssid;
	size_t ssid_size;

	u8 *channels;
	size_t channels_size;

	u8 *bssid;
};

/**
 * struct fil_power_mgmt - Data for device power management.
 * @ps_enable: Enable power save.
 * @wake_up: TODO verify what this does (see comment in fil_types.h).
 * @receive_dtims: Periodically wake up to receive DTIM's.
 */
struct fil_power_mgmt {
	bool ps_enable;
	bool wake_up;
	bool receive_dtims;
};

/**
 * struct fil_gain - TODO document this
 */
struct fil_gain {
	u8 tx_mode;
	u8 rx_mode;
	u8 tx_gain;
	u8 rx_gain;
};

/**
 * struct fil_t_mic_failure_req - Michael MIC failure event frame.
 * @fhdr: &struct fil_t_hdr
 * @count: Notify firmware that this is failure number @count.
 * @timer: Number of jiffies since the last failure.
 *
 * Michael Message Integrity Check must be done by the driver, in the
 * event of a failure use this frame type to notify the firmware of
 * the failure.
 */
struct fil_mic_failure {
	u16 count;
	u16 timer;
};

/* TODO document fil_phy_info (same as fil_t_phy_info_ind) */

/**
 * struct fil_phy_info - PHY information.
 * @rssi: Received signal strength indication.
 * @signal:
 * @noise:
 * @link_speed:
 * @tx_frame:
 * @rx_frame:
 * @tx_error:
 * @rx_error:
 */
struct fil_phy_info {
	u8 rssi;
	u8 signal;
	u8 noise;
	u8 link_speed;
	u32 tx_frame;
	u32 rx_frame;
	u32 tx_error;
	u32 rx_error;
};

/**
 * enum frame_type - Scan response frame type.
 * @FRAME_TYPE_PROBE_RESP: Frame returned in response to a probe
 *	request (active scan).
 * @FRAME_TYPE_BEACON: Frame beacon type.
 */
enum frame_type {
	FRAME_TYPE_PROBE_RESP,
	FRAME_TYPE_BEACON
};

#define FIL_AP_INFO_MAX_SIZE	1024

/**
 * struct fil_scan_ind - Data received from firmware after scan completes.
 * @bssid: Basic service set identifier.
 * @rssi: Received signal strength indication.
 * @signal: TODO document this
 * @noise: TODO document this
 * @channel: Channel for scanned network.
 * @beacon_period: Beacon period (interval) in time units.
 * @capability: Network capability flags, &enum fil_bss_capability_flags.
 * @type: Probe response or beacon, &enum frame_type.
 * @body_size: Size of @body in octets.
 * @body: Scan indication data, made up of consecutive &struct fil_ap_info.
 */
struct fil_scan_ind {
	u8 bssid[ETH_ALEN];
	u8 rssi;
	u8 signal;
	u8 noise;
	u8 channel;
	u16 beacon_period;
	u16 capability;
	enum frame_type type;

	size_t body_size;
	u8 body[FIL_AP_INFO_MAX_SIZE];
};

/**
 * struct fil_ap_info - Information element.
 * @element_id: Information element identifier.
 * @data_size: Size if IE
 * @data: IE data.
 */
struct fil_ap_info {
	u8 element_id;
	u8 data_size;
	u8 data[0];
};

/*
 * FIXME these are constants define by 802.11, does the kernel
 * define these already?
 */
enum element_id {
	ELEMENT_ID_RSN	= 0x30,
	ELEMENT_ID_WPA	= 0xdd
};

/**
 * enum conn_code - Connection code type.
 * @CONN_CODE_CONNECT: Connection.
 * @CONN_CODE_DISCONNECT: Disconnection.
 */
enum conn_code {
	CONN_CODE_CONNECT = 0,
	CONN_CODE_DISCONNECT,
};

#define KS7010_RATES_MAX_SIZE	16
#define KS7010_IE_MAX_SIZE	128

/**
 * struct fil_conn_ind - Data received from firmware on connection.
 * @bssid: Basic service set identifier.
 * @rssi: Received signal strength indication.
 * @signal: TODO document this
 * @noise: TODO document this
 * @channel: Network channel.
 * @beacon_period: Beacon period (interval) in time units.
 * @capability: Network capability flags, &enum fil_bss_capability_flags.
 * @rates_size: Size of rate set.
 * @rates: List of rates supported by connected network.
 * @element_id: IE identifier.
 * @ie_size: Size of data in IE's.
 * @ie: Information elements.
 */
struct fil_conn_ind {
	enum conn_code code;
	u8 bssid[ETH_ALEN];
	u8 rssi;
	u8 signal;
	u8 noise;
	u8 channel;

	u16 beacon_period;
	u16 capability;

	u8 rates_size;
	u8 rates[KS7010_RATES_MAX_SIZE];

	enum element_id element_id;
	size_t ie_size;
	u8 ie[KS7010_IE_MAX_SIZE];
};

/**
 * enum assoc_type -
 * @ASSOC_TYPE_ASSOC: Association type.
 * @ASSOC_TYPE_REASSOC: Re-association type.
 */
enum assoc_type {
	ASSOC_TYPE_ASSOC,
	ASSOC_TYPE_REASSOC
};

/**
 * struct fil_assoc_ind_req_info - Association request information.
 * @type: &enum assoc_type
 * @capability: Network capability flags, &enum fil_bss_capability_flags.
 * @listen_interval: Listen interval.
 * @ap_addr: Current access point MAC address.
 * @ie_size: Number of octets in IE.
 * @ie: Information elements.
 */
struct fil_assoc_ind_req_info {
	enum assoc_type type;
	u16 capability;
	u16 listen_interval;
	u8 ap_addr[ETH_ALEN];
	size_t ie_size;
	u8 *ie;
};

/**
 * struct fil_assoc_ind_resp_info - Association response information.
 * @type: &enum assoc_type
 * @capability: Network capability flags, &enum fil_bss_capability_flags.
 * @status: TODO unknown.
 * @assoc_id: Association identifier.
 * @ie_size: Number of octets in IE.
 * @ie: Information elements.
 */
struct fil_assoc_ind_resp_info {
	enum assoc_type type;
	u16 capability;
	u16 status;
	u16 assoc_id;
	size_t ie_size;
	u8 *ie;
};

/**
 * struct fil_assoc_ind - Data received from firmware on association.
 * @req: &struct fil_assoc_ind_req
 * @resp: &struct fil_assoc_ind_resp
 */
struct fil_assoc_ind {
	struct fil_assoc_ind_req_info req;
	struct fil_assoc_ind_resp_info resp;
};

/**
 * enum fil_tx_type - Tx frame type.
 * @TX_TYPE_AUTH: Authentication frame type.
 * @TX_TYPE_DATA: Data frame type.
 */
enum fil_tx_type {
	TX_TYPE_AUTH,
	TX_TYPE_DATA
};

/**
 * struct fil_tx_data - Data required to initiate a transmission.
 * @da: Destination MAC address.
 * @sa: Source MAC address.
 * @proto: Ethernet protocol.
 * @add_snap_hdr_to_frame: True if frame should include LLC and SNAP headers.
 * @add_protocol_to_frame: True if frame should include the protocol.
 * @type: Authentication/data frame, &enum fil_tx_type.
 * @data: Frame data.
 * @data_size: Frame data size.
 * @skb: Pointer to the sk_buff passed down from networking stack.
 */
struct fil_tx_data {
	u8 *da;
	u8 *sa;
	u16 proto;
	enum fil_tx_type type;
	u8 *data;
	size_t data_size;
	struct sk_buff *skb;
};

/**
 * enum fil_result_code - FIL result code.
 * @RESULT_SUCCESS: Firmware request successful.
 * @RESULT_INVALID_PARAMETERS: Firmware request failed, invalid parameters.
 * @RESULT_NOT_SUPPORTED: Request not supported by firmware.
 */
enum fil_result_code {
	RESULT_SUCCESS = 0,
	RESULT_INVALID_PARAMETERS,
	RESULT_NOT_SUPPORTED,
};

/**
 * struct fil_ops - Firmware Interface Layer callbacks.
 * @start_conf: Confirmation of ks7010_fil_start().
 */
struct fil_ops {
	void (*start_conf)(struct ks7010 *ks, enum fil_result_code result);
	void (*stop_conf)(struct ks7010 *ks, enum fil_result_code result);
	void (*sleep_conf)(struct ks7010 *ks, enum fil_result_code result);
	void (*mic_failure_conf)(struct ks7010 *ks,
				 enum fil_result_code result);
	void (*set_power_mgmt_conf)(struct ks7010 *ks,
				    enum fil_result_code result);
	void (*set_infra_conf)(struct ks7010 *ks, enum fil_result_code result);
	void (*set_infra_bssid_conf)(struct ks7010 *ks,
				     enum fil_result_code result);

	void (*set_mac_addr_conf)(struct ks7010 *ks);
	void (*set_mcast_addresses_conf)(struct ks7010 *ks);
	void (*mcast_filter_enable_conf)(struct ks7010 *ks);
	void (*privacy_invoked_conf)(struct ks7010 *ks);
	void (*set_default_key_index_conf)(struct ks7010 *ks);
	void (*set_key_1_conf)(struct ks7010 *ks);
	void (*set_key_2_conf)(struct ks7010 *ks);
	void (*set_key_3_conf)(struct ks7010 *ks);
	void (*set_key_4_conf)(struct ks7010 *ks);
	void (*set_wpa_enable_conf)(struct ks7010 *ks);
	void (*set_wpa_mode_conf)(struct ks7010 *ks);
	void (*set_wpa_ucast_suite_conf)(struct ks7010 *ks);
	void (*set_wpa_mcast_suite_conf)(struct ks7010 *ks);
	void (*set_wpa_key_mgmt_suite_conf)(struct ks7010 *ks);
	void (*set_ptk_tsc_conf)(struct ks7010 *ks);
	void (*set_gtk_1_tsc_conf)(struct ks7010 *ks);
	void (*set_gtk_2_tsc_conf)(struct ks7010 *ks);
	void (*set_pmk_conf)(struct ks7010 *ks); /* TODO */
	void (*set_region_conf)(struct ks7010 *ks);
	void (*set_rts_thresh_conf)(struct ks7010 *ks);
	void (*set_frag_thresh_conf)(struct ks7010 *ks);
	void (*set_gain_conf)(struct ks7010 *ks);

	void (*get_mac_addr_conf)(struct ks7010 *ks, u8 *data, u16 size);
	void (*get_fw_version_conf)(struct ks7010 *ks, u8 *data, u16 size);
	void (*get_eeprom_cksum_conf)(struct ks7010 *ks, u8 *data, u16 size);
	void (*get_rts_thresh_conf)(struct ks7010 *ks, u8 *data, u16 size);
	void (*get_frag_thresh_conf)(struct ks7010 *ks, u8 *data, u16 size);
	void (*get_gain_conf)(struct ks7010 *ks, u8 *data, u16 size);

	void (*get_phy_info_ind)(struct ks7010 *ks, struct fil_phy_info *ind);

	void (*scan_conf)(struct ks7010 *ks, enum fil_result_code result);

	void (*scan_ind)(struct ks7010 *ks, struct fil_scan_ind *ind);

	/* FIXME understand how connection and association are initiated */
	void (*conn_ind)(struct ks7010 *ks, struct fil_conn_ind *ind);
	void (*assoc_ind)(struct ks7010 *ks, struct fil_assoc_ind *ind);

	void (*data_ind)(struct ks7010 *ks, int key_index,
			 u8 *data, size_t data_size);
};

void ks7010_fil_start(struct ks7010 *ks, enum fil_nw_type type);
void ks7010_fil_stop(struct ks7010 *ks);
void ks7010_fil_sleep(struct ks7010 *ks);

void ks7010_fil_mic_failure(struct ks7010 *ks, struct fil_mic_failure *req);

void ks7010_fil_set_power_mgmt(struct ks7010 *ks, struct fil_power_mgmt *req);

void ks7010_fil_set_infra(struct ks7010 *ks, struct fil_set_infra *req);
void ks7010_fil_set_infra_bssid(struct ks7010 *ks,
				struct fil_set_infra *req, u8 *bssid);

void ks7010_fil_set_mac_addr(struct ks7010 *ks, u8 *addr);
void ks7010_fil_set_mcast_addresses(struct ks7010 *ks,
				    u8 *addresses, int num_addresses);
void ks7010_fil_mcast_filter_enable(struct ks7010 *ks, bool enable);

void ks7010_fil_privacy_invoked(struct ks7010 *ks, bool enable);
void ks7010_fil_set_default_key_index(struct ks7010 *ks, int index);

void ks7010_fil_set_key_1(struct ks7010 *ks, u8 *key, size_t key_size);
void ks7010_fil_set_key_2(struct ks7010 *ks, u8 *key, size_t key_size);
void ks7010_fil_set_key_3(struct ks7010 *ks, u8 *key, size_t key_size);
void ks7010_fil_set_key_4(struct ks7010 *ks, u8 *key, size_t key_size);

void ks7010_fil_wpa_enable(struct ks7010 *ks, bool enable);
void ks7010_fil_set_wpa_mode(struct ks7010 *ks, enum fil_wpa_mode mode);

void ks7010_fil_set_wpa_ucast_suite(struct ks7010 *ks, u8 *cipher,
				    size_t cipher_size);
void ks7010_fil_set_wpa_mcast_suite(struct ks7010 *ks, u8 *cipher,
				    size_t cipher_size);
void ks7010_fil_set_wpa_key_mgmt_suite(struct ks7010 *ks, u8 *cipher,
				       size_t cipher_size);

void ks7010_fil_set_ptk_tsc(struct ks7010 *ks, u8 *seq, size_t seq_size);
void ks7010_fil_set_gtk_1_tsc(struct ks7010 *ks, u8 *seq, size_t seq_size);
void ks7010_fil_set_gtk_2_tsc(struct ks7010 *ks, u8 *seq, size_t seq_size);

void ks7010_set_pmk(struct ks7010 *ks); /* TODO */

void ks7010_fil_set_region(struct ks7010 *ks, u32 region);
void ks7010_fil_set_rts_thresh(struct ks7010 *ks, u32 thresh);
void ks7010_fil_set_frag_thresh(struct ks7010 *ks, u32 thresh);
void ks7010_fil_set_gain(struct ks7010 *ks, struct fil_gain *gain);

void ks7010_fil_get_mac_addr(struct ks7010 *ks);
void ks7010_fil_get_fw_version(struct ks7010 *ks);
void ks7010_fil_get_eeprom_cksum(struct ks7010 *ks);

void ks7010_fil_get_rts_thresh(struct ks7010 *ks);
void ks7010_fil_get_frag_thresh(struct ks7010 *ks);
void ks7010_fil_get_gain(struct ks7010 *ks);

void ks7010_fil_get_phy_info(struct ks7010 *ks, u16 timer);
void ks7010_fil_scan(struct ks7010 *ks, struct fil_scan *req);

int ks7010_fil_tx(struct ks7010 *ks, struct sk_buff *skb,
		  enum fil_tx_type type, struct tx_data *txd);

int ks7010_fil_rx(struct ks7010 *ks, u8 *data, size_t data_size);

#endif	/* _KS7010_FIL_H */
