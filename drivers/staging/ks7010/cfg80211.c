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

#include <net/cfg80211.h>
#include <linux/inetdevice.h>

#include "ks7010.h"
#include "cfg80211.h"

#define RATE_TAB_ENT(_rate, _rateid, _flags) {  \
	.bitrate    = (_rate),                  \
	.flags      = (_flags),                 \
	.hw_value   = (_rateid),                \
}

#define CHAN_TAB_ENT(_channel, _freq, _flags) { \
	.band           = NL80211_BAND_2GHZ,    \
	.hw_value       = (_channel),           \
	.center_freq    = (_freq),              \
	.flags          = (_flags),             \
	.max_antenna_gain   = 0,                \
	.max_power      = 30,                   \
}

static struct ieee80211_rate ks7010_rates[] = {
	RATE_TAB_ENT(10, 0x1, 0),
	RATE_TAB_ENT(20, 0x2, 0),
	RATE_TAB_ENT(55, 0x4, 0),
	RATE_TAB_ENT(110, 0x8, 0),
	RATE_TAB_ENT(60, 0x10, 0),
	RATE_TAB_ENT(90, 0x20, 0),
	RATE_TAB_ENT(120, 0x40, 0),
	RATE_TAB_ENT(180, 0x80, 0),
	RATE_TAB_ENT(240, 0x100, 0),
	RATE_TAB_ENT(360, 0x200, 0),
	RATE_TAB_ENT(480, 0x400, 0),
	RATE_TAB_ENT(540, 0x800, 0),
};

static struct ieee80211_channel ks7010_2ghz_channels[] = {
	CHAN_TAB_ENT(1, 2412, 0),
	CHAN_TAB_ENT(2, 2417, 0),
	CHAN_TAB_ENT(3, 2422, 0),
	CHAN_TAB_ENT(4, 2427, 0),
	CHAN_TAB_ENT(5, 2432, 0),
	CHAN_TAB_ENT(6, 2437, 0),
	CHAN_TAB_ENT(7, 2442, 0),
	CHAN_TAB_ENT(8, 2447, 0),
	CHAN_TAB_ENT(9, 2452, 0),
	CHAN_TAB_ENT(10, 2457, 0),
	CHAN_TAB_ENT(11, 2462, 0),
	CHAN_TAB_ENT(12, 2467, 0),
	CHAN_TAB_ENT(13, 2472, 0),
	CHAN_TAB_ENT(14, 2484, 0),
};

static struct ieee80211_supported_band ks7010_band_2ghz = {
	.n_channels = ARRAY_SIZE(ks7010_2ghz_channels),
	.channels = ks7010_2ghz_channels,
	.n_bitrates = ARRAY_SIZE(ks7010_rates),
	.bitrates = ks7010_rates,
};

static bool ks7010_cfg80211_ready(struct ks7010_vif *vif)
{
	ks_debug("not implemented");
	return false;
}

static void ks7010_set_wpa_version(struct ks7010_vif *vif,
				   enum nl80211_wpa_versions wpa_version)
{
	ks_debug("%s: %u\n", __func__, wpa_version);

	vif->wpa_enabled = true;

	if (wpa_version & NL80211_WPA_VERSION_1)
		vif->auth_mode = AUTH_WPA;
	else
		vif->auth_mode = AUTH_WPA2;
}

static int
ks7010_set_dot11_auth_mode(struct ks7010_vif *vif, enum nl80211_auth_type auth)
{
	ks_debug("%s: 0x%x\n", __func__, auth);

	switch (auth) {
	case NL80211_AUTHTYPE_OPEN_SYSTEM:
		vif->dot11_auth_mode = DOT11_AUTH_OPEN;
		break;

	case NL80211_AUTHTYPE_SHARED_KEY:
		vif->dot11_auth_mode = DOT11_AUTH_SHARED;
		break;

	case NL80211_AUTHTYPE_AUTOMATIC:
		vif->dot11_auth_mode = DOT11_AUTH_OPEN | DOT11_AUTH_SHARED;
		break;

	default:
		ks_err("%s: 0x%x not supported\n", __func__, auth);
		return -ENOTSUPP;
	}

	return 0;
}

static int _set_cipher(struct ks7010_vif *vif, u32 cipher, bool ucast)
{
	enum hif_crypt_type *type;
	size_t *size;

	type = ucast ? &vif->pairwise_crypto : &vif->group_crypto;
	size = ucast ? &vif->pairwise_crypto_size : &vif->group_crypto_size;

	ks_debug("%s: cipher 0x%x, ucast %u\n", __func__, cipher, ucast);

	switch (cipher) {
	case 0:
		*type = CRYPT_NONE;
		*size = 0;
		break;

	case WLAN_CIPHER_SUITE_WEP40:
		*type = CRYPT_WEP;
		*size = WLAN_KEY_LEN_WEP40;
		break;

	case WLAN_CIPHER_SUITE_WEP104:
		*type = CRYPT_WEP;
		*size = WLAN_KEY_LEN_WEP104;
		break;

	case WLAN_CIPHER_SUITE_TKIP:
		*type = CRYPT_TKIP;
		*size = WLAN_KEY_LEN_TKIP; /* FIXME ath6kl uses 0 here? */
		break;

	case WLAN_CIPHER_SUITE_CCMP:
		*type = CRYPT_AES;
		*size = 0; /* FIXME what value? */
		break;

	default:
		ks_err("cipher 0x%x not supported\n", cipher);
		return -ENOTSUPP;
	}

	return 0;
}

static int ks7010_set_cipher_ucast(struct ks7010_vif *vif, u32 cipher)
{
	return _set_cipher(vif, cipher, true);
}

static int ks7010_set_cipher_mcast(struct ks7010_vif *vif, u32 cipher)
{
	return _set_cipher(vif, cipher, false);
}

static void ks7010_set_key_mgmt(struct ks7010_vif *vif, u32 key_mgmt)
{
	ks_debug("%s: 0x%x\n", __func__, key_mgmt);

	if (key_mgmt == WLAN_AKM_SUITE_PSK) {
		if (vif->auth_mode == AUTH_WPA)
			vif->auth_mode = AUTH_WPA_PSK;

		else if (vif->auth_mode == AUTH_WPA2)
			vif->auth_mode = AUTH_WPA2_PSK;

	/* FIXME understand this */
	} else if (key_mgmt != WLAN_AKM_SUITE_8021X) {
		vif->auth_mode = AUTH_NONE;
	}
}

static int
ks7010_cfg80211_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct ks7010_vif *vif = ks7010_wdev_to_vif(request->wdev);
	struct ks7010 *ks = vif->ks;
	struct hif_channels channels;
	struct hif_ssid ssid;
	int n_channels;
	int i;

	if (!ks7010_cfg80211_ready(vif))
		return -EIO;

	memset(&channels, 0, sizeof(channels));
	memset(&ssid, 0, sizeof(ssid));

	vif->scan_req = request;

	n_channels = request->n_channels;
	if (n_channels > HIF_MAX_CHANNELS) {
		ks_warn("only scanning first %d channels of request",
			HIF_MAX_CHANNELS);
		n_channels = HIF_MAX_CHANNELS;
	}
	channels.size = n_channels;

	for (i = 0; i < channels.size; i++) {
		u16 ch = request->channels[i]->center_freq;

		if (ch > MAX_U8_VAL)
			ks_debug("channel overflows u8");

		channels.list[i] = (u8)ch;
	}

	if (request->n_ssids > 0) {
		struct cfg80211_ssid *ptr = &request->ssids[0];

		if (request->n_ssids > 1) {
			char buf[IEEE80211_MAX_SSID_LEN + 1];

			strncpy(buf, ptr->ssid, IEEE80211_MAX_SSID_LEN);
			buf[IEEE80211_MAX_SSID_LEN] = '\0';

			ks_warn("driver supports single SSID only, scanning %s",
				buf);
		}

		ssid.size = ptr->ssid_len;
		/* src/dst buffers are the same size */
		memcpy(&ssid.buf[0], &ptr->ssid[0], ptr->ssid_len);
	}

	/* FIXME should we be using request->rates */
	ks7010_hif_scan(ks, vif->scan_type, &channels, &ssid);

	return 0;
}

static void _scan_event(struct ks7010_vif *vif, bool aborted)
{
	struct cfg80211_scan_info info = {
		.aborted = aborted,
	};

	if (!vif->scan_req)
		return;

	cfg80211_scan_done(vif->scan_req, &info);
	vif->scan_req = NULL;
}

void ks7010_cfg80211_scan_aborted(struct ks7010 *ks)
{
	_scan_event(ks->vif, false);
}

void ks7010_cfg80211_scan_complete(struct ks7010 *ks)
{
	_scan_event(ks->vif, true);
}

/* key handling is still a bit messy, let's document some assumptions here */
static void _debug_add_wpa_key(struct ks7010_vif *vif, int key_index,
			       bool pairwise, struct  key_params *params)
{
	enum hif_crypt_type key_type;

	if (params->cipher == WLAN_CIPHER_SUITE_TKIP) {
		key_type = CRYPT_TKIP;
	} else if (params->cipher == WLAN_CIPHER_SUITE_CCMP) {
		key_type = CRYPT_AES;
	} else {
		ks_debug("unknown key type");
		return;
	}

	if (!vif->wpa_enabled)
		ks_debug("adding WPA key without WPA enabled");

	if (key_type == CRYPT_TKIP) {
		if (!(vif->auth_mode == AUTH_WPA ||
		      vif->auth_mode == AUTH_WPA_PSK)) {
			ks_debug("WPA TKIP cryto mismatch");
		}
	}

	if (key_type == CRYPT_AES) {
		if (!(vif->auth_mode == AUTH_WPA2 ||
		      vif->auth_mode == AUTH_WPA2_PSK)) {
			ks_debug("WPA2 AES cryto mismatch");
		}
	}

	if (pairwise && key_index != 0)
		ks_debug("unusual index for pairwise key (is this the PTK?)");

	if (!pairwise && !(key_index == 1 || key_index == 2))
		ks_debug("unusual index for group key (is this the GTK?)");
}

/* key handling is still a bit messy, let's document some assumptions here */
static void
_debug_add_wep_key(struct ks7010_vif *vif, int key_index, bool pairwise)
{
	if (!vif->privacy_invoked)
		ks_debug("adding WEP key without WEP enabled");

	if (pairwise && !vif->pairwise_crypto)
		ks_debug("adding pairwise WEP key without cipher suite");

	if (!pairwise && !vif->group_crypto)
		ks_debug("adding group WEP key without group suite");

	if (vif->wpa_enabled)
		ks_debug("adding WEP key with WPA enabled");
}

static int _add_wep_key(struct ks7010_vif *vif, int key_index,
			const u8 *key_val, size_t key_size)
{
	struct ks7010 *ks = vif->ks;
	struct ks7010_wep_key *key;
	int ret;

	if (key_index > KS7010_MAX_WEP_KEY_INDEX) {
		ks_debug("key index %d out of bounds\n", key_index);
		return -ENOENT;
	}

	if (key_size > KS7010_WEP_KEY_MAX_SIZE)
		return -EOVERFLOW;

	key = &vif->wep_keys[key_index];
	memcpy(key->key_val, key_val, key_size);

	ret = ks7010_hif_add_wep_key(ks, key_index);
	if (ret) {
		ks_debug("failed to add WEP key");
		return ret;
	}

	return 0;
}

static int _add_wpa_key(struct ks7010_vif *vif, int key_index, bool pairwise,
			struct key_params *params)
{
	struct ks7010 *ks = vif->ks;
	struct ks7010_wpa_key *key;
	int ret;

	if (key_index > KS7010_MAX_WPA_KEY_INDEX)
		return -EINVAL;

	if (params->key_len > WLAN_MAX_KEY_LEN)
		return -EOVERFLOW;

	if (params->seq_len > KS7010_KEY_SEQ_MAX_SIZE) {
		ks_debug("seq overflow");
		return -EOVERFLOW;
	}

	_debug_add_wpa_key(vif, key_index, pairwise, params);

	key = &vif->wpa_keys[key_index];

	/* FIXME what about the tx_mic_key/rx_mic_key? */
	memset(key, 0, sizeof(*key));

	memcpy(key->key_val, params->key, params->key_len);
	key->key_size = params->key_len;

	memcpy(key->seq, params->seq, params->seq_len);
	key->seq_size = params->seq_len;

	key->cipher = params->cipher;

	ret = ks7010_hif_add_wpa_key(ks, key_index);
	if (ret) {
		ks_debug("failed to add WPA key");
		return ret;
	}

	return 0;
}

static int ks7010_cfg80211_add_key(struct wiphy *wiphy, struct net_device *ndev,
				   u8 key_index, bool pairwise,
				   const u8 *mac_addr,
				   struct key_params *params)
{
	struct ks7010_vif *vif = netdev_priv(ndev);
	int ret;

	if (!ks7010_cfg80211_ready(vif))
		return -EIO;

	if (params->cipher == WLAN_CIPHER_SUITE_WEP40 ||
	    params->cipher == WLAN_CIPHER_SUITE_WEP104) {
		if (key_index > KS7010_MAX_WEP_KEY_INDEX) {
			ks_debug("WEP key index %d out of bounds\n", key_index);
			return -ENOENT;
		}

		_debug_add_wep_key(vif, key_index, pairwise);
		ret = _add_wep_key(vif, key_index, params->key,
				   params->key_len);
		if (ret) {
			ks_debug("failed to add WEP key");
			return ret;
		}

		return 0;
	}

	if (params->cipher == WLAN_CIPHER_SUITE_TKIP ||
	    params->cipher == WLAN_CIPHER_SUITE_CCMP) {
		if (key_index > KS7010_MAX_WPA_KEY_INDEX) {
			ks_debug("WPA key index %d out of bounds\n", key_index);
			return -ENOENT;
		}

		ret = _add_wpa_key(vif, key_index, pairwise, params);
		if (ret) {
			ks_debug("failed to add WPA key");
			return ret;
		}

		return 0;
	}

	ks_debug("cipher suite unsupported");
	return -ENOTSUPP;
}

static int ks7010_cfg80211_del_key(struct wiphy *wiphy, struct net_device *ndev,
				   u8 key_index, bool pairwise,
				   const u8 *mac_addr)
{
	struct ks7010_vif *vif = netdev_priv(ndev);

	if (!ks7010_cfg80211_ready(vif))
		return -EIO;

	/* FIXME is this a WEP key or a WPA key?
	 * firmware does not support removing of keys so the best we
	 * can do is clear the entry in the VIF
	 */

	return 0;
}

static int ks7010_cfg80211_set_default_key(struct wiphy *wiphy,
					   struct net_device *ndev,
					   u8 key_index, bool unicast,
					   bool multicast)
{
	struct ks7010_vif *vif = netdev_priv(ndev);
	struct ks7010 *ks = vif->ks;
	int ret;

	if (key_index > KS7010_MAX_WEP_KEY_INDEX) {
		ks_debug("key index %d out of bounds", key_index);
		return -ENOENT;
	}

	if (key_index > KS7010_MAX_WPA_KEY_INDEX) {
		ks_debug("key index %d too big for WPA, was this a WEP key?",
			 key_index);
	}

	ret = ks7010_hif_set_default_key(ks, key_index);
	if (ret) {
		ks_debug("failed to set default key");
		return ret;
	}

	return 0;
}

static int ks7010_cfg80211_get_key(
	struct wiphy *wiphy, struct net_device *ndev, u8 key_index,
	bool pairwise, const u8 *mac_addr, void *cookie,
	void (*callback)(void *cookie, struct key_params *))
{
	struct ks7010_vif *vif = netdev_priv(ndev);
	struct ks7010_wpa_key *key = NULL;
	struct key_params params;

	if (!ks7010_cfg80211_ready(vif))
		return -EIO;

	if (key_index > KS7010_MAX_WPA_KEY_INDEX) {
		ks_debug("key index %d out of bounds\n", key_index);
		return -ENOENT;
	}

	/* FIXME is this only called for WPA keys? */
	key = &vif->wpa_keys[key_index];

	if (key->key_size == 0)
		return -ENOENT;

	memset(&params, 0, sizeof(params));
	params.cipher = key->cipher;
	params.key_len = key->key_size;
	params.seq_len = key->seq_size;
	params.seq = key->seq;
	params.key = key->key_val;

	callback(cookie, &params);

	return 0;
}

static int _connect_with_reconnect_flag(struct ks7010_vif *vif,
					bool is_reconnect)
{
	struct ks7010 *ks = vif->ks;
	int ret;

	if (is_reconnect)
		ret = ks7010_hif_reconnect(ks);
	else
		ret = ks7010_hif_connect(ks);

	if (ret == -EINVAL) {
		memset(vif->ssid, 0, sizeof(vif->ssid));
		vif->ssid_len = 0;
		ks_debug("invalid request\n");

		return -ENOENT;

	} else if (ret) {
		return -EIO;
	}

	set_bit(CONNECT_PEND, &vif->flags);

	return 0;
}

static int _reconnect(struct ks7010_vif *vif)
{
	int ret;

	ret = _connect_with_reconnect_flag(vif, true);
	if (ret) {
		ks_debug("failed to reconnect");
		return ret;
	}

	return 0;
}

static int _connect(struct ks7010_vif *vif, struct cfg80211_connect_params *sme)
{
	struct ks7010 *ks = vif->ks;
	u32 cipher;
	int ret;

	ks7010_hif_disconnect(ks);

	memset(vif->ssid, 0, sizeof(vif->ssid));
	vif->ssid_len = sme->ssid_len;
	memcpy(vif->ssid, sme->ssid, sme->ssid_len);

	if (sme->channel)
		vif->ch_hint = sme->channel->center_freq;

	memset(vif->req_bssid, 0, sizeof(vif->req_bssid));
	if (sme->bssid && !is_broadcast_ether_addr(sme->bssid))
		memcpy(vif->req_bssid, sme->bssid, sizeof(vif->req_bssid));

	if (sme->crypto.wpa_versions)
		ks7010_set_wpa_version(vif, sme->crypto.wpa_versions);

	ret = ks7010_set_dot11_auth_mode(vif, sme->auth_type);
	if (ret) {
		ks_debug("failed to set dot11 auth mode");
		return ret;
	}

	cipher = 0;
	if (sme->crypto.n_ciphers_pairwise) {
		if (sme->crypto.n_ciphers_pairwise > 1)
			ks_debug("only using first cipher");

		cipher = sme->crypto.ciphers_pairwise[0];
	}

	ret = ks7010_set_cipher_ucast(vif, cipher);
	if (ret) {
		ks_debug("failed to set ucast cipher");
		return ret;
	}

	ret = ks7010_set_cipher_mcast(vif, sme->crypto.cipher_group);
	if (ret) {
		ks_debug("failed to set mcast cipher");
		return ret;
	}

	if (sme->crypto.n_akm_suites > 0) {
		if (sme->crypto.n_akm_suites > 1)
			ks_debug("only using first akm cipher");

		ks7010_set_key_mgmt(vif, sme->crypto.akm_suites[0]);
	}

	/* FIXME is this correct? */
	if (sme->key_len && vif->privacy_invoked &&
	    vif->auth_mode == AUTH_NONE &&
	    vif->pairwise_crypto == CRYPT_WEP) {
		_add_wep_key(vif, sme->key_idx, sme->key, sme->key_len);
		ks7010_hif_set_default_key(ks, sme->key_idx);
	}

	ret = _connect_with_reconnect_flag(vif, false);
	if (ret) {
		ks_debug("failed to connect");
		return ret;
	}

	return 0;
}

static int ks7010_cfg80211_connect(struct wiphy *wiphy, struct net_device *ndev,
				   struct cfg80211_connect_params *sme)
{
	struct ks7010_vif *vif = netdev_priv(ndev);
	bool connect_to_cur_ssid = false;

	if (!ks7010_cfg80211_ready(vif))
		return -EIO;

	/* FIXME ath6kl uses a binary semaphore here? */

	if (vif->ssid_len == sme->ssid_len &&
	    (memcmp(vif->ssid, sme->ssid, vif->ssid_len) == 0))
		connect_to_cur_ssid = true;

	if (connect_to_cur_ssid &&
	    test_bit(CONNECTED, &vif->flags)) {
		return _reconnect(vif);
	}

	return _connect(vif, sme);
}

static int ks7010_cfg80211_disconnect(
	struct wiphy *wiphy, struct net_device *ndev, u16 reason_code)
{
	struct ks7010_vif *vif = netdev_priv(ndev);

	ks_debug("disconnect reason=%u\n", reason_code);

	if (!ks7010_cfg80211_ready(vif))
		return -EIO;

	ks7010_hif_disconnect(vif->ks);

	memset(vif->ssid, 0, sizeof(vif->ssid));
	vif->ssid_len = 0;

	return 0;
}

static int ks7010_cfg80211_set_wiphy_params(struct wiphy *wiphy, u32 changed)
{
	struct ks7010 *ks = (struct ks7010 *)wiphy_priv(wiphy);
	struct ks7010_vif *vif = ks->vif;

	ks_debug("%s: changed 0x%x\n", __func__, changed);

	if (!ks7010_cfg80211_ready(vif))
		return -EIO;

	if (changed & WIPHY_PARAM_RTS_THRESHOLD)
		ks7010_hif_set_rts_thresh(ks, wiphy->rts_threshold);

	if (changed & WIPHY_PARAM_FRAG_THRESHOLD)
		ks7010_hif_set_frag_thresh(ks, wiphy->frag_threshold);

	return 0;
}

static struct cfg80211_ops ks7010_cfg80211_ops = {
	.scan = ks7010_cfg80211_scan,
	.add_key = ks7010_cfg80211_add_key,
	.get_key = ks7010_cfg80211_get_key,
	.del_key = ks7010_cfg80211_del_key,
	.set_default_key = ks7010_cfg80211_set_default_key,
	.connect = ks7010_cfg80211_connect,
	.disconnect = ks7010_cfg80211_disconnect,
	.set_wiphy_params = ks7010_cfg80211_set_wiphy_params,
};

static const struct ethtool_ops ks7010_ethtool_ops = {
	.get_drvinfo = cfg80211_get_drvinfo,
	.get_link = ethtool_op_get_link,
};

static const u32 cipher_suites[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
};

/* FIXME understand this */
static const struct ieee80211_txrx_stypes
ks7010_mgmt_stypes[NUM_NL80211_IFTYPES] = {
	[NL80211_IFTYPE_STATION] = {
		.tx = BIT(IEEE80211_STYPE_ACTION >> 4) |
		BIT(IEEE80211_STYPE_PROBE_RESP >> 4),
		.rx = BIT(IEEE80211_STYPE_ACTION >> 4) |
		BIT(IEEE80211_STYPE_PROBE_REQ >> 4)
	},
};

void ks7010_cfg80211_stop(struct ks7010_vif *vif)
{
	if (test_bit(CONNECT_PEND, &vif->flags)) {
		cfg80211_connect_result(vif->ndev, vif->bssid, NULL, 0,
					NULL, 0,
					WLAN_STATUS_UNSPECIFIED_FAILURE,
					GFP_KERNEL);
	}

	if (test_bit(CONNECTED, &vif->flags))
		cfg80211_disconnected(vif->ndev, 0, NULL, 0, true, GFP_KERNEL);

	clear_bit(CONNECTED, &vif->flags);
	clear_bit(CONNECT_PEND, &vif->flags);

	if (vif->scan_req)
		ks7010_cfg80211_scan_aborted(vif->ks);
}

static int ks7010_cfg80211_hw_init(struct ks7010_vif *vif)
{
	struct ks7010 *ks = vif->ks;

	ks7010_hif_get_mac_addr(ks);

	/* FIXME add completion */

	if (!ks->mac_addr_valid)
		return -ENODEV;

	ks7010_hif_get_fw_version(ks);

	ks7010_hif_set_rts_thresh(ks, vif->rts_thresh);
	ks7010_hif_set_frag_thresh(ks, vif->frag_thresh);

	return 0;
}

/* called from ks7010_cfg80211_add_interface() */
static int ks7010_cfg80211_vif_init(struct ks7010_vif *vif,
				    enum hif_network_type nw_type)
{
	int ret;

	vif->ssid_len = 0;
	memset(vif->ssid, 0, sizeof(vif->ssid));

	vif->nw_type = nw_type;
	vif->dot11_auth_mode = DOT11_AUTH_OPEN;

	vif->auth_mode = AUTH_NONE;
	vif->pairwise_crypto = CRYPT_NONE;
	vif->pairwise_crypto_size = 0;
	vif->group_crypto = CRYPT_NONE;
	vif->group_crypto_size = 0;
	vif->privacy_invoked = false;
	vif->wpa_enabled = false;

	memset(vif->wep_keys, 0, sizeof(vif->wep_keys));
	memset(vif->wpa_keys, 0, sizeof(vif->wpa_keys));
	memset(vif->bssid, 0, sizeof(vif->bssid));

	spin_lock_init(&vif->if_lock);

	vif->scan_type = BSS_SCAN_ACTIVE;
	vif->tx_rate = TX_RATE_AUTO;
	vif->preamble = PREAMBLE_LONG;
	vif->power_mgmt = POWER_MGMT_ACTIVE;

	vif->beacon_lost_count = KS7010_DEFAULT_BEACON_LOST_COUNT;
	vif->rts_thresh = KS7010_DEFAULT_RTS_THRESHOLD;
	vif->frag_thresh = KS7010_DEFAULT_FRAG_THRESHOLD;

	/* FIXME default to 802.11g? */
	vif->phy_type = PHY_MODE_11BG_COMPATIBLE;

	vif->cts_mode = CTS_MODE_FALSE;

	ret = ks7010_cfg80211_hw_init(vif);
	if (ret) {
		ks_err("failed to init hw");
		return ret;
	}

	return 0;
}

/**
 * ks7010_cfg80211_rm_interface() - Remove virtual interface.
 * @ks: The ks7010 device.
 *
 * Caller must hold the RTNL lock.
 */
void ks7010_cfg80211_rm_interface(struct ks7010 *ks)
{
	struct ks7010_vif *vif = ks->vif;

	unregister_netdevice(vif->ndev);

	if (ks->vif)
		ks->vif = NULL;
}

/**
 * ks7010_cfg80211_add_interface() - Initializes and adds a virtual interface.
 * @vif: The ks7010 device virtual interface.
 * @name: Device name format string, passed to alloc_netdev().
 * @name_assign_type: Origin of device name, passed to alloc_netdev().
 * @type: &enum hif_network_type network type.
 *
 * Caller must hold the RTNL lock.
 */
struct wireless_dev *
ks7010_cfg80211_add_interface(struct ks7010 *ks, const char *name,
			      unsigned char name_assign_type,
			      enum hif_network_type nw_type)
{
	struct net_device *ndev;
	struct ks7010_vif *vif;
	enum nl80211_iftype nl_iftype;
	int ret;

	if (nw_type != INFRA_NETWORK) {
		ks_debug("unsupported network type");
		return ERR_PTR(-EINVAL);
	}
	nl_iftype = NL80211_IFTYPE_STATION;

	ndev = alloc_netdev(sizeof(*vif), name, name_assign_type, ether_setup);
	if (!ndev)
		return ERR_PTR(-ENOMEM);

	vif = netdev_priv(ndev);
	vif->ndev = ndev;

	ks->vif = vif;
	vif->ks = ks;

	ndev->ieee80211_ptr = &vif->wdev;
	vif->wdev.wiphy = ks->wiphy;
	SET_NETDEV_DEV(ndev, wiphy_dev(vif->wdev.wiphy));
	vif->wdev.netdev = ndev;
	vif->wdev.iftype = nl_iftype;

	ks7010_init_netdev(ndev);

	ret = ks7010_cfg80211_vif_init(vif, nw_type);
	if (ret)
		goto err_cleanup;

	netdev_set_default_ethtool_ops(ndev, &ks7010_ethtool_ops);
	if (!ks->mac_addr_valid) {
		ret = -ENODEV;
		goto err_cleanup;
	}

	ether_addr_copy(ndev->dev_addr, ks->mac_addr);

	if (register_netdevice(ndev)) {
		ret = -ENODEV;
		goto err_cleanup;
	}

	ks->vif = vif;

	return &vif->wdev;

err_cleanup:
	ks7010_cfg80211_rm_interface(ks);
	return ERR_PTR(ret);
}

/**
 * ks7010_cfg80211_init() - cfg80211 initialization.
 * @ks: The ks7010 device.
 */
int ks7010_cfg80211_init(struct ks7010 *ks)
{
	struct wiphy *wiphy = ks->wiphy;
	int ret;

	wiphy->mgmt_stypes = ks7010_mgmt_stypes;

	/* set device pointer for wiphy */
	set_wiphy_dev(wiphy, ks->dev);

	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);

	ret = wiphy_register(wiphy);
	if (ret < 0) {
		ks_err("couldn't register wiphy device\n");
		return ret;
	}

	wiphy->bands[NL80211_BAND_2GHZ] = &ks7010_band_2ghz;

	wiphy->cipher_suites = cipher_suites;
	wiphy->n_cipher_suites = ARRAY_SIZE(cipher_suites);

	ks->wiphy_registered = true;

	return 0;
}

/**
 * ks7010_cfg80211_cleanup() - cfg80211 cleanup.
 * @ks: The ks7010 device.
 */
void ks7010_cfg80211_cleanup(struct ks7010 *ks)
{
	wiphy_unregister(ks->wiphy);

	ks->wiphy_registered = false;
}

/**
 * ks7010_cfg80211_create() - Create wiphy.
 */
struct ks7010 *ks7010_cfg80211_create(void)
{
	struct ks7010 *ks;
	struct wiphy *wiphy;

	/* create a new wiphy for use with cfg80211 */
	wiphy = wiphy_new(&ks7010_cfg80211_ops, sizeof(*ks));

	if (!wiphy) {
		ks_err("couldn't allocate wiphy device\n");
		return NULL;
	}

	ks = wiphy_priv(wiphy);
	ks->wiphy = wiphy;

	return ks;
}

/**
 * ks7010_cfg80211_destroy() - Free wiphy.
 * @ks: The ks7010 device.
 */
void ks7010_cfg80211_destroy(struct ks7010 *ks)
{
	wiphy_free(ks->wiphy);
}

