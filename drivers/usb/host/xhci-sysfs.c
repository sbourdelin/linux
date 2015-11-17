/*
 * sysfs interface for xHCI host controller driver
 *
 * Copyright (C) 2015 Intel Corp.
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>

#include "xhci.h"

/*
 * Return the register offset of a extended capability specified
 * by @cap_id. Return 0 if @cap_id capability is not supported or
 * in error cases.
 */
static int get_extended_capability_offset(struct xhci_hcd *xhci,
					int cap_id)
{
	int		offset;
	void __iomem	*base = (void __iomem *) xhci->cap_regs;
	struct usb_hcd	*hcd = xhci_to_hcd(xhci);

	offset = xhci_find_next_cap_offset(base, XHCI_HCC_PARAMS_OFFSET);
	if (!HCD_HW_ACCESSIBLE(hcd) || !offset)
		return 0;

	return xhci_find_ext_cap_by_id(base, offset, cap_id);
}

static ssize_t debug_port_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int		count = 0, offset;
	char		*state;
	void __iomem	*dbc_base;
	u32		dcctrl_reg;
	struct xhci_hcd	*xhci = hcd_to_xhci(dev_get_drvdata(dev));

	offset = get_extended_capability_offset(xhci, XHCI_EXT_CAPS_DEBUG);
	if (!offset)
		return 0;

	dbc_base = (void __iomem *) xhci->cap_regs + offset;
	dcctrl_reg = readl(dbc_base + XHCI_DBC_DCCTRL);

	if (!(dcctrl_reg & XHCI_DBC_DCCTRL_DCE))
		state = "disabled";
	else if (dcctrl_reg & XHCI_DBC_DCCTRL_DCR)
		state = "configured";
	else
		state = "enabled";

	count = scnprintf(buf, PAGE_SIZE, "%s\n", state);

	return count;
}
static DEVICE_ATTR_RO(debug_port_state);

int xhci_sysfs_create_files(struct xhci_hcd *xhci)
{
	struct device *dev = xhci_to_hcd(xhci)->self.controller;

	if (get_extended_capability_offset(xhci, XHCI_EXT_CAPS_DEBUG))
		return device_create_file(dev, &dev_attr_debug_port_state);

	return 0;
}

void xhci_sysfs_remove_files(struct xhci_hcd *xhci)
{
	struct device *dev = xhci_to_hcd(xhci)->self.controller;

	if (get_extended_capability_offset(xhci, XHCI_EXT_CAPS_DEBUG))
		device_remove_file(dev, &dev_attr_debug_port_state);
}
