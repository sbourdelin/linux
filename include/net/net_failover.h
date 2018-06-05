/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018, Intel Corporation. */

#ifndef _NET_FAILOVER_H
#define _NET_FAILOVER_H

#include <net/failover.h>

struct net_device *net_failover_create(struct net_device *standby_dev);
void net_failover_destroy(struct net_device *failover_dev);

#endif /* _NET_FAILOVER_H */
