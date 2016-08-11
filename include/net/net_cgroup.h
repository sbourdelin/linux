/*
 * net_cgroup.h			Networking Control Group
 *
 * Copyright (C) 2016 Google, Inc.
 *
 * Authors:	Anoop Naravaram <anaravaram@google.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#ifndef _NET_CGROUP_H
#define _NET_CGROUP_H

#include <linux/cgroup.h>
#include <linux/types.h>

#ifdef CONFIG_CGROUP_NET
/* range type */
enum {
	NETCG_LISTEN_RANGES,
	NETCG_BIND_RANGES,
	NETCG_NUM_RANGE_TYPES
};

struct net_range {
	u16 min_value;
	u16 max_value;
};

struct net_ranges {
	int			num_entries;
	struct rcu_head		rcu;
	struct net_range	range[0];
};

struct net_range_types {
	struct net_ranges __rcu	*ranges;
	u16			lower_limit;
	u16			upper_limit;
};

struct net_cgroup {
	struct cgroup_subsys_state	css;

	/* these fields are required for bind/listen port ranges */
	struct mutex			range_lock;
	struct net_range_types		whitelists[NETCG_NUM_RANGE_TYPES];
};

bool net_cgroup_bind_allowed(u16 port);
bool net_cgroup_listen_allowed(u16 port);

#else /* !CONFIG_CGROUP_NET */
static inline bool net_cgroup_bind_allowed(u16 port)
{
	return true;
}
static inline bool net_cgroup_listen_allowed(u16 port)
{
	return true;
}

#endif /* CONFIG_CGROUP_NET */
#endif  /* _NET_CGROUP_H */
