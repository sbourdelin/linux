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

void ncsi_ethtool_register_dev(struct net_device *dev)
{
	struct ethtool_ops *ops;

	ops = (struct ethtool_ops *)(dev->ethtool_ops);
	if (!ops)
		return;

	ops->get_ncsi_channels = ncsi_get_channels;
}

void ncsi_ethtool_unregister_dev(struct net_device *dev)
{
	struct ethtool_ops *ops;

	ops = (struct ethtool_ops *)(dev->ethtool_ops);
	if (!ops)
		return;

	ops->get_ncsi_channels = NULL;
}
