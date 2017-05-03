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

void ncsi_ethtool_register_dev(struct net_device *dev)
{
	struct ethtool_ops *ops;

	ops = (struct ethtool_ops *)(dev->ethtool_ops);
	if (!ops)
		return;

	ops->get_ncsi_channels = ncsi_get_channels;
	ops->get_ncsi_channel_info = ncsi_get_channel_info;
}

void ncsi_ethtool_unregister_dev(struct net_device *dev)
{
	struct ethtool_ops *ops;

	ops = (struct ethtool_ops *)(dev->ethtool_ops);
	if (!ops)
		return;

	ops->get_ncsi_channels = NULL;
	ops->get_ncsi_channel_info = NULL;
}
