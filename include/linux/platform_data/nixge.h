/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018 National Instruments Corp.
 *
 * Author: Moritz Fischer <mdf@kernel.org>
 */

#ifndef __NIXGE_PDATA_H__
#define __NIXGE_PDATA_H__

#include <linux/phy.h>

struct nixge_platform_data {
	phy_interface_t phy_interface;
	int phy_speed;
	int phy_duplex;
};

#endif /* __NIXGE_PDATA_H__ */

