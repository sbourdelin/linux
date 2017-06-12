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

#include <linux/device.h>
#include <linux/init.h>
#include <linux/interconnect-consumer.h>
#include <linux/interconnect-provider.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

static DEFINE_MUTEX(interconnect_provider_list_mutex);
static LIST_HEAD(interconnect_provider_list);

/**
 * struct interconnect_path - interconnect path structure
 * @num_nodes: number of hops (nodes)
 * @reqs: array of the requests applicable to this path of nodes
 */
struct interconnect_path {
	size_t num_nodes;
	struct interconnect_req reqs[0];
};

static struct interconnect_node *node_find(const char *dev_id, int con_id)
{
	struct icp *icp;
	struct interconnect_node *node = ERR_PTR(-EPROBE_DEFER);
	int match, best = 0;

	mutex_lock(&interconnect_provider_list_mutex);

	list_for_each_entry(icp, &interconnect_provider_list, icp_list) {
		struct interconnect_node *n;

		match = 0;

		list_for_each_entry(n, &icp->nodes, icn_list) {
			if (n->dev_id) {
				if (!dev_id || strncmp(n->dev_id, dev_id,
						       strlen(dev_id)))
					continue;
				match += 2;
			}
			if (n->con_id) {
				if (!con_id || n->con_id != con_id)
					continue;
				match += 1;
			}
			if (match > best) {
				node = n;
				if (match == 3)
					goto out;

				best = match;
			}
		}
	}

out:
	mutex_unlock(&interconnect_provider_list_mutex);

	return node;
}

static struct interconnect_path *path_allocate(struct interconnect_node *node,
					       ssize_t num_nodes)
{
	struct interconnect_path *path;
	size_t i;

	path = kzalloc(sizeof(*path) + num_nodes * sizeof(*path->reqs),
		       GFP_KERNEL);
	if (!path)
		return ERR_PTR(-ENOMEM);

	path->num_nodes = num_nodes;

	for (i = 0; i < num_nodes; i++) {
		hlist_add_head(&path->reqs[i].req_node, &node->req_list);

		/* TODO: populate default values */
		path->reqs[i].node = node;
		node = node->reverse;
	}

	return path;
}

static struct interconnect_path *path_find(struct interconnect_node *src,
					   struct interconnect_node *dst)
{
	struct list_head edge_list;
	struct list_head traverse_list;
	struct list_head tmp_list;
	struct interconnect_node *node = NULL;
	size_t i, number = 1;
	bool found = false;

	INIT_LIST_HEAD(&traverse_list);
	INIT_LIST_HEAD(&edge_list);
	INIT_LIST_HEAD(&tmp_list);

	list_add_tail(&src->search_list, &traverse_list);

	do {
		list_for_each_entry(node, &traverse_list, search_list) {
			if (node == dst) {
				found = true;
				list_add(&node->search_list, &tmp_list);
				break;
			}
			for (i = 0; i < node->num_links; i++) {
				struct interconnect_node *tmp = node->links[i];

				if (!tmp) {
					WARN_ON(1);
					return ERR_PTR(-ENOENT);
				}

				if (tmp->is_traversed)
					continue;

				tmp->is_traversed = true;
				tmp->reverse = node;
				list_add_tail(&tmp->search_list, &edge_list);
			}
		}
		if (found)
			break;

		list_splice_init(&traverse_list, &tmp_list);
		list_splice_init(&edge_list, &traverse_list);

		/* count the number of nodes */
		number++;

	} while (!list_empty(&traverse_list));

	/* reset the traversed state */
	list_for_each_entry(node, &tmp_list, search_list) {
		node->is_traversed = false;
	}

	if (found)
		return path_allocate(dst, number);

	return ERR_PTR(-EPROBE_DEFER);
}

static int path_init(struct interconnect_path *path)
{
	struct interconnect_node *node;
	size_t i;

	for (i = 0; i < path->num_nodes; i++) {
		node = path->reqs[i].node;

		mutex_lock(&node->icp->lock);
		node->icp->users++;
		mutex_unlock(&node->icp->lock);
	}

	return 0;
}

static int interconnect_aggregate(struct interconnect_node *node,
				  struct interconnect_creq *creq)
{
	int ret = 0;

	mutex_lock(&node->icp->lock);

	if (node->icp->ops->aggregate) {
		ret = node->icp->ops->aggregate(node, creq);
		if (ret) {
			pr_info("%s: error (%d)\n", __func__, ret);
			goto out;
		}
	} else {
		/* do not aggregate by default */
		struct icp *icp = node->icp;

		icp->creq.avg_bw = creq->avg_bw;
		icp->creq.max_bw = creq->max_bw;
	}

out:
	mutex_unlock(&node->icp->lock);
	return ret;
}

/**
 * interconnect_set() - set constraints on a path between two endpoints
 * @path: reference to the path returned by interconnect_get()
 * @creq: request from the consumer, containing its requirements
 *
 * This function is used by an interconnect consumer to express its own needs
 * in term of bandwidth and QoS for a previously requested path between two
 * endpoints. The requests are aggregated and each node is updated accordingly.
 *
 * Returns 0 on success, or an approproate error code otherwise.
 */
int interconnect_set(struct interconnect_path *path,
		     struct interconnect_creq *creq)
{
	struct interconnect_node *next, *prev = NULL;
	size_t i;
	int ret = 0;

	for (i = 0; i < path->num_nodes; i++, prev = next) {
		next = path->reqs[i].node;

		if (!next || !prev)
			continue;

		if (next->icp != prev->icp)
			continue;

		/* aggregate requests from consumers */
		ret = interconnect_aggregate(next, creq);
		if (ret)
			goto out;

		if (next->icp->ops->set) {
			mutex_lock(&next->icp->lock);
			/* commit the aggregated constraints */
			ret = next->icp->ops->set(prev, next, &next->icp->creq);
			mutex_unlock(&next->icp->lock);
			if (ret)
				goto out;
		}
	}

out:
	return ret;
}

/**
 * interconnect_get() - return a handle for path between two endpoints
 * @sdev: source device identifier
 * @sid: source device port id
 * @ddev: destination device identifier
 * @did: destination device port id
 *
 * This function will search for a path between two endpoints and return an
 * interconnect_path handle on success. Use interconnect_put() to release
 * constraints when the they are not needed anymore.
 *
 * Return: interconnect_path pointer on success, or ERR_PTR() on error
 */
struct interconnect_path *interconnect_get(const char *sdev, const int sid,
					   const char *ddev, const int did)
{
	struct interconnect_node *src, *dst;
	struct interconnect_path *path;
	int ret;

	src = node_find(sdev, sid);
	if (IS_ERR(src))
		return ERR_CAST(src);

	dst = node_find(ddev, did);
	if (IS_ERR(dst))
		return ERR_CAST(dst);

	/* TODO: cache the path */
	path = path_find(src, dst);
	if (IS_ERR(path)) {
		pr_err("error finding path between %p and %p (%ld)\n",
		       src, dst, PTR_ERR(path));
		return path;
	}

	ret = path_init(path);
	if (ret)
		return ERR_PTR(ret);

	return path;
}
EXPORT_SYMBOL_GPL(interconnect_get);

/**
 * interconnect_put() - release the reference to the interconnect_path
 *
 * @path: interconnect path
 *
 * Use this function to release the path and free the memory when setting
 * constraints on the path is no longer needed.
 */
void interconnect_put(struct interconnect_path *path)
{
	struct interconnect_creq creq = { 0, 0 };
	struct interconnect_node *node;
	size_t i;
	int ret;

	if (IS_ERR(path))
		return;

	for (i = 0; i < path->num_nodes; i++) {
		node = path->reqs[i].node;

		/*
		 * Remove the constraints from the path,
		 * update the nodes and free the memory
		 */
		ret = interconnect_set(path, &creq);
		if (ret)
			pr_err("%s error %d\n", __func__, ret);

		node->icp->users--;
	}

	kfree(path);
}
EXPORT_SYMBOL_GPL(interconnect_put);

/**
 * interconnect_add_provider() - add a new interconnect provider
 * @icp: the interconnect provider that will be added into topology
 *
 * Return: 0 on success, or an error code otherwise
 */
int interconnect_add_provider(struct icp *icp)
{
	WARN(!icp->ops->set, "%s: .set is not implemented\n", __func__);

	mutex_lock(&interconnect_provider_list_mutex);
	mutex_init(&icp->lock);
	list_add(&icp->icp_list, &interconnect_provider_list);
	mutex_unlock(&interconnect_provider_list_mutex);

	dev_info(icp->dev, "interconnect provider is added to topology\n");

	return 0;
}
EXPORT_SYMBOL_GPL(interconnect_add_provider);

/**
 * interconnect_del_provider() - delete previously added interconnect provider
 * @icp: the interconnect provider that will be removed from topology
 *
 * Return: 0 on success, or an error code otherwise
 */
int interconnect_del_provider(struct icp *icp)
{
	mutex_lock(&icp->lock);
	if (icp->users) {
		mutex_lock(&icp->lock);
		return -EBUSY;
	}
	mutex_unlock(&icp->lock);

	mutex_lock(&interconnect_provider_list_mutex);
	list_del(&icp->icp_list);
	mutex_unlock(&interconnect_provider_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(interconnect_del_provider);

MODULE_AUTHOR("Georgi Djakov <georgi.djakov@linaro.org");
MODULE_DESCRIPTION("Interconnect Driver Core");
MODULE_LICENSE("GPL v2");
