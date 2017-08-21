/*
 * drivers/jtag/jtag.c
 *
 * Copyright (c) 2017 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2017 Oleksandr Shamray <oleksandrs@mellanox.com>
 *
 * Released under the GPLv2 only.
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/jtag.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>
#include <linux/spinlock.h>
#include <uapi/linux/jtag.h>

struct jtag {
	struct list_head list;
	struct device *dev;
	struct cdev cdev;
	int id;
	spinlock_t lock;
	int open;
	const struct jtag_ops *ops;
	unsigned long priv[0] __aligned(ARCH_DMA_MINALIGN);
};

static dev_t jtag_devt;
static LIST_HEAD(jtag_list);
static DEFINE_MUTEX(jtag_mutex);
static DEFINE_IDA(jtag_ida);

void *jtag_priv(struct jtag *jtag)
{
	return jtag->priv;
}
EXPORT_SYMBOL_GPL(jtag_priv);

static __u64 jtag_copy_from_user(__u64 udata, unsigned long bit_size)
{
	unsigned long size;
	void *kdata;

	size = DIV_ROUND_UP(bit_size, BITS_PER_BYTE);
	kdata = memdup_user(u64_to_user_ptr(udata), size);

	return (__u64)(uintptr_t)kdata;
}

static unsigned long jtag_copy_to_user(__u64 udata, __u64 kdata,
				       unsigned long bit_size)
{
	unsigned long size;

	size = DIV_ROUND_UP(bit_size, BITS_PER_BYTE);

	return copy_to_user(u64_to_user_ptr(udata), jtag_u64_to_ptr(kdata),
			    size);
}

static struct class jtag_class = {
	.name = "jtag",
	.owner = THIS_MODULE,
};

static int jtag_run_test_idle_op(struct jtag *jtag,
				 struct jtag_run_test_idle *idle)
{
	if (jtag->ops->idle)
		return jtag->ops->idle(jtag, idle);
	else
		return -EOPNOTSUPP;
}

static int jtag_xfer_op(struct jtag *jtag, struct jtag_xfer *xfer)
{
	if (jtag->ops->xfer)
		return jtag->ops->xfer(jtag, xfer);
	else
		return -EOPNOTSUPP;
}

static long jtag_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct jtag *jtag = file->private_data;
	__u32 *uarg = (__u32 __user *)arg;
	void  *varg = (void __user *)arg;
	struct jtag_run_test_idle idle;
	struct jtag_xfer xfer;
	__u64 tdio_user;
	__u32 value;
	int err;

	switch (cmd) {
	case JTAG_GIOCFREQ:
		if (jtag->ops->freq_get)
			err = jtag->ops->freq_get(jtag, &value);
		else
			err = -EOPNOTSUPP;
		if (err)
			break;

		err = put_user(value, uarg);
		break;

	case JTAG_SIOCFREQ:
		err = __get_user(value, uarg);

		if (value == 0)
			err = -EINVAL;
		if (err)
			break;

		if (jtag->ops->freq_set)
			err = jtag->ops->freq_set(jtag, value);
		else
			err = -EOPNOTSUPP;
		break;

	case JTAG_IOCRUNTEST:
		if (copy_from_user(&idle, varg,
				   sizeof(struct jtag_run_test_idle)))
			return -ENOMEM;
		err = jtag_run_test_idle_op(jtag, &idle);
		break;

	case JTAG_IOCXFER:
		if (copy_from_user(&xfer, varg, sizeof(struct jtag_xfer)))
			return -EFAULT;

		if (xfer.length >= JTAG_MAX_XFER_DATA_LEN)
			return -EFAULT;

		tdio_user = xfer.tdio;
		xfer.tdio = jtag_copy_from_user(xfer.tdio, xfer.length);
		if (!xfer.tdio)
			return -ENOMEM;

		err = jtag_xfer_op(jtag, &xfer);
		if (jtag_copy_to_user(tdio_user, xfer.tdio, xfer.length)) {
			kfree(jtag_u64_to_ptr(xfer.tdio));
			return -EFAULT;
		}

		kfree(jtag_u64_to_ptr(xfer.tdio));
		xfer.tdio = tdio_user;
		if (copy_to_user(varg, &xfer, sizeof(struct jtag_xfer)))
			return -EFAULT;
		break;

	case JTAG_GIOCSTATUS:
		if (jtag->ops->status_get)
			err = jtag->ops->status_get(jtag, &value);
		else
			err = -EOPNOTSUPP;
		if (err)
			break;

		err = put_user(value, uarg);
		break;

	default:
		return -EINVAL;
	}
	return err;
}

#ifdef CONFIG_COMPAT
static long jtag_ioctl_compat(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	return jtag_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static int jtag_open(struct inode *inode, struct file *file)
{
	struct jtag *jtag = container_of(inode->i_cdev, struct jtag, cdev);

	spin_lock(&jtag->lock);

	if (jtag->open) {
		dev_info(NULL, "jtag already opened\n");
		spin_unlock(&jtag->lock);
		return -EBUSY;
	}

	jtag->open++;
	file->private_data = jtag;
	spin_unlock(&jtag->lock);
	return 0;
}

static int jtag_release(struct inode *inode, struct file *file)
{
	struct jtag *jtag = file->private_data;

	spin_lock(&jtag->lock);
	jtag->open--;
	spin_unlock(&jtag->lock);
	return 0;
}

static const struct file_operations jtag_fops = {
	.owner		= THIS_MODULE,
	.open		= jtag_open,
	.release	= jtag_release,
	.llseek		= noop_llseek,
	.unlocked_ioctl = jtag_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= jtag_ioctl_compat,
#endif
};

struct jtag *jtag_alloc(size_t priv_size, const struct jtag_ops *ops)
{
	struct jtag *jtag;

	jtag = kzalloc(sizeof(*jtag) + round_up(priv_size, ARCH_DMA_MINALIGN),
		       GFP_KERNEL);
	if (!jtag)
		return NULL;

	jtag->ops = ops;
	return jtag;
}
EXPORT_SYMBOL_GPL(jtag_alloc);

void jtag_free(struct jtag *jtag)
{
	kfree(jtag);
}
EXPORT_SYMBOL_GPL(jtag_free);

int jtag_register(struct jtag *jtag)
{
	int id;
	int err;

	id = ida_simple_get(&jtag_ida, 0, 0, GFP_KERNEL);
	if (id < 0)
		return id;

	jtag->id = id;
	cdev_init(&jtag->cdev, &jtag_fops);
	jtag->cdev.owner = THIS_MODULE;
	err = cdev_add(&jtag->cdev, MKDEV(MAJOR(jtag_devt), jtag->id), 1);
	if (err)
		goto err_cdev;

	/* Register this jtag device with the driver core */
	jtag->dev = device_create(&jtag_class, NULL, MKDEV(MAJOR(jtag_devt),
							   jtag->id),
				  NULL, "jtag%d", jtag->id);
	if (!jtag->dev)
		goto err_device_create;

	jtag->open = 0;
	dev_set_drvdata(jtag->dev, jtag);
	spin_lock_init(&jtag->lock);
	mutex_lock(&jtag_mutex);
	list_add_tail(&jtag->list, &jtag_list);
	mutex_unlock(&jtag_mutex);
	return err;

err_device_create:
	cdev_del(&jtag->cdev);
err_cdev:
	ida_simple_remove(&jtag_ida, id);
	return err;
}
EXPORT_SYMBOL_GPL(jtag_register);

void jtag_unregister(struct jtag *jtag)
{
	struct device *dev = jtag->dev;

	mutex_lock(&jtag_mutex);
	list_del(&jtag->list);
	mutex_unlock(&jtag_mutex);
	cdev_del(&jtag->cdev);
	device_unregister(dev);
	ida_simple_remove(&jtag_ida, jtag->id);
}
EXPORT_SYMBOL_GPL(jtag_unregister);

static int __init jtag_init(void)
{
	int err;

	err = alloc_chrdev_region(&jtag_devt, 0, 1, "jtag");
	if (err)
		return err;
	return class_register(&jtag_class);
}

static void __exit jtag_exit(void)
{
	class_unregister(&jtag_class);
}

module_init(jtag_init);
module_exit(jtag_exit);

MODULE_AUTHOR("Oleksandr Shamray <oleksandrs@mellanox.com>");
MODULE_DESCRIPTION("Generic jtag support");
MODULE_LICENSE("GPL v2");
