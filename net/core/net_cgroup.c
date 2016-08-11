/*
 * net/core/net_cgroup.c	Networking Control Group
 *
 * Copyright (C) 2016 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * Authors:	Anoop Naravaram <anaravaram@google.com>
 */

#include <linux/slab.h>
#include <linux/ctype.h>
#include <net/net_cgroup.h>

#define BYTES_PER_ENTRY		sizeof(struct net_range)
#define MAX_WRITE_SIZE		4096

#define MIN_PORT_VALUE		0
#define MAX_PORT_VALUE		65535

/* Deriving MAX_ENTRIES from MAX_WRITE_SIZE as a rough estimate */
#define MAX_ENTRIES ((MAX_WRITE_SIZE - offsetof(struct net_ranges, range)) /   \
		     BYTES_PER_ENTRY)

#define DEFAULT_UDP_LIMIT	-1
#define UDP_FBITS		32
#define UDP_FMASK		(((u64)1 << UDP_FBITS) - 1)

/* Upper 32 bits are 'limit' and lower 32 bits are 'usage' */
static s32 get_udp_limit(u64 limitandusage)
{
	return (s32)(limitandusage >> UDP_FBITS);
}

static s32 get_udp_usage(u64 limitandusage)
{
	return (s32)(limitandusage & UDP_FMASK);
}

static u64 set_udp_usage(u64 limitandusage, s32 usage)
{
	return (u64)((limitandusage & ~UDP_FMASK) | usage);
}

static u64 set_udp_limit_usage(s32 limit, s32 usage)
{
	return (u64)(((u64)limit << UDP_FBITS) | usage);
}

static struct net_cgroup *css_to_net_cgroup(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct net_cgroup, css) : NULL;
}

static struct net_cgroup *task_to_net_cgroup(struct task_struct *p)
{
	return css_to_net_cgroup(task_css(p, net_cgrp_id));
}

static struct net_cgroup *net_cgroup_to_parent(struct net_cgroup *netcg)
{
	return css_to_net_cgroup(netcg->css.parent);
}

static struct net_ranges *alloc_net_ranges(int num_entries)
{
	struct net_ranges *ranges;

	ranges = kmalloc(offsetof(struct net_ranges, range[num_entries]),
			 GFP_KERNEL);
	if (!ranges)
		return NULL;

	ranges->num_entries = num_entries;

	return ranges;
}

static int alloc_init_net_ranges(struct net_range_types *r, int min_value,
				 int max_value)
{
	struct net_ranges *ranges;

	ranges = alloc_net_ranges(1);
	if (!ranges)
		return -ENOMEM;

	ranges->range[0].min_value = min_value;
	ranges->range[0].max_value = max_value;
	r->lower_limit = min_value;
	r->upper_limit = max_value;
	rcu_assign_pointer(r->ranges, ranges);

	return 0;
}

static int alloc_copy_net_ranges(struct net_range_types *r,
				 int min_value,
				 int max_value,
				 struct net_range_types *parent_rt)
{
	struct net_ranges *ranges;
	struct net_ranges *parent_ranges;
	int i; /* loop counter */

	parent_ranges = rcu_dereference(parent_rt->ranges);
	ranges = alloc_net_ranges(parent_ranges->num_entries);
	if (!ranges)
		return -ENOMEM;
	for (i = 0; i < parent_ranges->num_entries; i++) {
		ranges->range[i].min_value = parent_ranges->range[i].min_value;
		ranges->range[i].max_value = parent_ranges->range[i].max_value;
	}

	r->lower_limit = min_value;
	r->upper_limit = max_value;
	rcu_assign_pointer(r->ranges, ranges);

	return 0;
}

static void free_net_cgroup(struct net_cgroup *netcg)
{
	int i;

	mutex_lock(&netcg->range_lock);
	for (i = 0; i < NETCG_NUM_RANGE_TYPES; i++) {
		struct net_ranges *range =
		    rcu_dereference_protected(netcg->whitelists[i].ranges,
					      1);

		if (range)
			kfree_rcu(range, rcu);
	}
	mutex_unlock(&netcg->range_lock);

	kfree(netcg);
}

static struct cgroup_subsys_state *
cgrp_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct net_cgroup *netcg;
	struct net_cgroup *parent_netcg = css_to_net_cgroup(parent_css);
	s32 parent_udp_limit;

	netcg = kzalloc(sizeof(*netcg), GFP_KERNEL);
	if (!netcg)
		return ERR_PTR(-ENOMEM);

	mutex_init(&netcg->range_lock);

	/* allocate the listen and bind range whitelists */
	if (!parent_netcg) {
		/* if root, then init ranges with full range */
		if (alloc_init_net_ranges(
				&netcg->whitelists[NETCG_BIND_RANGES],
				MIN_PORT_VALUE, MAX_PORT_VALUE) ||
		    alloc_init_net_ranges(
				&netcg->whitelists[NETCG_LISTEN_RANGES],
				MIN_PORT_VALUE, MAX_PORT_VALUE)) {
			free_net_cgroup(netcg);
			/* if any of these cause an error, return ENOMEM */
			return ERR_PTR(-ENOMEM);
		}
		/* and set no limit on udp ports */
		atomic64_set(&netcg->udp_stats.udp_limitandusage,
			     set_udp_limit_usage(DEFAULT_UDP_LIMIT, 0));
	} else {
		/* if not root, then, inherit ranges from parent */
		if (alloc_copy_net_ranges(
				&netcg->whitelists[NETCG_BIND_RANGES],
				MIN_PORT_VALUE, MAX_PORT_VALUE,
				&parent_netcg->whitelists[NETCG_BIND_RANGES]) ||
		    alloc_copy_net_ranges(
				&netcg->whitelists[NETCG_LISTEN_RANGES],
				MIN_PORT_VALUE, MAX_PORT_VALUE,
				&parent_netcg->whitelists[NETCG_LISTEN_RANGES])) {
			free_net_cgroup(netcg);
			/* if any of these cause an error, return ENOMEM */
			return ERR_PTR(-ENOMEM);
		}
		/* and inherit udp port limit from parent */
		parent_udp_limit = get_udp_limit(atomic64_read(
			&parent_netcg->udp_stats.udp_limitandusage));
		atomic64_set(&netcg->udp_stats.udp_limitandusage,
			     set_udp_limit_usage(parent_udp_limit, 0));
	}

	return &netcg->css;
}

static void cgrp_css_free(struct cgroup_subsys_state *css)
{
	free_net_cgroup(css_to_net_cgroup(css));
}

static bool value_in_range(struct net_range_types *r, u16 val)
{
	int i;
	struct net_ranges *ranges;

	ranges = rcu_dereference(r->ranges);
	for (i = 0; i < ranges->num_entries; i++) {
		if (val >= ranges->range[i].min_value &&
		    val <= ranges->range[i].max_value)
			return true;
	}

	return false;
}

static bool net_cgroup_value_allowed(u16 value, int type)
{
	struct net_cgroup *netcg;
	bool retval;

	rcu_read_lock();
	netcg = task_to_net_cgroup(current);
	retval = value_in_range(&netcg->whitelists[type], value);
	rcu_read_unlock();
	return retval;
}

bool net_cgroup_bind_allowed(u16 port)
{
	return net_cgroup_value_allowed(port, NETCG_BIND_RANGES);
}
EXPORT_SYMBOL_GPL(net_cgroup_bind_allowed);

bool net_cgroup_listen_allowed(u16 port)
{
	return net_cgroup_value_allowed(port, NETCG_LISTEN_RANGES);
}
EXPORT_SYMBOL_GPL(net_cgroup_listen_allowed);

static s64 net_udp_read_s64(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct  net_cgroup *netcg = css_to_net_cgroup(css);
	s32 value = 0;

	switch (cft->private) {
	case NETCG_LIMIT_UDP:
		value = get_udp_limit(
			atomic64_read(&netcg->udp_stats.udp_limitandusage));
		break;
	case NETCG_USAGE_UDP:
		value = get_udp_usage(
			atomic64_read(&netcg->udp_stats.udp_limitandusage));
		break;
	case NETCG_MAXUSAGE_UDP:
		value = atomic_read(&netcg->udp_stats.udp_maxusage);
		break;
	case NETCG_FAILCNT_UDP:
		value = atomic_read(&netcg->udp_stats.udp_failcnt);
		break;
	case NETCG_UNDERFLOWCNT_UDP:
		value = atomic_read(&netcg->udp_stats.udp_underflowcnt);
		break;
	default:
		value = 0; /* this shouldn't happen */
		break;
	}

	return (s64)value;
}

static int net_udp_write_s64(struct cgroup_subsys_state *css,
			     struct cftype *cft, s64 val)
{
	struct net_cgroup *netcg = css_to_net_cgroup(css);
	s32 oldlimit, usage;
	u64     limitandusage, oldbits, newbits;

	/* Make sure that 'val' is a 32 bit int */
	if (val != (s32)val || val < -1)
		return -EINVAL;

	limitandusage = atomic64_read(&netcg->udp_stats.udp_limitandusage);
	for (;;) {
		oldlimit = get_udp_limit(limitandusage);
		usage = get_udp_usage(limitandusage);
		if (unlikely(oldlimit == (s32)val))
			break;
		newbits = set_udp_limit_usage((s32)val, usage);
		oldbits = atomic64_cmpxchg(&netcg->udp_stats.udp_limitandusage,
					   limitandusage, newbits);
		if (likely(oldbits == limitandusage))
			break;
		limitandusage = oldbits;
	}

	return 0;
}

static bool try_inc_udp_usage(struct net_cgroup *netcg)
{
	s32 limit, usage, oldusage, maxusage;
	u64 limitandusage, newbits, oldbits;

	limitandusage = atomic64_read(&netcg->udp_stats.udp_limitandusage);
	for (;;) {
		usage = get_udp_usage(limitandusage);
		limit = get_udp_limit(limitandusage);
		/* Default indicates no restriction. */
		if ((limit != DEFAULT_UDP_LIMIT) && unlikely(usage >= limit)) {
			atomic_inc(&netcg->udp_stats.udp_failcnt);
			return false;
		}
		/* Increment the usage irrespective of the fact that there is
		 * limit set or not to record the usage.
		 */
		++usage;
		newbits = set_udp_usage(limitandusage, usage);
		oldbits = atomic64_cmpxchg(&netcg->udp_stats.udp_limitandusage,
					   limitandusage, newbits);
		if (likely(oldbits == limitandusage))
			break;
		limitandusage = oldbits;
	}

	maxusage = atomic_read(&netcg->udp_stats.udp_maxusage);

	while (usage > maxusage) {
		oldusage = atomic_cmpxchg(&netcg->udp_stats.udp_maxusage,
					  maxusage, usage);
		if (likely(oldusage == maxusage))
			break;
		maxusage = oldusage;
	}
	return true;
}

static bool try_dec_udp_usage(struct net_cgroup *netcg)
{
	s32 usage;
	u64 limitandusage, newbits, oldbits;

	limitandusage = atomic64_read(&netcg->udp_stats.udp_limitandusage);
	for (;;) {
		usage = get_udp_usage(limitandusage);
		if (unlikely(usage <= 0)) {
			atomic_inc(&netcg->udp_stats.udp_underflowcnt);
			return false;
		}
		--usage;
		newbits = set_udp_usage(limitandusage, usage);
		oldbits = atomic64_cmpxchg(&netcg->udp_stats.udp_limitandusage,
					   limitandusage, newbits);
		if (likely(oldbits == limitandusage))
			break;
		limitandusage = oldbits;
	}

	return true;
}

/* The feature exposes following values through the cgroup interface:
 * (1) udp_limit: Maximum number of UDP ports that processes from this
 *                container can use.
 * (2) udp_usage: Current usage of UDP ports by processes in this container.
 * (3) udp_maxusage: The peak usage of UDP ports since container creation.
 * (4) udp_failcnt: Number of port allocation requests failed because of
 *                  ports depletion for this container.
 * (5) udp_underflowcnt: Number of port release requests that would have
 *                       pushed the usage below zero (see description below).
 *
 * Caveats:
 *    If a process is moved to a different container, the udp sockets are not
 * accounted for that process in the destination container. When that process
 * finishes; that much credit will not be returned to the source container.
 * This will create some sort of discrepancy at both source and destination
 * containers. This transfer would create a pseudo-permanent transfer of port
 * credits to the destination container. The trasfer effect will be nullified
 * only if all the processes in this destination container stop using the udp
 * ports momentarily and total usage drops to ZERO (since we do not allow the
 * usage count to go negative, the pseudo-permanent transfer gets nullified
 * at the destination container). It's assumed that this kind of process
 * migration is minimal.
 *    The limitandusage (64 bit) field holds limit in upper 32 bits and current
 * usage in lower 32 bits. Since this is one field, the check and update atomic
 * operations eliminate possible mismatch on multi-core systems.
 */
bool net_cgroup_acquire_udp_port(void)
{
	struct net_cgroup *netcg;
	struct net_cgroup *curr;
	struct net_cgroup *curr2;
	bool success = true;

	rcu_read_lock();
	netcg = task_to_net_cgroup(current);

	/* iterate this net_cgroup and its ancestors, attempting increment the
	 * usage at each step
	 */
	for (curr = netcg;
	     net_cgroup_to_parent(curr);
	     curr = net_cgroup_to_parent(curr)) {
		if (!try_inc_udp_usage(curr)) {
			/* get out if any one ancestor fails */
			success = false;
			break;
		}
	}

	if (!success) {
		/* one of the ancestors failed to increment its usage. now, we
		 * need to undo all the increments we did
		 */
		for (curr2 = netcg;
		     curr2 != curr;
		     curr2 = net_cgroup_to_parent(curr2)) {
			try_dec_udp_usage(curr2);
		}
	}

	rcu_read_unlock();
	return success;
}
EXPORT_SYMBOL_GPL(net_cgroup_acquire_udp_port);

void net_cgroup_release_udp_port(void)
{
	struct net_cgroup *netcg;
	struct net_cgroup *curr;

	rcu_read_lock();
	netcg = task_to_net_cgroup(current);

	/* iterate this net_cgroup and its ancestors, attempting decrement the
	 * usage at each step
	 */
	for (curr = netcg;
	     net_cgroup_to_parent(curr);
	     curr = net_cgroup_to_parent(curr)) {
		try_dec_udp_usage(curr);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(net_cgroup_release_udp_port);

/* Returns true if the range r is a subset of at least one of the ranges in
 * rs, and returns false otherwise.
 */
static bool range_in_ranges(struct net_range *r, struct net_ranges *rs)
{
	int ri;

	for (ri = 0; ri < rs->num_entries; ri++)
		if (r->min_value >= rs->range[ri].min_value &&
		    r->max_value <= rs->range[ri].max_value)
			return true;

	return false;
}

/* Returns true if all the ranges in rs1 are subsets of at least one of the
 * ranges in rs2, ans returns false otherwise.
 */
static bool ranges_in_ranges(struct net_ranges *rs1, struct net_ranges *rs2)
{
	int ri;

	for (ri = 0; ri < rs1->num_entries; ri++)
		if (!range_in_ranges(&rs1->range[ri], rs2))
			return false;

	return true;
}

static ssize_t update_ranges(struct net_cgroup *netcg, int type,
			     const char *bp)
{
	unsigned int a, b;
	int curr_index = 0;
	ssize_t retval = 0;
	struct net_ranges *ranges, *new, *old, *parent_ranges, *child_ranges;
	struct cgroup_subsys_state *child_pos;
	struct net_cgroup *child_netcg;

	ranges = alloc_net_ranges(MAX_ENTRIES);
	if (!ranges)
		return -ENOMEM;

	while (*bp != '\0' && *bp != '\n' && curr_index < MAX_ENTRIES) {
		if (!isdigit(*bp)) {
			retval = -EINVAL;
			goto out;
		}

		a = simple_strtoul(bp, (char **)&bp, 10);
		b = a;
		if (*bp == '-') {
			bp++;
			if (!isdigit(*bp)) {
				retval = -EINVAL;
				goto out;
			}
			b = simple_strtoul(bp, (char **)&bp, 10);
		}

		if (!(a <= b)) {
			retval = -EINVAL;
			goto out;
		}

		if (a < netcg->whitelists[type].lower_limit ||
		    b > netcg->whitelists[type].upper_limit) {
			retval = -EINVAL;
			goto out;
		}

		ranges->range[curr_index].min_value = a;
		ranges->range[curr_index].max_value = b;

		if (*bp == ',')
			bp++;

		curr_index++;
	}

	if (curr_index == MAX_ENTRIES) {
		retval = -E2BIG;
		goto out;
	}

	new = alloc_net_ranges(curr_index);
	if (!new) {
		retval = -ENOMEM;
		goto out;
	}

	memcpy(new->range, ranges->range,
	       sizeof(struct net_range) * curr_index);

	/* make sure this cgroup is still a subset of its parent's */
	parent_ranges = rcu_dereference(
			net_cgroup_to_parent(netcg)->whitelists[type].ranges);
	if (!ranges_in_ranges(new, parent_ranges)) {
		retval = -EINVAL;
		goto out;
	}

	/* make sure children's ranges are still subsets of this cgroup's */
	css_for_each_child(child_pos, &netcg->css) {
		child_netcg = css_to_net_cgroup(child_pos);
		child_ranges = rcu_dereference(
				child_netcg->whitelists[type].ranges);
		if (!ranges_in_ranges(child_ranges, new)) {
			retval = -EINVAL;
			goto out;
		}
	}

	mutex_lock(&netcg->range_lock);
	old = rcu_dereference_protected(netcg->whitelists[type].ranges, 1);
	rcu_assign_pointer(netcg->whitelists[type].ranges, new);
	mutex_unlock(&netcg->range_lock);

	kfree_rcu(old, rcu);
out:
	kfree(ranges);
	return retval;
}

static ssize_t net_write_ranges(struct kernfs_open_file *of,
				char *buf, size_t nbytes, loff_t off)
{
	struct net_cgroup *netcg = css_to_net_cgroup(of_css(of));
	int type = of_cft(of)->private;

	return update_ranges(netcg, type, buf) ?: nbytes;
}

static void net_seq_printf_list(struct seq_file *s, struct net_range_types *r)
{
	int i;
	struct net_ranges *ranges;

	ranges = rcu_dereference(r->ranges);

	for (i = 0; i < ranges->num_entries; i++) {
		if (i)
			seq_puts(s, ",");
		seq_printf(s, "%d-%d", ranges->range[i].min_value,
			   ranges->range[i].max_value);
	}
	seq_puts(s, "\n");
}

static int net_read_ranges(struct seq_file *sf, void *v)
{
	struct net_cgroup *netcg = css_to_net_cgroup(seq_css(sf));
	int type = seq_cft(sf)->private;

	rcu_read_lock();
	net_seq_printf_list(sf, &netcg->whitelists[type]);
	rcu_read_unlock();

	return 0;
}

static struct cftype ss_files[] = {
	{
		.name		= "listen_port_ranges",
		.flags		= CFTYPE_ONLY_ON_ROOT,
		.seq_show	= net_read_ranges,
		.private	= NETCG_LISTEN_RANGES,
	},
	{
		.name		= "listen_port_ranges",
		.flags		= CFTYPE_NOT_ON_ROOT,
		.seq_show	= net_read_ranges,
		.write		= net_write_ranges,
		.private	= NETCG_LISTEN_RANGES,
		.max_write_len	= MAX_WRITE_SIZE,
	},
	{
		.name		= "bind_port_ranges",
		.flags		= CFTYPE_ONLY_ON_ROOT,
		.seq_show	= net_read_ranges,
		.private	= NETCG_BIND_RANGES,
	},
	{
		.name		= "bind_port_ranges",
		.flags		= CFTYPE_NOT_ON_ROOT,
		.seq_show	= net_read_ranges,
		.write		= net_write_ranges,
		.private	= NETCG_BIND_RANGES,
		.max_write_len	= MAX_WRITE_SIZE,
	},
	{
		.name		= "udp_limit",
		.flags		= CFTYPE_ONLY_ON_ROOT,
		.read_s64	= net_udp_read_s64,
		.private	= NETCG_LIMIT_UDP,
	},
	{
		.name		= "udp_limit",
		.flags		= CFTYPE_NOT_ON_ROOT,
		.read_s64	= net_udp_read_s64,
		.write_s64	= net_udp_write_s64,
		.private	= NETCG_LIMIT_UDP,
	},
	{
		.name		= "udp_usage",
		.read_s64	= net_udp_read_s64,
		.private	= NETCG_USAGE_UDP,
	},
	{
		.name		= "udp_maxusage",
		.read_s64	= net_udp_read_s64,
		.private	= NETCG_MAXUSAGE_UDP,
	},
	{
		.name		= "udp_failcnt",
		.read_s64	= net_udp_read_s64,
		.private	= NETCG_FAILCNT_UDP,
	},
	{
		.name		= "udp_underflowcnt",
		.read_s64	= net_udp_read_s64,
		.private	= NETCG_UNDERFLOWCNT_UDP,
	},
	{ }	/* terminate */
};

struct cgroup_subsys net_cgrp_subsys = {
	.css_alloc		= cgrp_css_alloc,
	.css_free		= cgrp_css_free,
	.legacy_cftypes		= ss_files,
};
