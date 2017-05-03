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
#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <linux/debugfs.h>
#include <linux/skbuff.h>

#include <net/ncsi.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include "internal.h"
#include "ncsi-pkt.h"

void ncsi_dev_update_stats(struct ncsi_dev_priv *ndp,
			   int type, int subtype, int errno)
{
	unsigned long flags;

	if (errno >= ETHTOOL_NCSI_SW_STAT_MAX)
		return;

	spin_lock_irqsave(&ndp->lock, flags);

	if (type == NCSI_PKT_AEN) {
		if (subtype < 256)
			ndp->stats.aen[subtype][errno]++;
	} else if (type < 128) {
		ndp->stats.command[type][errno]++;
	} else if (type < 256) {
		ndp->stats.response[type - 128][errno]++;
	}

	spin_unlock_irqrestore(&ndp->lock, flags);
}
