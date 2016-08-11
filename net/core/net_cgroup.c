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
#include <net/net_cgroup.h>

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

static void free_net_cgroup(struct net_cgroup *netcg)
{
	kfree(netcg);
}

static struct cgroup_subsys_state *
cgrp_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct net_cgroup *netcg;

	netcg = kzalloc(sizeof(*netcg), GFP_KERNEL);
	if (!netcg)
		return ERR_PTR(-ENOMEM);

	return &netcg->css;
}

static void cgrp_css_free(struct cgroup_subsys_state *css)
{
	free_net_cgroup(css_to_net_cgroup(css));
}

static struct cftype ss_files[] = {
	{ }	/* terminate */
};

struct cgroup_subsys net_cgrp_subsys = {
	.css_alloc		= cgrp_css_alloc,
	.css_free		= cgrp_css_free,
	.legacy_cftypes		= ss_files,
};
