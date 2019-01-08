// SPDX-License-Identifier: GPL-2.0
//
// Copyright 2018 Google LLC.

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_data/mtk_scp.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/rpmsg/mtk_rpmsg.h>
#include <linux/workqueue.h>

#include "rpmsg_internal.h"

/*
 * TODO: This is built on top of scp_ipi_register / scp_ipi_send in mtk_scp.h.
 * It's probably better to move the implementation of register / send to here
 * instead of in remoteproc/mtk_scp_ipi.c
 */
/*
 * TODO: Do we need some sort of vring for performance? We may be able to get
 * rid of this file if so, but that would require SCP firmware support too.
 */

struct mtk_rpmsg_device {
	struct rpmsg_device rpdev;
	struct platform_device *scp_pdev;
};

struct mtk_rpmsg_endpoint {
	struct rpmsg_endpoint ept;
	struct platform_device *scp_pdev;
};

/* TODO: Naming is hard... */
struct mtk_rpmsg_rproc_subdev {
	struct rproc *scp_rproc;
	struct platform_device *scp_pdev;
	struct rpmsg_endpoint *ns_ept;
	struct rproc_subdev subdev;

	struct work_struct register_work;
	struct list_head rpmsg_channel_info_list;
};

#define to_mtk_subdev(d) container_of(d, struct mtk_rpmsg_rproc_subdev, subdev)

struct mtk_rpmsg_channel_info {
	struct rpmsg_channel_info info;
	bool registered;
	struct list_head list;
};

/**
 * TODO: This is copied from virtio_rpmsg_bus.
 * struct rpmsg_ns_msg - dynamic name service announcement message
 * @name: name of remote service that is published
 * @addr: address of remote service that is published
 *
 * This message is sent across to publish a new service, or announce
 * about its removal. When we receive these messages, an appropriate
 * rpmsg channel (i.e device) is created/destroyed. In turn, the ->probe()
 * or ->remove() handler of the appropriate rpmsg driver will be invoked
 * (if/as-soon-as one is registered).
 */
struct rpmsg_ns_msg {
	char name[RPMSG_NAME_SIZE];
	u32 addr;
} __packed;

#define to_scp_device(r)	container_of(r, struct mtk_rpmsg_device, rpdev)
#define to_scp_endpoint(r)	container_of(r, struct mtk_rpmsg_endpoint, ept)

static const struct rpmsg_endpoint_ops mtk_rpmsg_endpoint_ops;

static void __ept_release(struct kref *kref)
{
	struct rpmsg_endpoint *ept = container_of(kref, struct rpmsg_endpoint,
						  refcount);
	kfree(to_scp_endpoint(ept));
}

static void mtk_rpmsg_ipi_handler(void *data, unsigned int len, void *priv)
{
	struct mtk_rpmsg_endpoint *mept = priv;
	struct rpmsg_endpoint *ept = &mept->ept;

	/* TODO: What if the cb() returns error? */
	(*ept->cb)(ept->rpdev, data, len, ept->priv, ept->addr);
}

static struct rpmsg_endpoint *
__rpmsg_create_ept(struct platform_device *scp_pdev, struct rpmsg_device *rpdev,
		   rpmsg_rx_cb_t cb, void *priv,
		   u32 id, const char *name)
{
	struct mtk_rpmsg_endpoint *mept;
	struct rpmsg_endpoint *ept;

	int ret;

	mept = kzalloc(sizeof(*mept), GFP_KERNEL);
	if (!mept)
		return NULL;
	mept->scp_pdev = scp_pdev;

	ept = &mept->ept;
	kref_init(&ept->refcount);

	ept->rpdev = rpdev;
	ept->cb = cb;
	ept->priv = priv;
	ept->ops = &mtk_rpmsg_endpoint_ops;
	ept->addr = id;

	ret = scp_ipi_register(scp_pdev, id, mtk_rpmsg_ipi_handler, name, mept);
	if (ret) {
		dev_err(&scp_pdev->dev, "ipi register failed, id = %d", id);
		kref_put(&ept->refcount, __ept_release);
		return NULL;
	}

	return ept;
}

static struct rpmsg_endpoint *
mtk_rpmsg_create_ept(struct rpmsg_device *rpdev, rpmsg_rx_cb_t cb, void *priv,
		     struct rpmsg_channel_info chinfo)
{
	struct platform_device *scp_pdev = to_scp_device(rpdev)->scp_pdev;
	/* TODO: Is using src as IPI id "correct"? */
	return __rpmsg_create_ept(scp_pdev, rpdev, cb, priv, chinfo.src,
				  chinfo.name);
}

static const struct rpmsg_device_ops mtk_rpmsg_device_ops = {
	.create_ept = mtk_rpmsg_create_ept,
};

static void mtk_rpmsg_destroy_ept(struct rpmsg_endpoint *ept)
{
	kref_put(&ept->refcount, __ept_release);
}

static int __mtk_rpmsg_send(struct mtk_rpmsg_endpoint *mept, void *data,
			    int len, bool wait)
{
	/*
	 * TODO: The "wait" is not same as what scp_ipi_send's "wait" means, so
	 * this is not correct.
	 * (first wait for there's space of tx buffer, second wait for the ack
	 * from scp.)
	 */
	return scp_ipi_send(mept->scp_pdev, mept->ept.addr, data, len,
			    wait ? 200 : 0);
}

static int mtk_rpmsg_send(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct mtk_rpmsg_endpoint *mept = to_scp_endpoint(ept);

	return __mtk_rpmsg_send(mept, data, len, true);
}

static int mtk_rpmsg_trysend(struct rpmsg_endpoint *ept, void *data, int len)
{
	struct mtk_rpmsg_endpoint *mept = to_scp_endpoint(ept);

	return __mtk_rpmsg_send(mept, data, len, false);
}

static const struct rpmsg_endpoint_ops mtk_rpmsg_endpoint_ops = {
	.destroy_ept = mtk_rpmsg_destroy_ept,
	.send = mtk_rpmsg_send,
	.trysend = mtk_rpmsg_trysend,
};

static void mtk_rpmsg_release_device(struct device *dev)
{
	struct rpmsg_device *rpdev = to_rpmsg_device(dev);
	struct mtk_rpmsg_device *mdev = to_scp_device(rpdev);

	kfree(mdev);
}

static int mtk_rpmsg_register_device(struct platform_device *scp_pdev,
				     struct rpmsg_channel_info *info)
{
	struct rpmsg_device *rpdev;
	struct mtk_rpmsg_device *mdev;
	int ret;

	mdev = kzalloc(sizeof(*mdev), GFP_KERNEL);
	if (!mdev)
		return -ENOMEM;

	mdev->scp_pdev = scp_pdev;

	rpdev = &mdev->rpdev;
	rpdev->ops = &mtk_rpmsg_device_ops;
	rpdev->src = info->src;
	rpdev->dst = info->dst;
	strncpy(rpdev->id.name, info->name, RPMSG_NAME_SIZE);

	rpdev->dev.parent = &scp_pdev->dev;
	rpdev->dev.release = mtk_rpmsg_release_device;

	ret = rpmsg_register_device(rpdev);
	if (ret)
		return ret;

	return 0;
}

static void mtk_register_device_work_function(struct work_struct *register_work)
{
	struct mtk_rpmsg_rproc_subdev *mtk_subdev = container_of(
		register_work, struct mtk_rpmsg_rproc_subdev, register_work);
	struct platform_device *scp_pdev = mtk_subdev->scp_pdev;

	int ret;

	struct mtk_rpmsg_channel_info *info;

	/* TODO: lock the list */
	list_for_each_entry(info, &mtk_subdev->rpmsg_channel_info_list, list) {
		if (info->registered)
			continue;

		ret = mtk_rpmsg_register_device(scp_pdev, &info->info);
		if (ret) {
			dev_err(&scp_pdev->dev, "Can't create rpmsg_device\n");
			continue;
		}

		info->registered = true;
	}
}

static int mtk_rpmsg_create_device(struct mtk_rpmsg_rproc_subdev *mtk_subdev,
				   char *name, u32 addr)
{
	struct mtk_rpmsg_channel_info *info;

	/* This is called in interrupt context from name service callback. */
	info = kzalloc(sizeof(*info), GFP_ATOMIC);
	if (!info)
		return -ENOMEM;

	strncpy(info->info.name, name, RPMSG_NAME_SIZE);
	info->info.src = addr;
	info->info.dst = RPMSG_ADDR_ANY;
	/* TODO: lock the list. */
	list_add(&info->list, &mtk_subdev->rpmsg_channel_info_list);

	schedule_work(&mtk_subdev->register_work);
	return 0;
}

static int mtk_rpmsg_ns_cb(struct rpmsg_device *rpdev, void *data, int len,
			   void *priv, u32 src)
{
	struct rpmsg_ns_msg *msg = data;
	struct mtk_rpmsg_rproc_subdev *mtk_subdev = priv;
	struct device *dev = &mtk_subdev->scp_pdev->dev;

	int ret;

	if (len != sizeof(*msg)) {
		dev_err(dev, "malformed ns msg (%d)\n", len);
		return -EINVAL;
	}

	/*
	 * the name service ept does _not_ belong to a real rpmsg channel,
	 * and is handled by the rpmsg bus itself.
	 * for sanity reasons, make sure a valid rpdev has _not_ sneaked
	 * in somehow.
	 */
	if (rpdev) {
		dev_err(dev, "anomaly: ns ept has an rpdev handle\n");
		return -EINVAL;
	}

	/* don't trust the remote processor for null terminating the name */
	msg->name[RPMSG_NAME_SIZE - 1] = '\0';

	dev_info(dev, "creating channel %s addr 0x%x\n", msg->name, msg->addr);

	ret = mtk_rpmsg_create_device(mtk_subdev, msg->name, msg->addr);
	if (ret) {
		dev_err(dev, "create rpmsg device failed\n");
		return ret;
	}

	return 0;
}

int mtk_rpmsg_prepare(struct rproc_subdev *subdev)
{
	struct mtk_rpmsg_rproc_subdev *mtk_subdev;
	struct platform_device *scp_pdev;

	mtk_subdev = to_mtk_subdev(subdev);
	scp_pdev = mtk_subdev->scp_pdev;

	/* a dedicated endpoint handles the name service msgs */
	mtk_subdev->ns_ept =
		__rpmsg_create_ept(scp_pdev, NULL, mtk_rpmsg_ns_cb, mtk_subdev,
				   SCP_IPI_NS_SERVICE, "name-service");
	if (!mtk_subdev->ns_ept) {
		dev_err(&scp_pdev->dev,
			"failed to create name service endpoint\n");
		return -ENOMEM;
	}

	return 0;
}

void mtk_rpmsg_unprepare(struct rproc_subdev *subdev)
{
	struct mtk_rpmsg_channel_info *info, *next;
	struct device *dev;
	struct mtk_rpmsg_rproc_subdev *mtk_subdev = to_mtk_subdev(subdev);

	dev = &mtk_subdev->scp_pdev->dev;

	/* TODO: cancel pending work, lock the list */
	list_for_each_entry(info, &mtk_subdev->rpmsg_channel_info_list, list) {
		if (!info->registered)
			continue;
		if (rpmsg_unregister_device(dev, &info->info)) {
			dev_warn(
				dev,
				"rpmsg_unregister_device failed for %s.%d.%d\n",
				info->info.name, info->info.src,
				info->info.dst);
		}
	}

	list_for_each_entry_safe(info, next,
				 &mtk_subdev->rpmsg_channel_info_list, list) {
		list_del(&info->list);
		kfree(info);
	}

	kref_put(&mtk_subdev->ns_ept->refcount, __ept_release);
}

struct rproc_subdev *
mtk_rpmsg_create_rproc_subdev(struct platform_device *scp_pdev,
			      struct rproc *scp_rproc)
{
	struct mtk_rpmsg_rproc_subdev *mtk_subdev;

	mtk_subdev = kzalloc(sizeof(*mtk_subdev), GFP_KERNEL);
	if (!mtk_subdev)
		return NULL;

	mtk_subdev->scp_pdev = scp_pdev;
	mtk_subdev->scp_rproc = scp_rproc;
	mtk_subdev->subdev.prepare = mtk_rpmsg_prepare;
	mtk_subdev->subdev.unprepare = mtk_rpmsg_unprepare;
	INIT_LIST_HEAD(&mtk_subdev->rpmsg_channel_info_list);
	INIT_WORK(&mtk_subdev->register_work,
		  mtk_register_device_work_function);

	return &mtk_subdev->subdev;
}
EXPORT_SYMBOL_GPL(mtk_rpmsg_create_rproc_subdev);

void mtk_rpmsg_destroy_rproc_subdev(struct rproc_subdev *subdev)
{
	struct mtk_rpmsg_rproc_subdev *mtk_subdev = to_mtk_subdev(subdev);

	kfree(mtk_subdev);
}
EXPORT_SYMBOL_GPL(mtk_rpmsg_destroy_rproc_subdev);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek scp rpmsg driver");
