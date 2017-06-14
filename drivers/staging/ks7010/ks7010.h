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

#ifndef _KS7010_H
#define _KS7010_H

#include <net/cfg80211.h>
#include <linux/list.h>
#include <linux/if_ether.h>
#include <linux/interrupt.h>
#include <linux/wireless.h>
#include <linux/skbuff.h>
#include <crypto/hash.h>
#include <linux/kfifo.h>

#include "common.h"
#include "hif.h"

#define DRIVER_PREFIX "ks7010: "

#define ks_err(fmt, arg...) \
	pr_err(DRIVER_PREFIX "ERROR " fmt "\n", ##arg)

#define ks_info(fmt, arg...) \
	pr_info(DRIVER_PREFIX "INFO " fmt "\n", ##arg)

#define ks_warn(fmt, arg...) \
	pr_warn(DRIVER_PREFIX "WARNING " fmt "\n", ##arg)

#define ks_debug(fmt, arg...) \
	pr_debug(DRIVER_PREFIX "%s: " fmt "\n", __func__, ##arg)

#define	KS7010_ROM_FILE "ks7010sd.rom"

/**
 * enum ks7010_state - ks7010 device state.
 * @KS7010_STATE_OFF: Device is off.
 * @KS7010_STATE_READY:	Device ready.
 */
enum ks7010_state {
	KS7010_STATE_OFF,
	KS7010_STATE_READY
};

/* SDIO function private data */
struct ks7010_sdio;

#define KS7010_TX_QUEUE_SIZE 1024 /* must be a power of 2 */
#define KS7010_RX_QUEUE_SIZE 32	  /* must be a power of 2 */
#define	RX_DATA_MAX_SIZE	(2 + 2 + 2347 + 1)

/**
 * struct tx_data - Transmit path data.
 * @data: The data.
 * @data_size: Size of the data, in octets.
 */
struct tx_data {
	u8 *datap;
	size_t size;
};

/**
 * struct tx_queue - Transmit path queue.
 * @buf: Buffer used to hold the queue.
 * @head: Head of the queue.
 * @tail: Tail of the queue.
 *
 * Tx queue uses a circular buffer. Single producer is enforced by
 * networking layer, single consumer is enforced due to consumer
 * being called from the interrupt handler. No further queue locking
 * is required.
 */
struct tx_queue {
	struct tx_data buf[KS7010_TX_QUEUE_SIZE];
	int head;
	int tail;
	spinlock_t producer_lock; /* enforce single producer */
	spinlock_t consumer_lock; /* enforce single consumer */
};

/**
 * struct rx_data - Receive path data.
 * @data: The data.
 * @data_size: Size of the data, in octets.
 */
struct rx_data {
	u8 data[RX_DATA_MAX_SIZE];
	size_t data_size;
};

/**
 * struct rx_queue - Receive path queue.
 * @buf: Buffer used to hold the queue.
 * @head: Head of the queue.
 * @tail: Tail of the queue.
 *
 * Rx queue uses a circular buffer. Rx queue data is produced during
 * interrupt handling, no further locking is required. Single consumer
 * must be enforced by the driver.
 */
struct rx_queue {
	struct rx_data buf[KS7010_RX_QUEUE_SIZE];
	int head;
	int tail;
	spinlock_t producer_lock; /* enforce single producer */
	spinlock_t consumer_lock; /* enforce single consumer */
};

/* vif flags info */
/**
 * enum ks7010_vif_state - VIF flags
 * @CONNECTED: Connected to a network.
 * @CONNECT_PEND: Network connection initiated.
 * @WLAN_ENABLED:
 */
enum ks7010_vif_state {
	CONNECTED,
	CONNECT_PEND,
	WLAN_ENABLED,
};

#define KS7010_WEP_KEY_MAX_SIZE 64

struct ks7010_wep_key {
	u8 key_size;
	u8 key_val[KS7010_WEP_KEY_MAX_SIZE];
};

#define KS7010_KEY_SEQ_MAX_SIZE 8
#define KS7010_MIC_KEY_SIZE	8

struct ks7010_wpa_key {
	u8 key_val[WLAN_MAX_KEY_LEN];
	u8 key_size;

	u8 seq[KS7010_KEY_SEQ_MAX_SIZE];
	u8 seq_size;

	u32 cipher;

	u8 tx_mic_key[KS7010_MIC_KEY_SIZE];
	u8 rx_mic_key[KS7010_MIC_KEY_SIZE];
};

#define KS7010_NUM_WEP_KEYS		4
#define KS7010_MAX_WEP_KEY_INDEX	3

#define KS7010_NUM_WPA_KEYS		3 /* ptk, gtk1, gtk2 */
#define KS7010_MAX_WPA_KEY_INDEX	2

/**
 * ks7010_vif - Virtual interface (net_device private data).
 * @ndev: Pointer to the net_device for this VIF.
 */
struct ks7010_vif {
	struct net_device *ndev;
	struct wireless_dev wdev;

	struct ks7010 *ks;

	/* Protect VIF flags */
	spinlock_t if_lock;	/* TODO use this lock when needed */
	unsigned long flags;

	u8 req_bssid[ETH_ALEN];

	size_t ssid_len;
	u8 ssid[IEEE80211_MAX_SSID_LEN];

	enum hif_network_type nw_type;
	enum hif_dot11_auth_mode dot11_auth_mode;
	enum hif_auth_mode auth_mode;

	enum hif_crypt_type pairwise_crypto;
	size_t pairwise_crypto_size;
	enum hif_crypt_type group_crypto;
	size_t group_crypto_size;

	enum hif_bss_scan_type scan_type;
	struct cfg80211_scan_request *scan_req;

	u8 tx_rate;
	enum hif_preamble_type preamble;

	u16 beacon_lost_count;
	u32 rts_thresh;
	u32 frag_thresh;
	u16 ch_hint;

	enum hif_nw_phy_type phy_type;
	enum hif_nw_cts_mode cts_mode;

	enum hif_power_mgmt_type power_mgmt;

	u8 bssid[ETH_ALEN] __aligned(2);

	bool privacy_invoked;
	struct ks7010_wep_key wep_keys[KS7010_NUM_WEP_KEYS];

	bool wpa_enabled;
	struct ks7010_wpa_key wpa_keys[KS7010_NUM_WPA_KEYS];

	int def_txkey_index;
};

/**
 * struct ks7010 - The ks7010 device.
 * @priv: Pointer to the SDIO private data.
 * @vif: The virtual interface (driver supports single VIF only).
 * @state: The device state, &enum ks7010_state.
 * @wiphy: The device wiphy.
 * @dev: Pointer to the device embedded within the SDIO func.
 * @fil_ops: Firmware interface layer operations.
 * @mac_addr: Device MAC address.
 * @mac_addr_valid: True if @mac_addr is valid.
 */
struct ks7010 {
	struct ks7010_sdio *priv;
	struct ks7010_vif *vif;

	enum ks7010_state state;

	struct wiphy *wiphy;
	bool wiphy_registered;

	struct device *dev;

	struct fil_ops *fil_ops;

	u8 mac_addr[ETH_ALEN] __aligned(2);
	bool mac_addr_valid;

	/* firmware */
	u8 *fw;
	size_t fw_size;

	char fw_version[ETHTOOL_FWVERS_LEN];
	size_t fw_version_len;

	/* tx and rx */
	struct tasklet_struct rx_bh_task;

	struct tx_queue txq;
	struct rx_queue rxq;

	/* TKIP */
	struct crypto_shash *tx_tfm_mic;
	struct crypto_shash *rx_tfm_mic;

	/* TODO add stat updates to driver */
	struct net_device_stats nstats;
	struct iw_statistics wstats;
	spinlock_t stats_lock;	/* protect stats */
};

static inline struct ks7010_vif *ks7010_wdev_to_vif(struct wireless_dev *wdev)
{
	return container_of(wdev, struct ks7010_vif, wdev);
}

static inline struct ks7010 *ks7010_ndev_to_ks(struct net_device *ndev)
{
	return ((struct ks7010_vif *)netdev_priv(ndev))->ks;
}

/* main.c */
bool ks7010_is_asleep(struct ks7010 *ks);
void ks7010_request_wakeup(struct ks7010 *ks);
void ks7010_request_sleep(struct ks7010 *ks);

void ks7010_init_netdev(struct net_device *ndev);
int ks7010_init_hw(struct ks7010 *ks);

int ks7010_init(struct ks7010 *ks);
void ks7010_cleanup(struct ks7010 *ks);

struct ks7010 *ks7010_create(struct device *dev);
void ks7010_destroy(struct ks7010 *ks);

/* tx.c */
int ks7010_tx_start(struct sk_buff *skb, struct net_device *ndev);

int ks7010_tx_enqueue(struct ks7010 *ks, u8 *data, size_t data_size);
void ks7010_tx_hw(struct ks7010 *ks);

int ks7010_tx_init(struct ks7010 *ks);
void ks7010_tx_cleanup(struct ks7010 *ks);

/* rx.c */
void ks7010_rx(struct ks7010 *ks, u16 size);

int ks7010_rx_init(struct ks7010 *ks);
void ks7010_rx_cleanup(struct ks7010 *ks);

#endif	/* _KS7010_H */
