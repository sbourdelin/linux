/*
 * Copyright (c) 2017, Linaro Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _LINUX_INTERCONNECT_PROVIDER_H
#define _LINUX_INTERCONNECT_PROVIDER_H

#include <linux/interconnect-consumer.h>

/**
 * struct icp_ops - platform specific callback operations for interconnect
 * providers that will be called from drivers
 *
 * @set: set constraints on interconnect
 * @xlate: provider-specific callback for mapping nodes from phandle arguments
 */
struct icp_ops {
	int (*set)(struct interconnect_node *node, u32 bandwidth);
	struct interconnect_node *(*xlate)(struct of_phandle_args *spec, void *data);
};

/**
 * struct icp - interconnect provider (controller) entity that might
 * provide multiple interconnect controls
 *
 * @icp_list: list of the registered interconnect providers
 * @nodes: internal list of the interconnect provider nodes
 * @ops: pointer to device specific struct icp_ops
 * @dev: the device this interconnect provider belongs to
 * @of_node: the corresponding device tree node as phandle target
 * @data: pointer to private data
 */
struct icp {
	struct list_head	icp_list;
	struct list_head	nodes;
	const struct icp_ops	*ops;
	struct device		*dev;
	const char		*name;
	struct device_node	*of_node;
	void			*data;
};

/**
 * struct interconnect_node - entity that is part of the interconnect topology
 *
 * @links: links to other interconnect nodes
 * @num_links: number of interconnect nodes
 * @icp: points to the interconnect provider struct this node belongs to
 * @icn_list: list of interconnect nodes
 * @search_list: list used when walking the nodes graph
 * @reverse: pointer to previous node when walking the nodes graph
 * @is_traversed: flag that is used when walking the nodes graph
 * @qos_list: a list of QoS constraints
 */
struct interconnect_node {
	struct interconnect_node **links;
	size_t			num_links;

	struct icp		*icp;
	struct list_head	icn_list;
	struct list_head	search_list;
	struct interconnect_node *reverse;
	bool			is_traversed;
	struct hlist_head	qos_list;
};

/**
 * struct icn_qos - constraints that are attached to each node
 *
 * @node: linked list node
 * @path: the interconnect path which is using this constraint
 * @bandwidth: an integer describing the bandwidth in kbps
 */
struct icn_qos {
	struct hlist_node node;
	struct interconnect_path *path;
	u32 bandwidth;
};

int interconnect_add_provider(struct icp *icp);
int interconnect_del_provider(struct icp *icp);

#endif /* _LINUX_INTERCONNECT_PROVIDER_H */
