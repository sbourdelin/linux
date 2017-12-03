/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __THINKPAD_ACPI_H__
#define __THINKPAD_ACPI_H__

/* These two functions return 0 if success, or negative error code
   (e g -ENODEV if no led present) */

enum {
	TPACPI_LED_MUTE,
	TPACPI_LED_MICMUTE,
	TPACPI_LED_MAX,
};

#ifdef CONFIG_THINKPAD_ACPI_BWC

enum {
    TPACPI_BATTERY_ANY = 0,
    TPACPI_BATTERY_PRIMARY = 1,
    TPACPI_BATTERY_SECONDARY = 2
};

int tpacpi_battery_get_functionality(void);
int tpacpi_get_start_threshold(int battery, int* res);
int tpacpi_get_stop_threshold(int battery, int* res);
int tpacpi_set_start_threshold(int battery, int value);
int tpacpi_set_stop_threshold(int battery, int value);

#endif

int tpacpi_led_set(int whichled, bool on);

#endif
