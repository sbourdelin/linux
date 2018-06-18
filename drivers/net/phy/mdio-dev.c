/*
 * mdio-dev.c - mdio-bus driver, char device interface
 *
 * Copyright (C) 2018 Wei Li <liwei1412@163.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mdio-dev.h>
#include <linux/mdio.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/phy.h>

/*
 * An mdio_dev represents a mii_bus.  It's coupled
 * with a character special file which is accessed by user mode drivers.
 *
 * The list of mdio_dev structures is parallel to the mii_bus lists
 * maintained by the driver model, and is updated using bus notifications.
 */
struct mdio_dev {
	struct list_head list;
	struct mii_bus *bus;
	int nr;
	struct device *dev;
	struct cdev cdev;
};

static DEFINE_MUTEX(mdio_dev_lock);
static DEFINE_IDR(mdio_dev_idr);
static LIST_HEAD(mdio_dev_list);
static DEFINE_SPINLOCK(mdio_dev_list_lock);

static struct mdio_dev *mdio_dev_get_by_bus(struct mii_bus *bus)
{
	struct mdio_dev *mdio_dev;

	if (!bus)
		return NULL;

	spin_lock(&mdio_dev_list_lock);
	list_for_each_entry(mdio_dev, &mdio_dev_list, list) {
		if (mdio_dev->bus == bus)
			goto found;
	}
	mdio_dev = NULL;
found:
	spin_unlock(&mdio_dev_list_lock);
	return mdio_dev;
}

static int alloc_mdio_dev_id(struct mdio_dev *mdio_dev)
{
	int id;

	mutex_lock(&mdio_dev_lock);
	id = idr_alloc(&mdio_dev_idr, mdio_dev, 0, MDIO_MINORS, GFP_KERNEL);
	mutex_unlock(&mdio_dev_lock);
	if (WARN(id < 0, "couldn't get idr"))
		return id == -ENOSPC ? -EBUSY : id;

	mdio_dev->nr = id;
	return 0;
}

static void free_mdio_dev_id(struct mdio_dev *mdio_dev)
{
	mutex_lock(&mdio_dev_lock);
	idr_remove(&mdio_dev_idr, mdio_dev->nr);
	mutex_unlock(&mdio_dev_lock);
	mdio_dev->nr = -1;
}

static struct mii_bus *mdiodev_get_bus(int nr)
{
	struct mdio_dev *found;

	mutex_lock(&mdio_dev_lock);
	found = idr_find(&mdio_dev_idr, nr);
	if (!found)
		goto exit;

	if (try_module_get(found->bus->owner))
		get_device(&found->bus->dev);
	else
		found = NULL;

exit:
	mutex_unlock(&mdio_dev_lock);
	return found ? found->bus : NULL;
}

static void mdiodev_put_bus(struct mii_bus *bus)
{
	if (!bus)
		return;

	put_device(&bus->dev);
	module_put(bus->owner);
}

static struct mdio_dev *get_free_mdio_dev(struct mii_bus *bus)
{
	struct mdio_dev *mdio_dev;

	mdio_dev = kzalloc(sizeof(*mdio_dev), GFP_KERNEL);
	if (!mdio_dev)
		return ERR_PTR(-ENOMEM);
	mdio_dev->bus = bus;

	if (alloc_mdio_dev_id(mdio_dev)) {
		printk(KERN_ERR "mdio-dev: Out of device minors\n");
		kfree(mdio_dev);
		return ERR_PTR(-ENODEV);
	}

	spin_lock(&mdio_dev_list_lock);
	list_add_tail(&mdio_dev->list, &mdio_dev_list);
	spin_unlock(&mdio_dev_list_lock);
	return mdio_dev;
}

static void put_mdio_dev(struct mdio_dev *mdio_dev)
{
	spin_lock(&mdio_dev_list_lock);
	list_del(&mdio_dev->list);
	spin_unlock(&mdio_dev_list_lock);
	free_mdio_dev_id(mdio_dev);
	kfree(mdio_dev);
}

static ssize_t name_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct mii_bus *bus = mdiodev_get_bus(MINOR(dev->devt));

	if (!bus)
		return -ENODEV;
	return sprintf(buf, "%s\n", bus->name);
}
static DEVICE_ATTR_RO(name);

static struct attribute *mdio_attrs[] = {
	&dev_attr_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mdio);

/*-------------------------------------------------------------------------*/

static long mdiodev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mii_bus *bus = file->private_data;
	struct mii_ioctl_data data;
	int res;

	dev_dbg(&bus->dev, "ioctl, cmd=0x%02x, arg=0x%02lx\n", cmd, arg);

	switch (cmd) {
	case SIOCSMIIREG:
		if (copy_from_user(&data,
					(struct mii_ioctl_data __user *)arg, sizeof(data)))
			return -EFAULT;

		res = mdiobus_write(bus, data.phy_id, data.reg_num, data.val_in);
		if (res < 0)
			return -EIO;
		return 0;

	case SIOCGMIIREG:
		if (copy_from_user(&data,
					(struct mii_ioctl_data __user *)arg, sizeof(data)))
			return -EFAULT;

		res = mdiobus_read(bus, data.phy_id, data.reg_num);
		if (res < 0)
			return -EIO;

		data.val_out = res;
		if (copy_to_user((struct mii_ioctl_data __user *)arg,
					&data, sizeof(data)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
	return 0;
}

static int mdiodev_open(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);
	struct mii_bus *bus;

	bus = mdiodev_get_bus(minor);
	if (!bus)
		return -ENODEV;

	file->private_data = bus;

	return 0;
}

static int mdiodev_release(struct inode *inode, struct file *file)
{
	struct mii_bus *bus = file->private_data;

	mdiodev_put_bus(bus);
	file->private_data = NULL;

	return 0;
}

static const struct file_operations mdiodev_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.unlocked_ioctl	= mdiodev_ioctl,
	.open		= mdiodev_open,
	.release	= mdiodev_release,
};

/*-------------------------------------------------------------------------*/

static int mdio_major;
static struct class *mdio_dev_class;

static int mdiodev_attach_bus(struct device *dev, void *dummy)
{
	struct mii_bus *bus;
	struct mdio_dev *mdio_dev;
	int res;

	if (dev->class != &mdio_bus_class)
		return 0;
	bus = to_mii_bus(dev);

	mdio_dev = get_free_mdio_dev(bus);
	if (IS_ERR(mdio_dev))
		return PTR_ERR(mdio_dev);

	cdev_init(&mdio_dev->cdev, &mdiodev_fops);
	mdio_dev->cdev.owner = THIS_MODULE;
	res = cdev_add(&mdio_dev->cdev, MKDEV(mdio_major, mdio_dev->nr), 1);
	if (res)
		goto error_cdev;

	/* register this mdio device with the driver core */
	mdio_dev->dev = device_create(mdio_dev_class, &bus->dev,
						MKDEV(mdio_major, mdio_dev->nr), NULL,
						"mdio-%d", mdio_dev->nr);
	if (IS_ERR(mdio_dev->dev)) {
		res = PTR_ERR(mdio_dev->dev);
		goto error;
	}

	pr_debug("mdio-dev: bus [%s] registered as minor %d\n",
			bus->name, mdio_dev->nr);
	return 0;
error:
	cdev_del(&mdio_dev->cdev);
error_cdev:
	put_mdio_dev(mdio_dev);
	return res;
}

static int mdiodev_detach_bus(struct device *dev, void *dummy)
{
	struct mii_bus *bus;
	struct mdio_dev *mdio_dev;

	if (dev->class != &mdio_bus_class)
		return 0;
	bus = to_mii_bus(dev);

	mdio_dev = mdio_dev_get_by_bus(bus);
	if (!mdio_dev) /* attach_bus must have failed */
		return 0;

	cdev_del(&mdio_dev->cdev);
	device_destroy(mdio_dev_class, MKDEV(mdio_major, mdio_dev->nr));
	put_mdio_dev(mdio_dev);

	pr_debug("mdio-dev: bus [%s] unregistered\n", bus->name);
	return 0;
}

static int mdiodev_notifier_call(struct notifier_block *nb, unsigned long action,
			void *data)
{
	struct device *dev = data;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		return mdiodev_attach_bus(dev, NULL);
	case BUS_NOTIFY_DEL_DEVICE:
		return mdiodev_detach_bus(dev, NULL);
	}

	return 0;
}

static struct notifier_block mdiodev_notifier = {
	.notifier_call = mdiodev_notifier_call,
};

/*-------------------------------------------------------------------------*/

static int __init mdio_dev_init(void)
{
	int res;
	dev_t devid;

	printk(KERN_INFO "mdio /dev entries driver\n");

	res = alloc_chrdev_region(&devid, 0, MDIO_MINORS, "mdio");
	if (res)
		goto out;

	mdio_major = MAJOR(devid);
	mdio_dev_class = class_create(THIS_MODULE, "mdio-dev");
	if (IS_ERR(mdio_dev_class)) {
		res = PTR_ERR(mdio_dev_class);
		goto out_unreg_chrdev;
	}
	mdio_dev_class->dev_groups = mdio_groups;

	/* Keep track of buses which will be added or removed later */
	res = mdiobus_register_notifier(&mdiodev_notifier);
	if (res)
		goto out_unreg_class;

	/* Bind to already existing buses right away */
	class_for_each_device(&mdio_bus_class, NULL, NULL, mdiodev_attach_bus);

	return 0;

out_unreg_class:
	class_destroy(mdio_dev_class);
out_unreg_chrdev:
	unregister_chrdev_region(MKDEV(mdio_major, 0), MDIO_MINORS);
out:
	printk(KERN_ERR "%s: Driver Initialisation failed\n", __FILE__);
	return res;
}

static void __exit mdio_dev_exit(void)
{
	mdiobus_unregister_notifier(&mdiodev_notifier);
	class_for_each_device(&mdio_bus_class, NULL, NULL, mdiodev_detach_bus);
	class_destroy(mdio_dev_class);
	unregister_chrdev_region(MKDEV(mdio_major, 0), MDIO_MINORS);
}

MODULE_AUTHOR("Wei Li <liwei1412@163.com>");
MODULE_DESCRIPTION("MDIO /dev entries driver");
MODULE_LICENSE("GPL");

module_init(mdio_dev_init);
module_exit(mdio_dev_exit);
