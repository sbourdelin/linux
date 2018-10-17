/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * USB roothub wrapper
 *
 * Copyright (C) 2018 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 */

#include <linux/usb.h>
#include <linux/usb/hcd.h>

#ifndef __USB_CORE_PHY_H_
#define __USB_CORE_PHY_H_

struct device;
struct usb_phy_roothub;

struct usb_phy_roothub *usb_phy_roothub_alloc(struct usb_hcd *hcd);

int usb_phy_roothub_init(struct usb_phy_roothub *phy_roothub);
int usb_phy_roothub_exit(struct usb_phy_roothub *phy_roothub);

int usb_phy_roothub_power_on(struct usb_phy_roothub *phy_roothub);
void usb_phy_roothub_power_off(struct usb_phy_roothub *phy_roothub);

int usb_phy_roothub_suspend(struct usb_hcd *hcd,
			    struct usb_phy_roothub *phy_roothub);
int usb_phy_roothub_resume(struct usb_hcd *hcd,
			   struct usb_phy_roothub *phy_roothub);

#endif /* __USB_CORE_PHY_H_ */
