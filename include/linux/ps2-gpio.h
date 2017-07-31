/*
 * ps2-gpio interface to platform code
 *
 * Author: Danilo Krummrich <danilokrummrich@dk-develop.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _LINUX_PS2_GPIO_H
#define _LINUX_PS2_GPIO_H

/**
 * struct ps2_gpio_platform_data - Platform-dependent data for ps2-gpio
 * @gpio_data: GPIO pin ID to use for DATA
 * @gpio_clk: GPIO pin ID to use for CLOCK
 * @write_enable: Indicates whether write function is provided to serio
 *	device. Most probably providing the write fn will not work,
 *	because of the tough timing libps2 requires.
 */
struct ps2_gpio_platform_data {
	unsigned int	gpio_data;
	unsigned int	gpio_clk;
	unsigned int	write_enable;
};

#endif /* _LINUX_PS2_GPIO_H */
