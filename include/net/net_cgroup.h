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

#ifdef CONFIG_CGROUP_NET

struct net_cgroup {
	struct cgroup_subsys_state	css;
};

#endif /* CONFIG_CGROUP_NET */
#endif  /* _NET_CGROUP_H */
