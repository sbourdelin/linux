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

#include <net/netlink.h>
#include <net/mac80211.h>

#include "core.h"
#include "wmi-ops.h"
#include "debug.h"

static int ath10k_vendor_bss_filter_vendor_handler(struct wiphy *wiphy,
						   struct wireless_dev *wdev,
						   const void *data,
						   int data_len);

static const struct nla_policy
ath10k_vendor_bss_filter_policy[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAX + 1] = {
	[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAC_ADDR] = { .len = ETH_ALEN },
	[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_TYPE] = { .type = NLA_U8 },
	[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_ACTION] = { .type = NLA_U8 },
	[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS] = { .type = NLA_NESTED },
};

static struct wiphy_vendor_command ath10k_vendor_commands[] = {
	{
		.info.vendor_id = QCA_NL80211_VENDOR_ID,
		.info.subcmd = QCA_NL80211_VENDOR_SUBCMD_BSS_FILTER,
		.flags = WIPHY_VENDOR_CMD_NEED_WDEV |
			 WIPHY_VENDOR_CMD_NEED_RUNNING,
		.doit = ath10k_vendor_bss_filter_vendor_handler,
	}
};

static u32
ath10k_vendor_unassoc_sta_table_hash(const void *addr, u32 len, u32 seed)
{
	/* Use last four bytes of hw addr as hash index */
	return jhash_1word(*(u32 *)(addr + 2), seed);
}

static const struct rhashtable_params ath10k_vendor_unassoc_sta_rht = {
	.nelem_hint = 2,
	.automatic_shrinking = true,
	.key_len = ETH_ALEN,
	.key_offset = offsetof(struct ath10k_vendor_unassoc_sta, stats.addr),
	.head_offset = offsetof(struct ath10k_vendor_unassoc_sta, rhash),
	.hashfn = ath10k_vendor_unassoc_sta_table_hash,
};

struct ath10k_vendor_unassoc_sta *
ath10k_vendor_unassoc_sta_lookup(struct ath10k_vendor_unassoc_sta_tbl *tbl,
				 u8 *addr)
{
	return rhashtable_lookup_fast(&tbl->rhead,
				      addr, ath10k_vendor_unassoc_sta_rht);
}

static u8 ath10k_vendor_bssid_filter_get_free_idx(struct ath10k *ar, u8 *addr)
{
	struct ath10k_vendor_bssid_info *bssid;
	u8 idx = ATH10K_VENDOR_BSSID_FILTER_INVALID_IDX, loop = 0;

	while (loop < ar->vendor.bss_filter.max) {
		bssid = &ar->vendor.bss_filter.bssid[loop];

		/* check given MAC already configured */
		if (addr && !is_zero_ether_addr(addr)) {
			if (memcmp(bssid->addr, addr, ETH_ALEN) == 0) {
				idx = loop;
				break;
			}
		}

		/* Get the least free index */
		if (idx == ATH10K_VENDOR_BSSID_FILTER_INVALID_IDX &&
		    is_zero_ether_addr(bssid->addr)) {
			idx = loop;
		}

		loop++;
	}
	return idx;
}

static int ath10k_vendor_bssid_filter_add(struct ath10k *ar, u8 *addr,
					  u32 vdev_id)
{
	struct ath10k_vendor_bssid_info *bssid;
	int ret;
	u8 idx;

	/* If monitor started then no point to enable other bss filter */
	if (ar->monitor_started) {
		ath10k_warn(ar, "not able to enable other bss filter if monitor alive\n");
		return -EBUSY;
	}

	idx = ath10k_vendor_bssid_filter_get_free_idx(ar, addr);
	if (idx == ATH10K_VENDOR_BSSID_FILTER_INVALID_IDX) {
		ath10k_warn(ar, "No Free idx to add BSS filter\n");
		return -EBUSY;
	}

	bssid = &ar->vendor.bss_filter.bssid[idx];

	/* If valid MAC already configured in the given index, check both are
	 * same one or not. If its same, just add the reference count
	 * otherwise throw error.
	 */
	if (!is_zero_ether_addr(bssid->addr)) {
		if (memcmp(bssid->addr, addr, ETH_ALEN) != 0) {
			ath10k_warn(ar, "Already used idx %d\n", idx);
			return -EINVAL;
		}
		goto add_ref;
	}

	/* Send wmi message to receive the given BSSID frames, zeroth
	 * index is reserved so we cannot use that index.
	 */
	ret = ath10k_wmi_set_neighbor_rx_param(ar, vdev_id,
					       addr, idx + 1,
					       WMI_NEIGHBOR_RX_ACTION_ADD,
					       WMI_NEIGHBOR_RX_TYPE_BSSID);
	if (ret) {
		ath10k_warn(ar, "BSS Add Filter failed on idx %d addr %pM\n",
			    idx, addr);
		return ret;
	}

	ether_addr_copy(bssid->addr, addr);

	spin_lock_bh(&ar->data_lock);
	ar->vendor.bss_filter.n_bssid++;
	spin_unlock_bh(&ar->data_lock);

add_ref:
	/*If the VAP already configured, then no need to add reference count */
	if (!(bssid->vdev_map & (1LL << vdev_id))) {
		bssid->ref++;
		bssid->vdev_map |= 1LL << vdev_id;
	}

	ath10k_dbg(ar, ATH10K_DBG_MAC, "Filter added vdev %d idx %d addr %pM\n",
		   vdev_id, idx, addr);
	return 0;
}

static int ath10k_vendor_bssid_filter_delete(struct ath10k *ar, u8 *addr,
					     u8 idx, u32 vdev_id)
{
	struct ath10k_vendor_bssid_info *bssid;
	int ret;

	if (idx >= ar->vendor.bss_filter.max) {
		ath10k_warn(ar, "Invalid idx %d\n", idx);
		return -EINVAL;
	}

	bssid = ar->vendor.bss_filter.bssid;
	/* Check this bssid filter configured by the given VAP */
	if (!(bssid[idx].vdev_map & (1LL << vdev_id))) {
		ath10k_warn(ar, "BSS Filter addr %pM not configured by vdev %d\n",
			    addr, vdev_id);
		return -EINVAL;
	}

	bssid[idx].ref--;
	bssid[idx].vdev_map &= ~(1LL << vdev_id);

	/* If no reference count, then Send wmi message to not receive
	 * the given BSSID frames, zeroth index is reserved so we cannot
	 * use that index.
	 */
	if (!bssid[idx].ref) {
		ret = ath10k_wmi_set_neighbor_rx_param(ar,
						       vdev_id,
						       addr, idx + 1,
						       WMI_NEIGHBOR_RX_ACTION_DEL,
						       WMI_NEIGHBOR_RX_TYPE_BSSID);
		if (ret) {
			ath10k_warn(ar, "wmi filter delete failed ret %d\n", ret);
			bssid[idx].ref++;
			bssid[idx].vdev_map &= ~(1LL << vdev_id);
			return ret;
		}

		ath10k_dbg(ar, ATH10K_DBG_MAC, "Filter deleted vdev %d addr %pM\n",
			   vdev_id, addr);

		memset(bssid[idx].addr, 0, ETH_ALEN);

		spin_lock_bh(&ar->data_lock);
		ar->vendor.bss_filter.n_bssid--;
		spin_unlock_bh(&ar->data_lock);
	}

	return ret;
}

static int
ath10k_vendor_bssid_filter_action(struct ath10k *ar,
				  u8 *addr,
				  enum qca_wlan_vendor_bss_filter_type type,
				  enum qca_wlan_vendor_bss_filter_action action,
				  u32 vdev_id)
{
	struct ath10k_vendor_bssid_info *bssid;
	int ret;
	u8 loop, idx = ATH10K_VENDOR_BSSID_FILTER_INVALID_IDX;

	if (!is_valid_ether_addr(addr))
		return -EINVAL;

	if (action == QCA_WLAN_VENDOR_BSS_FILTER_ACTION_ADD) {
		ret = ath10k_vendor_bssid_filter_add(ar, addr, vdev_id);
	} else if (action == QCA_WLAN_VENDOR_BSS_FILTER_ACTION_DEL) {
		if (!ar->vendor.bss_filter.n_bssid) {
			ath10k_warn(ar, "No BSS Filter to delete\n");
			return -EINVAL;
		}

		bssid = ar->vendor.bss_filter.bssid;
		loop = 0;
		while (loop < ar->vendor.bss_filter.max) {
			if (memcmp(bssid[loop].addr,
				   addr, ETH_ALEN) == 0) {
				idx = loop;
				break;
			}

			loop++;
		}

		if (idx == ATH10K_VENDOR_BSSID_FILTER_INVALID_IDX) {
			ath10k_warn(ar, "Invalid BSS addr %pM\n", addr);
			return -EINVAL;
		}

		ret = ath10k_vendor_bssid_filter_delete(ar, addr, idx, vdev_id);
	} else {
		ath10k_warn(ar, "Invalid action %d\n", action);
		return -EINVAL;
	}

	return ret;
}

static int ath10k_vendor_unassoc_sta_filter_add(struct ath10k *ar, u8 *addr)
{
	struct ath10k_vendor_unassoc_sta_tbl *tbl = &ar->vendor.bss_filter.tbl;
	struct ath10k_vendor_unassoc_sta *sta, *tmp = NULL;
	int ret;

	if (!ar->vendor.bss_filter.max) {
		ath10k_warn(ar, "Not supported by platform\n");
		return -EOPNOTSUPP;
	}

	sta = kzalloc(sizeof(*sta), GFP_KERNEL);
	if (!sta)
		return -ENOMEM;

	if (!tbl->entries)
		rhashtable_init(&ar->vendor.bss_filter.tbl.rhead,
				&ath10k_vendor_unassoc_sta_rht);

	ether_addr_copy(sta->stats.addr, addr);

	rcu_read_lock();
	do {
		ret = rhashtable_lookup_insert_fast(&tbl->rhead,
						    &sta->rhash,
						    ath10k_vendor_unassoc_sta_rht);
		if (ret == -EEXIST)
			tmp = rhashtable_lookup_fast(&tbl->rhead,
						     sta->stats.addr,
						     ath10k_vendor_unassoc_sta_rht);
	} while (unlikely(ret == -EEXIST && !tmp));

	if (ret)
		goto error;

	tbl->entries++;
	rcu_read_unlock();

	ath10k_dbg(ar, ATH10K_DBG_MAC, "Unassoc sta %pM added\n", addr);
	return 0;

error:
	rcu_read_unlock();
	kfree(sta);
	return ret;
}

static void
ath10k_vendor_unassoc_sta_delete(struct ath10k_vendor_unassoc_sta_tbl *tbl,
				 struct ath10k_vendor_unassoc_sta *sta)
{
	if (!tbl || !sta)
		return;

	tbl->entries--;
	kfree_rcu(sta, rcu);
}

static void ath10k_vendor_unassoc_sta_rht_free(void *ptr, void *tblptr)
{
	struct ath10k_vendor_unassoc_sta_tbl *tbl = tblptr;
	struct ath10k_vendor_unassoc_sta *sta = ptr;

	ath10k_vendor_unassoc_sta_delete(tbl, sta);
}

static int ath10k_vendor_unassoc_sta_filter_delete(struct ath10k *ar, u8 *addr)
{
	struct ath10k_vendor_unassoc_sta_tbl *tbl = &ar->vendor.bss_filter.tbl;
	struct ath10k_vendor_unassoc_sta *sta;

	if (!tbl->entries) {
		ath10k_warn(ar, "No sta to delete\n");
		return -EINVAL;
	}

	rcu_read_lock();
	sta = rhashtable_lookup_fast(&tbl->rhead,
				     addr,
				     ath10k_vendor_unassoc_sta_rht);
	if (!sta) {
		rcu_read_unlock();
		ath10k_warn(ar, "Failed: Given addr %pM not in the list\n", addr);
		return -ENXIO;
	}

	rhashtable_remove_fast(&tbl->rhead, &sta->rhash,
			       ath10k_vendor_unassoc_sta_rht);
	tbl->entries--;
	kfree_rcu(sta, rcu);
	rcu_read_unlock();

	if (!tbl->entries)
		rhashtable_destroy(&tbl->rhead);

	ath10k_dbg(ar, ATH10K_DBG_MAC, "Unassoc sta %pM deleted\n", addr);

	return 0;
}

static int
ath10k_vendor_get_unassoc_sta_stats(struct ath10k *ar, u8 *addr,
				    struct ath10k_vendor_bss_filter_get_reply *reply)
{
	struct ath10k_vendor_unassoc_sta_tbl *tbl = &ar->vendor.bss_filter.tbl;
	struct ath10k_vendor_unassoc_sta *sta;
	struct ath10k_vendor_unassoc_sta_stats *sta_s;

	if (!tbl->entries) {
		ath10k_warn(ar, "No sta exist to get statistics\n");
		return -EINVAL;
	}

	rcu_read_lock();

	sta = rhashtable_lookup_fast(&tbl->rhead,
				     addr,
				     ath10k_vendor_unassoc_sta_rht);
	if (!sta) {
		ath10k_warn(ar, "sta %pM not exist\n", addr);
		rcu_read_unlock();
		return -ENXIO;
	}

	reply->n_sta = 1;
	sta_s = (struct ath10k_vendor_unassoc_sta_stats *)reply->data;
	memcpy(sta_s, &sta->stats, sizeof(sta->stats));
	ath10k_dbg(ar, ATH10K_DBG_MAC, "Get unassoc stats sta %pM rssi %d ts 0x%llx\n",
		   sta_s->addr, sta_s->rssi, sta_s->rssi_ts);

	rcu_read_unlock();

	return 0;
}

static int
ath10k_vendor_dump_unassoc_sta_stats(struct ath10k *ar,
				     struct ath10k_vendor_bss_filter_get_reply *reply)
{
	struct ath10k_vendor_unassoc_sta_tbl *tbl = &ar->vendor.bss_filter.tbl;
	struct ath10k_vendor_unassoc_sta *sta;
	struct rhashtable_iter iter;
	struct ath10k_vendor_unassoc_sta_stats *sta_s;
	int ret, idx;

	if (!tbl->entries) {
		ath10k_warn(ar, "No sta exist to get statistics\n");
		return -EINVAL;
	}

	ret = rhashtable_walk_init(&tbl->rhead, &iter, GFP_ATOMIC);
	if (ret) {
		ath10k_warn(ar, "rhashtbl walk init Failed ret %d\n", ret);
		return ret;
	}

	rhashtable_walk_start(&iter);

	idx = 0;
	sta_s = (struct ath10k_vendor_unassoc_sta_stats *)reply->data;
	ath10k_dbg(ar, ATH10K_DBG_MAC, "Get All Statistics\n");

	while ((sta = rhashtable_walk_next(&iter))) {
		if (IS_ERR(sta) && PTR_ERR(sta) == -EAGAIN)
			continue;

		if (IS_ERR(sta)) {
			ret = -EINVAL;
			break;
		}

		if (idx >= reply->n_sta)
			break;

		memcpy(sta_s, &sta->stats, sizeof(sta->stats));
		ath10k_dbg(ar, ATH10K_DBG_MAC, "[%d] sta %pM rssi %d ts 0x%llx\n",
			   idx, sta_s->addr, sta_s->rssi, (u64)(sta_s->rssi_ts));
		sta_s++;
		idx++;
	}
	reply->n_sta = idx;

	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	return ret;
}

static int
ath10k_vendor_unassoc_sta_filter_action(struct ath10k *ar,
					u8 *addr,
					enum qca_wlan_vendor_bss_filter_type type,
					enum qca_wlan_vendor_bss_filter_action action)
{
	struct ath10k_vendor_bss_filter_get_reply *reply_msg;
	struct ath10k_vendor_unassoc_sta_stats *sta;
	struct sk_buff *reply_skb;
	struct nlattr *nl_stats, *nl_sta;
	int msglen, n_sta, ret, i;
	bool all;

	switch (action) {
	case QCA_WLAN_VENDOR_BSS_FILTER_ACTION_ADD:
		if (!is_valid_ether_addr(addr)) {
			ret = -EINVAL;
			goto out;
		}

		ret = ath10k_vendor_unassoc_sta_filter_add(ar, addr);
		if (ret) {
			ath10k_warn(ar, "sta add failed ret %d\n", ret);
			goto out;
		}
		break;
	case QCA_WLAN_VENDOR_BSS_FILTER_ACTION_DEL:
		ret = ath10k_vendor_unassoc_sta_filter_delete(ar, addr);
		if (ret) {
			ath10k_warn(ar, "sta delete failed ret %d\n", ret);
			goto out;
		}
		break;
	case QCA_WLAN_VENDOR_BSS_FILTER_ACTION_GET:
		/* If the given MAC is Broadcast address , then we need to get
		 * the statistics for all the stations
		 */
		if (is_broadcast_ether_addr(addr)) {
			n_sta = ar->vendor.bss_filter.tbl.entries;
			all = true;
		} else if (is_valid_ether_addr(addr)) {
			n_sta = 1;
			all = false;
		} else {
			ath10k_warn(ar, "Invalid addr %pM\n", addr);
			ret = -EINVAL;
			goto out;
		}

		msglen = sizeof(*reply_msg) + (n_sta * sizeof(*sta));
		reply_msg = kzalloc(msglen, GFP_KERNEL);
		if (!reply_msg) {
			ath10k_warn(ar, "Failed to alloc %d bytes\n", msglen);
			ret = -ENOMEM;
			goto out;
		}

		reply_msg->n_sta = n_sta;
		if (all) {
			ret = ath10k_vendor_dump_unassoc_sta_stats(ar,
								   reply_msg);
		} else {
			ret = ath10k_vendor_get_unassoc_sta_stats(ar, addr,
								  reply_msg);
		}

		if (ret) {
			ath10k_warn(ar, "Get stats Failed ret %d\n", ret);
			goto free_reply_msg;
		}

		/* Send a response to the command */
		reply_skb = cfg80211_vendor_cmd_alloc_reply_skb(ar->hw->wiphy,
								msglen);
		if (!reply_skb) {
			ret = -ENOMEM;
			goto free_reply_msg;
		}

		nl_stats = nla_nest_start(reply_skb,
					  QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS);
		if (!nl_stats) {
			ret = -ENOBUFS;
			goto nla_put_failure;
		}

		sta = (struct ath10k_vendor_unassoc_sta_stats *)reply_msg->data;
		for (i = 0; i < reply_msg->n_sta; i++) {
			nl_sta = nla_nest_start(reply_skb, i);
			if (!nl_sta) {
				ret = -ENOBUFS;
				goto nla_put_failure;
			}

			if (nla_put
			       (reply_skb,
				QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS_MAC,
				ETH_ALEN, sta->addr)) {
				ret = -ENOBUFS;
				goto nla_put_failure;
			}

			if (sta->filled & (1ULL << NL80211_STA_INFO_SIGNAL) &&
			    nla_put_u8
			       (reply_skb,
				QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS_RSSI,
				sta->rssi)) {
				ret = -ENOBUFS;
				goto nla_put_failure;
			}

			if (sta->filled & (1ULL << NL80211_STA_INFO_SIGNAL) &&
			    nla_put_u64_64bit
			       (reply_skb,
				QCA_WLAN_VENDOR_ATTR_BSS_FILTER_STA_STATS_RSSI_TS,
				sta->rssi_ts,
				QCA_WLAN_VENDOR_ATTR_BSS_FILTER_PAD)) {
				ret = -ENOBUFS;
				goto nla_put_failure;
			}

			nla_nest_end(reply_skb, nl_sta);
			sta++;
		}

		nla_nest_end(reply_skb, nl_stats);

		ath10k_dbg(ar, ATH10K_DBG_MAC, "sending vendor cmd reply\n");

		ret = cfg80211_vendor_cmd_reply(reply_skb);
		if (ret) {
			ath10k_warn(ar, "failed to send vendor reply %d\n", ret);
			goto free_reply_msg;
		}

		kfree(reply_msg);
		break;
	default:
		ath10k_warn(ar, "Invalid action %d\n", action);
		ret = -EINVAL;
		goto out;
	}

	return 0;

nla_put_failure:
	kfree_skb(reply_skb);
free_reply_msg:
	kfree(reply_msg);
out:
	return ret;
}

static void ath10k_vendor_unassoc_sta_cleanup(struct ath10k *ar)
{
	struct ath10k_vendor_unassoc_sta_tbl *tbl = &ar->vendor.bss_filter.tbl;

	if (!tbl->entries)
		return;

	ath10k_dbg(ar, ATH10K_DBG_MAC, "unassoc sta cleanup\n");
	rhashtable_free_and_destroy(&tbl->rhead,
				    ath10k_vendor_unassoc_sta_rht_free, tbl);
	tbl->entries = 0;
}

void ath10k_vendor_bss_filter_cleanup(struct ath10k_vif *arvif)
{
	struct ath10k *ar = arvif->ar;
	struct ath10k_vendor_bssid_info *bssid;
	u32 vdev_id;
	u8 i;

	if (!ar->vendor.bss_filter.max || arvif->vdev_type != WMI_VDEV_TYPE_AP)
		return;

	if (!ar->vendor.bss_filter.n_bssid)
		goto sta_cleanup;

	ath10k_dbg(ar, ATH10K_DBG_MAC, "BSS filter cleanup\n");
	vdev_id = arvif->vdev_id;
	i = 0;
	bssid = ar->vendor.bss_filter.bssid;
	while (i < ar->vendor.bss_filter.max) {
		if (is_valid_ether_addr(bssid[i].addr)) {
			ath10k_vendor_bssid_filter_delete(ar, bssid[i].addr, i,
							  vdev_id);
		}
		i++;
	}

sta_cleanup:
	/* Do station cleanup only no other bss filter enabled */
	if (!ar->vendor.bss_filter.n_bssid)
		ath10k_vendor_unassoc_sta_cleanup(ar);
}

static int ath10k_vendor_bss_filter_vendor_handler(struct wiphy *wiphy,
						   struct wireless_dev *wdev,
						   const void *data,
						   int data_len)
{
	struct ieee80211_vif *vif;
	struct ath10k_vif *arvif;
	struct ath10k *ar;
	struct nlattr *tb[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAX + 1];
	u8 addr[ETH_ALEN];
	enum qca_wlan_vendor_bss_filter_type type;
	enum qca_wlan_vendor_bss_filter_action action;
	int ret;

	if (!wdev)
		return -EINVAL;

	vif = wdev_to_ieee80211_vif(wdev);
	if (!vif)
		return -EINVAL;

	arvif = (void *)vif->drv_priv;
	if (!arvif)
		return -EINVAL;

	ar = arvif->ar;

	mutex_lock(&ar->conf_mutex);

	/* Check BSSID filter is supported in the platform and then allow
	 * filter only for the AP VAP.
	 */
	if (!ar->vendor.bss_filter.max || arvif->vdev_type != WMI_VDEV_TYPE_AP) {
		ath10k_warn(ar, "BSS filter not supported Max %d vdev type %d\n",
			    ar->vendor.bss_filter.max, arvif->vdev_type);
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* Check the given Data is valid */
	if (!data) {
		ath10k_warn(ar, "invalid Data\n");
		ret = -EINVAL;
		goto out;
	}

	ret = nla_parse(tb, QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAX, data, data_len,
			ath10k_vendor_bss_filter_policy, NULL);
	if (ret) {
		ath10k_warn(ar, "invalid BSS filter policy ATTR\n");
		goto out;
	}

	if (!tb[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAC_ADDR] ||
	    !tb[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_TYPE] ||
	    !tb[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_ACTION]) {
		ath10k_warn(ar, "invalid BSS filter ATTR\n");
		ret = -EINVAL;
		goto out;
	}

	ether_addr_copy(addr,
			nla_data(tb[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_MAC_ADDR]));
	type = nla_get_u8(tb[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_TYPE]);
	action = nla_get_u8(tb[QCA_WLAN_VENDOR_ATTR_BSS_FILTER_ACTION]);

	ath10k_dbg(ar, ATH10K_DBG_MAC, "Req MAC %pM type %d action %d\n",
		   addr, type, action);

	if (type == QCA_WLAN_VENDOR_BSS_FILTER_TYPE_BSSID) {
		ret = ath10k_vendor_bssid_filter_action(arvif->ar, addr, type,
							action, arvif->vdev_id);
	} else if (type == QCA_WLAN_VENDOR_BSS_FILTER_TYPE_STA) {
		ret = ath10k_vendor_unassoc_sta_filter_action(arvif->ar, addr,
							      type, action);
	} else {
		ath10k_warn(ar, "invalid BSS filter type %d\n", type);
		ret = -EINVAL;
		goto out;
	}

out:
	mutex_unlock(&ar->conf_mutex);
	return ret;
}

int ath10k_vendor_register(struct ath10k *ar)
{
	int ret;
	int len;
	u8 count;

	/* Initialize other BSS filter, If platform support */
	if (test_bit(WMI_SERVICE_VDEV_FILTER_NEIGHBOR, ar->wmi.svc_map)) {
		count = ATH10K_VENDOR_BSSID_FILTER_COUNT;
		len = count * sizeof(struct ath10k_vendor_bssid_info);

		ar->vendor.bss_filter.bssid = kzalloc(len, GFP_KERNEL);
		if (!ar->vendor.bss_filter.bssid) {
			ret = -ENOMEM;
			goto err;
		}

		ar->vendor.bss_filter.max = count;
		ar->vendor.bss_filter.tbl.entries = 0;
	}

	ar->hw->wiphy->vendor_commands = ath10k_vendor_commands;
	ar->hw->wiphy->n_vendor_commands = ARRAY_SIZE(ath10k_vendor_commands);

	return 0;

err:
	return ret;
}

void ath10k_vendor_unregister(struct ath10k *ar)
{
	if (ar->vendor.bss_filter.bssid) {
		ar->vendor.bss_filter.max = 0;
		kfree(ar->vendor.bss_filter.bssid);
		ar->vendor.bss_filter.bssid = NULL;
	}
	return;
}
