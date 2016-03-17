/**
 * intel_mux.c - USB Port Mux support
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/extcon.h>
#include <linux/err.h>

struct intel_usb_mux {
	struct device	*dev;
	char		*cable_name;
	int		(*cable_set_cb)(struct device *dev);
	int		(*cable_unset_cb)(struct device *dev);

	struct notifier_block		nb;
	struct extcon_specific_cable_nb	obj;

	/*
	 * The state of the mux.
	 * 0, 1 - mux switch state
	 * -1   - uninitialized state
	 */
	int				mux_state;

	 /* lock for mux_state */
	struct mutex			mux_mutex;
};

static int usb_mux_change_state(struct intel_usb_mux *mux, int state)
{
	int ret;
	struct device *dev = mux->dev;

	dev_WARN_ONCE(dev,
		      !mutex_is_locked(&mux->mux_mutex),
		      "mutex is unlocked\n");

	mux->mux_state = state;

	if (mux->mux_state)
		ret = mux->cable_set_cb(dev);
	else
		ret = mux->cable_unset_cb(dev);

	return ret;
}

static int usb_mux_notifier(struct notifier_block *nb,
			    unsigned long event, void *ptr)
{
	struct intel_usb_mux *mux;
	int state;
	int ret = NOTIFY_DONE;

	mux = container_of(nb, struct intel_usb_mux, nb);

	state = extcon_get_cable_state(mux->obj.edev,
				       mux->cable_name);

	if (mux->mux_state == -1 || mux->mux_state != state) {
		mutex_lock(&mux->mux_mutex);
		ret = usb_mux_change_state(mux, state);
		mutex_unlock(&mux->mux_mutex);
	}

	return ret;
}

static ssize_t port_mux_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct intel_usb_mux *mux = dev_get_drvdata(dev);

	if (dev_WARN_ONCE(dev, !mux, "mux without data structure\n"))
		return 0;

	return sprintf(buf, "%s\n", mux->mux_state ? "host" : "peripheral");
}

static ssize_t port_mux_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct intel_usb_mux *mux = dev_get_drvdata(dev);
	int state;

	if (dev_WARN_ONCE(dev, !mux, "mux without data structure\n"))
		return -EINVAL;

	if (sysfs_streq(buf, "peripheral"))
		state = 0;
	else if (sysfs_streq(buf, "host"))
		state = 1;
	else
		return -EINVAL;

	mutex_lock(&mux->mux_mutex);
	usb_mux_change_state(mux, state);
	mutex_unlock(&mux->mux_mutex);

	return count;
}
static DEVICE_ATTR_RW(port_mux);

int intel_usb_mux_bind_cable(struct device *dev,
			     char *extcon_name,
			     char *cable_name,
			     int (*cable_set_cb)(struct device *dev),
			     int (*cable_unset_cb)(struct device *dev))
{
	int ret;
	struct intel_usb_mux *mux;

	if (!cable_name)
		return -ENODEV;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	mux->dev = dev;
	mux->cable_name = kstrdup(cable_name, GFP_KERNEL);
	mux->cable_set_cb = cable_set_cb;
	mux->cable_unset_cb = cable_unset_cb;
	mux->nb.notifier_call = usb_mux_notifier;
	mutex_init(&mux->mux_mutex);
	mux->mux_state = -1;
	dev_set_drvdata(dev, mux);
	ret = extcon_register_interest(&mux->obj, extcon_name,
				       cable_name, &mux->nb);
	if (ret) {
		kfree(mux->cable_name);
		dev_err(dev, "failed to register extcon notifier\n");
		return -ENODEV;
	}

	usb_mux_notifier(&mux->nb, 0, NULL);

	/* register the sysfs interface */
	ret = device_create_file(dev, &dev_attr_port_mux);
	if (ret) {
		extcon_unregister_interest(&mux->obj);
		kfree(mux->cable_name);
		dev_err(dev, "failed to create sysfs attribute\n");
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(intel_usb_mux_bind_cable);

int intel_usb_mux_unbind_cable(struct device *dev)
{
	struct intel_usb_mux *mux = dev_get_drvdata(dev);

	device_remove_file(dev, &dev_attr_port_mux);
	extcon_unregister_interest(&mux->obj);
	kfree(mux->cable_name);

	return 0;
}
EXPORT_SYMBOL_GPL(intel_usb_mux_unbind_cable);

#ifdef CONFIG_PM_SLEEP
void intel_usb_mux_complete(struct device *dev)
{
	struct intel_usb_mux *mux = dev_get_drvdata(dev);

	usb_mux_notifier(&mux->nb, 0, NULL);
}
EXPORT_SYMBOL_GPL(intel_usb_mux_complete);
#endif
