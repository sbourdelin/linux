// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2018, Intel Corporation. */

/* A library for managing chained upper/oower devices such as
 * drivers to enable accelerated datapath and support VF live migration.
 */

#include <linux/module.h>
#include <linux/etherdevice.h>
#include <uapi/linux/if_arp.h>
#include <linux/rtnetlink.h>
#include <linux/if_vlan.h>
#include <net/failover.h>

/* failover_join - Join an lower netdev with an upper device. */
int netdev_failover_join(struct net_device *lower_dev,
			 struct net_device *upper_dev,
			 rx_handler_func_t *rx_handler)
{
	int err;

	ASSERT_RTNL();

	/* Don't allow joining devices of different protocols */
	if (upper_dev->type != lower_dev->type)
		return -EINVAL;

	err = netdev_rx_handler_register(lower_dev, rx_handler, upper_dev);
	if (err) {
		netdev_err(lower_dev,
			   "can not register failover rx handler (err = %d)\n",
			   err);
		return err;
	}

	err = netdev_master_upper_dev_link(lower_dev, upper_dev, NULL,
					   NULL, NULL);
	if (err) {
		netdev_err(lower_dev,
			   "can not set failover device %s (err = %d)\n",
			   upper_dev->name, err);
		netdev_rx_handler_unregister(lower_dev);
		return err;
	}

	dev_hold(lower_dev);
	lower_dev->priv_flags |= IFF_FAILOVER_SLAVE;
	return 0;
}
EXPORT_SYMBOL_GPL(netdev_failover_join);

/* Find upper network device for failover slave device */
struct net_device *netdev_failover_upper_get(struct net_device *lower_dev)
{
	if (!netif_is_failover_slave(lower_dev))
		return NULL;

	return netdev_master_upper_dev_get(lower_dev);
}
EXPORT_SYMBOL_GPL(netdev_failover_upper_get);

/* failover_unjoin - Break connection between lower and upper device. */
void netdev_failover_unjoin(struct net_device *lower_dev,
			    struct net_device *upper_dev)
{
	ASSERT_RTNL();

	netdev_rx_handler_unregister(lower_dev);
	netdev_upper_dev_unlink(lower_dev, upper_dev);
	dev_put(lower_dev);
	lower_dev->priv_flags &= ~IFF_FAILOVER_SLAVE;
}
EXPORT_SYMBOL_GPL(netdev_failover_unjoin);
