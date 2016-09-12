/*
 * netpolicy.h: Net policy support
 * Copyright (c) 2016, Intel Corporation.
 * Author: Kan Liang (kan.liang@intel.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#ifndef __LINUX_NETPOLICY_H
#define __LINUX_NETPOLICY_H

enum netpolicy_name {
	NET_POLICY_NONE		= 0,
	NET_POLICY_MAX,
};

extern const char *policy_name[];

struct netpolicy_info {
	enum netpolicy_name	cur_policy;
	unsigned long avail_policy[BITS_TO_LONGS(NET_POLICY_MAX)];
};

#endif /*__LINUX_NETPOLICY_H*/
