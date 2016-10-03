/*
 *  HAPROXY Aloha Pocket board support
 *
 *  Copyright (C) 2011 Gabor Juhos <juhosg@openwrt.org>
 *  Copyright (C) 2016 Neil Armstrong <narmstrong@baylibre.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 */

#include "machtypes.h"
#include "dev-gpio-buttons.h"
#include "dev-leds-gpio.h"
#include "dev-spi.h"
#include "dev-usb.h"
#include "dev-wmac.h"

#define ALOHA_POCKET_GPIO_LED_WLAN		0
#define ALOHA_POCKET_GPIO_LED_LAN		13

#define ALOHA_POCKET_GPIO_BTN_RESET		11

#define ALOHA_POCKET_KEYS_POLL_INTERVAL	20	/* msecs */
#define ALOHA_POCKET_KEYS_DEBOUNCE_INTERVAL	\
					(3 * ALOHA_POCKET_KEYS_POLL_INTERVAL)

#define ALOHA_POCKET_CAL_DATA_ADDR	0x1fff1000

static struct gpio_led aloha_pocket_leds_gpio[] __initdata = {
	{
		.name		= "aloha-pocket:red:wlan",
		.gpio		= ALOHA_POCKET_GPIO_LED_WLAN,
		.active_low	= 0,
	},
	{
		.name		= "aloha-pocket:green:lan",
		.gpio		= ALOHA_POCKET_GPIO_LED_LAN,
		.active_low	= 0,
		.default_state	= 1,
	},
};

static struct gpio_keys_button aloha_pocket_gpio_keys[] __initdata = {
	{
		.desc		= "reset button",
		.type		= EV_KEY,
		.code		= KEY_RESTART,
		.debounce_interval = ALOHA_POCKET_KEYS_DEBOUNCE_INTERVAL,
		.gpio		= ALOHA_POCKET_GPIO_BTN_RESET,
		.active_low	= 0,
	}
};

static struct spi_board_info aloha_pocket_spi_info[] = {
	{
		.bus_num	= 0,
		.chip_select	= 0,
		.max_speed_hz	= 25000000,
		.modalias	= "mx25l1606e",
	}
};

static struct ath79_spi_platform_data aloha_pocket_spi_data = {
	.bus_num	= 0,
	.num_chipselect = 1,
};

static void __init aloha_pocket_setup(void)
{
	u8 *cal_data = (u8 *) KSEG1ADDR(ALOHA_POCKET_CAL_DATA_ADDR);

	ath79_register_leds_gpio(-1, ARRAY_SIZE(aloha_pocket_leds_gpio),
				 aloha_pocket_leds_gpio);
	ath79_register_gpio_keys_polled(-1, ALOHA_POCKET_KEYS_POLL_INTERVAL,
					ARRAY_SIZE(aloha_pocket_gpio_keys),
					aloha_pocket_gpio_keys);

	ath79_register_spi(&aloha_pocket_spi_data, aloha_pocket_spi_info,
			   ARRAY_SIZE(aloha_pocket_spi_info));
	ath79_register_usb();
	ath79_register_wmac(cal_data);
}

MIPS_MACHINE(ATH79_MACH_ALOHA_POCKET, "ALOHA-Pocket",
	     "HAPROXY ALOHA Pocket board", aloha_pocket_setup);
