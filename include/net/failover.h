/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018, Intel Corporation. */

#ifndef _FAILOVER_H
#define _FAILOVER_H

#include <linux/netdevice.h>

struct failover_ops {
	int (*slave_register)(struct net_device *slave_dev,
			      struct net_device *failover_dev);
	int (*slave_unregister)(struct net_device *slave_dev,
				struct net_device *failover_dev);
	int (*slave_link_change)(struct net_device *slave_dev,
				 struct net_device *failover_dev);
	int (*slave_name_change)(struct net_device *slave_dev,
				 struct net_device *failover_dev);
};

struct failover {
	struct list_head list;
	struct net_device __rcu *failover_dev;
	struct failover_ops __rcu *ops;
};

struct failover *failover_register(struct net_device *dev,
				   struct failover_ops *ops);
void failover_unregister(struct failover *failover);
int failover_slave_unregister(struct net_device *slave_dev);

#endif /* _FAILOVER_H */
