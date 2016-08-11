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

/* udp statistic type */
enum {
	NETCG_LIMIT_UDP,
	NETCG_USAGE_UDP,
	NETCG_MAXUSAGE_UDP,
	NETCG_FAILCNT_UDP,
	NETCG_UNDERFLOWCNT_UDP,
	NETCG_NUM_UDP_STATS
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

struct cgroup_udp_stats {
	/* Use atomics to protect against multiple writers */
	atomic64_t	udp_limitandusage; /* 32MSB => limit, 32LSB => usage */
	atomic_t	udp_maxusage;
	atomic_t	udp_failcnt;
	atomic_t	udp_underflowcnt;
};

struct net_cgroup {
	struct cgroup_subsys_state	css;

	struct cgroup_udp_stats		udp_stats;

	/* these fields are required for bind/listen port ranges */
	struct mutex			range_lock;
	struct net_range_types		whitelists[NETCG_NUM_RANGE_TYPES];
};

bool net_cgroup_bind_allowed(u16 port);
bool net_cgroup_listen_allowed(u16 port);
bool net_cgroup_acquire_udp_port(void);
void net_cgroup_release_udp_port(void);

#else /* !CONFIG_CGROUP_NET */
static inline bool net_cgroup_bind_allowed(u16 port)
{
	return true;
}
static inline bool net_cgroup_listen_allowed(u16 port)
{
	return true;
}
static inline bool net_cgroup_acquire_udp_port(void)
{
	return true;
}
static inline void net_cgroup_release_udp_port(void)
{
}

#endif /* CONFIG_CGROUP_NET */
#endif  /* _NET_CGROUP_H */
