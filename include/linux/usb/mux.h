/**
 * mux.h - USB Port Mux defines
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_USB_MUX_H
#define __LINUX_USB_MUX_H

#include <linux/extcon.h>
#include <linux/usb.h>

struct usb_mux_dev {
	struct device	*dev;
	char		*extcon_name;
	char		*cable_name;
	int		(*cable_set_cb)(struct usb_mux_dev *mux);
	int		(*cable_unset_cb)(struct usb_mux_dev *mux);
};

struct usb_mux {
	struct usb_mux_dev		*umdev;
	struct notifier_block		nb;
	struct extcon_specific_cable_nb	obj;

	/*
	 * The state of the mux.
	 * 0, 1 - mux switch state
	 * -1   - uninitialized state
	 *
	 * mux_mutex is lock to protect mux_state
	 */
	int				mux_state;
	struct mutex			mux_mutex;

	struct dentry			*debug_file;
};

#if IS_ENABLED(CONFIG_USB_MUX)
extern int usb_mux_register(struct usb_mux_dev *mux);
extern int usb_mux_unregister(struct device *dev);
extern struct usb_mux_dev *usb_mux_get_dev(struct device *dev);

#ifdef CONFIG_PM_SLEEP
extern void usb_mux_complete(struct device *dev);
#endif

#else /* CONFIG_USB_MUX */
static inline int usb_mux_register(struct usb_mux_dev *mux)
{
	return -ENODEV;
}

static inline int usb_mux_unregister(struct device *dev)
{
	return 0;
}

static inline struct usb_mux_dev *usb_mux_get_dev(struct device *dev)
{
	return NULL;
}
#endif /* CONFIG_USB_MUX */

#endif /* __LINUX_USB_MUX_H */
