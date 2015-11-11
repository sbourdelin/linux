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
 */

#include <linux/module.h>

#include <net/6lowpan.h>

#include "6lowpan_i.h"

int lowpan_netdev_setup(struct net_device *dev, enum lowpan_lltypes lltype)
{
	dev->addr_len = EUI64_ADDR_LEN;
	dev->type = ARPHRD_6LOWPAN;
	dev->mtu = IPV6_MIN_MTU;
	dev->priv_flags |= IFF_NO_QUEUE;

	lowpan_priv(dev)->lltype = lltype;

	return lowpan_dev_debugfs_init(dev);
}
EXPORT_SYMBOL(lowpan_netdev_setup);

static int __init lowpan_module_init(void)
{
	int ret;

	ret = lowpan_debugfs_init();
	if (ret < 0)
		return ret;

	request_module_nowait("ipv6");

	request_module_nowait("nhc_dest");
	request_module_nowait("nhc_fragment");
	request_module_nowait("nhc_hop");
	request_module_nowait("nhc_ipv6");
	request_module_nowait("nhc_mobility");
	request_module_nowait("nhc_routing");
	request_module_nowait("nhc_udp");

	return 0;
}

static void __exit lowpan_module_exit(void)
{
	lowpan_debugfs_exit();
}

module_init(lowpan_module_init);
module_exit(lowpan_module_exit);

MODULE_LICENSE("GPL");
