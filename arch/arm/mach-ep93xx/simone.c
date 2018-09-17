/*
 * arch/arm/mach-ep93xx/simone.c
 * Simplemachines Sim.One support.
 *
 * Copyright (C) 2010 Ryan Mallon
 *
 * Based on the 2.6.24.7 support:
 *   Copyright (C) 2009 Simplemachines
 *   MMC support by Peter Ivanov <ivanovp@gmail.com>, 2007
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/mmc/host.h>
#include <linux/spi/spi.h>
#include <linux/spi/mmc_spi.h>
#include <linux/platform_data/video-ep93xx.h>
#include <linux/platform_data/spi-ep93xx.h>
#include <linux/gpio/machine.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/gpio_keys.h>
#include <linux/property.h>

#include <mach/hardware.h>
#include <mach/gpio-ep93xx.h>

#include <asm/mach-types.h>
#include <asm/mach/arch.h>

#include "soc.h"

static const struct property_entry simone_key_enter_props[] __initconst = {
	PROPERTY_ENTRY_U32("linux,code",	KEY_ENTER),
	PROPERTY_ENTRY_STRING("label",		"enter"),
	PROPERTY_ENTRY_STRING("gpios",		"enter-gpios"),
	{ }
};

static const struct property_entry simone_key_up_props[] __initconst = {
	PROPERTY_ENTRY_U32("linux,code",	KEY_UP),
	PROPERTY_ENTRY_STRING("label",		"up"),
	PROPERTY_ENTRY_STRING("gpios",		"up-gpios"),
	{ }
};

static const struct property_entry simone_key_up_props[] __initconst = {
	PROPERTY_ENTRY_U32("linux,code",	KEY_LEFT),
	PROPERTY_ENTRY_STRING("label",		"left"),
	PROPERTY_ENTRY_STRING("gpios",		"left-gpios"),
	{ }
};

static const struct property_entry simone_key_props[] __initconst = {
	/* There are no properties at device level on this device */
	{ }
};

static struct gpiod_lookup_table simone_keys_gpiod_table = {
	.dev_id = "gpio-keys",
	.table = {
		/* Use local offsets on gpiochip/port "B" */
		GPIO_LOOKUP_IDX("B", 0, "enter-gpios", 0, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX("B", 1, "up-gpios", 1, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX("B", 2, "left-gpios", 2, GPIO_ACTIVE_LOW),
	},
};

static struct platform_device simone_keys_device = {
	.name = "gpio-keys",
	.id = -1,
};

static struct ep93xx_eth_data __initdata simone_eth_data = {
	.phy_id		= 1,
};

static struct ep93xxfb_mach_info __initdata simone_fb_info = {
	.flags		= EP93XXFB_USE_SDCSN0 | EP93XXFB_PCLK_FALLING,
};

static struct mmc_spi_platform_data simone_mmc_spi_data = {
	.detect_delay	= 500,
	.ocr_mask	= MMC_VDD_32_33 | MMC_VDD_33_34,
	.flags		= MMC_SPI_USE_CD_GPIO,
	.cd_gpio	= EP93XX_GPIO_LINE_EGPIO0,
	.cd_debounce	= 1,
};

static struct spi_board_info simone_spi_devices[] __initdata = {
	{
		.modalias		= "mmc_spi",
		.platform_data		= &simone_mmc_spi_data,
		/*
		 * We use 10 MHz even though the maximum is 3.7 MHz. The driver
		 * will limit it automatically to max. frequency.
		 */
		.max_speed_hz		= 10 * 1000 * 1000,
		.bus_num		= 0,
		.chip_select		= 0,
		.mode			= SPI_MODE_3,
	},
};

/*
 * Up to v1.3, the Sim.One used SFRMOUT as SD card chip select, but this goes
 * low between multi-message command blocks. From v1.4, it uses a GPIO instead.
 * v1.3 parts will still work, since the signal on SFRMOUT is automatic.
 */
static int simone_spi_chipselects[] __initdata = {
	EP93XX_GPIO_LINE_EGPIO1,
};

static struct ep93xx_spi_info simone_spi_info __initdata = {
	.chipselect	= simone_spi_chipselects,
	.num_chipselect	= ARRAY_SIZE(simone_spi_chipselects),
	.use_dma = 1,
};

static struct i2c_board_info __initdata simone_i2c_board_info[] = {
	{
		I2C_BOARD_INFO("ds1337", 0x68),
	},
};

static struct platform_device simone_audio_device = {
	.name		= "simone-audio",
	.id		= -1,
};

static void __init simone_register_audio(void)
{
	ep93xx_register_ac97();
	platform_device_register(&simone_audio_device);
}

static void __init simone_init_machine(void)
{
	ep93xx_init_devices();
	ep93xx_register_flash(2, EP93XX_CS6_PHYS_BASE, SZ_8M);
	ep93xx_register_eth(&simone_eth_data, 1);
	ep93xx_register_fb(&simone_fb_info);
	ep93xx_register_i2c(simone_i2c_board_info,
			    ARRAY_SIZE(simone_i2c_board_info));
	ep93xx_register_spi(&simone_spi_info, simone_spi_devices,
			    ARRAY_SIZE(simone_spi_devices));

	gpiod_add_lookup_table(&simone_keys_gpiod_table);
	device_add_properties(&simone_keys_device.dev,
			      simone_keys_device_props);
	device_add_child_properties(&simone_keys_device.dev,
				    dev_fwnode(&simone_keys_device.dev),
				    simone_key_enter_props);
	device_add_child_properties(&simone_keys_device.dev,
				    dev_fwnode(&simone_keys_device.dev),
				    simone_key_up_props);
	device_add_child_properties(&simone_keys_device.dev,
				    dev_fwnode(&simone_keys_device.dev),
				    simone_key_left_props);
	platform_device_register(&simone_keys_device);

	simone_register_audio();
}

MACHINE_START(SIM_ONE, "Simplemachines Sim.One Board")
	/* Maintainer: Ryan Mallon */
	.atag_offset	= 0x100,
	.map_io		= ep93xx_map_io,
	.init_irq	= ep93xx_init_irq,
	.init_time	= ep93xx_timer_init,
	.init_machine	= simone_init_machine,
	.init_late	= ep93xx_init_late,
	.restart	= ep93xx_restart,
MACHINE_END
