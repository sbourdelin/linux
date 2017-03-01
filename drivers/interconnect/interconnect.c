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
#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/interconnect-consumer.h>
#include <linux/interconnect-provider.h>
#include <linux/property.h>
#include <linux/slab.h>

static DEFINE_MUTEX(interconnect_provider_list_mutex);
static LIST_HEAD(interconnect_provider_list);

static struct interconnect_node *find_node(struct device_node *np)
{
	struct interconnect_node *node = ERR_PTR(-EPROBE_DEFER);
	int ret;
	struct of_phandle_args args;
	struct icp *i, *icp = NULL;

	/* find the target interconnect provider device_node */
	ret = of_parse_phandle_with_args(np, "interconnect-port",
					 "#interconnect-cells", 0, &args);
	if (ret) {
		pr_err("%s interconnect provider not found (%d)\n", __func__,
		       ret);
		return ERR_PTR(ret);
	}

	mutex_lock(&interconnect_provider_list_mutex);

	/* find the interconnect provider of the target node */
	list_for_each_entry(i, &interconnect_provider_list, icp_list) {
		if (args.np == i->of_node) {
			icp = i;
			node = icp->ops->xlate(&args, icp->data);
			break;
		}
	}

	mutex_unlock(&interconnect_provider_list_mutex);

	of_node_put(args.np);

	if (!icp) {
		pr_err("%s interconnect provider %s not found\n", __func__,
		       args.np->name);
		return ERR_PTR(-EPROBE_DEFER);
	}

	if (IS_ERR(node)) {
		pr_err("%s interconnect node %s not found (%ld)\n", __func__,
		       args.np->name, PTR_ERR(node));
		return node;
	}

	return node;
}

static int find_path(struct interconnect_node *src,
		     struct interconnect_node *dst,
		     struct interconnect_path *path)
{
	struct list_head edge_list;
	struct list_head traverse_list;
	struct interconnect_path *tmp_path;
	struct interconnect_node *node = NULL;
	size_t i;
	bool found = false;

	INIT_LIST_HEAD(&traverse_list);
	INIT_LIST_HEAD(&edge_list);

	tmp_path = kzalloc(sizeof(*tmp_path), GFP_KERNEL);
	if (!tmp_path)
		return -ENOMEM;

	INIT_LIST_HEAD(&tmp_path->node_list);

	list_add_tail(&src->search_list, &traverse_list);

	do {
		list_for_each_entry(node, &traverse_list, search_list) {
			if (node == dst) {
				found = true;
				list_add(&node->search_list,
					 &tmp_path->node_list);
				break;
			}
			for (i = 0; i < node->num_links; i++) {
				struct interconnect_node *tmp = node->links[i];

				/* try DT lookup */
				if (!tmp) {
					tmp = find_node(node->icp->of_node);
					if (IS_ERR(tmp))
						return PTR_ERR(tmp);
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

		list_splice_init(&traverse_list, &tmp_path->node_list);
		list_splice_init(&edge_list, &traverse_list);

	} while (!list_empty(&traverse_list));

	/* reset the is_traversed state */
	list_for_each_entry(node, &path->node_list, search_list) {
		node->is_traversed = false;
	}

	/* add the path */
	node = list_first_entry(&tmp_path->node_list, struct interconnect_node,
				search_list);
	while (node) {
		list_add_tail(&node->search_list, &path->node_list);
		node = node->reverse;
	}

	kfree(tmp_path);

	return 0;
}

int interconnect_set(struct interconnect_path *path, u32 bandwidth)
{
	struct interconnect_node *node;

	list_for_each_entry(node, &path->node_list, search_list) {
		if (node->icp->ops->set)
			node->icp->ops->set(node, bandwidth);
	}

	return 0;
}

struct interconnect_path *interconnect_get(struct device *dev, const char *id)
{
	struct device_node *np;
	struct platform_device *dst_pdev;
	struct interconnect_node *src, *dst, *node;
	struct interconnect_path *path;
	int ret, index;

	if (WARN_ON(!dev || !id))
		return ERR_PTR(-EINVAL);

	src = find_node(dev->of_node);
	if (IS_ERR(src))
		return ERR_CAST(src);

	index = of_property_match_string(dev->of_node,
					 "interconnect-path-names", id);
	if (index < 0) {
		dev_err(dev, "missing interconnect-path-names DT property on %s\n",
			dev->of_node->full_name);
		return ERR_PTR(index);
	}

	/* get the destination endpoint device_node */
	np = of_parse_phandle(dev->of_node, "interconnect-path", index);

	dst_pdev = of_find_device_by_node(np);
	if (!dst_pdev) {
		dev_err(dev, "error finding device by node %s\n", np->name);
		return ERR_PTR(-ENODEV);
	}

	dst = find_node(np);
	if (IS_ERR(dst))
		return ERR_CAST(dst);

	/* find a path between the source and destination */
	path = kzalloc(sizeof(*path), GFP_KERNEL);
	if (!path)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&path->node_list);
	path->src_dev = dev;
	path->dst_dev = &dst_pdev->dev;

	/* TODO: cache the path */
	ret = find_path(src, dst, path);
	if (ret) {
		dev_err(dev, "error finding path between %p and %p (%d)\n",
			src, dst, ret);
		return ERR_PTR(-EINVAL);
	}

	list_for_each_entry(node, &path->node_list, search_list) {
		struct icn_qos *req;

		/*
		 * Create icn_qos for each separate link between the nodes.
		 * They may have different constraints and may belong to
		 * different interconnect providers.
		 */
		req = kzalloc(sizeof(*req), GFP_KERNEL);
		if (!req)
			return ERR_PTR(-ENOMEM);

		req->path = path;
		req->bandwidth = 0;
		hlist_add_head(&req->node, &node->qos_list);
	}

	return path;
}
EXPORT_SYMBOL_GPL(interconnect_get);

void interconnect_put(struct interconnect_path *path)
{
	struct interconnect_node *node;
	struct icn_qos *req;
	struct hlist_node *tmp;

	if (IS_ERR(path))
		return;

	list_for_each_entry(node, &path->node_list, search_list) {
		hlist_for_each_entry_safe(req, tmp, &node->qos_list, node) {
			if (req->path == path) {
				hlist_del(&req->node);
				kfree(req);
			}
		}
	}

	kfree(path);
}
EXPORT_SYMBOL_GPL(interconnect_put);

int interconnect_add_provider(struct icp *icp)
{
	struct interconnect_node *node;

	WARN(!icp->ops->xlate, "%s: .xlate is not implemented\n", __func__);
	WARN(!icp->ops->set, "%s: .set is not implemented\n", __func__);

	mutex_lock(&interconnect_provider_list_mutex);
	list_add(&icp->icp_list, &interconnect_provider_list);
	mutex_unlock(&interconnect_provider_list_mutex);

	list_for_each_entry(node, &icp->nodes, icn_list) {
		INIT_HLIST_HEAD(&node->qos_list);
	}

	dev_info(icp->dev, "added interconnect provider %s\n", icp->name);

	return 0;
}
EXPORT_SYMBOL_GPL(interconnect_add_provider);

int interconnect_del_provider(struct icp *icp)
{
	mutex_lock(&interconnect_provider_list_mutex);
	of_node_put(icp->of_node);
	list_del(&icp->icp_list);
	mutex_unlock(&interconnect_provider_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(interconnect_del_provider);
