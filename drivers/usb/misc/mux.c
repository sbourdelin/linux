/**
 * mux.c - USB Port Mux support
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/notifier.h>
#include <linux/usb/mux.h>
#include <linux/debugfs.h>
#include <linux/err.h>

static int usb_mux_change_state(struct usb_mux *mux, int state)
{
	int ret;
	struct usb_mux_dev *umdev = mux->umdev;

	dev_WARN_ONCE(umdev->dev, !mutex_is_locked(&mux->mux_mutex),
			"mutex is unlocked\n");

	mux->mux_state = state;

	if (mux->mux_state)
		ret = umdev->cable_set_cb(umdev);
	else
		ret = umdev->cable_unset_cb(umdev);

	return ret;
}

static int usb_mux_notifier(struct notifier_block *nb,
		unsigned long event, void *ptr)
{
	struct usb_mux *mux;
	int state;
	int ret = NOTIFY_DONE;

	mux = container_of(nb, struct usb_mux, nb);

	state = extcon_get_cable_state(mux->obj.edev,
			mux->umdev->cable_name);

	if (mux->mux_state == -1 || mux->mux_state != state) {
		mutex_lock(&mux->mux_mutex);
		ret = usb_mux_change_state(mux, state);
		mutex_unlock(&mux->mux_mutex);
	}

	return ret;
}

static ssize_t mux_debug_read(struct file *file, char __user *user_buf,
		size_t len, loff_t *offset)
{
	struct usb_mux *mux = file->private_data;
	char output_buf[16];

	memset(output_buf, 0, sizeof(output_buf));
	if (mux->mux_state)
		strcpy(output_buf, "host\n");
	else
		strcpy(output_buf, "peripheral\n");

	return simple_read_from_buffer(user_buf, len, offset,
			output_buf, strlen(output_buf));
}

static ssize_t mux_debug_write(struct file *file, const char __user *user_buf,
		size_t count, loff_t *offset)
{
	struct usb_mux *mux = file->private_data;
	char input_buf[16];
	int size, state;

	size = min(count, sizeof(input_buf) - 1);
	memset(input_buf, 0, sizeof(input_buf));
	if (strncpy_from_user(input_buf, user_buf, size) < 0)
		return -EFAULT;

	if (!strncmp(input_buf, "host", 4))
		state = 1;
	else if (!strncmp(input_buf, "peripheral", 10))
		state = 0;
	else
		state = -1;

	if (state != -1) {
		mutex_lock(&mux->mux_mutex);
		usb_mux_change_state(mux, state);
		mutex_unlock(&mux->mux_mutex);
	}

	return count;
}

static const struct file_operations mux_debug_fops = {
	.read =		mux_debug_read,
	.write =	mux_debug_write,
	.open =		simple_open,
	.llseek =	default_llseek,
};

int usb_mux_register(struct usb_mux_dev *umdev)
{
	int ret;
	struct device *dev = umdev->dev;
	struct usb_mux *mux;

	if (!umdev->cable_name)
		return -ENODEV;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	mux->umdev = umdev;
	mux->nb.notifier_call = usb_mux_notifier;
	mutex_init(&mux->mux_mutex);
	mux->mux_state = -1;
	dev_set_drvdata(dev, mux);

	ret = extcon_register_interest(&mux->obj, umdev->extcon_name,
			umdev->cable_name, &mux->nb);
	if (ret) {
		dev_err(dev, "failed to register extcon notifier\n");
		return -ENODEV;
	}

	usb_mux_notifier(&mux->nb, 0, NULL);

	mux->debug_file = debugfs_create_file("usb_mux", 0600,
			usb_debug_root, mux, &mux_debug_fops);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_mux_register);

int usb_mux_unregister(struct device *dev)
{
	struct usb_mux *mux = dev_get_drvdata(dev);

	debugfs_remove(mux->debug_file);
	extcon_unregister_interest(&mux->obj);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_mux_unregister);

struct usb_mux_dev *usb_mux_get_dev(struct device *dev)
{
	struct usb_mux *mux = dev_get_drvdata(dev);

	if (mux)
		return mux->umdev;

	return NULL;
}
EXPORT_SYMBOL_GPL(usb_mux_get_dev);

#ifdef CONFIG_PM_SLEEP
void usb_mux_complete(struct device *dev)
{
	struct usb_mux *mux = dev_get_drvdata(dev);

	usb_mux_notifier(&mux->nb, 0, NULL);
}
EXPORT_SYMBOL_GPL(usb_mux_complete);
#endif
