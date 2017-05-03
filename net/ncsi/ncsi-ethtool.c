/*
 * Copyright Gavin Shan, IBM Corporation 2017.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>

#include <net/ncsi.h>

#include "internal.h"
#include "ncsi-pkt.h"

static int ncsi_get_channels(struct net_device *dev,
			     struct ethtool_ncsi_channels *enc)
{
	struct ncsi_dev *nd;
	struct ncsi_dev_priv *ndp;
	struct ncsi_package *np;
	struct ncsi_channel *nc;
	bool fill_data = !!(enc->nr_channels > 0);
	short nr_channels = 0;
	unsigned long flags;

	nd = ncsi_find_dev(dev);
	if (!nd)
		return -ENXIO;

	ndp = TO_NCSI_DEV_PRIV(nd);
	NCSI_FOR_EACH_PACKAGE(ndp, np) {
		NCSI_FOR_EACH_CHANNEL(np, nc) {
			if (!fill_data) {
				nr_channels++;
				continue;
			}

			enc->id[nr_channels] = NCSI_TO_CHANNEL(np->id, nc->id);
			spin_lock_irqsave(&nc->lock, flags);
			if (nc->state == NCSI_CHANNEL_ACTIVE)
				enc->id[nr_channels] |=
					ETHTOOL_NCSI_CHANNEL_ACTIVE;
			spin_unlock_irqrestore(&nc->lock, flags);
			nr_channels++;
		}
	}

	if (!fill_data)
		enc->nr_channels = nr_channels;

	return 0;
}

static int ncsi_get_channel_info(struct net_device *dev,
				 struct ethtool_ncsi_channel_info *enci)
{
	struct ncsi_dev *nd;
	struct ncsi_dev_priv *ndp;
	struct ncsi_channel *nc;
	unsigned long flags;
	int i;

	nd = ncsi_find_dev(dev);
	if (!nd)
		return -ENXIO;

	ndp = TO_NCSI_DEV_PRIV(nd);
	ncsi_find_package_and_channel(ndp, enci->id, NULL, &nc);
	if (!nc)
		return -ENXIO;

	spin_lock_irqsave(&nc->lock, flags);

	/* NCSI channel's version */
	enci->version = nc->version.version;
	enci->alpha2 = nc->version.alpha2;
	memcpy(enci->fw_name, nc->version.fw_name, 12);
	enci->fw_version = nc->version.fw_version;
	memcpy(enci->pci_ids, nc->version.pci_ids,
	       4 * sizeof(enci->pci_ids[0]));
	enci->mf_id = nc->version.mf_id;

	/* NCSI channel's capabilities */
	enci->cap_generic = (nc->caps[NCSI_CAP_GENERIC].cap &
			     ETHTOOL_NCSI_G_MASK);
	enci->cap_bc = (nc->caps[NCSI_CAP_BC].cap &
			ETHTOOL_NCSI_BC_MASK);
	enci->cap_mc = (nc->caps[NCSI_CAP_MC].cap &
			ETHTOOL_NCSI_MC_MASK);
	enci->cap_buf = nc->caps[NCSI_CAP_BUFFER].cap;
	enci->cap_aen = (nc->caps[NCSI_CAP_AEN].cap &
			 ETHTOOL_NCSI_AEN_MASK);
	enci->cap_vlan = (nc->caps[NCSI_CAP_VLAN].cap &
			  ETHTOOL_NCSI_VLAN_MASK);
	for (i = NCSI_FILTER_BASE; i < NCSI_FILTER_MAX; i++) {
		struct ncsi_channel_filter *ncf;
		unsigned char *p_cap_filter;
		unsigned int *p_valid_bits;
		int entry_size, s_idx, d_idx;
		void *dest;

		switch (i) {
		case NCSI_FILTER_VLAN:
			p_cap_filter = &enci->cap_vlan_filter;
			entry_size = 2;
			p_valid_bits = &enci->vlan_valid_bits;
			dest = enci->vlan;
			d_idx = 0;
			break;
		case NCSI_FILTER_UC:
			p_cap_filter = &enci->cap_uc_filter;
			entry_size = 6;
			p_valid_bits = &enci->mac_valid_bits;
			dest = enci->mac;
			d_idx = 0;
			break;
		case NCSI_FILTER_MC:
			p_cap_filter = &enci->cap_mc_filter;
			entry_size = 6;
			break;
		case NCSI_FILTER_MIXED:
			p_cap_filter = &enci->cap_mixed_filter;
			entry_size = 6;
			break;
		default:
			continue;
		}

		*p_cap_filter = 0;
		ncf = nc->filters[i];
		if (!ncf)
			continue;

		*p_cap_filter = ncf->total;
		s_idx = -1;
		while ((s_idx = find_next_bit((void *)&ncf->bitmap,
					      ncf->total, s_idx + 1))
			< ncf->total) {
			memcpy(dest + (d_idx * entry_size),
			       ((void *)(ncf->data)) + (s_idx * entry_size),
			       entry_size);
			*p_valid_bits |= (1 << d_idx);

			d_idx++;
		}
	}

	/* NCSI channel's settings */
	enci->setting_bc = nc->modes[NCSI_MODE_BC].enable ?
			   nc->modes[NCSI_MODE_BC].data[0] : 0;
	enci->setting_bc &= ETHTOOL_NCSI_BC_MASK;
	enci->setting_mc = nc->modes[NCSI_MODE_MC].enable ?
			   nc->modes[NCSI_MODE_MC].data[0] : 0;
	enci->setting_mc &= ETHTOOL_NCSI_MC_MASK;
	enci->setting_aen = nc->modes[NCSI_MODE_AEN].enable ?
			    nc->modes[NCSI_MODE_AEN].data[0] : 0;
	enci->setting_aen &= ETHTOOL_NCSI_AEN_MASK;
	enci->setting_vlan = nc->modes[NCSI_MODE_VLAN].enable ?
			     nc->modes[NCSI_MODE_VLAN].data[0] : 0;
	enci->setting_vlan &= ETHTOOL_NCSI_VLAN_MASK;

	/* NCSI channel's link status */
	enci->link_status = nc->modes[NCSI_MODE_LINK].data[2];
	enci->link_other_ind = nc->modes[NCSI_MODE_LINK].data[3];
	enci->link_oem = nc->modes[NCSI_MODE_LINK].data[4];

	spin_unlock_irqrestore(&nc->lock, flags);

	return 0;
}

static int ncsi_get_stats(struct net_device *dev,
			  struct ethtool_ncsi_stats *ens)
{
	struct ncsi_dev *nd;
	struct ncsi_dev_priv *ndp;
	struct ncsi_package *np;
	struct ncsi_channel *nc;
	struct ncsi_channel_stats *ncs;
	unsigned long flags;

	nd = ncsi_find_dev(dev);
	if (!nd)
		return -ENXIO;

	ndp = TO_NCSI_DEV_PRIV(nd);
	NCSI_FOR_EACH_PACKAGE(ndp, np) {
		NCSI_FOR_EACH_CHANNEL(np, nc) {
			spin_lock_irqsave(&nc->lock, flags);

			ncs = &nc->stats;
			ens->hnc_cnt_hi += ncs->hnc_cnt_hi;
			ens->hnc_cnt_lo += ncs->hnc_cnt_lo;
			ens->hnc_rx_bytes += ncs->hnc_rx_bytes;
			ens->hnc_tx_bytes += ncs->hnc_tx_bytes;
			ens->hnc_rx_uc_pkts += ncs->hnc_rx_uc_pkts;
			ens->hnc_rx_mc_pkts += ncs->hnc_rx_mc_pkts;
			ens->hnc_rx_bc_pkts += ncs->hnc_rx_bc_pkts;
			ens->hnc_tx_uc_pkts += ncs->hnc_tx_uc_pkts;
			ens->hnc_tx_mc_pkts += ncs->hnc_tx_mc_pkts;
			ens->hnc_tx_bc_pkts += ncs->hnc_tx_bc_pkts;
			ens->hnc_fcs_err += ncs->hnc_fcs_err;
			ens->hnc_align_err += ncs->hnc_align_err;
			ens->hnc_false_carrier += ncs->hnc_false_carrier;
			ens->hnc_runt_pkts += ncs->hnc_runt_pkts;
			ens->hnc_jabber_pkts += ncs->hnc_jabber_pkts;
			ens->hnc_rx_pause_xon += ncs->hnc_rx_pause_xon;
			ens->hnc_rx_pause_xoff += ncs->hnc_rx_pause_xoff;
			ens->hnc_tx_pause_xon += ncs->hnc_tx_pause_xon;
			ens->hnc_tx_pause_xoff += ncs->hnc_tx_pause_xoff;
			ens->hnc_tx_s_collision += ncs->hnc_tx_s_collision;
			ens->hnc_tx_m_collision += ncs->hnc_tx_m_collision;
			ens->hnc_l_collision += ncs->hnc_l_collision;
			ens->hnc_e_collision += ncs->hnc_e_collision;
			ens->hnc_rx_ctl_frames += ncs->hnc_rx_ctl_frames;
			ens->hnc_rx_64_frames += ncs->hnc_rx_64_frames;
			ens->hnc_rx_127_frames += ncs->hnc_rx_127_frames;
			ens->hnc_rx_255_frames += ncs->hnc_rx_255_frames;
			ens->hnc_rx_511_frames += ncs->hnc_rx_511_frames;
			ens->hnc_rx_1023_frames += ncs->hnc_rx_1023_frames;
			ens->hnc_rx_1522_frames += ncs->hnc_rx_1522_frames;
			ens->hnc_rx_9022_frames += ncs->hnc_rx_9022_frames;
			ens->hnc_tx_64_frames += ncs->hnc_tx_64_frames;
			ens->hnc_tx_127_frames += ncs->hnc_tx_127_frames;
			ens->hnc_tx_255_frames += ncs->hnc_tx_255_frames;
			ens->hnc_tx_511_frames += ncs->hnc_tx_511_frames;
			ens->hnc_tx_1023_frames += ncs->hnc_tx_1023_frames;
			ens->hnc_tx_1522_frames += ncs->hnc_tx_1522_frames;
			ens->hnc_tx_9022_frames += ncs->hnc_tx_9022_frames;
			ens->hnc_rx_valid_bytes += ncs->hnc_rx_valid_bytes;
			ens->hnc_rx_runt_pkts += ncs->hnc_rx_runt_pkts;
			ens->hnc_rx_jabber_pkts += ncs->hnc_rx_jabber_pkts;
			ens->ncsi_rx_cmds += ncs->ncsi_rx_cmds;
			ens->ncsi_dropped_cmds += ncs->ncsi_dropped_cmds;
			ens->ncsi_cmd_type_errs += ncs->ncsi_cmd_type_errs;
			ens->ncsi_cmd_csum_errs += ncs->ncsi_cmd_csum_errs;
			ens->ncsi_rx_pkts += ncs->ncsi_rx_pkts;
			ens->ncsi_tx_pkts += ncs->ncsi_tx_pkts;
			ens->ncsi_tx_aen_pkts += ncs->ncsi_tx_aen_pkts;
			ens->pt_tx_pkts += ncs->pt_tx_pkts;
			ens->pt_tx_dropped += ncs->pt_tx_dropped;
			ens->pt_tx_channel_err += ncs->pt_tx_channel_err;
			ens->pt_tx_us_err += ncs->pt_tx_us_err;
			ens->pt_rx_pkts += ncs->pt_rx_pkts;
			ens->pt_rx_dropped += ncs->pt_rx_dropped;
			ens->pt_rx_channel_err += ncs->pt_rx_channel_err;
			ens->pt_rx_us_err += ncs->pt_rx_us_err;
			ens->pt_rx_os_err += ncs->pt_rx_os_err;

			spin_unlock_irqrestore(&nc->lock, flags);
		}
	}

	return 0;
}

#ifdef CONFIG_NET_NCSI_DEBUG
static int ncsi_get_sw_stats(struct net_device *dev,
			     struct ethtool_ncsi_sw_stats *enss)
{
	struct ncsi_dev *nd;
	struct ncsi_dev_priv *ndp;
	unsigned long flags;

	nd = ncsi_find_dev(dev);
	if (!nd)
		return -ENXIO;

	ndp = TO_NCSI_DEV_PRIV(nd);
	spin_lock_irqsave(&ndp->lock, flags);
	memcpy(enss->command, ndp->stats.command,
	       128 * ETHTOOL_NCSI_SW_STAT_MAX * sizeof(enss->command[0][0]));
	memcpy(enss->response, ndp->stats.response,
	       128 * ETHTOOL_NCSI_SW_STAT_MAX * sizeof(enss->response[0][0]));
	memcpy(enss->aen, ndp->stats.aen,
	       256 * ETHTOOL_NCSI_SW_STAT_MAX * sizeof(enss->aen[0][0]));
	spin_unlock_irqrestore(&ndp->lock, flags);

	return 0;
}
#else
static int ncsi_get_sw_stats(struct net_device *dev,
			     struct ethtool_ncsi_sw_stats *enss)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_NET_NCSI_DEBUG */

void ncsi_ethtool_register_dev(struct net_device *dev)
{
	struct ethtool_ops *ops;

	ops = (struct ethtool_ops *)(dev->ethtool_ops);
	if (!ops)
		return;

	ops->get_ncsi_channels = ncsi_get_channels;
	ops->get_ncsi_channel_info = ncsi_get_channel_info;
	ops->get_ncsi_stats = ncsi_get_stats;
	ops->get_ncsi_sw_stats = ncsi_get_sw_stats;
}

void ncsi_ethtool_unregister_dev(struct net_device *dev)
{
	struct ethtool_ops *ops;

	ops = (struct ethtool_ops *)(dev->ethtool_ops);
	if (!ops)
		return;

	ops->get_ncsi_channels = NULL;
	ops->get_ncsi_channel_info = NULL;
	ops->get_ncsi_stats = NULL;
	ops->get_ncsi_sw_stats = NULL;
}
