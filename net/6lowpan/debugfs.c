/* This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Authors:
 * (C) 2015 Pengutronix, Alexander Aring <aar@pengutronix.de>
 * Copyright (c)  2015 Nordic Semiconductor. All Rights Reserved.
 */

#include <linux/debugfs.h>

#include <net/6lowpan.h>

#include "6lowpan_i.h"

static struct dentry *lowpan_debugfs;

int lowpan_dev_debugfs_init(struct net_device *dev)
{
	static struct dentry *iface;

	/* creating the root */
	iface = debugfs_create_dir(dev->name, lowpan_debugfs);
	if (!iface)
		goto fail;

	return 0;

fail:
	lowpan_debugfs_exit();
	return -EINVAL;
}

int __init lowpan_debugfs_init(void)
{
	lowpan_debugfs = debugfs_create_dir("6lowpan", NULL);
	if (!lowpan_debugfs)
		return -EINVAL;

	return 0;
}

void lowpan_debugfs_exit(void)
{
	debugfs_remove_recursive(lowpan_debugfs);
}
