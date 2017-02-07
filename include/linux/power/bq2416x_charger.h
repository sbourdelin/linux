/*
 * Driver for BQ2416X Li-Ion Battery Charger
 *
 * Copyright (C) 2015 Verifone, Inc.
 *
 * Author: Wojciech Ziemba <wojciech.ziemba@verifone.com>
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED AS IS AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * The bq2416x series is a 2.5A, Dual-Input, Single-Cell Switched-Mode
 * Li-Ion Battery Charger with Power
 * Path Management and I2C Interface
 *
 */

#ifndef _BQ2416X_CHARGER_H
#define _BQ2416X_CHARGER_H

/* IN(Wall) source limit */
enum in_curr_lim {
	IN_CURR_LIM_1500MA,
	IN_CURR_LIM_2500MA,
};

/* USB source current limit */
enum usb_curr_lim {
	USB_CURR_LIM_100MA,
	USB_CURR_LIM_150MA,
	USB_CURR_LIM_500MA,
	USB_CURR_LIM_800MA,
	USB_CURR_LIM_900MA,
	USB_CURR_LIM_1500MA,
};

/* Safety timer settings */
enum safe_tmr {
	TMR_27MIN,
	TMR_6H,
	TMR_9H,
	TMR_OFF,
};

/**
 * struct bq2416x_pdata - Platform data for bq2416x chip. It contains default
 *			  board voltages and currents.
 * @charge_voltage: charge voltage in [mV]
 * @charge_current: charge current in [mA]
 * @in_curr_limit: Current limit for IN source . Enum 1.5A or 2.5A
 * @usb_curr_limit: Current limit for USB source Enum 100mA - 1500mA
 * @curr_term_en: enable charge terination by current
 * @term_current: charge termination current in [mA]
 * @usb_dpm_voltage: USB DPM voltage [mV]
 * @in_dpm_voltage: IN DPM voltage [mV]
 * @stat_pin_en: status pin enable
 * @safety_timer: safety timer enum: 27min, 6h, 9h, off.
 * @num_supplicants: number of notify devices. Max 4.
 * @supplied_to: array of names of supplied to devices
 */
struct bq2416x_pdata {
	int charge_voltage;
	int charge_current;
	enum in_curr_lim in_curr_limit;
	enum usb_curr_lim usb_curr_limit;
	int curr_term_en;
	int term_current;
	int usb_dpm_voltage;
	int in_dpm_voltage;
	int stat_pin_en;
	enum safe_tmr safety_timer;
	int num_supplicants;
	const char *supplied_to[4];
};

#endif /* _BQ2416X_CHARGER_H */
