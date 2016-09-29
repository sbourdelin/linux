/*
 * Backlight driver for Arctic Sands ARC2C0608 Backlight Devices
 *
 * Copyright 2016 Arctic Sands Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#ifndef _ARCXCNN_H
#define _ARCXCNN_H

enum arcxcnn_chip_id {
	ARC2C0608
};

enum arcxcnn_brightness_source {
	ARCXCNN_PWM_ONLY,
	ARCXCNN_I2C_ONLY = 2,
};

#define ARCXCNN_MAX_PROGENTRIES	48	/* max a/v pairs for custom */

/**
 * struct arcxcnn_platform_data
 * @name : Backlight driver name. If it is not defined, default name is set.
 * @initial_brightness : initial value of backlight brightness
 * @period_ns : platform specific pwm period value. unit is nano.
		Only valid when mode is PWM_BASED.
 * @led_str : initial LED string
 */
struct arcxcnn_platform_data {
	const char *name;
	u16 initial_brightness;
	unsigned int period_ns;
	u8     ledstr;
	u8     prog_addr[ARCXCNN_MAX_PROGENTRIES];
	u8     prog_data[ARCXCNN_MAX_PROGENTRIES];
	u32    prog_entries;
};

#endif
