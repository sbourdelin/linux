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
	NET_POLICY_INVALID	= -1,
	NET_POLICY_NONE		= 0,
	NET_POLICY_CPU,
	NET_POLICY_BULK,
	NET_POLICY_LATENCY,
	NET_POLICY_MAX,

	/*
	 * Mixture of the above policy
	 * Can only be set as global policy.
	 */
	NET_POLICY_MIX,
};

enum netpolicy_traffic {
	NETPOLICY_RX		= 0,
	NETPOLICY_TX,
	NETPOLICY_RXTX,
};

#define POLICY_NAME_LEN_MAX	64
extern const char *policy_name[];

struct netpolicy_dev_info {
	u32	rx_num;
	u32	tx_num;
	u32	*rx_irq;
	u32	*tx_irq;
};

struct netpolicy_sys_map {
	u32	cpu;
	u32	queue;
	u32	irq;
};

struct netpolicy_sys_info {
	/*
	 * Record the cpu and queue 1:1 mapping
	 */
	u32				avail_rx_num;
	struct netpolicy_sys_map	*rx;
	u32				avail_tx_num;
	struct netpolicy_sys_map	*tx;
};

struct netpolicy_object {
	struct list_head	list;
	u32			cpu;
	u32			queue;
	atomic_t		refcnt;
};

struct netpolicy_info {
	enum netpolicy_name	cur_policy;
	unsigned long avail_policy[BITS_TO_LONGS(NET_POLICY_MAX)];
	bool irq_affinity;
	bool has_mix_policy;
	/* cpu and queue mapping information */
	struct netpolicy_sys_info	sys_info;
	/* List of policy objects 0 rx 1 tx */
	struct list_head	obj_list[NETPOLICY_RXTX][NET_POLICY_MAX];
};

struct netpolicy_instance {
	struct net_device	*dev;
	enum netpolicy_name	policy; /* required policy */
	void			*ptr;   /* pointers */
};

/* check if policy is valid */
static inline int is_net_policy_valid(enum netpolicy_name policy)
{
	return ((policy < NET_POLICY_MAX) && (policy > NET_POLICY_INVALID));
}

#ifdef CONFIG_NETPOLICY
extern void update_netpolicy_sys_map(void);
extern int netpolicy_register(struct netpolicy_instance *instance,
			      enum netpolicy_name policy);
extern void netpolicy_unregister(struct netpolicy_instance *instance);
#else
static inline void update_netpolicy_sys_map(void)
{
}

static inline int netpolicy_register(struct netpolicy_instance *instance,
				     enum netpolicy_name policy)
{	return 0;
}

static inline void netpolicy_unregister(struct netpolicy_instance *instance)
{
}

#endif

#endif /*__LINUX_NETPOLICY_H*/
