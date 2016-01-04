/*
 * net/l3mdev/l3mdev_cgroup.c	Control Group for L3 Master Devices
 *
 * Copyright (c) 2015 Cumulus Networks. All rights reserved.
 * Copyright (c) 2015 David Ahern <dsa@cumulusnetworks.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/cgroup.h>
#include <net/sock.h>
#include <net/l3mdev_cgroup.h>

struct l3mdev_cgroup {
	struct cgroup_subsys_state      css;
	struct net			*net;
	int				dev_idx;
};

static inline struct l3mdev_cgroup *css_l3mdev(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct l3mdev_cgroup, css) : NULL;
}

static void l3mdev_set_bound_dev(struct sock *sk)
{
	struct task_struct *tsk = current;
	struct l3mdev_cgroup *l3mdev_cgrp;

	rcu_read_lock();

	l3mdev_cgrp = css_l3mdev(task_css(tsk, l3mdev_cgrp_id));
	if (l3mdev_cgrp && l3mdev_cgrp->dev_idx)
		sk->sk_bound_dev_if = l3mdev_cgrp->dev_idx;

	rcu_read_unlock();
}

void sock_update_l3mdev(struct sock *sk)
{
	switch (sk->sk_family) {
	case AF_INET:
	case AF_INET6:
		l3mdev_set_bound_dev(sk);
		break;
	}
}

static bool is_root_cgroup(struct cgroup_subsys_state *css)
{
	return !css || !css->parent;
}

static struct cgroup_subsys_state *
l3mdev_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct l3mdev_cgroup *l3mdev_cgrp;

	/* nested l3mdev domains are not supportd */
	if (!is_root_cgroup(parent_css))
		return ERR_PTR(-EINVAL);

	l3mdev_cgrp = kzalloc(sizeof(*l3mdev_cgrp), GFP_KERNEL);
	if (!l3mdev_cgrp)
		return ERR_PTR(-ENOMEM);

	return &l3mdev_cgrp->css;
}

static int l3mdev_css_online(struct cgroup_subsys_state *css)
{
	return 0;
}

static void l3mdev_css_free(struct cgroup_subsys_state *css)
{
	kfree(css_l3mdev(css));
}

static int l3mdev_read(struct seq_file *sf, void *v)
{
	struct cgroup_subsys_state *css = seq_css(sf);
	struct l3mdev_cgroup *l3mdev_cgrp = css_l3mdev(css);

	if (!l3mdev_cgrp)
		return -EINVAL;

	if (l3mdev_cgrp->net) {
		struct net_device *dev;

		dev = dev_get_by_index(l3mdev_cgrp->net, l3mdev_cgrp->dev_idx);

		seq_printf(sf, "net[%u]: device index %d ==> %s\n",
			   l3mdev_cgrp->net->ns.inum, l3mdev_cgrp->dev_idx,
			   dev ? dev->name : "<none>");

		if (dev)
			dev_put(dev);
	}
	return 0;
}

static ssize_t l3mdev_write(struct kernfs_open_file *of,
			    char *buf, size_t nbytes, loff_t off)
{
	struct cgroup_subsys_state *css = of_css(of);
	struct l3mdev_cgroup *l3mdev_cgrp = css_l3mdev(css);
	struct net *net = current->nsproxy->net_ns;
	struct net_device *dev;
	char name[IFNAMSIZ];
	int rc = -EINVAL;

	/* once master device is set can not undo. Must delete
	 * cgroup and reset
	 */
	if (l3mdev_cgrp->dev_idx)
		goto out;

	/* root cgroup does not bind to an L3 domain */
	if (is_root_cgroup(css))
		goto out;

	if (sscanf(buf, "%" __stringify(IFNAMSIZ) "s", name) != 1)
		goto out;

	dev = dev_get_by_name(net, name);
	if (!dev) {
		rc = -ENODEV;
		goto out;
	}

	if (netif_is_l3_master(dev)) {
		l3mdev_cgrp->net = net;
		l3mdev_cgrp->dev_idx = dev->ifindex;
		rc = 0;
	}

	dev_put(dev);
out:
	return rc ? : nbytes;
}

/* make master device is set for non-root cgroups before tasks can be added */
static int l3mdev_can_attach(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *dst_css;
	struct task_struct *tsk;
	int rc = 0;

	cgroup_taskset_for_each(tsk, dst_css, tset) {
		struct l3mdev_cgroup *l3mdev_cgrp;

		l3mdev_cgrp = css_l3mdev(dst_css);
		if (!is_root_cgroup(dst_css) && !l3mdev_cgrp->dev_idx) {
			rc = -ENODEV;
			break;
		}
	}

	return rc;
}

static struct cftype ss_files[] = {
	{
		.name     = "master-device",
		.seq_show = l3mdev_read,
		.write    = l3mdev_write,
	},
	{ }	/* terminate */
};

struct cgroup_subsys l3mdev_cgrp_subsys = {
	.css_alloc	= l3mdev_css_alloc,
	.css_online	= l3mdev_css_online,
	.css_free	= l3mdev_css_free,
	.can_attach	= l3mdev_can_attach,
	.legacy_cftypes	= ss_files,
};

static int __init init_cgroup_l3mdev(void)
{
	return 0;
}

subsys_initcall(init_cgroup_l3mdev);
MODULE_AUTHOR("David Ahern");
MODULE_DESCRIPTION("Control Group for L3 Networking Domains");
MODULE_LICENSE("GPL");
