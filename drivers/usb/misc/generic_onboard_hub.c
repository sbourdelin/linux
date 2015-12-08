/*
 * usb_hub_generic.c	The generic onboard USB HUB driver
 *
 * Copyright (C) 2015 Freescale Semiconductor, Inc.
 * Author: Peter Chen <peter.chen@freescale.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This driver is only for the USB HUB devices which need to control
 * their external pins(clock, reset, etc), and these USB HUB devices
 * are soldered at the board.
 */

#define DEBUG
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/usb/generic_onboard_hub.h>

struct usb_hub_generic_data {
	struct clk *clk;
};

static int usb_hub_generic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct usb_hub_generic_platform_data *pdata = dev->platform_data;
	struct usb_hub_generic_data *hub_data;
	int reset_pol = 0, duration_us = 50, ret = 0;
	struct gpio_desc *gpiod_reset = NULL;

	hub_data = devm_kzalloc(dev, sizeof(*hub_data), GFP_KERNEL);
	if (!hub_data)
		return -ENOMEM;

	if (dev->of_node) {
		struct device_node *node = dev->of_node;

		hub_data->clk = devm_clk_get(dev, "external_clk");
		if (IS_ERR(hub_data->clk)) {
			dev_dbg(dev, "Can't get external clock: %ld\n",
					PTR_ERR(hub_data->clk));
		}

		/*
		 * Try to get the information for HUB reset, the
		 * default setting like below:
		 *
		 * - Reset state is low
		 * - The duration is 50us
		 */
		if (of_find_property(node, "hub-reset-active-high", NULL))
			reset_pol = 1;

		of_property_read_u32(node, "hub-reset-duration-us",
			&duration_us);

		gpiod_reset = devm_gpiod_get_optional(dev, "hub-reset",
			reset_pol ? GPIOD_OUT_HIGH : GPIOD_OUT_LOW);
		ret = PTR_ERR_OR_ZERO(gpiod_reset);
		if (ret) {
			dev_err(dev, "Failed to get reset gpio, err = %d\n",
				ret);
			return ret;
		}
	} else if (pdata) {
		hub_data->clk = pdata->ext_clk;
		duration_us = pdata->gpio_reset_duration_us;
		reset_pol = pdata->gpio_reset_polarity;

		if (gpio_is_valid(pdata->gpio_reset)) {
			ret = devm_gpio_request_one(
				dev, pdata->gpio_reset, reset_pol
				? GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW,
				dev_name(dev));
			if (!ret)
				gpiod_reset = gpio_to_desc(pdata->gpio_reset);
		}
	}

	if (!IS_ERR(hub_data->clk)) {
		ret = clk_prepare_enable(hub_data->clk);
		if (ret) {
			dev_err(dev,
				"Can't enable external clock: %d\n",
				ret);
			return ret;
		}
	}

	if (gpiod_reset) {
		/* Sanity check */
		if (duration_us > 1000000)
			duration_us = 50;
		usleep_range(duration_us, duration_us + 100);
		gpiod_set_value(gpiod_reset, reset_pol ? 0 : 1);
	}

	dev_set_drvdata(dev, hub_data);
	return ret;
}

static int usb_hub_generic_remove(struct platform_device *pdev)
{
	struct usb_hub_generic_data *hub_data = platform_get_drvdata(pdev);

	if (!IS_ERR(hub_data->clk))
		clk_disable_unprepare(hub_data->clk);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id usb_hub_generic_dt_ids[] = {
	{.compatible = "generic-onboard-hub"},
	{ },
};
MODULE_DEVICE_TABLE(of, usb_hub_generic_dt_ids);
#endif

static struct platform_driver usb_hub_generic_driver = {
	.probe = usb_hub_generic_probe,
	.remove = usb_hub_generic_remove,
	.driver = {
		.name = "usb_hub_generic_onboard",
		.of_match_table = usb_hub_generic_dt_ids,
	 },
};

static int __init usb_hub_generic_init(void)
{
	return platform_driver_register(&usb_hub_generic_driver);
}
subsys_initcall(usb_hub_generic_init);

static void __exit usb_hub_generic_exit(void)
{
	platform_driver_unregister(&usb_hub_generic_driver);
}
module_exit(usb_hub_generic_exit);

MODULE_AUTHOR("Peter Chen <peter.chen@freescale.com>");
MODULE_DESCRIPTION("Generic Onboard USB HUB driver");
MODULE_LICENSE("GPL");
