/*
 * Copyright(C) 2017 Linaro Limited. All rights reserved.
 * Author: Leo Yan <leo.yan@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/coresight.h>
#include <linux/coresight-pmu.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/workqueue.h>

static DEFINE_MUTEX(coresight_panic_lock);
static struct list_head coresight_panic_list;
static struct notifier_block coresight_panic_nb;

struct coresight_panic_node {
	char *name;
	struct coresight_device *csdev;
	struct list_head list;
};

static int coresight_panic_notify(struct notifier_block *nb,
				  unsigned long mode, void *_unused)
{
	int ret = 0, err;
	struct coresight_panic_node *node;
	struct coresight_device *csdev;
	u32 type;

	mutex_lock(&coresight_panic_lock);

	list_for_each_entry(node, &coresight_panic_list, list) {
		csdev = node->csdev;
		type = csdev->type;

		dev_info(&csdev->dev, "invoke panic dump...\n");

		switch (type) {
		case CORESIGHT_DEV_TYPE_SINK:
		case CORESIGHT_DEV_TYPE_LINKSINK:
			err = sink_ops(csdev)->panic_cb(csdev);
			if (err)
				ret = err;
			break;
		default:
			dev_err(&csdev->dev,
				"Unsupported type for panic dump\n");
			break;
		}
	}

	mutex_unlock(&coresight_panic_lock);
	return ret;
}

int coresight_add_panic_cb(struct coresight_device *csdev)
{
	struct coresight_panic_node *node;

	node = kzalloc(sizeof(struct coresight_panic_node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->name = kstrndup(dev_name(&csdev->dev), 16, GFP_KERNEL);
	if (!node->name) {
		kfree(node);
		return -ENOMEM;
	}
	node->csdev = csdev;

	mutex_lock(&coresight_panic_lock);
	list_add_tail(&node->list, &coresight_panic_list);
	mutex_unlock(&coresight_panic_lock);

	return 0;
}

void coresight_del_panic_cb(struct coresight_device *csdev)
{
	struct coresight_panic_node *node;

	mutex_lock(&coresight_panic_lock);

	list_for_each_entry(node, &coresight_panic_list, list) {
		if (node->csdev == csdev) {
			list_del(&node->list);
			kfree(node->name);
			kfree(node);
			mutex_unlock(&coresight_panic_lock);
			return;
		}
	}

	dev_err(&csdev->dev, "Failed to find panic node.\n");
	mutex_unlock(&coresight_panic_lock);
}

static int __init coresight_panic_init(void)
{
	int ret;

	INIT_LIST_HEAD(&coresight_panic_list);

	coresight_panic_nb.notifier_call = coresight_panic_notify;
	ret = atomic_notifier_chain_register(&panic_notifier_list,
					     &coresight_panic_nb);
	if (ret)
		return ret;

	return 0;
}
subsys_initcall(coresight_panic_init);
