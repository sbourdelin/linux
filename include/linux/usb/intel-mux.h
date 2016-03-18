/**
 * intel_mux.h - USB Port Mux definitions
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

#if IS_ENABLED(CONFIG_INTEL_USB_MUX)
int intel_usb_mux_bind_cable(struct device *dev, const char *extcon_name,
			     int (*cable_set_cb)(struct device *dev),
			     int (*cable_unset_cb)(struct device *dev));
int intel_usb_mux_unbind_cable(struct device *dev);
#ifdef CONFIG_PM_SLEEP
void intel_usb_mux_complete(struct device *dev);
#endif

#else
static inline int
int intel_usb_mux_bind_cable(struct device *dev, const char *extcon_name,
			     int (*cable_set_cb)(struct device *dev),
			     int (*cable_unset_cb)(struct device *dev))
{
	return -ENODEV;
}

static inline int intel_usb_mux_unbind_cable(struct device *dev)
{
	return 0;
}
#endif /* CONFIG_INTEL_USB_MUX */

#endif /* __LINUX_USB_INTEL_MUX_H */
