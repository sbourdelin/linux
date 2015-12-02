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

#define GET_DBC_EXT_CAP_OFFSET(h)	\
		xhci_find_next_ext_cap(&(h)->cap_regs->hc_capbase, \
		0, XHCI_EXT_CAPS_DEBUG)

static ssize_t debug_port_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int		count = 0, offset;
	char		*state;
	void __iomem	*dbc_base;
	u32		dcctrl_reg;
	struct xhci_hcd	*xhci = hcd_to_xhci(dev_get_drvdata(dev));

	offset = GET_DBC_EXT_CAP_OFFSET(xhci);
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

	if (GET_DBC_EXT_CAP_OFFSET(xhci))
		return device_create_file(dev, &dev_attr_debug_port_state);

	return 0;
}

void xhci_sysfs_remove_files(struct xhci_hcd *xhci)
{
	struct device *dev = xhci_to_hcd(xhci)->self.controller;

	if (GET_DBC_EXT_CAP_OFFSET(xhci))
		device_remove_file(dev, &dev_attr_debug_port_state);
}
