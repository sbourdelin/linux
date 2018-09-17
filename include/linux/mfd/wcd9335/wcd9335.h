/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018, Linaro Limited */

#ifndef __WCD9335_H__
#define __WCD9335_H__

#include <linux/slimbus.h>
#include <linux/regulator/consumer.h>

#define WCD9335_VERSION_2_0     2
#define WCD9335_MAX_SUPPLY	5

enum wcd_interface_type {
	WCD9335_INTERFACE_TYPE_SLIMBUS = 1,
	WCD9335_INTERFACE_TYPE_I2C,
};

/**
 * struct wcd9335 - wcd9335 device handle.
 * @version: Version of codec chip
 * @reset_gpio: rest gpio
 * @intf_type: Interface type, which can be SLIMBUS or I2C
 * @dev: wcd9335 evice instance
 * @mclk: MCLK clock handle.
 * @slim: wcd9335 slim device handle.
 * @slim_interface_dev: wcd9335 slim interface device handle
 * @regmap: wcd9335 slim device regmap
 * @interface_dev_regmap: wcd9335 interface device regmap.
 * @supplies: voltage supplies required for wcd9335
 */
struct wcd9335 {
	int version;
	int reset_gpio;
	enum wcd_interface_type intf_type;
	struct device *dev;
	struct clk *mclk;
	struct clk *native_clk;
	struct slim_device *slim;
	struct slim_device *slim_interface_dev;
	struct regmap *regmap;
	struct regmap *interface_dev_regmap;
	struct regulator_bulk_data supplies[WCD9335_MAX_SUPPLY];
};

#endif /* __WCD9335_H__ */
