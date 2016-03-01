/*
 * LED Core
 *
 * Copyright 2005 Openedhand Ltd.
 *
 * Author: Richard Purdie <rpurdie@openedhand.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#ifndef __LEDS_H_INCLUDED
#define __LEDS_H_INCLUDED

#include <linux/rwsem.h>
#include <linux/leds.h>

#define LED_BRIGHTNESS_MASK	0x000000ff
#define LED_HUE_SAT_MASK	0x00ffff00

static inline int led_get_brightness(struct led_classdev *led_cdev)
{
	return led_cdev->brightness;
}

static inline bool __is_brightness_set(enum led_brightness brightness)
{
	return (brightness & LED_BRIGHTNESS_MASK) != LED_OFF;
}

void led_init_core(struct led_classdev *led_cdev);
void led_stop_software_blink(struct led_classdev *led_cdev);
void led_set_brightness_nopm(struct led_classdev *led_cdev,
				enum led_brightness value);
void led_set_brightness_nosleep(struct led_classdev *led_cdev,
				enum led_brightness value);
#if IS_ENABLED(CONFIG_LEDS_RGB)
enum led_brightness led_confine_brightness(struct led_classdev *led_cdev,
					   enum led_brightness value);
#else
static inline enum led_brightness led_confine_brightness(
		struct led_classdev *led_cdev, enum led_brightness value)
{
	return min(value, led_cdev->max_brightness);
}
#endif

extern struct rw_semaphore leds_list_lock;
extern struct list_head leds_list;

#endif	/* __LEDS_H_INCLUDED */
