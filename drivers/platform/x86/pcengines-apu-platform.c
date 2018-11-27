// SPDX-License-Identifier: GPL-2.0
/*
 * System Specific setup for PC-Engines APU2/APU3 devices
 *
 * Copyright (C) 2018 Florian Eckert <fe@dev.tdt.de>
 */

#include <linux/dmi.h>
#include <linux/gpio_keys.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static const struct dmi_system_id apu2_gpio_dmi_table[] __initconst = {
	/* PC Engines APU2 with "Legacy" bios < 4.0.8 */
	{
		.ident = "apu2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "APU2")
		}
	},
	/* PC Engines APU2 with "Legacy" bios >= 4.0.8 */
	{
		.ident = "apu2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "apu2")
		}
	},
	/* PC Engines APU2 with "Mainline" bios */
	{
		.ident = "apu2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "PC Engines apu2")
		}
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, apu2_gpio_dmi_table);

static const struct dmi_system_id apu3_gpio_dmi_table[] __initconst = {
	/* PC Engines APU3 with "Legacy" bios < 4.0.8 */
	{
		.ident = "apu3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "APU3")
		}
	},
	/* PC Engines APU3 with "Legacy" bios >= 4.0.8 */
	{
		.ident = "apu3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "apu3")
		}
	},
	/* PC Engines APU3 with "Mainline" bios */
	{
		.ident = "apu3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PC Engines"),
			DMI_MATCH(DMI_BOARD_NAME, "PC Engines apu3")
		}
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, apu3_gpio_dmi_table);


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

static struct platform_device apu_button_dev = {
	.name			= "gpio-keys-polled",
	.id			= 1,
	.dev = {
		.platform_data		= &apu_buttons_data,
	}
};

static int __init apu_init(void)
{
	if (!(dmi_check_system(apu2_gpio_dmi_table)) &&
		!(dmi_check_system(apu3_gpio_dmi_table))) {
		return -ENODEV;
	}

	return platform_device_register(&apu_button_dev);
}

static void __exit apu_exit(void)
{
	platform_device_unregister(&apu_button_dev);
}

module_init(apu_init);
module_exit(apu_exit);
