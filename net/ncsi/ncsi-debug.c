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
#include <linux/proc_fs.h>
#include <linux/skbuff.h>

#include <net/ncsi.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#include "internal.h"
#include "ncsi-pkt.h"

static struct proc_dir_entry *ncsi_pde;

int ncsi_dev_init_debug(struct ncsi_dev_priv *ndp)
{
	if (WARN_ON_ONCE(ndp->pde))
		return 0;

	if (!ncsi_pde) {
		ncsi_pde = proc_mkdir("ncsi", NULL);
		if (!ncsi_pde) {
			pr_warn("NCSI: Cannot create /proc/ncsi\n");
			return -ENOENT;
		}
	}

	ndp->pde = proc_mkdir(netdev_name(ndp->ndev.dev), ncsi_pde);
	if (!ndp->pde)
		return -ENOMEM;

	return 0;
}

void ncsi_dev_release_debug(struct ncsi_dev_priv *ndp)
{
	if (ndp->pde)
		proc_remove(ndp->pde);
}

int ncsi_package_init_debug(struct ncsi_package *np)
{
	struct ncsi_dev_priv *ndp = np->ndp;
	char name[4];

	if (!ndp->pde)
		return -ENOENT;

	sprintf(name, "p%d", np->id);
	np->pde = proc_mkdir(name, ndp->pde);
	if (!np->pde)
		return -ENOMEM;

	return 0;
}

void ncsi_package_release_debug(struct ncsi_package *np)
{
	if (np->pde)
		proc_remove(np->pde);
}

int ncsi_channel_init_debug(struct ncsi_channel *nc)
{
	struct ncsi_package *np = nc->package;
	char name[3];

	if (!np->pde)
		return -ENOENT;

	sprintf(name, "c%d", nc->id);
	nc->pde = proc_mkdir(name, np->pde);
	if (!nc->pde)
		return -ENOMEM;

	return 0;
}

void ncsi_channel_release_debug(struct ncsi_channel *nc)
{
	if (nc->pde)
		proc_remove(nc->pde);
}
