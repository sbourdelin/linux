/*
 * Copyright (c) 2005-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2017 Qualcomm Atheros, Inc.
 * Copyright (c) 2018 Sebastian Gottschall <s.gottschall@dd-wrt.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/of.h>
#include <linux/dmi.h>
#include <linux/ctype.h>
#include <asm/byteorder.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/version.h>


#include "core.h"
#include "wmi.h"
#include "wmi-ops.h"

#if IS_ENABLED(CONFIG_GPIOLIB)
static int ath10k_gpio_pin_cfg_input(struct gpio_chip *chip, unsigned offset)
{
	struct ath10k_gpiocontrol *gpio = container_of(chip, struct ath10k_gpiocontrol, gchip);
	ath10k_wmi_gpio_config(gpio->ar, offset, 1, WMI_GPIO_PULL_NONE, WMI_GPIO_INTTYPE_DISABLE); /* configure to input */
	gpio->gpio_state_dir = 1;
	return 0;
}

/* gpio_chip handler : set GPIO to output */
static int ath10k_gpio_pin_cfg_output(struct gpio_chip *chip, unsigned offset,
				     int value)
{
	struct ath10k_gpiocontrol *gpio = container_of(chip, struct ath10k_gpiocontrol, gchip);

	ath10k_wmi_gpio_config(gpio->ar, offset, 0, WMI_GPIO_PULL_NONE, WMI_GPIO_INTTYPE_DISABLE); /* configure to output */
	ath10k_wmi_gpio_output(gpio->ar, offset, value);
	gpio->gpio_state_dir = 0;
	gpio->gpio_state_pin = value;
	return 0;
}

/* gpio_chip handler : query GPIO direction (0=out, 1=in) */
static int ath10k_gpio_pin_get_dir(struct gpio_chip *chip, unsigned offset)
{
	struct ath10k_gpiocontrol *gpio = container_of(chip, struct ath10k_gpiocontrol, gchip);

	return gpio->gpio_state_dir;
}

/* gpio_chip handler : get GPIO pin value */
static int ath10k_gpio_pin_get(struct gpio_chip *chip, unsigned offset)
{
	struct ath10k_gpiocontrol *gpio = container_of(chip, struct ath10k_gpiocontrol, gchip);

	return gpio->gpio_state_pin;
}

/* gpio_chip handler : set GPIO pin to value */
static void ath10k_gpio_pin_set(struct gpio_chip *chip, unsigned offset,
			       int value)
{
	struct ath10k_gpiocontrol *gpio = container_of(chip, struct ath10k_gpiocontrol, gchip);

	ath10k_wmi_gpio_output(gpio->ar, offset, value);
	gpio->gpio_state_pin = value;
}

/* register GPIO chip */
static int ath10k_register_gpio_chip(struct ath10k *ar)
{
 
	struct ath10k_gpiocontrol *gpio = ar->gpio;
	if (!gpio) {
		return -ENODEV;
	}
	gpio->gchip.parent = ar->dev;
	gpio->gchip.base = -1;	/* determine base automatically */
	gpio->gchip.ngpio = ar->hw_params.gpio_count;
	gpio->gchip.label = gpio->label;
	gpio->gchip.direction_input = ath10k_gpio_pin_cfg_input;
	gpio->gchip.direction_output = ath10k_gpio_pin_cfg_output;
	gpio->gchip.get_direction = ath10k_gpio_pin_get_dir;
	gpio->gchip.get = ath10k_gpio_pin_get;
	gpio->gchip.set = ath10k_gpio_pin_set;

	if (gpiochip_add(&gpio->gchip)) {
		dev_err(ar->dev, "Error while registering gpio chip\n");
		return -ENODEV;
	}
	gpio->gchip.owner = NULL;
	gpio->ar = ar;
	return 0;
}

/* remove GPIO chip */
void ath10k_unregister_gpio_chip(struct ath10k *ar)
{
	struct ath10k_gpiocontrol *gpio = ar->gpio; 
	if (gpio) {
		gpiochip_remove(&gpio->gchip);
	}
}

int ath10k_attach_gpio(struct ath10k *ar)
{
	if (ar->hw_params.led_pin) { /* only attach if non zero since some chipsets are unsupported yet */
		return ath10k_register_gpio_chip(ar);
	}

	return -ENODEV;
}

#endif


static void ath10k_led_brightness(struct led_classdev *led_cdev,
			       enum led_brightness brightness)
{
	struct ath10k_gpiocontrol *gpio = container_of(led_cdev, struct ath10k_gpiocontrol, cdev);
	struct gpio_led *led = &gpio->wifi_led;
	if (gpio->ar->state == ATH10K_STATE_ON) {
		gpio->gpio_state_pin = (brightness != LED_OFF) ^ led->active_low;
		ath10k_wmi_gpio_output(gpio->ar, led->gpio, gpio->gpio_state_pin);
	}
}

static int ath10k_add_led(struct ath10k *ar, struct gpio_led *gpioled)
{
	int ret;
	struct ath10k_gpiocontrol *gpio = ar->gpio; 
	gpio->cdev.name = gpioled->name;
	gpio->cdev.default_trigger = gpioled->default_trigger;
	gpio->cdev.brightness_set = ath10k_led_brightness;

	ret = led_classdev_register(wiphy_dev(ar->hw->wiphy), &gpio->cdev);
	if (ret < 0)
		return ret;

	return 0;
}

void ath10k_unregister_led(struct ath10k *ar)
{
	struct ath10k_gpiocontrol *gpio = ar->gpio; 
	if (gpio) {
		led_classdev_unregister(&gpio->cdev);
		kfree(gpio);
	}
}

void ath10k_reset_led_pin(struct ath10k *ar) 
{
	/* need to reset gpio state */
	if (ar->hw_params.led_pin) { 
		ath10k_wmi_gpio_config(ar, ar->hw_params.led_pin, 0, WMI_GPIO_PULL_NONE, WMI_GPIO_INTTYPE_DISABLE);
		ath10k_wmi_gpio_output(ar, ar->hw_params.led_pin, 1);
	}
}

int ath10k_attach_led(struct ath10k *ar)
{
	struct ath10k_gpiocontrol *gpio;
	if (ar->gpio) { /* already registered, ignore */
		return -EINVAL;
	}
	gpio = kzalloc(sizeof(struct ath10k_gpiocontrol), GFP_KERNEL);
	if (!gpio) {
		return -ENOMEM;
	}
	ar->gpio = gpio;
	snprintf(gpio->label, sizeof(gpio->label), "ath10k-%s",
		 wiphy_name(ar->hw->wiphy));
	gpio->wifi_led.active_low = 1;
	gpio->wifi_led.gpio = ar->hw_params.led_pin;
	gpio->wifi_led.name = gpio->label;
	gpio->wifi_led.default_state = LEDS_GPIO_DEFSTATE_KEEP;
		
	ath10k_add_led(ar, &gpio->wifi_led);
	ath10k_reset_led_pin(ar); /* initially we need to configure the led pin to output */
	return 0;
}		
