/*
 * Freescale SoC USB 2.0 PHY driver
 *
 * Copyright 2016 Freescale Semiconductor, Inc.
 * Author: Rajesh Bhagat <rajesh.bhagat@nxp.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#ifndef _PHY_QORIQ_USB2_H
#define _PHY_QORIQ_USB2_H

#include <linux/clk.h>
#include <linux/phy/phy.h>
#include <linux/device.h>
#include <linux/regmap.h>

#define ULPI_VIEWPORT           0x170

enum qoriq_usb2_phy_ver {
	QORIQ_PHY_LEGACY,
	QORIQ_PHY_NXP_ISP1508,
	QORIQ_PHY_UNKNOWN,
};

struct qoriq_usb2_phy_ctx {
	struct phy *phy;
	struct clk *clk;
	struct device *dev;
	void __iomem *regs;
	struct usb_phy *ulpi_phy;
	enum usb_phy_interface phy_type;
	enum qoriq_usb2_phy_ver phy_version;
};

#ifdef CONFIG_USB_ULPI_VIEWPORT
static inline struct usb_phy *qoriq_otg_ulpi_create(unsigned int flags)
{
	return otg_ulpi_create(&ulpi_viewport_access_ops, flags);
}
#else
static inline struct usb_phy *qoriq_otg_ulpi_create(unsigned int flags)
{
	return NULL;
}
#endif

#endif
