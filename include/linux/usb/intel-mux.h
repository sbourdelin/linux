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

#ifndef __LINUX_USB_INTEL_MUX_H
#define __LINUX_USB_INTEL_MUX_H

#include <linux/usb.h>

struct intel_mux_dev {
	struct device	*dev;
	char		*extcon_name;
	char		*cable_name;
	int		(*cable_set_cb)(struct intel_mux_dev *mux);
	int		(*cable_unset_cb)(struct intel_mux_dev *mux);
};

#if IS_ENABLED(CONFIG_INTEL_USB_MUX)
extern int intel_usb_mux_register(struct intel_mux_dev *mux);
extern int intel_usb_mux_unregister(struct device *dev);

#ifdef CONFIG_PM_SLEEP
extern void intel_usb_mux_complete(struct device *dev);
#endif

#else
static inline int intel_usb_mux_register(struct intel_mux_dev *mux)
{
	return -ENODEV;
}

static inline int intel_usb_mux_unregister(struct device *dev)
{
	return 0;
}

#endif /* CONFIG_INTEL_USB_MUX */

#endif /* __LINUX_USB_INTEL_MUX_H */
