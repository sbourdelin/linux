/*
 * net/dsa/dsa.c - Hardware switch handling
 * Copyright (c) 2008-2009 Marvell Semiconductor
 * Copyright (c) 2013 Florian Fainelli <florian@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <net/dsa.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_platform.h>
#include <linux/of_net.h>
#include <linux/of_gpio.h>
#include <linux/sysfs.h>
#include <linux/phy_fixed.h>
#include <linux/gpio/consumer.h>
#include "dsa_priv.h"

char dsa_driver_version[] = "0.1";

static struct sk_buff *dsa_slave_notag_xmit(struct sk_buff *skb,
					    struct net_device *dev)
{
	/* Just return the original SKB */
	return skb;
}

static const struct dsa_device_ops none_ops = {
	.xmit	= dsa_slave_notag_xmit,
	.rcv	= NULL,
};

const struct dsa_device_ops *dsa_device_ops[DSA_TAG_LAST] = {
#ifdef CONFIG_NET_DSA_TAG_DSA
	[DSA_TAG_PROTO_DSA] = &dsa_netdev_ops,
#endif
#ifdef CONFIG_NET_DSA_TAG_EDSA
	[DSA_TAG_PROTO_EDSA] = &edsa_netdev_ops,
#endif
#ifdef CONFIG_NET_DSA_TAG_TRAILER
	[DSA_TAG_PROTO_TRAILER] = &trailer_netdev_ops,
#endif
#ifdef CONFIG_NET_DSA_TAG_BRCM
	[DSA_TAG_PROTO_BRCM] = &brcm_netdev_ops,
#endif
	[DSA_TAG_PROTO_NONE] = &none_ops,
};

int dsa_cpu_dsa_setup(struct dsa_switch *ds, struct device *dev,
		      struct device_node *port_dn, int port)
{
	struct phy_device *phydev;
	int ret, mode;

	if (of_phy_is_fixed_link(port_dn)) {
		ret = of_phy_register_fixed_link(port_dn);
		if (ret) {
			dev_err(dev, "failed to register fixed PHY\n");
			return ret;
		}
		phydev = of_phy_find_device(port_dn);

		mode = of_get_phy_mode(port_dn);
		if (mode < 0)
			mode = PHY_INTERFACE_MODE_NA;
		phydev->interface = mode;

		genphy_config_init(phydev);
		genphy_read_status(phydev);
		if (ds->drv->adjust_link)
			ds->drv->adjust_link(ds, port, phydev);
	}

	return 0;
}

const struct dsa_device_ops *dsa_resolve_tag_protocol(int tag_protocol)
{
	const struct dsa_device_ops *ops;

	if (tag_protocol >= DSA_TAG_LAST)
		return ERR_PTR(-EINVAL);
	ops = dsa_device_ops[tag_protocol];

	if (!ops)
		return ERR_PTR(-ENOPROTOOPT);

	return ops;
}

int dsa_cpu_port_ethtool_setup(struct dsa_switch *ds)
{
	struct net_device *master;
	struct ethtool_ops *cpu_ops;

	master = ds->dst->master_netdev;
	if (ds->master_netdev)
		master = ds->master_netdev;

	cpu_ops = devm_kzalloc(ds->dev, sizeof(*cpu_ops), GFP_KERNEL);
	if (!cpu_ops)
		return -ENOMEM;

	memcpy(&ds->dst->master_ethtool_ops, master->ethtool_ops,
	       sizeof(struct ethtool_ops));
	ds->dst->master_orig_ethtool_ops = master->ethtool_ops;
	memcpy(cpu_ops, &ds->dst->master_ethtool_ops,
	       sizeof(struct ethtool_ops));
	dsa_cpu_port_ethtool_init(cpu_ops);
	master->ethtool_ops = cpu_ops;

	return 0;
}

void dsa_cpu_port_ethtool_restore(struct dsa_switch *ds)
{
	struct net_device *master;

	master = ds->dst->master_netdev;
	if (ds->master_netdev)
		master = ds->master_netdev;

	master->ethtool_ops = ds->dst->master_orig_ethtool_ops;
}

void dsa_cpu_dsa_destroy(struct device_node *port_dn)
{
	struct phy_device *phydev;

	if (of_phy_is_fixed_link(port_dn)) {
		phydev = of_phy_find_device(port_dn);
		if (phydev) {
			phy_device_free(phydev);
			fixed_phy_unregister(phydev);
		}
	}
}
