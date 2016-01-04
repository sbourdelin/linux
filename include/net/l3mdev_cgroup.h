/*
 * l3mdev_cgroup.h		Control Group for L3 Master Device
 *
 * Copyright (c) 2015 Cumulus Networks. All rights reserved.
 * Copyright (c) 2015 David Ahern <dsa@cumulusnetworks.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _L3MDEV_CGROUP_H
#define _L3MDEV_CGROUP_H

#if IS_ENABLED(CONFIG_CGROUP_L3MDEV)

void sock_update_l3mdev(struct sock *sk);

#else /* !CONFIG_CGROUP_L3MDEV */

static inline void sock_update_l3mdev(struct sock *sk)
{
}

#endif /* CONFIG_CGROUP_L3MDEV */
#endif /* _L3MDEV_CGROUP_H */
