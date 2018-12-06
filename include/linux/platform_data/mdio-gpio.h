/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MDIO-GPIO bus platform data structure
 */

#ifndef __LINUX_MDIO_GPIO_PDATA_H
#define __LINUX_MDIO_GPIO_PDATA_H

struct mdio_gpio_platform_data {
	int phy_mask;
	int phy_ignore_ta_mask;
};

#endif /* __LINUX_MDIO_GPIO_PDATA_H */
