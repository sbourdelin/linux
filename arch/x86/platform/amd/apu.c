// SPDX-License-Identifier: GPL-2.0
/*
 * System Specific setup for PC-Engines APU2/APU3 devices
 *
 * Copyright (C) 2018 Florian Eckert <fe@dev.tdt.de>
 */

#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/gpio_keys.h>
#include <linux/dmi.h>

static struct gpio_keys_button apu_gpio_buttons[] = {
	{
		.code			= KEY_RESTART,
		.gpio			= 20,
		.active_low		= 1,
		.desc			= "Reset button",
		.type			= EV_KEY,
		.debounce_interval	= 60,
	}
};

static struct gpio_keys_platform_data apu_buttons_data = {
	.buttons		= apu_gpio_buttons,
	.nbuttons		= ARRAY_SIZE(apu_gpio_buttons),
	.poll_interval		= 20,
};

static struct platform_device apu_buttons_dev = {
	.name			= "gpio-keys-polled",
	.id			= 1,
	.dev = {
		.platform_data		= &apu_buttons_data,
	}
};

static struct platform_device *apu_devs[] __initdata = {
		&apu_buttons_dev,
};

static void __init register_apu(void)
{
	/* Setup push button control through gpio-apu driver */
	platform_add_devices(apu_devs, ARRAY_SIZE(apu_devs));
}

static int __init apu_init(void)
{
	if (!dmi_match(DMI_SYS_VENDOR, "PC Engines")) {
		pr_err("No PC Engines board detected\n");
		return -ENODEV;
	}

	if (!(dmi_match(DMI_PRODUCT_NAME, "APU2") ||
	      dmi_match(DMI_PRODUCT_NAME, "apu2") ||
	      dmi_match(DMI_PRODUCT_NAME, "PC Engines apu2") ||
	      dmi_match(DMI_PRODUCT_NAME, "APU3") ||
	      dmi_match(DMI_PRODUCT_NAME, "apu3") ||
	      dmi_match(DMI_PRODUCT_NAME, "PC Engines apu3"))) {
		pr_err("Unknown PC Engines board: %s\n",
			dmi_get_system_info(DMI_PRODUCT_NAME));
		return -ENODEV;
	}

	register_apu();

	return 0;
}

device_initcall(apu_init);
