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

enum led_brightness led_hsv_to_rgb(enum led_brightness hsv)
{
	int h = min_t(int, (hsv >> 16) & 0xff, 251);
	int s = (hsv >> 8) & 0xff;
	int v = hsv & 0xff;
	int f, p, q, t, r, g, b;

	if (!v)
		return 0;
	if (!s)
		return (v << 16) + (v << 8) + v;

	f = DIV_ROUND_CLOSEST((h % 42) * 255, 42);
	p = v - DIV_ROUND_CLOSEST(s * v, 255);
	q = v - DIV_ROUND_CLOSEST(f * s * v, 255 * 255);
	t = v - DIV_ROUND_CLOSEST((255 - f) * s * v, 255 * 255);

	switch (h / 42) {
	case 0:
		r = v; g = t; b = p; break;
	case 1:
		r = q; g = v; b = p; break;
	case 2:
		r = p; g = v; b = t; break;
	case 3:
		r = p; g = q; b = v; break;
	case 4:
		r = t; g = p; b = v; break;
	case 5:
		r = v; g = p; b = q; break;
	}

	return (r << 16) + (g << 8) + b;
}
EXPORT_SYMBOL_GPL(led_hsv_to_rgb);
