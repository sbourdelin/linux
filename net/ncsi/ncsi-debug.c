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

static struct dentry *ncsi_dentry;

int ncsi_dev_init_debug(struct ncsi_dev_priv *ndp)
{
	if (WARN_ON_ONCE(ndp->dentry))
		return 0;

	if (!ncsi_dentry) {
		ncsi_dentry = debugfs_create_dir("ncsi", NULL);
		if (!ncsi_dentry) {
			pr_warn("NCSI: Cannot create /sys/kernel/debug/ncsi\n");
			return -ENOENT;
		}
	}

	ndp->dentry = debugfs_create_dir(netdev_name(ndp->ndev.dev),
					 ncsi_dentry);
	if (!ndp->dentry)
		return -ENOMEM;

	return 0;
}

void ncsi_dev_release_debug(struct ncsi_dev_priv *ndp)
{
	debugfs_remove(ndp->dentry);
}

int ncsi_package_init_debug(struct ncsi_package *np)
{
	struct ncsi_dev_priv *ndp = np->ndp;
	char name[4];

	if (!ndp->dentry)
		return -ENOENT;

	sprintf(name, "p%d", np->id);
	np->dentry = debugfs_create_dir(name, ndp->dentry);
	if (!np->dentry)
		return -ENOMEM;

	return 0;
}

void ncsi_package_release_debug(struct ncsi_package *np)
{
	debugfs_remove(np->dentry);
}

int ncsi_channel_init_debug(struct ncsi_channel *nc)
{
	struct ncsi_package *np = nc->package;
	char name[3];

	if (!np->dentry)
		return -ENOENT;

	sprintf(name, "c%d", nc->id);
	nc->dentry = debugfs_create_dir(name, np->dentry);
	if (!nc->dentry)
		return -ENOMEM;

	return 0;
}

void ncsi_channel_release_debug(struct ncsi_channel *nc)
{
	debugfs_remove(nc->dentry);
}
