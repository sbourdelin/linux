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
 * @hot_reset: if hot-reset is ongoing
 */
struct vfio_ccw_device {
	struct ccw_device	*cdev;
	bool			going_away;
	bool			hot_reset;
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
	struct vfio_ccw_device *vcdev = device_data;
	unsigned long minsz;

	if (cmd == VFIO_DEVICE_GET_INFO) {
		struct vfio_device_info info;

		minsz = offsetofend(struct vfio_device_info, num_irqs);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = VFIO_DEVICE_FLAGS_CCW;
		info.num_regions = 0;
		info.num_irqs = 0;

		return copy_to_user((void __user *)arg, &info, minsz);

	} else if (cmd == VFIO_DEVICE_CCW_HOT_RESET) {
		unsigned long flags;
		int ret;

		spin_lock_irqsave(get_ccwdev_lock(vcdev->cdev), flags);
		if (!vcdev->cdev->online) {
			spin_unlock_irqrestore(get_ccwdev_lock(vcdev->cdev),
					       flags);
			return -EINVAL;
		}

		if (vcdev->hot_reset) {
			spin_unlock_irqrestore(get_ccwdev_lock(vcdev->cdev),
					       flags);
			return -EBUSY;
		}
		vcdev->hot_reset = true;
		spin_unlock_irqrestore(get_ccwdev_lock(vcdev->cdev), flags);

		ret = ccw_device_set_offline(vcdev->cdev);
		if (!ret)
			ret = ccw_device_set_online(vcdev->cdev);

		spin_lock_irqsave(get_ccwdev_lock(vcdev->cdev), flags);
		vcdev->hot_reset = false;
		spin_unlock_irqrestore(get_ccwdev_lock(vcdev->cdev), flags);
		return ret;
	}

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
	if (!vdev || vdev->hot_reset || vdev->going_away)
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
	struct vfio_device *device = vfio_device_get_from_dev(&cdev->dev);
	struct vfio_ccw_device *vdev;
	int ret;

	if (!device)
		goto create_device;

	vdev = vfio_device_data(device);
	vfio_device_put(device);
	if (!vdev)
		goto create_device;

	/*
	 * During hot reset, we just want to disable/enable the
	 * subchannel and need not setup anything again.
	 */
	if (vdev->hot_reset)
		return 0;

create_device:
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
