/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018, Intel Corporation. */

#ifndef _FAILOVER_H
#define _FAILOVER_H

#include <linux/netdevice.h>

int netdev_failover_join(struct net_device *lower, struct net_device *upper,
			 rx_handler_func_t *rx_handler);
struct net_device *netdev_failover_upper_get(struct net_device *lower);
void netdev_failover_unjoin(struct net_device *lower,
			    struct net_device *upper);

#endif /* _FAILOVER_H */
