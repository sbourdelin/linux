/*
 * phy-da8xx-usb.h - TI DA8xx USB PHY driver
 *
 * Copyright (C) 2016 David Lechner <david@lechnology.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __PHY_DA8XX_USB_H
#define __PHY_DA8XX_USB_H

#include <linux/usb/musb.h>

extern int da8xx_usb20_phy_set_mode(struct phy *phy, enum musb_mode mode);

#endif /* __PHY_DA8XX_USB_H */
