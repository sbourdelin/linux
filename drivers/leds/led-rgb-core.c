/*
 * LED Class Color Support
 *
 * Author: Heiner Kallweit <hkallweit1@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/leds.h>
#include "leds.h"

/*
 * The color extension handles RGB LEDs but uses a HSV color model internally.
 * led_rgb_adjust_hue_sat sets hue and saturation part of the HSV color value.
 */
static enum led_brightness led_rgb_adjust_hue_sat(struct led_classdev *led_cdev,
						  enum led_brightness value)
{
	/* LED_SET_HUE_SAT sets hue and saturation even if both are zero */
	if (value & LED_SET_HUE_SAT || value > LED_FULL)
		return value & LED_HUE_SAT_MASK;
	else
		return led_cdev->brightness & ~LED_BRIGHTNESS_MASK;
}

enum led_brightness led_confine_brightness(struct led_classdev *led_cdev,
					   enum led_brightness value)
{
	enum led_brightness brightness = 0;

	if (led_cdev->flags & LED_DEV_CAP_RGB)
		brightness = led_rgb_adjust_hue_sat(led_cdev, value);

	return brightness |
	       min(value & LED_BRIGHTNESS_MASK, led_cdev->max_brightness);
}
