/*
 * Copyright (c) 2015-2016 Quantenna Communications, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "util.h"

struct qtnf_sta_node *qtnf_sta_list_lookup(struct qtnf_list *list,
					   const u8 *mac)
{
	struct qtnf_sta_node *node;

	if (unlikely(!mac))
		return NULL;

	list_for_each_entry(node, &list->head, list) {
		if (ether_addr_equal(node->mac_addr, mac))
			return node;
	}

	return NULL;
}

struct qtnf_sta_node *qtnf_sta_list_lookup_index(struct qtnf_list *list,
						 size_t index)
{
	struct qtnf_sta_node *node;

	if (qtnf_list_size(list) <= index)
		return NULL;

	list_for_each_entry(node, &list->head, list) {
		if (index-- == 0)
			return node;
	}

	return NULL;
}

struct qtnf_sta_node *qtnf_sta_list_add(struct qtnf_list *list, const u8 *mac)
{
	struct qtnf_sta_node *node;

	if (unlikely(!mac))
		return NULL;

	node = qtnf_sta_list_lookup(list, mac);

	if (node)
		goto done;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (unlikely(!node))
		goto done;

	ether_addr_copy(node->mac_addr, mac);
	list_add_tail(&node->list, &list->head);
	atomic_inc(&list->size);

done:
	return node;
}

int qtnf_sta_list_del(struct qtnf_list *list, const u8 *mac)
{
	struct qtnf_sta_node *node;
	int ret = 0;

	node = qtnf_sta_list_lookup(list, mac);

	if (node) {
		list_del(&node->list);
		atomic_dec(&list->size);
		kfree(node);
		ret = 1;
	}

	return ret;
}

void qtnf_sta_list_free(struct qtnf_list *list)
{
	struct qtnf_sta_node *node, *tmp;

	atomic_set(&list->size, 0);

	list_for_each_entry_safe(node, tmp, &list->head, list) {
		list_del(&node->list);
		kfree(node);
	}

	INIT_LIST_HEAD(&list->head);
}

struct qtnf_vif *qtnf_vlan_list_lookup(struct qtnf_list *list, const u16 vlanid)
{
	struct qtnf_vif *node;

	list_for_each_entry(node, &list->head, u.vlan.list) {
		if (node->u.vlan.vlanid ==  vlanid)
			return node;
	}

	return NULL;
}

struct qtnf_vif *qtnf_vlan_list_add(struct qtnf_list *list, const u16 vlanid)
{
	struct qtnf_vif *node;

	/* don't use existing vlan vif */
	node = qtnf_vlan_list_lookup(list, vlanid);
	if (node)
		return NULL;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (unlikely(!node))
		return NULL;

	list_add_tail(&node->u.vlan.list, &list->head);
	atomic_inc(&list->size);

	return node;
}

int qtnf_vlan_list_del(struct qtnf_list *list, const u16 vlanid)
{
	struct qtnf_vif *node;
	int ret = 0;

	node = qtnf_vlan_list_lookup(list, vlanid);

	if (node) {
		list_del(&node->u.vlan.list);
		atomic_dec(&list->size);
		kfree(node);
		ret = 1;
	}

	return ret;
}
