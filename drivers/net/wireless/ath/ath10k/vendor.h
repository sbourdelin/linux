/*
 * Copyright (c) 2018 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _VENDOR_H_
#define _VENDOR_H_

#include <linux/rhashtable.h>
#include <net/mac80211.h>
#include <linux/ktime.h>

#include "htt.h"

#define ATH10K_VENDOR_BSSID_FILTER_INVALID_IDX	0xFF
#define ATH10K_VENDOR_BSSID_FILTER_COUNT	0x3

/* Vendor id to be used in vendor specific command and events to user space
 * NOTE: The authoritative place for definition of QCA_NL80211_VENDOR_ID,
 * vendor subcmd definitions prefixed with QCA_NL80211_VENDOR_SUBCMD, and
 * qca_wlan_vendor_attr is open source file src/common/qca-vendor.h in
 * git://w1.fi/srv/git/hostap.git; the values here are just a copy of that
 */
#define QCA_NL80211_VENDOR_ID 0x001374

/* enum qca_nl80211_vendor_subcmds - QCA nl80211 vendor command identifiers */
enum qca_nl80211_vendor_subcmds {
	/* This command is used to configure an RX filter to receive frames
	 * from stations that are active on the operating channel, but not
	 * associated with the local device (e.g., STAs associated with other
	 * APs). Filtering is done based on a list of BSSIDs and STA MAC
	 * addresses added by the user. This command is also used to fetch the
	 * statistics of unassociated stations. The attributes used with this
	 * command are defined in enum qca_wlan_vendor_attr_bss_filter.
	 */
	QCA_NL80211_VENDOR_SUBCMD_BSS_FILTER = 170,
};

/**
 * enum qca_wlan_vendor_attr_bss_filter - Used by the vendor command
 * QCA_NL80211_VENDOR_SUBCMD_BSS_FILTER.
 * The user can add/delete the filter by specifying the BSSID/STA MAC address in
 * QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAC_ADDR, filter type in
 * QCA_WLAN_VENDOR_ATTR_BSS_FILTER_TYPE, add/delete action in
 * QCA_WLAN_VENDOR_ATTR_BSS_FILTER_ACTION in the request. The user can get the
 * statistics of an unassociated station by specifying the MAC address in
 * QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAC_ADDR, station type in
 * QCA_WLAN_VENDOR_ATTR_BSS_FILTER_TYPE, GET action in
 * QCA_WLAN_VENDOR_ATTR_BSS_FILTER_ACTION in the request. The user also can get
 * the statistics of all unassociated stations by specifying the Broadcast MAC
 * address (ff:ff:ff:ff:ff:ff) in QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAC_ADDR with
 * above procedure. In the response, driver shall specify statistics
 * information nested in QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS.
 */
enum qca_wlan_vendor_attr_bss_filter {
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_INVALID = 0,
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAC_ADDR = 1,

	/* Other BSS filter type, unsigned 8 bit value. One of the values
	 * in enum qca_wlan_vendor_bss_filter_type.
	 */
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_TYPE = 2,

	/* Other BSS filter action, unsigned 8 bit value. One of the values
	 * in enum qca_wlan_vendor_bss_filter_action.
	 */
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_ACTION = 3,

	/* Array of nested attributes where each entry is the statistics
	 * information of the specified station that belong to another BSS.
	 * Attributes for each entry are taken from enum
	 * qca_wlan_vendor_bss_filter_sta_stats.
	 * Other BSS station configured in
	 * QCA_NL80211_VENDOR_SUBCMD_BSS_FILTER with filter type
	 * QCA_WLAN_VENDOR_BSS_FILTER_TYPE_STA.
	 * Statistics returned by QCA_NL80211_VENDOR_SUBCMD_BSS_FILTER
	 * with filter action QCA_WLAN_VENDOR_BSS_FILTER_ACTION_GET.
	 */
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS = 4,

	/* Dummy (NOP) attribute for 64 bit padding */
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_PAD = 13,

	/* keep last */
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAX
};

/* enum qca_wlan_vendor_bss_filter_type - Type of
 * filter used in other BSS filter operations. Used by the vendor command
 * QCA_NL80211_VENDOR_SUBCMD_BSS_FILTER.
 */
enum qca_wlan_vendor_bss_filter_type {
	/* BSSID filter */
	QCA_WLAN_VENDOR_BSS_FILTER_TYPE_BSSID,

	/* Station MAC address filter */
	QCA_WLAN_VENDOR_BSS_FILTER_TYPE_STA
};

/* enum qca_wlan_vendor_bss_filter_action - Type of
 * action in other BSS filter operations. Used by the vendor command
 * QCA_NL80211_VENDOR_SUBCMD_BSS_FILTER.
 */
enum qca_wlan_vendor_bss_filter_action {
	/* Add filter */
	QCA_WLAN_VENDOR_BSS_FILTER_ACTION_ADD,

	/* Delete filter */
	QCA_WLAN_VENDOR_BSS_FILTER_ACTION_DEL,

	/* Get the statistics */
	QCA_WLAN_VENDOR_BSS_FILTER_ACTION_GET
};

/* enum qca_wlan_vendor_bss_filter_sta_stats - Attributes for
 * the statistics of a specific unassociated station belong to another BSS.
 * The statistics provides information of the unassociated station
 * filtered by other BSS operation - such as MAC, signal value.
 * Used by the vendor command QCA_NL80211_VENDOR_SUBCMD_BSS_FILTER.
 */
enum qca_wlan_vendor_attr_bss_filter_sta_stats {
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS_INVALID,

	/* MAC address of the station */
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS_MAC,

	/* Last received signal strength of the station.
	 * Unsigned 8 bit number containing RSSI.
	 */
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS_RSSI,

	/* Time stamp of the host driver for the last received RSSI.
	 * Unsigned 64 bit number containing nanoseconds from the boottime.
	 */
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS_RSSI_TS,

	/* Keep last */
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS_AFTER_LAST,
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS_MAX =
	QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS_AFTER_LAST - 1
};

/* struct ath10k_vendor_bssid_info - BSSID information */
struct ath10k_vendor_bssid_info {
	/* vdev map used to indicate which VAPs are own */
	unsigned long long vdev_map;

	/* BSSID */
	u8 addr[ETH_ALEN];
	u16 ref;
};

/* struct ath10k_vendor_unassoc_sta_stats - Unassociated station statistics
 * information.
 */
struct ath10k_vendor_unassoc_sta_stats {
	/* bitflag of flags using the bits of &enum nl80211_sta_info to
	 * indicate the relevant values in this struct for them
	 */
	u64 filled;

	/* host driver time stamp for the signal (rssi) */
	u64 rssi_ts;

	/* signal value of the station */
	s8 rssi;

	/* MAC address of the station */
	u8 addr[ETH_ALEN];
};

/* struct ath10k_vendor_unassoc_sta - Unassociated station information
 * This structure protected by rcu lock.
 */
struct ath10k_vendor_unassoc_sta {
	struct ath10k_vendor_unassoc_sta_stats stats;

	/* rhashtable list pointer */
	struct rhash_head rhash;

	/* rcu head for freeing unassociated station */
	struct rcu_head rcu;
};

/* struct ath10k_vendor_unassoc_sta_tbl - Unassociated station table info */
struct ath10k_vendor_unassoc_sta_tbl {
	/* The rhashtable containing struct ath10k_vendor_unassoc_sta, keyed
	 * by MAC addr
	 */
	struct rhashtable rhead;

	/* Total Number of entries */
	u16 entries;
};

/* struct ath10k_vendor_bss_filter - BSS filter information */
struct ath10k_vendor_bss_filter {
	/* Array of BSSID information */
	struct ath10k_vendor_bssid_info *bssid;

	/* Unassociated station table */
	struct ath10k_vendor_unassoc_sta_tbl tbl;

	/* Maximum other bss filter supported by the platform */
	u8 max;

	/* number of bssid filter configured by user */
	u8 n_bssid;
};

struct ath10k_vendor_bss_filter_get_reply {
	u32 n_sta;		/* Number of stations */
	u8 data[0];		/* Array of ath10k_vendor_unassoc_sta_stats */
};

struct ath10k_vendor {
	/* BSS filter */
	struct ath10k_vendor_bss_filter bss_filter;
};

struct ath10k_vendor_unassoc_sta *
ath10k_vendor_unassoc_sta_lookup(struct ath10k_vendor_unassoc_sta_tbl *tbl, u8 *addr);
void ath10k_vendor_bss_filter_cleanup(struct ath10k_vif *arvif);
int ath10k_vendor_register(struct ath10k *ar);
void ath10k_vendor_unregister(struct ath10k *ar);

static inline bool
ath10k_vendor_rx_h_bssid_filter(struct ath10k_vendor *vendor,
				struct sk_buff_head *amsdu,
				struct ieee80211_rx_status *rx_status)
{
	struct sk_buff *first;
	struct htt_rx_desc *rxd;
	struct ath10k_vendor_unassoc_sta_tbl *tbl;
	struct ath10k_vendor_unassoc_sta *sta;
	struct ieee80211_hdr *hdr;

	/* If BSS filter are not enabled, then
	 * no need to filter sta so allow all frames
	 */
	if (!vendor->bss_filter.n_bssid)
		return false;

	first = skb_peek(amsdu);
	rxd = (void *)first->data - sizeof(*rxd);

	if (!rxd)
		return false;

	/* other bssid frames are with invalid peer idx flags */
	if (rxd->attention.flags &
	    __cpu_to_le32(RX_ATTENTION_FLAGS_PEER_IDX_INVALID)) {
		tbl = &vendor->bss_filter.tbl;

		/* If unassociacted sta configured, then do lookup
		 * and maintain the last rssi value.
		 * Don't allow other bssid frames.
		 */
		if (tbl->entries) {
			hdr = (void *)rxd->rx_hdr_status;
			rcu_read_lock();

			sta = ath10k_vendor_unassoc_sta_lookup(tbl, hdr->addr2);
			if (sta) {
				sta->stats.rssi = rx_status->signal;
				sta->stats.rssi_ts = ktime_to_ns(ktime_get_boottime());
				sta->stats.filled |= BIT(NL80211_STA_INFO_SIGNAL);
			}

			rcu_read_unlock();
		}
		return true;
	}

	return false;
}

#endif /* _VENDOR_H_ */
