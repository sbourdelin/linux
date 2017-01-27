/*
 * Backlight driver for ArcticSand ARC_X_C_0N_0N Devices
 *
 * Copyright 2016 ArcticSand, Inc.
 * Author : Brian Dodge <bdodge@arcticsand.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _ARCXCNN_H
#define _ARCXCNN_H

enum arcxcnn_chip_id {
	ARC2C0608
};

/**
 * struct arcxcnn_platform_data
 * @name		: Backlight driver name (NULL will use default)
 * @initial_brightness	: initial value of backlight brightness
 * @leden		: initial LED string enables, upper bit is global on/off
 * @led_config_0	: fading speed (period between intensity steps)
 * @led_config_1	: misc settings, see datasheet
 * @dim_freq		: pwm dimming frequency if in pwm mode
 * @comp_config		: misc config, see datasheet
 * @filter_config	: RC/PWM filter config, see datasheet
 * @trim_config		: full scale current trim, see datasheet
 */
struct arcxcnn_platform_data {
	const char *name;
	u16 initial_brightness;
	u8	leden;
	u8	led_config_0;
	u8	led_config_1;
	u8	dim_freq;
	u8	comp_config;
	u8	filter_config;
	u8	trim_config;
};

#endif
