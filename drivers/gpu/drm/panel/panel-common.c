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

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_panel.h>

#include "panel-common.h"

int panel_common_init(struct device *dev, struct panel_common *common,
		      const char *supply_name, const char *gpio_name,
		      const char *backlight_name)
{
	struct device_node *backlight;
	int err;

	common->dev = dev;
	common->enabled = false;
	common->prepared = false;

	common->supply = devm_regulator_get(dev, supply_name);
	if (IS_ERR(common->supply))
		return PTR_ERR(common->supply);

	common->enable_gpio = devm_gpiod_get_optional(dev, gpio_name,
						     GPIOD_OUT_LOW);
	if (IS_ERR(common->enable_gpio)) {
		err = PTR_ERR(common->enable_gpio);
		dev_err(dev, "failed to request GPIO: %d\n", err);
		return err;
	}

	backlight = of_parse_phandle(dev->of_node, backlight_name, 0);
	if (backlight) {
		common->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!common->backlight)
			return -EPROBE_DEFER;
	}

	return 0;
}
EXPORT_SYMBOL(panel_common_init);

void panel_common_fini(struct panel_common *common)
{
	if (common->backlight)
		put_device(&common->backlight->dev);
}
EXPORT_SYMBOL(panel_common_fini);

int panel_common_prepare(struct panel_common *common, unsigned int delay)
{
	int err;

	if (common->prepared)
		return 0;

	err = regulator_enable(common->supply);
	if (err < 0) {
		dev_err(common->dev, "failed to enable supply: %d\n", err);
		return err;
	}

	if (common->enable_gpio)
		gpiod_set_value_cansleep(common->enable_gpio, 1);

	if (delay)
		msleep(delay);

	common->prepared = true;

	return 0;
}
EXPORT_SYMBOL(panel_common_prepare);

int panel_common_unprepare(struct panel_common *common, unsigned int delay)
{
	if (!common->prepared)
		return 0;

	if (common->enable_gpio)
		gpiod_set_value_cansleep(common->enable_gpio, 0);

	regulator_disable(common->supply);

	if (delay)
		msleep(delay);

	common->prepared = false;

	return 0;
}
EXPORT_SYMBOL(panel_common_unprepare);

int panel_common_enable(struct panel_common *common, unsigned int delay)
{
	if (common->enabled)
		return 0;

	if (delay)
		msleep(delay);

	if (common->backlight) {
		common->backlight->props.state &= ~BL_CORE_FBBLANK;
		common->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(common->backlight);
	}

	common->enabled = true;

	return 0;
}
EXPORT_SYMBOL(panel_common_enable);

int panel_common_disable(struct panel_common *common, unsigned int delay)
{
	if (!common->enabled)
		return 0;

	if (common->backlight) {
		common->backlight->props.power = FB_BLANK_POWERDOWN;
		common->backlight->props.state |= BL_CORE_FBBLANK;
		backlight_update_status(common->backlight);
	}

	if (delay)
		msleep(delay);

	common->enabled = false;

	return 0;
}
EXPORT_SYMBOL(panel_common_disable);
