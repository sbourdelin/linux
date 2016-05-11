/*
 * Copyright (C) 2015 Pengutronix, Uwe Kleine-König <kernel@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "siox.h"

#define CREATE_TRACE_POINTS
#include <trace/events/siox.h>

struct workqueue_struct *wqueue;
static bool siox_is_registered;

void siox_master_lock(struct siox_master *smaster)
{
	mutex_lock(&smaster->lock);
}

void siox_master_unlock(struct siox_master *smaster)
{
	mutex_unlock(&smaster->lock);
}

struct siox_device *siox_device_add(struct siox_master *smaster);
void siox_device_remove(struct siox_master *smaster);

static void __siox_poll(struct siox_master *smaster)
{
	struct siox_device *sdevice;
	size_t i = 0;
	unsigned int devno = smaster->num_devices;
	u8 prevstatus = smaster->status;

	if (++smaster->status > 0x0d)
		smaster->status = 0;

	memset(smaster->buf, 0, smaster->setbuf_len);

	list_for_each_entry_reverse(sdevice, &smaster->devices, node) {
		struct siox_driver *sdriver =
			sdevice->dev.driver ? to_siox_driver(sdevice->dev.driver) : NULL;

		devno--;

		if (sdriver)
			sdriver->set_data(sdevice, smaster->status,
					  &smaster->buf[i + 1]);

		smaster->buf[i] = smaster->status;

		trace_siox_set_data(smaster, sdevice, devno, i);

		i += sdevice->inbytes;
	}

	BUG_ON(i != smaster->setbuf_len);
	BUG_ON(devno);

	smaster->pushpull(smaster, smaster->setbuf_len, smaster->buf,
			  smaster->getbuf_len,
			  smaster->buf + smaster->setbuf_len);

	list_for_each_entry(sdevice, &smaster->devices, node) {
		struct siox_driver *sdriver =
			sdevice->dev.driver ? to_siox_driver(sdevice->dev.driver) : NULL;
		u8 sdev_status = smaster->buf[i + sdevice->outbytes - 1];

		/*
		 * bits 4:2 of status sample the respective bit in the status
		 * byte written in the previous cycle. Mask them out
		 * accordingly such that a set bit there indicates an error.
		 */
		sdev_status ^= ~prevstatus & 0xe;

		if ((sdevice->status ^ sdev_status) & 1)
			sysfs_notify_dirent(sdevice->watchdog_kn);

		if (!(sdev_status & 1)) {
			sdevice->watchdog_errors++;
			sysfs_notify_dirent(sdevice->watchdog_errors_kn);
		}

		if (sdev_status & 0xe) {
			sdevice->status_errors++;
			sysfs_notify_dirent(sdevice->status_errors_kn);
		}

		sdevice->status = sdev_status;

		/*
		 * XXX trigger events for watchdog, changed jumper and misread
		 * counter. Should the bus stop to poll in these cases?
		 */

		trace_siox_get_data(smaster, sdevice, devno, i);

		if (sdriver)
			sdriver->get_data(sdevice, &smaster->buf[i]);

		devno++;
		i += sdevice->outbytes;
	}

	if (smaster->active)
		queue_delayed_work(wqueue, &smaster->poll,
				   smaster->poll_interval);
}

static void siox_poll(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct siox_master *smaster =
		container_of(dwork, struct siox_master, poll);

	get_device(&smaster->dev);
	siox_master_lock(smaster);
	if (likely(smaster->active))
		__siox_poll(smaster);
	siox_master_unlock(smaster);
	put_device(&smaster->dev);
}

static int __siox_start(struct siox_master *smaster)
{
	if (!(smaster->setbuf_len + smaster->getbuf_len))
		return -ENODEV;

	if (!smaster->buf)
		return -ENOMEM;

	smaster->active = 1;

	__siox_poll(smaster);

	return 0;
}

static int siox_start(struct siox_master *smaster)
{
	int ret;

	siox_master_lock(smaster);
	ret = __siox_start(smaster);
	siox_master_unlock(smaster);

	return ret;
}

static int __siox_stop(struct siox_master *smaster)
{
	smaster->active = 0;
	cancel_delayed_work(&smaster->poll);
	return 0;
}

static int siox_stop(struct siox_master *smaster)
{
	int ret;

	siox_master_lock(smaster);
	ret = __siox_stop(smaster);
	siox_master_unlock(smaster);

	return ret;
}

static ssize_t type_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct siox_device *sdev = to_siox_device(dev);

	return sprintf(buf, "%s\n", sdev->type);
}

static DEVICE_ATTR_RO(type);

static ssize_t inbytes_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct siox_device *sdev = to_siox_device(dev);

	return sprintf(buf, "%zu\n", sdev->inbytes);
}

static DEVICE_ATTR_RO(inbytes);

static ssize_t outbytes_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct siox_device *sdev = to_siox_device(dev);

	return sprintf(buf, "%zu\n", sdev->outbytes);
}

static DEVICE_ATTR_RO(outbytes);

static ssize_t status_errors_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct siox_device *sdev = to_siox_device(dev);
	unsigned status_errors;

	siox_master_lock(sdev->smaster);

	status_errors = sdev->status_errors;

	siox_master_unlock(sdev->smaster);

	return sprintf(buf, "%u\n", status_errors);
}

static DEVICE_ATTR_RO(status_errors);

static ssize_t watchdog_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct siox_device *sdev = to_siox_device(dev);
	u8 status;

	siox_master_lock(sdev->smaster);

	status = sdev->status;

	siox_master_unlock(sdev->smaster);

	return sprintf(buf, "%d\n", status & 1);
}

static DEVICE_ATTR_RO(watchdog);

static ssize_t watchdog_errors_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct siox_device *sdev = to_siox_device(dev);
	unsigned int watchdog_errors;

	siox_master_lock(sdev->smaster);

	watchdog_errors = sdev->watchdog_errors;

	siox_master_unlock(sdev->smaster);

	return sprintf(buf, "%u\n", watchdog_errors);
}

static DEVICE_ATTR_RO(watchdog_errors);

static struct attribute *siox_device_attrs[] = {
	&dev_attr_type.attr,
	&dev_attr_inbytes.attr,
	&dev_attr_outbytes.attr,
	&dev_attr_status_errors.attr,
	&dev_attr_watchdog.attr,
	&dev_attr_watchdog_errors.attr,
	NULL
};
ATTRIBUTE_GROUPS(siox_device);

static void siox_device_release(struct device *dev)
{
	struct siox_device *sdevice = to_siox_device(dev);

	kfree(sdevice);
}

static struct device_type siox_device_type = {
	.groups = siox_device_groups,
	.release = siox_device_release,
};

static int siox_match(struct device *dev, struct device_driver *drv)
{
	if (dev->type != &siox_device_type)
		return 0;

	/* up to now there is only a single driver so keeping this simple */
	return 1;
}

static struct bus_type siox_bus_type = {
	.name = "siox",
	.match = siox_match,
};

static int siox_driver_probe(struct device *dev)
{
	struct siox_driver *sdriver = to_siox_driver(dev->driver);
	struct siox_device *sdevice = to_siox_device(dev);
	int ret;

	ret = sdriver->probe(sdevice);
	return ret;
}

static int siox_driver_remove(struct device *dev)
{
	struct siox_driver *sdriver =
		container_of(dev->driver, struct siox_driver, driver);
	struct siox_device *sdevice = to_siox_device(dev);
	int ret;

	ret = sdriver->remove(sdevice);
	return ret;
}

static void siox_driver_shutdown(struct device *dev)
{
	struct siox_driver *sdriver =
		container_of(dev->driver, struct siox_driver, driver);
	struct siox_device *sdevice = to_siox_device(dev);

	sdriver->shutdown(sdevice);
}

static ssize_t active_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct siox_master *smaster = to_siox_master(dev);

	return sprintf(buf, "%d\n", smaster->active);
}

static ssize_t active_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct siox_master *smaster = to_siox_master(dev);
	int ret;
	int active;

	ret = kstrtoint(buf, 0, &active);
	if (ret < 0)
		return ret;

	if (!active == !smaster->active)
		/* no change */
		return count;

	if (active)
		ret = siox_start(smaster);
	else
		ret = siox_stop(smaster);

	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(active);

static ssize_t device_add_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct siox_master *smaster = to_siox_master(dev);
	int ret;
	char type[20] = "";
	size_t inbytes = 0, outbytes = 0;

	ret = sscanf(buf, "%20s %zu %zu", type, &inbytes, &outbytes);
	if (ret != 3 || strcmp(type, "siox-12x8") ||
	    inbytes != 2 || outbytes != 4)
		return -EINVAL;

	siox_device_add(smaster);

	return count;
}

static DEVICE_ATTR_WO(device_add);

static ssize_t device_remove_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct siox_master *smaster = to_siox_master(dev);

	/* XXX? require to write <type> <inbytes> <outbytes> */
	siox_device_remove(smaster);

	return count;
}

static DEVICE_ATTR_WO(device_remove);

static ssize_t poll_interval_ns_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct siox_master *smaster = to_siox_master(dev);

	return sprintf(buf, "%lld\n", jiffies_to_nsecs(smaster->poll_interval));
}

static ssize_t poll_interval_ns_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct siox_master *smaster = to_siox_master(dev);
	int ret;
	u64 val;

	siox_master_lock(smaster);

	ret = kstrtou64(buf, 0, &val);
	if (ret)
		goto out_unlock;

	smaster->poll_interval = nsecs_to_jiffies(val);

out_unlock:
	siox_master_unlock(smaster);

	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(poll_interval_ns);

static struct attribute *siox_master_attrs[] = {
	&dev_attr_active.attr,
	&dev_attr_device_add.attr,
	&dev_attr_device_remove.attr,
	&dev_attr_poll_interval_ns.attr,
	NULL
};
ATTRIBUTE_GROUPS(siox_master);

static void siox_master_release(struct device *dev)
{
	struct siox_master *smaster = to_siox_master(dev);

	kfree(smaster);
}

static struct device_type siox_master_type = {
	.groups = siox_master_groups,
	.release = siox_master_release,
};

struct siox_master *siox_master_alloc(struct device *dev,
				      size_t size)
{
	struct siox_master *smaster;

	if (!dev)
		return NULL;

	smaster = kzalloc(sizeof(*smaster) + size, GFP_KERNEL);
	if (!smaster)
		return NULL;

	device_initialize(&smaster->dev);

	smaster->busno = -1;
	smaster->dev.bus = &siox_bus_type;
	smaster->dev.type = &siox_master_type;
	smaster->dev.parent = dev;
	smaster->poll_interval = DIV_ROUND_UP(HZ, 40);

	dev_set_drvdata(&smaster->dev, &smaster[1]);

	return smaster;
}
EXPORT_SYMBOL_GPL(siox_master_alloc);

int siox_master_register(struct siox_master *smaster)
{
	if (!siox_is_registered)
		return -EPROBE_DEFER;

	if (!smaster->pushpull)
		return -EINVAL;

	dev_set_name(&smaster->dev, "siox-%d", smaster->busno);

	mutex_init(&smaster->lock);
	INIT_LIST_HEAD(&smaster->devices);
	INIT_DELAYED_WORK(&smaster->poll, siox_poll);

	return device_add(&smaster->dev);
}
EXPORT_SYMBOL_GPL(siox_master_register);

void siox_master_unregister(struct siox_master *smaster)
{
	/* remove device */
	device_del(&smaster->dev);

	siox_master_lock(smaster);

	__siox_stop(smaster);

	while (smaster->num_devices) {
		struct siox_device *sdevice;

		sdevice = container_of(smaster->devices.prev, struct siox_device, node);
		list_del(&sdevice->node);
		smaster->num_devices--;

		siox_master_unlock(smaster);

		device_unregister(&sdevice->dev);

		siox_master_lock(smaster);
	}

	siox_master_unlock(smaster);

	put_device(&smaster->dev);
}
EXPORT_SYMBOL_GPL(siox_master_unregister);

struct siox_device *siox_device_add(struct siox_master *smaster)
{
	struct siox_device *sdevice;
	int ret;

	sdevice = kzalloc(sizeof(*sdevice), GFP_KERNEL);
	if (!sdevice)
		return NULL;

	sdevice->type = "siox-12x8";
	sdevice->inbytes = 2;
	sdevice->outbytes = 4;

	sdevice->smaster = smaster;
	sdevice->dev.parent = &smaster->dev;
	sdevice->dev.bus = &siox_bus_type;
	sdevice->dev.type = &siox_device_type;

	siox_master_lock(smaster);

	dev_set_name(&sdevice->dev, "siox-%d-%d",
		     smaster->busno, smaster->num_devices);

	ret = device_register(&sdevice->dev);
	if (ret) {
		dev_err(&smaster->dev, "failed to register device: %d\n", ret);

		goto err_device_register;
	}

	smaster->num_devices++;
	list_add_tail(&sdevice->node, &smaster->devices);

	smaster->setbuf_len += sdevice->inbytes;
	smaster->getbuf_len += sdevice->outbytes;

	if (smaster->buf_len < smaster->setbuf_len + smaster->getbuf_len) {
		smaster->buf_len = smaster->setbuf_len + smaster->getbuf_len;
		smaster->buf = krealloc(smaster->buf,
					smaster->buf_len, GFP_KERNEL);
		if (!smaster->buf) {
			dev_err(&smaster->dev,
				"failed to realloc buffer to %zu\n",
				smaster->buf_len);
			if (smaster->active)
				__siox_stop(smaster);
		}
	}

	siox_master_unlock(smaster);

	sdevice->status_errors_kn = sysfs_get_dirent(sdevice->dev.kobj.sd,
						     "status_errors");
	sdevice->watchdog_kn = sysfs_get_dirent(sdevice->dev.kobj.sd,
						"watchdog");
	sdevice->watchdog_errors_kn = sysfs_get_dirent(sdevice->dev.kobj.sd,
						       "watchdog_errors");

	return sdevice;

err_device_register:
	siox_master_unlock(smaster);

	kfree(sdevice);

	return ERR_PTR(ret);
}

void siox_device_remove(struct siox_master *smaster)
{
	struct siox_device *sdevice;

	siox_master_lock(smaster);

	if (!smaster->num_devices)
		return;

	sdevice = container_of(smaster->devices.prev, struct siox_device, node);
	list_del(&sdevice->node);
	smaster->num_devices--;

	smaster->setbuf_len -= sdevice->inbytes;
	smaster->getbuf_len -= sdevice->outbytes;

	if (!smaster->num_devices)
		__siox_stop(smaster);

	siox_master_unlock(smaster);

	/*
	 * This must be done without holding the master lock because we're
	 * called from device_remove_store which also holds a sysfs mutex.
	 * device_unregister tries to aquire the same lock.
	 */
	device_unregister(&sdevice->dev);
}

int __siox_driver_register(struct siox_driver *sdriver, struct module *owner)
{
	int ret;

	if (unlikely(!siox_is_registered))
		return -EPROBE_DEFER;

	if (!sdriver->set_data && !sdriver->get_data) {
		pr_err("Driver %s doesn't provide needed callbacks\n",
		       sdriver->driver.name);
		return -EINVAL;
	}

	sdriver->driver.owner = owner;
	sdriver->driver.bus = &siox_bus_type;

	if (sdriver->probe)
		sdriver->driver.probe = siox_driver_probe;
	if (sdriver->remove)
		sdriver->driver.remove = siox_driver_remove;
	if (sdriver->shutdown)
		sdriver->driver.shutdown = siox_driver_shutdown;

	ret = driver_register(&sdriver->driver);
	if (ret)
		pr_err("Failed to register siox driver %s (%d)\n",
		       sdriver->driver.name, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(__siox_driver_register);

static int __init siox_init(void)
{
	int ret;

	ret = bus_register(&siox_bus_type);
	if (ret) {
		pr_err("Registration of SIOX bus type failed: %d\n", ret);
		return ret;
	}

	wqueue = create_singlethread_workqueue("siox");
	if (!wqueue) {
		pr_err("Creation of siox workqueue failed\n");
		bus_unregister(&siox_bus_type);
		return -ENOMEM;
	}

	siox_is_registered = true;

	return 0;
}
subsys_initcall(siox_init);

static void __exit siox_exit(void)
{
	flush_workqueue(wqueue);
	destroy_workqueue(wqueue);
	bus_unregister(&siox_bus_type);
}
module_exit(siox_exit);

MODULE_AUTHOR("Uwe Kleine-Koenig <u.kleine-koenig@pengutronix.de>");
MODULE_DESCRIPTION("Eckelmann SIOX driver core");
MODULE_LICENSE("GPL v2");
