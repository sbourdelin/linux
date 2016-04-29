/*
 * vfio based ccw device driver
 *
 * Copyright IBM Corp. 2016
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 * Author(s): Dong Jia Shi <bjsdjshi@linux.vnet.ibm.com>
 *            Xiao Feng Ren <renxiaof@linux.vnet.ibm.com>
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/iommu.h>
#include <linux/vfio.h>
#include <asm/ccwdev.h>
#include <asm/cio.h>

/**
 * struct vfio_ccw_device
 * @cdev: ccw device
 * @going_away: if an offline procedure was already ongoing
 */
struct vfio_ccw_device {
	struct ccw_device	*cdev;
	bool			going_away;
};

enum vfio_ccw_device_type {
	vfio_dasd_eckd,
};

struct ccw_device_id vfio_ccw_ids[] = {
	{ CCW_DEVICE_DEVTYPE(0x3990, 0, 0x3390, 0),
	  .driver_info = vfio_dasd_eckd},
	{ /* End of list. */ },
};
MODULE_DEVICE_TABLE(ccw, vfio_ccw_ids);

/*
 * vfio callbacks
 */
static int vfio_ccw_open(void *device_data)
{
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	return 0;
}

static void vfio_ccw_release(void *device_data)
{
	module_put(THIS_MODULE);
}

static long vfio_ccw_ioctl(void *device_data, unsigned int cmd,
			   unsigned long arg)
{
	return -ENOTTY;
}

static const struct vfio_device_ops vfio_ccw_ops = {
	.name		= "vfio_ccw",
	.open		= vfio_ccw_open,
	.release	= vfio_ccw_release,
	.ioctl		= vfio_ccw_ioctl,
};

static int vfio_ccw_probe(struct ccw_device *cdev)
{
	struct iommu_group *group = vfio_iommu_group_get(&cdev->dev);

	if (!group)
		return -EINVAL;

	return 0;
}

static int vfio_ccw_set_offline(struct ccw_device *cdev)
{
	struct vfio_device *device = vfio_device_get_from_dev(&cdev->dev);
	struct vfio_ccw_device *vdev;

	if (!device)
		return 0;

	vdev = vfio_device_data(device);
	vfio_device_put(device);
	if (!vdev || vdev->going_away)
		return 0;

	vdev->going_away = true;
	vfio_del_group_dev(&cdev->dev);
	kfree(vdev);

	return 0;
}

void vfio_ccw_remove(struct ccw_device *cdev)
{
	if (cdev && cdev->online)
		vfio_ccw_set_offline(cdev);

	vfio_iommu_group_put(cdev->dev.iommu_group, &cdev->dev);
}

static int vfio_ccw_set_online(struct ccw_device *cdev)
{
	struct vfio_ccw_device *vdev;
	int ret;

	vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
	if (!vdev)
		return -ENOMEM;

	vdev->cdev = cdev;

	ret = vfio_add_group_dev(&cdev->dev, &vfio_ccw_ops, vdev);
	if (ret)
		kfree(vdev);

	return ret;
}

static int vfio_ccw_notify(struct ccw_device *cdev, int event)
{
	/* LATER: We probably need to handle device/path state changes. */
	return 0;
}

static struct ccw_driver vfio_ccw_driver = {
	.driver = {
		.name	= "vfio_ccw",
		.owner	= THIS_MODULE,
	},
	.ids	     = vfio_ccw_ids,
	.probe	     = vfio_ccw_probe,
	.remove      = vfio_ccw_remove,
	.set_offline = vfio_ccw_set_offline,
	.set_online  = vfio_ccw_set_online,
	.notify      = vfio_ccw_notify,
	.int_class   = IRQIO_VFC,
};

static int __init vfio_ccw_init(void)
{
	return ccw_driver_register(&vfio_ccw_driver);
}

static void __exit vfio_ccw_cleanup(void)
{
	ccw_driver_unregister(&vfio_ccw_driver);
}

module_init(vfio_ccw_init);
module_exit(vfio_ccw_cleanup);

MODULE_LICENSE("GPL v2");
