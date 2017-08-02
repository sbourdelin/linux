/*
 * Copyright (c) 2017 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2017 Oleksandr Shamray <oleksandrs@mellanox.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
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
	bool is_open;
	const struct jtag_ops *ops;
	unsigned long priv[0];
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

static void *jtag_copy_from_user(void __user *udata, unsigned long bit_size)
{
	void *kdata;
	unsigned long size;
	unsigned long err;

	size = DIV_ROUND_UP(bit_size, BITS_PER_BYTE);
	kdata = kzalloc(size, GFP_KERNEL);
	if (!kdata)
		return NULL;

	err = copy_from_user(kdata, udata, size);
	if (!err)
		return kdata;

	kfree(kdata);
	return NULL;
}

static unsigned long jtag_copy_to_user(void __user *udata, void *kdata,
				       unsigned long bit_size)
{
	unsigned long size;

	size = DIV_ROUND_UP(bit_size, BITS_PER_BYTE);
	return copy_to_user(udata, kdata, size);
}

static struct class jtag_class = {
	.name = "jtag",
	.owner = THIS_MODULE,
};

static int jtag_run_test_idle(struct jtag *jtag,
			      struct jtag_run_test_idle *idle)
{
	if (jtag->ops->idle)
		return jtag->ops->idle(jtag, idle);
	else
		return -EOPNOTSUPP;
}

static int jtag_xfer(struct jtag *jtag, struct jtag_xfer *xfer)
{
	if (jtag->ops->xfer)
		return jtag->ops->xfer(jtag, xfer);
	else
		return -EOPNOTSUPP;
}

static long jtag_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct jtag *jtag = file->private_data;
	struct jtag_run_test_idle idle;
	struct jtag_xfer xfer;
	void *user_tdio_data;
	unsigned long value;
	int err;

	switch (cmd) {
	case JTAG_GIOCFREQ:
		if (jtag->ops->freq_get)
			err = jtag->ops->freq_get(jtag, &value);
		else
			err = -EOPNOTSUPP;
		if (err)
			break;

		err = __put_user(value, (unsigned long __user *)arg);
		break;

	case JTAG_SIOCFREQ:
		err = __get_user(value, (unsigned long __user *)arg);

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
		if (copy_from_user(&idle, (void __user *)arg,
				   sizeof(struct jtag_run_test_idle)))
			return -ENOMEM;
		err = jtag_run_test_idle(jtag, &idle);
		break;

	case JTAG_IOCXFER:
		if (copy_from_user(&xfer, (void __user *)arg,
				   sizeof(struct jtag_xfer)))
			return -EFAULT;

		user_tdio_data = xfer.tdio;
		xfer.tdio = jtag_copy_from_user((void __user *)user_tdio_data,
				xfer.length);
		if (!xfer.tdio)
			return -ENOMEM;

		err = jtag_xfer(jtag, &xfer);
		if (jtag_copy_to_user((void __user *)user_tdio_data,
				      xfer.tdio, xfer.length)) {
			kfree(xfer.tdio);
			return -EFAULT;
		}

		kfree(xfer.tdio);
		xfer.tdio = user_tdio_data;
		if (copy_to_user((void __user *)arg, &xfer,
				 sizeof(struct jtag_xfer))) {
			kfree(xfer.tdio);
			return -EFAULT;
		}
		break;

	case JTAG_GIOCSTATUS:
		if (jtag->ops->status_get)
			err = jtag->ops->status_get(jtag,
						(enum jtag_endstate *)&value);
		else
			err = -EOPNOTSUPP;
		if (err)
			break;

		err = __put_user(value, (unsigned int __user *)arg);
		break;

	default:
		return -EINVAL;
	}
	return err;
}

static struct jtag *jtag_get_dev(int id)
{
	struct jtag *jtag;

	mutex_lock(&jtag_mutex);
	list_for_each_entry(jtag, &jtag_list, list) {
		if (jtag->id == id)
			goto found;
	}
	jtag = NULL;
found:
	mutex_unlock(&jtag_mutex);
	return jtag;
}

static int jtag_open(struct inode *inode, struct file *file)
{
	struct jtag *jtag;
	unsigned int minor = iminor(inode);

	jtag = jtag_get_dev(minor);
	if (!jtag)
		return -ENODEV;

	spin_lock(&jtag->lock);

	if (jtag->is_open) {
		dev_info(NULL, "jtag already opened\n");
		spin_unlock(&jtag->lock);
		return -EBUSY;
	}

	jtag->is_open = true;
	file->private_data = jtag;
	spin_unlock(&jtag->lock);
	return 0;
}

static int jtag_release(struct inode *inode, struct file *file)
{
	struct jtag *jtag = file->private_data;

	spin_lock(&jtag->lock);
	jtag->is_open = false;
	spin_unlock(&jtag->lock);
	return 0;
}

static const struct file_operations jtag_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.unlocked_ioctl	= jtag_ioctl,
	.open		= jtag_open,
	.release	= jtag_release,
};

struct jtag *jtag_alloc(size_t priv_size, const struct jtag_ops *ops)
{
	struct jtag *jtag = kzalloc(sizeof(*jtag) + priv_size, GFP_KERNEL);

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
				  jtag->id), NULL, "jtag%d", jtag->id);
	if (!jtag->dev)
		goto err_device_create;

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
	list_add_tail(&jtag->list, &jtag_list);
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
MODULE_LICENSE("Dual BSD/GPL");
