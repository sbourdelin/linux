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

#include <crypto/hash.h>
#include <uapi/linux/wireless.h>
#include <linux/skbuff.h>

#include "ks7010.h"
#include "sdio.h"
#include "fil.h"
#include "eap.h"
#include "fil_types.h"

/**
 * DOC: Firmware Interface Layer - Set and get variables to and from
 * the device firmware.
 */

/*
 * fil_t_hdr->size has different meaning depending on receive path or
 * transmit path. Keep all the logic here in one place.
 */

static __le16 tx_frame_size_to_fil_t_hdr_size(size_t frame_size)
{
	struct fil_t_hdr fhdr;

	return cpu_to_le16((u16)(frame_size - sizeof(fhdr.size)));
}

static size_t rx_fil_t_hdr_to_frame_size(struct fil_t_hdr *fhdr)
{
	return le16_to_cpu(fhdr->size);
}

/**
 * fil_alloc_tx_frame() - Allocate a tx frame buffer.
 * @frame_size: Frame size in octets.
 * @event: &struct fil_t_event
 *
 * Allocates an aligned frame big enough to fit @frame_size
 * octets. Once fil_alloc_frame() returns we do not know how much
 * memory was allocated, _tx_align() recalculates the aligned size.
 *
 * Sets the &struct fil_t_hdr size and event members.
 */
static void *fil_alloc_tx_frame(size_t frame_size, enum fil_t_event event)
{
	struct fil_t_hdr *fhdr;
	size_t aligned_size;

	aligned_size = fil_align_size(frame_size);

	if (aligned_size > MAX_U16_VAL) {
		ks_err("aligning frame overflows u16: %zu", frame_size);
		return NULL;
	}

	fhdr = kzalloc(aligned_size, GFP_ATOMIC);
	if (!fhdr)
		return NULL;

	fhdr->size = tx_frame_size_to_fil_t_hdr_size(frame_size);
	fhdr->event = cpu_to_le16((u16)event);

	return fhdr;
}

static void fil_tx(struct ks7010 *ks, void *data, size_t frame_size)
{
	int ret;
	size_t data_size;

	data_size = fil_align_size(frame_size);

	ret = ks7010_hif_tx(ks, data, data_size);
	if (ret)
		ks_debug("failed to queue tx data");
}

static void fil_mib_get_req(struct ks7010 *ks, enum mib_attribute attr)
{
	struct fil_t_mib_get_req *hdr;
	size_t frame_size;

	frame_size = sizeof(*hdr);

	hdr = fil_alloc_tx_frame(frame_size, FIL_T_MIB_GET_REQ);
	if (!hdr) {
		ks_debug("fil_alloc_tx_frame failed for attr: %d", (int)attr);
		return;
	}

	hdr->attribute = cpu_to_le32(attr);
	fil_tx(ks, hdr, frame_size);
}

static void _fil_mib_set_req(struct ks7010 *ks,
			     enum mib_attribute attr,
			     enum mib_data_type type,
			     u8 *data, size_t data_size)
{
	struct fil_t_mib_set_req *hdr;
	size_t frame_size;

	frame_size = sizeof(*hdr) + data_size;
	if (frame_size > MAX_U16_VAL) {
		ks_debug("u16 overflow, attr: %d size: %d",
			 (int)attr, (int)frame_size);
		return;
	}

	hdr = fil_alloc_tx_frame(frame_size, FIL_T_MIB_SET_REQ);
	if (!hdr) {
		ks_debug("fil_alloc_tx_frame failed for attr: %d", (int)attr);
		return;
	}

	hdr->attribute = cpu_to_le32(attr);
	hdr->data_size = cpu_to_le16((u16)data_size);
	hdr->data_type = cpu_to_le16(type);
	memcpy(&hdr->data, data, data_size);

	fil_tx(ks, hdr, frame_size);
}

static void
fil_mib_set_req_int(struct ks7010 *ks, enum mib_attribute attr, u32 val)
{
	__le32 v = cpu_to_le32(val);

	_fil_mib_set_req(ks, attr, FIL_T_MIB_TYPE_INT, (u8 *)&v, sizeof(v));
}

static void
fil_mib_set_req_bool(struct ks7010 *ks, enum mib_attribute attr, bool val)
{
	__le32 v = cpu_to_le32((u32)val);

	_fil_mib_set_req(ks, attr, FIL_T_MIB_TYPE_BOOL, (u8 *)&v, sizeof(v));
}

static void fil_mib_set_req_ostring(struct ks7010 *ks, enum mib_attribute attr,
				    u8 *data, size_t data_size)
{
	_fil_mib_set_req(ks, attr, FIL_T_MIB_TYPE_OSTRING, data, data_size);
}

static void fil_simple_req(struct ks7010 *ks, enum fil_t_event event)
{
	struct fil_t_hdr *hdr;
	size_t frame_size = sizeof(*hdr);

	hdr = fil_alloc_tx_frame(frame_size, event);
	if (!hdr)
		return;

	fil_tx(ks, hdr, frame_size);
}

void ks7010_fil_start(struct ks7010 *ks, enum fil_nw_type nw_type)
{
	struct fil_t_start_req *hdr;
	size_t frame_size = sizeof(*hdr);

	if (nw_type != NW_TYPE_INFRA) {
		ks_debug("driver supports infrastructure networks only");
		return;
	}

	hdr = fil_alloc_tx_frame(frame_size, FIL_T_START_REQ);
	if (!hdr)
		return;

	hdr->nw_type = cpu_to_le16((u16)nw_type);

	fil_tx(ks, hdr, frame_size);
}

void ks7010_fil_stop(struct ks7010 *ks)
{
	fil_simple_req(ks, FIL_T_STOP_REQ);
}

void ks7010_fil_sleep(struct ks7010 *ks)
{
	fil_simple_req(ks, FIL_T_SLEEP_REQ);
}

void
ks7010_fil_mic_failure(struct ks7010 *ks, struct fil_mic_failure *req)
{
	struct fil_t_mic_failure_req *hdr;
	size_t frame_size = sizeof(*hdr);

	hdr = fil_alloc_tx_frame(frame_size, FIL_T_MIC_FAILURE_REQ);
	if (!hdr)
		return;

	hdr->count = cpu_to_le16(req->count);
	hdr->timer = cpu_to_le16(req->timer);

	fil_tx(ks, hdr, frame_size);
}

void ks7010_fil_set_power_mgmt(struct ks7010 *ks, struct fil_power_mgmt *req)
{
	struct fil_t_power_mgmt_req *hdr;
	size_t frame_size = sizeof(*hdr);

	hdr = fil_alloc_tx_frame(frame_size, FIL_T_POWER_MGMT_REQ);
	if (!hdr)
		return;

	if (req->ps_enable)
		hdr->mode = cpu_to_le32(FIL_T_POWER_MGMT_MODE_SAVE);
	else
		hdr->mode = cpu_to_le32(FIL_T_POWER_MGMT_MODE_ACTIVE);

	if (req->wake_up)
		hdr->wake_up = cpu_to_le32(FIL_T_POWER_MGMT_WAKE_UP_TRUE);
	else
		hdr->wake_up = cpu_to_le32(FIL_T_POWER_MGMT_WAKE_UP_FALSE);

	if (req->receive_dtims)
		hdr->receive_dtims =
			cpu_to_le32(FIL_T_POWER_MGMT_RECEIVE_DTIMS_TRUE);
	else
		hdr->receive_dtims =
			cpu_to_le32(FIL_T_POWER_MGMT_RECEIVE_DTIMS_FALSE);

	fil_tx(ks, hdr, frame_size);
}

static bool _set_infra_req_is_valid(struct fil_set_infra *req)
{
	if (req->ssid_size > FIL_T_SSID_MAX_SIZE) {
		ks_debug("ssid size to big: %zu", req->ssid_size);
		return false;
	}

	if (req->channels_size > FIL_T_CHANNELS_MAX_SIZE) {
		ks_debug("channels size to big: %zu", req->channels_size);
		return false;
	}

	if (req->rates_size > FIL_T_INFRA_SET_REQ_RATES_MAX_SIZE) {
		ks_debug("rates size to big: %zu", req->rates_size);
		return false;
	}

	return true;
}

void ks7010_fil_set_infra(struct ks7010 *ks, struct fil_set_infra *req)
{
	struct fil_t_infra_set_req *hdr;
	struct _infra_set_req *ptr;
	size_t frame_size = sizeof(*hdr);

	if (!_set_infra_req_is_valid(req))
		return;

	hdr = fil_alloc_tx_frame(frame_size, FIL_T_INFRA_SET_REQ);
	if (!hdr)
		return;

	ptr = &hdr->req;

	ptr->phy_type = cpu_to_le16((u16)req->phy_type);
	ptr->cts_mode = cpu_to_le16((u16)req->cts_mode);
	ptr->scan_type = cpu_to_le16((u16)req->scan_type);
	ptr->auth_type = cpu_to_le16((u16)req->auth_type);

	ptr->capability = cpu_to_le16(req->capability);
	ptr->beacon_lost_count = cpu_to_le16(req->beacon_lost_count);

	memcpy(&ptr->rates.body[0], &req->rates, req->rates_size);
	ptr->rates.size = req->rates_size;

	memcpy(&ptr->ssid.body[0], req->ssid, req->ssid_size);
	ptr->ssid.size = req->ssid_size;

	memcpy(&ptr->channels.body[0], req->channels, req->channels_size);
	ptr->channels.size = req->channels_size;

	fil_tx(ks, hdr, frame_size);
}

void ks7010_fil_set_infra_bssid(
	struct ks7010 *ks, struct fil_set_infra *req, u8 *bssid)
{
	struct fil_t_infra_set2_req *hdr;
	struct _infra_set_req *ptr;
	size_t frame_size = sizeof(*hdr);

	if (!_set_infra_req_is_valid(req))
		return;

	hdr = fil_alloc_tx_frame(frame_size, FIL_T_INFRA_SET2_REQ);
	if (!hdr)
		return;

	ptr = &hdr->req;

	ptr->phy_type = cpu_to_le16((u16)req->phy_type);
	ptr->cts_mode = cpu_to_le16((u16)req->cts_mode);
	ptr->scan_type = cpu_to_le16((u16)req->scan_type);
	ptr->auth_type = cpu_to_le16((u16)req->auth_type);

	ptr->capability = cpu_to_le16(req->capability);
	ptr->beacon_lost_count = cpu_to_le16(req->beacon_lost_count);

	memcpy(&ptr->rates.body[0], &req->rates, req->rates_size);
	ptr->rates.size = req->rates_size;

	memcpy(&ptr->ssid.body[0], req->ssid, req->ssid_size);
	ptr->ssid.size = req->ssid_size;

	memcpy(&ptr->channels.body[0], req->channels, req->channels_size);
	ptr->channels.size = req->channels_size;

	memcpy(hdr->bssid, bssid, ETH_ALEN);

	fil_tx(ks, hdr, frame_size);
}

void ks7010_fil_set_mac_addr(struct ks7010 *ks, u8 *addr)
{
	fil_mib_set_req_ostring(ks, LOCAL_CURRENT_ADDRESS, addr, ETH_ALEN);
}

#define FIL_T_MCAST_MAX_NUM_ADDRS 32

/**
 * ks7010_fil_set_mcast_addr() - Set multicast address list.
 * @ks: The ks7010 device.
 * @addresses: Consecutive Ethernet addresses.
 * @num_addresses: Number of addresses in @addresses.
 */
void ks7010_fil_set_mcast_addresses(
	struct ks7010 *ks, u8 *addresses, int num_addresses)
{
	size_t size;

	if (num_addresses > FIL_T_MCAST_MAX_NUM_ADDRS) {
		ks_debug("to many mcast addresses: %d", num_addresses);
		return;
	}

	size = num_addresses * ETH_ALEN;
	fil_mib_set_req_ostring(ks, LOCAL_MULTICAST_ADDRESS, addresses, size);
}

void ks7010_fil_mcast_filter_enable(struct ks7010 *ks, bool enable)
{
	fil_mib_set_req_bool(ks, LOCAL_MULTICAST_FILTER, enable);
}

void ks7010_fil_privacy_invoked(struct ks7010 *ks, bool enable)
{
	fil_mib_set_req_bool(ks, DOT11_PRIVACY_INVOKED, enable);
}

void ks7010_fil_set_default_key_index(struct ks7010 *ks, int idx)
{
	fil_mib_set_req_int(ks, MIB_DEFAULT_KEY_INDEX, idx);
}

void ks7010_fil_set_key_1(struct ks7010 *ks, u8 *key, size_t key_size)
{
	fil_mib_set_req_ostring(ks, MIB_KEY_VALUE_1, key, key_size);
}

void ks7010_fil_set_key_2(struct ks7010 *ks, u8 *key, size_t key_size)
{
	fil_mib_set_req_ostring(ks, MIB_KEY_VALUE_2, key, key_size);
}

void ks7010_fil_set_key_3(struct ks7010 *ks, u8 *key, size_t key_size)
{
	fil_mib_set_req_ostring(ks, MIB_KEY_VALUE_3, key, key_size);
}

void ks7010_fil_set_key_4(struct ks7010 *ks, u8 *key, size_t key_size)
{
	fil_mib_set_req_ostring(ks, MIB_KEY_VALUE_4, key, key_size);
}

void ks7010_fil_wpa_enable(struct ks7010 *ks, bool enable)
{
	fil_mib_set_req_bool(ks, MIB_WPA_ENABLE, enable);
}

void ks7010_fil_set_wpa_mode(struct ks7010 *ks, enum fil_wpa_mode mode)
{
	struct {
		__le32 mode;
		__le16 capability;
	} __packed mct;

	mct.mode = cpu_to_le32((u32)mode);
	mct.capability = 0;

	fil_mib_set_req_ostring(ks, MIB_WPA_MODE, (u8 *)&mct, sizeof(mct));
}

void ks7010_fil_set_wpa_ucast_suite(struct ks7010 *ks, u8 *cipher,
				    size_t cipher_size)
{
	fil_mib_set_req_ostring(ks, MIB_WPA_CONFIG_UCAST_SUITE,
				cipher, cipher_size);
}

void ks7010_fil_set_wpa_mcast_suite(struct ks7010 *ks, u8 *cipher,
				    size_t cipher_size)
{
	fil_mib_set_req_ostring(ks, MIB_WPA_CONFIG_MCAST_SUITE,
				cipher, cipher_size);
}

void ks7010_fil_set_wpa_key_mgmt_suite(struct ks7010 *ks, u8 *cipher,
				       size_t cipher_size)
{
	fil_mib_set_req_ostring(ks, MIB_WPA_CONFIG_AUTH_SUITE,
				cipher, cipher_size);
}

void ks7010_fil_set_ptk_tsc(struct ks7010 *ks, u8 *seq, size_t seq_size)
{
	fil_mib_set_req_ostring(ks, MIB_PTK_TSC, seq, seq_size);
}

void ks7010_fil_set_gtk_1_tsc(struct ks7010 *ks, u8 *seq, size_t seq_size)
{
	fil_mib_set_req_ostring(ks, MIB_GTK_1_TSC, seq, seq_size);
}

void ks7010_fil_set_gtk_2_tsc(struct ks7010 *ks, u8 *seq, size_t seq_size)
{
	fil_mib_set_req_ostring(ks, MIB_GTK_2_TSC, seq, seq_size);
}

void ks7010_set_pmk(struct ks7010 *ks)
{
	/* TODO */
}

void ks7010_fil_set_region(struct ks7010 *ks, u32 region)
{
	fil_mib_set_req_int(ks, LOCAL_REGION, region);
}

void ks7010_fil_set_rts_thresh(struct ks7010 *ks, u32 thresh)
{
	fil_mib_set_req_int(ks, DOT11_RTS_THRESHOLD, thresh);
}

void ks7010_fil_set_frag_thresh(struct ks7010 *ks, u32 thresh)
{
	fil_mib_set_req_int(ks, DOT11_FRAGMENTATION_THRESHOLD, thresh);
}

void ks7010_fil_set_gain(struct ks7010 *ks, struct fil_gain *gain)
{
	fil_mib_set_req_ostring(ks, LOCAL_GAIN, (u8 *)gain, sizeof(*gain));
}

void ks7010_fil_get_mac_addr(struct ks7010 *ks)
{
	fil_mib_get_req(ks, DOT11_MAC_ADDRESS);
}

void ks7010_fil_get_fw_version(struct ks7010 *ks)
{
	fil_mib_get_req(ks, MIB_FIRMWARE_VERSION);
}

void ks7010_fil_get_eeprom_cksum(struct ks7010 *ks)
{
	fil_mib_get_req(ks, LOCAL_EEPROM_SUM);
}

void ks7010_fil_get_rts_thresh(struct ks7010 *ks)
{
	fil_mib_get_req(ks, DOT11_RTS_THRESHOLD);
}

void ks7010_fil_get_frag_thresh(struct ks7010 *ks)
{
	fil_mib_get_req(ks, DOT11_FRAGMENTATION_THRESHOLD);
}

void ks7010_fil_get_gain(struct ks7010 *ks)
{
	fil_mib_get_req(ks, LOCAL_GAIN);
}

/**
 * ks7010_fil_get_phy_info() - Get PHY information.
 * @ks: The ks7010 device.
 * @timer: 0 for no timer.
 */
void ks7010_fil_get_phy_info(struct ks7010 *ks, u16 timer)
{
	struct fil_t_phy_info_req *hdr;
	size_t frame_size = sizeof(*hdr);

	hdr = fil_alloc_tx_frame(frame_size, FIL_T_PHY_INFO_REQ);
	if (!hdr)
		return;

	if (timer) {
		hdr->type = cpu_to_le16((u16)FIL_T_PHY_INFO_TYPE_TIME);
		hdr->time = cpu_to_le16(timer);
	} else {
		hdr->type = cpu_to_le16((u16)FIL_T_PHY_INFO_TYPE_NORMAL);
		hdr->time = 0;
	}

	fil_tx(ks, hdr, frame_size);
}

static bool _scan_req_is_valid(struct fil_scan *req)
{
	if (req->ssid_size > FIL_T_SSID_MAX_SIZE) {
		ks_debug("ssid size to big: %zu", req->ssid_size);
		return false;
	}

	if (req->channels_size > FIL_T_CHANNELS_MAX_SIZE) {
		ks_debug("channels size to big: %zu", req->channels_size);
		return false;
	}

	return true;
}

void ks7010_fil_scan(struct ks7010 *ks, struct fil_scan *req)
{
	struct fil_t_scan_req *hdr;
	size_t frame_size = sizeof(*hdr);

	hdr = fil_alloc_tx_frame(frame_size, FIL_T_SCAN_REQ);
	if (!hdr)
		return;

	if (!_scan_req_is_valid(req))
		return;

	hdr->ch_time_min = cpu_to_le32((u32)FIL_T_DEFAULT_CH_TIME_MIN);
	hdr->ch_time_max = cpu_to_le32((u32)FIL_T_DEFAULT_CH_TIME_MAX);

	memcpy(hdr->channels.body, req->channels, req->channels_size);
	hdr->channels.size = req->channels_size;

	if (req->scan_type == FIL_SCAN_TYPE_ACTIVE) {
		if (req->ssid_size) {
			size_t size = req->ssid_size;

			if (size > FIL_T_SSID_MAX_SIZE) {
				ks_debug("ssid too long, truncating");
				size = FIL_T_SSID_MAX_SIZE;
			}

			memcpy(hdr->ssid.body, req->ssid, size);
			hdr->ssid.size = (u8)size;
			hdr->scan_type = FIL_SCAN_TYPE_ACTIVE;
		} else {
			ks_debug("no ssid, falling back to passive scan");
			hdr->scan_type = FIL_SCAN_TYPE_PASSIVE;
		}
	} else {
		hdr->scan_type = FIL_SCAN_TYPE_PASSIVE;
	}

	fil_tx(ks, hdr, frame_size);
}

static void _fil_mib_set_conf(struct ks7010 *ks, u32 attribute)
{
	struct fil_ops *fil_ops = ks->fil_ops;
	void (*callback)(struct ks7010 *ks);

	switch (attribute) {
	case LOCAL_CURRENT_ADDRESS:
		callback = fil_ops->set_mac_addr_conf;
		break;

	case LOCAL_MULTICAST_ADDRESS:
		callback = fil_ops->set_mcast_addresses_conf;
		break;

	case LOCAL_MULTICAST_FILTER:
		callback = fil_ops->mcast_filter_enable_conf;
		break;

	case DOT11_PRIVACY_INVOKED:
		callback = fil_ops->privacy_invoked_conf;
		break;

	case MIB_DEFAULT_KEY_INDEX:
		callback = fil_ops->set_default_key_index_conf;
		break;

	case MIB_KEY_VALUE_1:
		callback = fil_ops->set_key_1_conf;
		break;

	case MIB_KEY_VALUE_2:
		callback = fil_ops->set_key_2_conf;
		break;

	case MIB_KEY_VALUE_3:
		callback = fil_ops->set_key_3_conf;
		break;

	case MIB_KEY_VALUE_4:
		callback = fil_ops->set_key_4_conf;
		break;

	case MIB_WPA_ENABLE:
		callback = fil_ops->set_wpa_enable_conf;
		break;

	case MIB_WPA_MODE:
		callback = fil_ops->set_wpa_mode_conf;
		break;

	case MIB_WPA_CONFIG_MCAST_SUITE:
		callback = fil_ops->set_wpa_mcast_suite_conf;
		break;

	case MIB_WPA_CONFIG_UCAST_SUITE:
		callback = fil_ops->set_wpa_ucast_suite_conf;
		break;

	case MIB_WPA_CONFIG_AUTH_SUITE:
		callback = fil_ops->set_wpa_key_mgmt_suite_conf;
		break;

	case MIB_PTK_TSC:
		callback = fil_ops->set_ptk_tsc_conf;
		break;

	case MIB_GTK_1_TSC:
		callback = fil_ops->set_gtk_1_tsc_conf;
		break;

	case MIB_GTK_2_TSC:
		callback = fil_ops->set_gtk_2_tsc_conf;
		break;

	case LOCAL_PMK:
		callback = fil_ops->set_pmk_conf;
		break;

	case LOCAL_REGION:
		callback = fil_ops->set_region_conf;
		break;

	case DOT11_RTS_THRESHOLD:
		callback = fil_ops->set_rts_thresh_conf;
		break;

	case DOT11_FRAGMENTATION_THRESHOLD:
		callback = fil_ops->set_frag_thresh_conf;
		break;

	case LOCAL_GAIN:
		callback = fil_ops->set_gain_conf;
		break;

	default:
		ks_debug("unknown attribute %d", attribute);
		callback = NULL;
		break;
	}

	if (callback)
		callback(ks);
}

static void fil_mib_set_conf(struct ks7010 *ks, struct fil_t_mib_set_conf *hdr)
{
	u32 status, attribute;

	status = le32_to_cpu(hdr->status);
	attribute = le32_to_cpu(hdr->attribute);

	switch (status) {
	case MIB_STATUS_INVALID:
		ks_debug("invalid status for attribute %d", attribute);
		break;

	case MIB_STATUS_READ_ONLY:
		ks_debug("read only status for attribute %d", attribute);
		break;

	case MIB_STATUS_WRITE_ONLY:
		ks_debug("write only status for attribute %d", attribute);
		break;

	case MIB_STATUS_SUCCESS:
		_fil_mib_set_conf(ks, attribute);

	default:
		ks_debug("unknown status for attribute %d", attribute);
		break;
	}
}

static bool _mib_get_conf_attribute_and_type_is_valid(u32 attribute, u16 type)
{
	/* check the firmware behavior, confirm attributes match types ? */
	return 0;
}

static void
_fil_mib_get_conf(struct ks7010 *ks, u32 attribute, u8 *data, u16 data_size)
{
	struct fil_ops *fil_ops = ks->fil_ops;
	void (*callback)(struct ks7010 *ks, u8 *data, u16 data_size);

	switch (attribute) {
	case DOT11_MAC_ADDRESS:
		callback = fil_ops->get_mac_addr_conf;
		break;

	case MIB_FIRMWARE_VERSION:
		callback = fil_ops->get_fw_version_conf;
		break;

	case LOCAL_EEPROM_SUM:
		callback = fil_ops->get_eeprom_cksum_conf;
		break;

	case DOT11_RTS_THRESHOLD:
		callback = fil_ops->get_rts_thresh_conf;
		break;

	case DOT11_FRAGMENTATION_THRESHOLD:
		callback = fil_ops->get_frag_thresh_conf;
		break;

	case LOCAL_GAIN:
		callback = fil_ops->get_gain_conf;
		break;

	default:
		ks_debug("unknown status for attribute %d", attribute);
		callback = NULL;
	}

	if (callback)
		callback(ks, data, data_size);
}

static void fil_mib_get_conf(struct ks7010 *ks, struct fil_t_mib_get_conf *hdr)
{
	u32 status, attribute;
	u16 data_size, type;

	status = le32_to_cpu(hdr->status);
	attribute = le32_to_cpu(hdr->attribute);
	data_size = le16_to_cpu(hdr->data_size);
	type = le16_to_cpu(hdr->data_type);

	if (!_mib_get_conf_attribute_and_type_is_valid(attribute, type))
		return;

	switch (status) {
	case MIB_STATUS_INVALID:
		ks_debug("invalid status for attribute %d", attribute);
		break;

	case MIB_STATUS_READ_ONLY:
		ks_debug("read only status for attribute %d", attribute);
		break;

	case MIB_STATUS_WRITE_ONLY:
		ks_debug("write only status for attribute %d", attribute);
		break;

	case MIB_STATUS_SUCCESS:
		_fil_mib_get_conf(ks, attribute, hdr->data, data_size);

	default:
		ks_debug("unknown status for attribute %d", attribute);
		break;
	}
}

static bool _result_code_is_valid(u16 result_code)
{
	if (result_code != RESULT_SUCCESS &&
	    result_code != RESULT_INVALID_PARAMETERS &&
	    result_code != RESULT_NOT_SUPPORTED) {
		ks_debug("unknown result_code");
		return false;
	}

	return true;
}

static void fil_result_code_conf(struct ks7010 *ks, u16 event,
				 struct fil_t_result_code_conf *hdr)
{
	struct fil_ops *fil_ops = ks->fil_ops;
	u16 result_code = le16_to_cpu(hdr->result_code);
	void (*callback)(struct ks7010 *ks, enum fil_result_code result);

	if (!_result_code_is_valid(result_code))
		return;

	switch (event) {
	case FIL_T_START_CONF:
		callback = fil_ops->start_conf;
		break;

	case FIL_T_STOP_CONF:
		callback = fil_ops->stop_conf;
		break;

	case FIL_T_SLEEP_CONF:
		callback = fil_ops->sleep_conf;
		break;

	case FIL_T_MIC_FAILURE_CONF:
		callback = fil_ops->mic_failure_conf;
		break;

	case FIL_T_POWER_MGMT_CONF:
		callback = fil_ops->set_power_mgmt_conf;
		break;

	case FIL_T_INFRA_SET_CONF:
		callback = fil_ops->set_infra_conf;
		break;

	case FIL_T_INFRA_SET2_CONF:
		callback = fil_ops->set_infra_bssid_conf;
		break;

	default:
		ks_debug("invalid event: %04X\n", event);
		callback = NULL;
		break;
	}

	if (callback)
		callback(ks, result_code);
}

static void fil_phy_info_ind(struct ks7010 *ks, struct fil_t_phy_info_ind *le)
{
	struct fil_phy_info cpu;

	cpu.rssi = le->rssi;
	cpu.signal = le->signal;
	cpu.noise = le->noise;
	cpu.link_speed = le->link_speed;
	cpu.tx_frame = le32_to_cpu(le->tx_frame);
	cpu.rx_frame = le32_to_cpu(le->rx_frame);
	cpu.rx_error = le32_to_cpu(le->tx_error);
	cpu.rx_error = le32_to_cpu(le->rx_error);

	ks_debug("PHY information indication received\n"
		 "\tRSSI: %u\n\tSignal: %u\n\tNoise: %u\n"
		 "\tLink Speed: %ux500Kbps\n"
		 "\tTransmitted Frame Count: %u\n\tReceived Frame Count: %u\n"
		 "\tTx Failed Count: %u\n\tFCS Error Count: %u\n",
		 cpu.rssi, cpu.signal, cpu.noise, cpu.link_speed,
		 cpu.tx_frame, cpu.rx_frame, cpu.tx_error, cpu.rx_error);

	if (ks->fil_ops->get_phy_info_ind)
		ks->fil_ops->get_phy_info_ind(ks, &cpu);
}

static void fil_phy_info_conf(struct ks7010 *ks, struct fil_t_hdr *fhdr)
{
	size_t frame_size;

	ks_debug("Firmware appears to treat phy_info_conf the same as phy_info_ind?");

	frame_size = rx_fil_t_hdr_to_frame_size(fhdr);
	if (frame_size < sizeof(struct fil_t_phy_info_ind)) {
		ks_debug("received frame size is too small");
		return;
	}

	ks_debug("passing fhdr to fil_phy_info_ind()");
	fil_phy_info_ind(ks, (struct fil_t_phy_info_ind *)fhdr);
}

/*
 * struct fil_scan_conf contains a 'reserved' member, keep it separate
 * from the other result_code headers for documentation purposes
 */
static void fil_scan_conf(struct ks7010 *ks, struct fil_t_scan_conf *hdr)
{
	u16 result_code;
	void (*callback)(struct ks7010 *ks, enum fil_result_code result);

	callback = ks->fil_ops->scan_conf;

	result_code = le16_to_cpu(hdr->result_code);
	if (!_result_code_is_valid(result_code))
		return;

	if (callback)
		callback(ks, result_code);
}

static void fil_scan_ind(struct ks7010 *ks, struct fil_t_scan_ind *le)
{
	struct fil_ops *fil_ops = ks->fil_ops;
	struct fil_scan_ind *cpu;
	size_t size;

	if (!fil_ops->scan_ind) {
		ks_debug("fil_ops->scan_ind is NULL");
		return;
	}

	cpu = kzalloc(sizeof(*cpu), GFP_KERNEL);
	if (!cpu)
		return;

	ether_addr_copy(cpu->bssid, le->bssid);

	cpu->rssi = le->rssi;
	cpu->signal = le->signal;
	cpu->noise = le->noise;
	cpu->channel = le->channel;

	cpu->beacon_period = le16_to_cpu(le->beacon_period);
	cpu->capability = le16_to_cpu(le->capability);

	if (le->frame_type == FIL_T_FRAME_TYPE_PROBE_RESP) {
		cpu->type = FRAME_TYPE_PROBE_RESP;

	} else if (le->frame_type == FIL_T_FRAME_TYPE_BEACON) {
		cpu->type = FRAME_TYPE_BEACON;

	} else {
		ks_debug("frame type is not a scan indication frame");
		return;
	}

	size = le16_to_cpu(le->body_size);
	memcpy(cpu->body, le->body, size);
	cpu->body_size = size;

	fil_ops->scan_ind(ks, cpu);
}

static void
_conn_ind_copy_ie(struct fil_conn_ind *cpu, struct fil_t_conn_ind *le)
{
	size_t size;

	size = le->ies.size < IE_MAX_SIZE ? le->ies.size : IE_MAX_SIZE;
	memcpy(cpu->ie, le->ies.body, size);
	cpu->ie_size = size;
}

static void fil_conn_ind(struct ks7010 *ks, struct fil_t_conn_ind *le)
{
	struct fil_ops *fil_ops = ks->fil_ops;
	struct fil_conn_ind cpu;
	u16 conn_code;
	size_t size;

	if (!fil_ops->conn_ind) {
		ks_debug("fil_ops->conn_ind is NULL");
		return;
	}

	conn_code = le16_to_cpu(le->conn_code);
	if (conn_code != CONN_CODE_CONNECT &&
	    conn_code != CONN_CODE_DISCONNECT) {
		ks_debug("conn_code invalid");
		return;
	}
	cpu.code = conn_code;

	ether_addr_copy(cpu.bssid, le->bssid);

	cpu.rssi = le->rssi;
	cpu.signal = le->signal;
	cpu.noise = le->noise;
	cpu.channel = le->ds.channel;

	cpu.beacon_period = le16_to_cpu(le->beacon_period);
	cpu.capability = le16_to_cpu(le->capability);

	size = le->rates.size;
	memcpy(cpu.rates, le->rates.body, size);
	cpu.rates_size = size;

	if (le->ext_rates.size > 0) {
		size_t size, available;
		u8 *ptr;

		available = KS7010_RATES_MAX_SIZE - cpu.rates_size;
		size = le->ext_rates.size;
		if (size > available) {
			ks_debug("ext rates don't all fit");
			size = available;
		}

		ptr = &cpu.rates[cpu.rates_size];
		memcpy(ptr, le->ext_rates.body, size);
		cpu.rates_size += size;
	}

	if (le->wpa_mode == FIL_WPA_MODE_WPA) {
		cpu.element_id = ELEMENT_ID_WPA;
		_conn_ind_copy_ie(&cpu, le);
	}

	if (le->wpa_mode == FIL_WPA_MODE_RSN) {
		cpu.element_id = ELEMENT_ID_RSN;
		_conn_ind_copy_ie(&cpu, le);
	}

	fil_ops->conn_ind(ks, &cpu);
}

static void fil_assoc_ind(struct ks7010 *ks, struct fil_t_assoc_ind *le)
{
	struct fil_ops *fil_ops = ks->fil_ops;
	struct fil_assoc_ind cpu;
	u8 type;

	if (!fil_ops->assoc_ind) {
		ks_debug("fil_ops->assoc_ind is NULL");
		return;
	}

	memset(&cpu, 0, sizeof(cpu));

	type = le->req.type;
	if (type != FIL_T_FRAME_TYPE_ASSOC_REQ &&
	    type != FIL_T_FRAME_TYPE_REASSOC_REQ) {
		ks_debug("assoc req frame type is invalid");
		return;
	}
	cpu.req.type = type;

	cpu.req.capability = le16_to_cpu(le->req.capability);
	cpu.req.listen_interval = le16_to_cpu(le->req.listen_interval);
	ether_addr_copy(cpu.req.ap_addr, le->req.ap_addr);
	cpu.req.ie_size = (size_t)le16_to_cpu(le->req.ie_size);
	cpu.req.ie = le->ies;

	type = le->resp.type;
	if (type != FIL_T_FRAME_TYPE_ASSOC_RESP &&
	    type != FIL_T_FRAME_TYPE_REASSOC_RESP) {
		ks_debug("assoc resp frame type is invalid");
		return;
	}
	cpu.resp.type = type;

	cpu.resp.capability = le16_to_cpu(le->resp.capability);
	cpu.resp.status = le16_to_cpu(le->resp.status);
	cpu.resp.assoc_id = le16_to_cpu(le->resp.assoc_id);
	cpu.resp.ie_size = (size_t)le16_to_cpu(le->resp.ie_size);

	cpu.resp.ie = le->ies + cpu.req.ie_size;

	fil_ops->assoc_ind(ks, &cpu);
}

static void fil_data_ind(struct ks7010 *ks, struct fil_t_data_ind *le)
{
	struct fil_ops *fil_ops = ks->fil_ops;
	u16 auth_type;
	int key_index;
	size_t frame_size;
	size_t data_size;
	u8 *data;

	if (!fil_ops->data_ind) {
		ks_debug("fil_ops->data_ind is NULL");
		return;
	}

	auth_type = le16_to_cpu(le->auth_type);

	if (auth_type != AUTH_TYPE_PTK &&
	    auth_type != AUTH_TYPE_GTK1 &&
	    auth_type != AUTH_TYPE_GTK2) {
		ks_debug("auth type is invalid");
		return;
	}

	key_index = auth_type - 1;
	frame_size = le16_to_cpu(le->fhdr.size);
	data_size = frame_size - sizeof(*le);
	data = le->data;

	fil_ops->data_ind(ks, key_index, data, data_size);
}

static void fil_event_check(struct ks7010 *ks, struct fil_t_hdr *fhdr)
{
	u16 event = le16_to_cpu(fhdr->event);

	switch (event) {
	case FIL_T_START_CONF:
	case FIL_T_STOP_CONF:
	case FIL_T_SLEEP_CONF:
	case FIL_T_MIC_FAILURE_CONF:
	case FIL_T_POWER_MGMT_CONF:
	case FIL_T_INFRA_SET_CONF:
	case FIL_T_INFRA_SET2_CONF:
		fil_result_code_conf(ks, event,
				     (struct fil_t_result_code_conf *)fhdr);
		break;

	case FIL_T_MIB_SET_CONF:
		fil_mib_set_conf(ks, (struct fil_t_mib_set_conf *)fhdr);
		break;

	case FIL_T_MIB_GET_CONF:
		fil_mib_get_conf(ks, (struct fil_t_mib_get_conf *)fhdr);
		break;

	case FIL_T_PHY_INFO_CONF:
		fil_phy_info_conf(ks, fhdr);
		break;

	case FIL_T_PHY_INFO_IND:
		fil_phy_info_ind(ks, (struct fil_t_phy_info_ind *)fhdr);
		break;

	case FIL_T_SCAN_CONF:
		fil_scan_conf(ks, (struct fil_t_scan_conf *)fhdr);
		break;

	case FIL_T_SCAN_IND:
		fil_scan_ind(ks, (struct fil_t_scan_ind *)fhdr);
		break;

	case FIL_T_CONNECT_IND:
		fil_conn_ind(ks, (struct fil_t_conn_ind *)fhdr);
		break;

	case FIL_T_ASSOC_IND:
		fil_assoc_ind(ks, (struct fil_t_assoc_ind *)fhdr);
		break;

	case FIL_T_DATA_IND:
		fil_data_ind(ks, (struct fil_t_data_ind *)fhdr);
		break;

	default:
		ks_debug("undefined MIB event: %04X\n", event);
		break;
	}
}

static const struct snap_hdr SNAP = {
	.dsap = 0xAA,
	.ssap = 0xAA,
	.cntl = 0x03
	/* OUI is all zero */
};

/**
 * ks7010_fil_tx() - Build FIL tx frame.
 * @ks: The ks7010 device.
 * @skb: sk_buff from networking stack.
 * @type: Type of frame to build, &enum fil_tx_type.
 * @txd: Return argument for frame data (and size).
 */
int ks7010_fil_tx(struct ks7010 *ks, struct sk_buff *skb,
		  enum fil_tx_type type, struct tx_data *txd)
{
	struct ethhdr *eh;
	struct fil_eap_hdr *fh;
	struct fil_t_data_req *hdr;
	size_t max_frame_size;
	size_t frame_size;
	u16 proto;
	size_t size;
	u8 *src_ptr, *dst_ptr;
	int i;

	memset(txd, 0, sizeof(*txd));

	if (skb->len < ETH_HLEN)
		return -EINVAL;

	/* hdr->size must be updated after the frame is built */
	max_frame_size =
		sizeof(*hdr) + (sizeof(*fh) - sizeof(eh)) + skb->len;

	hdr = fil_alloc_tx_frame(max_frame_size, FIL_T_DATA_REQ);
	if (!hdr)
		return -ENOMEM;

	eh = (struct ethhdr *)skb->data;
	proto = ntohs(eh->h_proto);
	frame_size = 0;

	if (proto >= ETH_P_802_3_MIN) {
		fh = (struct fil_eap_hdr *)hdr->data;

		ether_addr_copy(fh->da, eh->h_dest);
		ether_addr_copy(fh->sa, eh->h_source);

		fh->dsap = SNAP.dsap;
		fh->ssap = SNAP.ssap;
		fh->cntl = SNAP.cntl;
		for (i = 0; i < OUI_SIZE; i++)
			fh->oui[i] = 0;
		fh->type = eh->h_proto;

		frame_size += sizeof(*fh);
		dst_ptr = (u8 *)(fh + 1);
	} else {		/* DIX */
		dst_ptr = hdr->data;

		ether_addr_copy(dst_ptr, eh->h_dest);
		frame_size += ETH_ALEN;
		dst_ptr += ETH_ALEN;

		ether_addr_copy(dst_ptr, eh->h_source);
		frame_size += ETH_ALEN;
		dst_ptr += ETH_ALEN;
	}

	src_ptr = (u8 *)(eh + 1);
	size = skb->len - sizeof(*eh);
	memcpy(dst_ptr, src_ptr, size);
	frame_size += size;

	if (type == TX_TYPE_AUTH)
		hdr->type = cpu_to_le16((u16)FIL_T_DATA_REQ_TYPE_AUTH);
	else
		hdr->type = cpu_to_le16((u16)FIL_T_DATA_REQ_TYPE_DATA);

	/* update hif_hdr size now we know the final packet size */
	hdr->fhdr.size = tx_frame_size_to_fil_t_hdr_size(frame_size);

	/* pass frame data back */
	txd->datap = (u8 *)hdr;
	txd->size = fil_align_size(frame_size);

	return 0;
}

/**
 * ks7010_fil_rx() - FIL response to an rx event.
 * @ks: The ks7010 device.
 * @data: The rx data.
 * @data_size: Size of data.
 *
 * Called by the rx interrupt bottom half tasklet to respond to an rx event.
 */
int ks7010_fil_rx(struct ks7010 *ks, u8 *data, size_t data_size)
{
	struct fil_t_hdr *fhdr;
	u16 size;

	fhdr = (struct fil_t_hdr *)data;
	size = le16_to_cpu(fhdr->size);

	if (data_size != size) {
		ks_debug("rx size mismatch");
		return -EINVAL;
	}

	fil_event_check(ks, fhdr);

	return 0;
}
