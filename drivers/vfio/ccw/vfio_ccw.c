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
#include <asm/orb.h>
#include "ccwchain.h"

/**
 * struct vfio_ccw_device
 * @cdev: ccw device
 * @curr_intparm: record current interrupt parameter,
 *                used for wait interrupt.
 * @wait_q: wait for interrupt
 * @ccwchain_cmd: address map for current ccwchain.
 * @irb: irb info received from interrupt
 * @orb: orb for the currently processed ssch request
 * @scsw: scsw info
 * @going_away: if an offline procedure was already ongoing
 * @hot_reset: if hot-reset is ongoing
 */
struct vfio_ccw_device {
	struct ccw_device	*cdev;
	u32			curr_intparm;
	wait_queue_head_t	wait_q;
	struct ccwchain_cmd	ccwchain_cmd;
	struct irb		irb;
	union orb		orb;
	union scsw		scsw;
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
 * LATER:
 * This is good for Linux guests; but we may need an interface to
 * deal with further bits in the orb.
 */
static unsigned long flags_from_orb(union orb *orb)
{
	unsigned long flags = 0;

	flags |= orb->cmd.pfch ? 0 : DOIO_DENY_PREFETCH;
	flags |= orb->cmd.spnd ? DOIO_ALLOW_SUSPEND : 0;
	flags |= orb->cmd.ssic ? (DOIO_SUPPRESS_INTER | DOIO_ALLOW_SUSPEND) : 0;

	return flags;
}

/* Check if the current intparm has been set. */
static int doing_io(struct vfio_ccw_device *vcdev, u32 intparm)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(get_ccwdev_lock(vcdev->cdev), flags);
	ret = (vcdev->curr_intparm == intparm);
	spin_unlock_irqrestore(get_ccwdev_lock(vcdev->cdev), flags);
	return ret;
}

int vfio_ccw_io_helper(struct vfio_ccw_device *vcdev)
{
	struct ccwchain_cmd *ccwchain_cmd;
	struct ccw1 *cpa;
	u32 intparm;
	unsigned long io_flags, lock_flags;
	int ret;

	ccwchain_cmd = &vcdev->ccwchain_cmd;
	cpa = ccwchain_get_cpa(ccwchain_cmd);
	intparm = (u32)(u64)ccwchain_cmd->k_ccwchain;
	io_flags = flags_from_orb(&vcdev->orb);

	spin_lock_irqsave(get_ccwdev_lock(vcdev->cdev), lock_flags);
	ret = ccw_device_start(vcdev->cdev, cpa, intparm,
			       vcdev->orb.cmd.lpm, io_flags);
	if (!ret)
		vcdev->curr_intparm = 0;
	spin_unlock_irqrestore(get_ccwdev_lock(vcdev->cdev), lock_flags);

	if (!ret)
		wait_event(vcdev->wait_q,
			   doing_io(vcdev, intparm));

	ccwchain_update_scsw(ccwchain_cmd, &(vcdev->irb.scsw));

	return ret;
}

/* Deal with the ccw command request from the userspace. */
int vfio_ccw_cmd_request(struct vfio_ccw_device *vcdev,
			 struct vfio_ccw_cmd *ccw_cmd)
{
	union orb *orb = &vcdev->orb;
	union scsw *scsw = &vcdev->scsw;
	struct irb *irb = &vcdev->irb;
	int ret;

	memcpy(orb, ccw_cmd->orb_area, sizeof(*orb));
	memcpy(scsw, ccw_cmd->scsw_area, sizeof(*scsw));
	vcdev->ccwchain_cmd.u_ccwchain = (void *)ccw_cmd->ccwchain_buf;
	vcdev->ccwchain_cmd.k_ccwchain = NULL;
	vcdev->ccwchain_cmd.nr = ccw_cmd->ccwchain_nr;

	if (scsw->cmd.fctl & SCSW_FCTL_START_FUNC) {
		/*
		 * XXX:
		 * Only support prefetch enable mode now.
		 * Only support 64bit addressing idal.
		 */
		if (!orb->cmd.pfch || !orb->cmd.c64)
			return -EOPNOTSUPP;

		ret = ccwchain_alloc(&vcdev->ccwchain_cmd);
		if (ret)
			return ret;

		ret = ccwchain_prefetch(&vcdev->ccwchain_cmd);
		if (ret) {
			ccwchain_free(&vcdev->ccwchain_cmd);
			return ret;
		}

		/* Start channel program and wait for I/O interrupt. */
		ret = vfio_ccw_io_helper(vcdev);
		if (!ret) {
			/* Get irb info and copy it to irb_area. */
			memcpy(ccw_cmd->irb_area, irb, sizeof(*irb));
		}

		ccwchain_free(&vcdev->ccwchain_cmd);
	} else if (scsw->cmd.fctl & SCSW_FCTL_HALT_FUNC) {
		/* XXX: Handle halt. */
		ret = -EOPNOTSUPP;
	} else if (scsw->cmd.fctl & SCSW_FCTL_CLEAR_FUNC) {
		/* XXX: Handle clear. */
		ret = -EOPNOTSUPP;
	} else {
		ret = -EOPNOTSUPP;
	}

	return ret;
}

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

	} else if (cmd == VFIO_DEVICE_CCW_CMD_REQUEST) {
		struct vfio_ccw_cmd ccw_cmd;
		int ret;

		minsz = offsetofend(struct vfio_ccw_cmd, ccwchain_buf);

		if (copy_from_user(&ccw_cmd, (void __user *)arg, minsz))
			return -EFAULT;

		if (ccw_cmd.argsz < minsz)
			return -EINVAL;

		ret = vfio_ccw_cmd_request(vcdev, &ccw_cmd);
		if (ret)
			return ret;

		return copy_to_user((void __user *)arg, &ccw_cmd, minsz);
	}

	return -ENOTTY;
}

static const struct vfio_device_ops vfio_ccw_ops = {
	.name		= "vfio_ccw",
	.open		= vfio_ccw_open,
	.release	= vfio_ccw_release,
	.ioctl		= vfio_ccw_ioctl,
};

static void vfio_ccw_int_handler(struct ccw_device *cdev,
				unsigned long intparm,
				struct irb *irb)
{
	struct vfio_device *device = dev_get_drvdata(&cdev->dev);
	struct vfio_ccw_device *vdev;

	if (!device)
		return;

	vdev = vfio_device_data(device);
	if (!vdev)
		return;

	vdev->curr_intparm = intparm;
	memcpy(&vdev->irb, irb, sizeof(*irb));
	wake_up(&vdev->wait_q);
}

static int vfio_ccw_probe(struct ccw_device *cdev)
{
	struct iommu_group *group = vfio_iommu_group_get(&cdev->dev);

	if (!group)
		return -EINVAL;

	cdev->handler = vfio_ccw_int_handler;

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

	/* Put the vfio_device reference we got during the online process. */
	vfio_device_put(device);

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

	cdev->handler = NULL;
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
	if (ret) {
		kfree(vdev);
		return ret;
	}

	/*
	 * Get a reference to the vfio_device for this device, and don't put
	 * it until device offline. Thus we don't need to get/put a reference
	 * every time we run into the int_handler. And we will get rid of a
	 * wrong usage of mutex in int_handler.
	 */
	device = vfio_device_get_from_dev(&cdev->dev);
	if (!device) {
		vfio_del_group_dev(&cdev->dev);
		kfree(vdev);
		return -ENODEV;
	}

	init_waitqueue_head(&vdev->wait_q);

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
