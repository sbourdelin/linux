/*
 * Copyright (C) 2017 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _PANEL_COMMON_H_
#define _PANEL_COMMON_H_

struct backlight_device;
struct regulator;
struct i2c_adapter;
struct gpio_desc;

struct panel_common {
	struct device *dev;

	bool prepared;
	bool enabled;

	struct backlight_device *backlight;
	struct regulator *supply;

	struct gpio_desc *enable_gpio;
};

int panel_common_init(struct device *dev, struct panel_common *common,
		      const char *supply_name, const char *gpio_name,
		      const char *backlight_name);
void panel_common_fini(struct panel_common *common);

int panel_common_prepare(struct panel_common *common, unsigned int delay);
int panel_common_unprepare(struct panel_common *common, unsigned int delay);
int panel_common_enable(struct panel_common *common, unsigned int delay);
int panel_common_disable(struct panel_common *common, unsigned int delay);

#endif // _PANEL_COMMON_H_
