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

#ifndef _LINUX_INTERCONNECT_CONSUMER_H
#define _LINUX_INTERCONNECT_CONSUMER_H

struct interconnect_node;

/**
 * struct interconnect_path - interconnect path structure
 *
 * @node_list: list of the interconnect nodes
 * @src_dev: source endpoint
 * @dst_dev: destination endpoint
 */
struct interconnect_path {
	struct list_head node_list;
	struct device *src_dev;
	struct device *dst_dev;
};

/**
 * interconnect_get() - get an interconnect path from a given id
 *
 * @dev: the source device which will set constraints on the path
 * @id: endpoint node string identifier
 *
 * This function will search for a path between the source device (caller)
 * and a destination endpoint. It returns an interconnect_path handle on
 * success. Use interconnect_put() to release constraints when the they
 * are not needed anymore.
 *
 * This function returns a handle on success, or ERR_PTR() otherwise.
 */
struct interconnect_path *interconnect_get(struct device *dev, const char *id);

/**
 * interconnect_put() - release the reference to the interconnect path
 *
 * @path: interconnect path
 *
 * Use this function to release the path and free the memory when setting
 * constraints on the path is no longer needed.
 */
void interconnect_put(struct interconnect_path *path);

/**
 * interconnect_set() - set constraints on a path between two endpoints
 * @path: reference to the path returned by interconnect_get()
 * @bandwidth: the requested bandwidth in kpbs between the path endpoints
 *
 * This function sends a request for bandwidth between the two endpoints,
 * (path). It aggragates the requests for constraints and updates each node
 * accordingly.
 *
 * Returns 0 on success, or an approproate error code otherwise.
 */
int interconnect_set(struct interconnect_path *path, u32 bandwidth);

#endif /* _LINUX_INTERCONNECT_CONSUMER_H */
