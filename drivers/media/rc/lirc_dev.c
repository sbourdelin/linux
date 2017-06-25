/*
 * LIRC base driver
 *
 * by Artur Lipowski <alipowski@interia.pl>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/idr.h>

#include <media/rc-core.h>
#include <media/lirc.h>
#include <media/lirc_dev.h>

static dev_t lirc_base_dev;

/* Used to keep track of allocated lirc devices */
#define LIRC_MAX_DEVICES 256
static DEFINE_IDA(lirc_ida);

/* Only used for sysfs but defined to void otherwise */
static struct class *lirc_class;

static void lirc_release_device(struct device *ld)
{
	struct lirc_dev *d = container_of(ld, struct lirc_dev, dev);

	if (d->buf_internal) {
		lirc_buffer_free(d->buf);
		kfree(d->buf);
		d->buf = NULL;
	}
	kfree(d);
	module_put(THIS_MODULE);
}

static int lirc_allocate_buffer(struct lirc_dev *d)
{
	int err;

	if (d->buf) {
		d->buf_internal = false;
		return 0;
	}

	d->buf = kmalloc(sizeof(struct lirc_buffer), GFP_KERNEL);
	if (!d->buf)
		return -ENOMEM;

	err = lirc_buffer_init(d->buf, d->chunk_size, d->buffer_size);
	if (err) {
		kfree(d->buf);
		d->buf = NULL;
		return err;
	}

	d->buf_internal = true;
	return 0;
}

struct lirc_dev *
lirc_allocate_device(void)
{
	struct lirc_dev *d;

	d = kzalloc(sizeof(struct lirc_dev), GFP_KERNEL);
	if (d) {
		mutex_init(&d->mutex);
		device_initialize(&d->dev);
		d->dev.class = lirc_class;
		d->dev.release = lirc_release_device;
		__module_get(THIS_MODULE);
	}

	return d;
}
EXPORT_SYMBOL(lirc_allocate_device);

void lirc_free_device(struct lirc_dev *d)
{
	if (!d)
		return;

	put_device(&d->dev);
}
EXPORT_SYMBOL(lirc_free_device);

int lirc_register_device(struct lirc_dev *d)
{
	int minor;
	int err;
	const char *path;

	if (!d) {
		pr_err("driver pointer must be not NULL!\n");
		return -EBADRQC;
	}

	if (!d->dev.parent) {
		pr_err("dev parent pointer not filled in!\n");
		return -EINVAL;
	}

	if (!d->fops) {
		pr_err("fops pointer not filled in!\n");
		return -EINVAL;
	}

	if (!d->buf && d->chunk_size < 1) {
		pr_err("chunk_size must be set!\n");
		return -EINVAL;
	}

	if (!d->buf && d->buffer_size < 1) {
		pr_err("buffer_size must be set!\n");
		return -EINVAL;
	}

	if (d->code_length < 1 || d->code_length > 128) {
		dev_err(&d->dev, "invalid code_length!\n");
		return -EBADRQC;
	}

	if (!d->buf && !(d->fops && d->fops->read &&
			  d->fops->poll && d->fops->unlocked_ioctl)) {
		dev_err(&d->dev, "undefined read, poll, ioctl\n");
		return -EBADRQC;
	}

	d->name[sizeof(d->name)-1] = '\0';

	if (d->features == 0)
		d->features = LIRC_CAN_REC_LIRCCODE;

	if (LIRC_CAN_REC(d->features)) {
		err = lirc_allocate_buffer(d);
		if (err)
			return err;
	}

	minor = ida_simple_get(&lirc_ida, 0, LIRC_MAX_DEVICES, GFP_KERNEL);
	if (minor < 0)
		return minor;

	d->minor = minor;
	d->dev.devt = MKDEV(MAJOR(lirc_base_dev), d->minor);
	dev_set_name(&d->dev, "lirc%d", d->minor);

	cdev_init(&d->cdev, d->fops);
	d->cdev.owner = d->owner;
	d->attached = true;

	err = cdev_device_add(&d->cdev, &d->dev);
	if (err) {
		ida_simple_remove(&lirc_ida, minor);
		return err;
	}

	path = kobject_get_path(&d->dev.kobj, GFP_KERNEL);
	dev_info(&d->dev, "%s as %s\n", d->name, path ?: "N/A");
	kfree(path);

	return 0;
}
EXPORT_SYMBOL(lirc_register_device);

void lirc_unregister_device(struct lirc_dev *d)
{
	if (!d)
		return;

	dev_dbg(&d->dev, "lirc_dev: driver %s unregistered from minor = %d\n",
		d->name, d->minor);

	mutex_lock(&d->mutex);

	d->attached = false;
	if (d->open)
		wake_up_interruptible(&d->buf->wait_poll);

	mutex_unlock(&d->mutex);

	cdev_device_del(&d->cdev, &d->dev);
	ida_simple_remove(&lirc_ida, d->minor);
	put_device(&d->dev);
}
EXPORT_SYMBOL(lirc_unregister_device);

void lirc_init_pdata(struct inode *inode, struct file *file)
{
	struct lirc_dev *d = container_of(inode->i_cdev, struct lirc_dev, cdev);

	file->private_data = d;
}
EXPORT_SYMBOL(lirc_init_pdata);

void *lirc_get_pdata(struct file *file)
{
	struct lirc_dev *d = file->private_data;

	return d->data;
}
EXPORT_SYMBOL(lirc_get_pdata);


static int __init lirc_dev_init(void)
{
	int retval;

	lirc_class = class_create(THIS_MODULE, "lirc");
	if (IS_ERR(lirc_class)) {
		pr_err("class_create failed\n");
		return PTR_ERR(lirc_class);
	}

	retval = alloc_chrdev_region(&lirc_base_dev, 0, LIRC_MAX_DEVICES,
				     "BaseRemoteCtl");
	if (retval) {
		class_destroy(lirc_class);
		pr_err("alloc_chrdev_region failed\n");
		return retval;
	}

	pr_info("IR Remote Control driver registered, major %d\n",
						MAJOR(lirc_base_dev));

	return 0;
}

static void __exit lirc_dev_exit(void)
{
	class_destroy(lirc_class);
	unregister_chrdev_region(lirc_base_dev, LIRC_MAX_DEVICES);
	pr_info("module unloaded\n");
}

module_init(lirc_dev_init);
module_exit(lirc_dev_exit);

MODULE_DESCRIPTION("LIRC base driver module");
MODULE_AUTHOR("Artur Lipowski");
MODULE_LICENSE("GPL");
