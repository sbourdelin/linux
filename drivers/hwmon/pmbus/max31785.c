/*
 * Copyright (C) 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include "pmbus.h"

enum max31785_regs {
	PMBUS_MFR_FAN_CONFIG		= 0xF1,
	PMBUS_MFR_READ_FAN_PWM		= 0xF3,
	PMBUS_MFR_FAN_FAULT_LIMIT	= 0xF5,
	PMBUS_MFR_FAN_WARN_LIMIT	= 0xF6,
	PMBUS_MFR_FAN_PWM_AVG		= 0xF8,
};

static const struct pmbus_coeffs fan_coeffs[] = {
	[percent]	= { .m = 1, .b = 0, .R = 2 },
	[rpm]		= { .m = 1, .b = 0, .R = 0 },
};

static const struct pmbus_coeffs *max31785_get_fan_coeffs(
		const struct pmbus_driver_info *info, int index,
		enum pmbus_fan_mode mode, int command)
{
	switch (command) {
	case PMBUS_FAN_COMMAND_1:
	case PMBUS_MFR_FAN_FAULT_LIMIT:
	case PMBUS_MFR_FAN_WARN_LIMIT:
		return &fan_coeffs[mode];
	case PMBUS_READ_FAN_SPEED_1:
		return &fan_coeffs[rpm];
	case PMBUS_MFR_FAN_PWM_AVG:
		return &fan_coeffs[percent];
	default:
		break;
	}

	return NULL;
}

static int max31785_get_pwm_mode(int id, u8 fan_config, u16 fan_command)
{
	enum pmbus_fan_mode mode;

	/* MAX31785 only supports fan 1 on the fan pages */
	if (WARN_ON(id > 0))
		return -ENODEV;

	mode = (fan_config & PB_FAN_1_RPM) ? rpm : percent;

	switch (mode) {
	case percent:
		if (fan_command >= 0x8000)
			return 2;
		else if (fan_command >= 0x2711)
			return 0;
		else
			return 1;
	case rpm:
		if (fan_command >= 0x8000)
			return 2;
		else
			return 1;
		break;
	}

	return 0;
}

static const int max31785_pwm_modes[] = { 0x7fff, 0x2710, 0xffff };

int max31785_set_pwm_mode(int id, long mode, u8 *fan_config, u16 *fan_command)
{
	/* MAX31785 only supports fan 1 on the fan pages */
	if (WARN_ON(id > 0))
		return -ENODEV;

	*fan_config &= ~PB_FAN_1_RPM;

	if (mode >= ARRAY_SIZE(max31785_pwm_modes))
		return -ENOTSUPP;

	*fan_command = max31785_pwm_modes[mode];

	return 0;
}

static struct pmbus_driver_info max31785_info = {
	.pages = 23,

	.get_fan_coeffs = max31785_get_fan_coeffs,
	.get_pwm_mode = max31785_get_pwm_mode,
	.set_pwm_mode = max31785_set_pwm_mode,

	.format[PSC_FAN] = direct,
	.func[0] = PMBUS_HAVE_FAN12 | PMBUS_HAVE_STATUS_FAN12,
	.func[1] = PMBUS_HAVE_FAN12 | PMBUS_HAVE_STATUS_FAN12,
	.func[2] = PMBUS_HAVE_FAN12 | PMBUS_HAVE_STATUS_FAN12,
	.func[3] = PMBUS_HAVE_FAN12 | PMBUS_HAVE_STATUS_FAN12,
	.func[4] = PMBUS_HAVE_FAN12 | PMBUS_HAVE_STATUS_FAN12,
	.func[5] = PMBUS_HAVE_FAN12 | PMBUS_HAVE_STATUS_FAN12,

	.format[PSC_TEMPERATURE] = direct,
	.coeffs[PSC_TEMPERATURE].m = 1,
	.coeffs[PSC_TEMPERATURE].b = 0,
	.coeffs[PSC_TEMPERATURE].R = 2,
	.func[6]  = PMBUS_HAVE_STATUS_TEMP,
	.func[7]  = PMBUS_HAVE_STATUS_TEMP,
	.func[8]  = PMBUS_HAVE_STATUS_TEMP,
	.func[9]  = PMBUS_HAVE_STATUS_TEMP,
	.func[10] = PMBUS_HAVE_STATUS_TEMP,
	.func[11] = PMBUS_HAVE_STATUS_TEMP,
	.func[12] = PMBUS_HAVE_STATUS_TEMP,
	.func[13] = PMBUS_HAVE_STATUS_TEMP,
	.func[14] = PMBUS_HAVE_STATUS_TEMP,
	.func[15] = PMBUS_HAVE_STATUS_TEMP,
	.func[16] = PMBUS_HAVE_STATUS_TEMP,

	.format[PSC_VOLTAGE_OUT] = direct,
	.coeffs[PSC_VOLTAGE_OUT].m = 1,
	.coeffs[PSC_VOLTAGE_OUT].b = 0,
	.coeffs[PSC_VOLTAGE_OUT].R = 0,
	.func[17] = PMBUS_HAVE_STATUS_VOUT,
	.func[18] = PMBUS_HAVE_STATUS_VOUT,
	.func[19] = PMBUS_HAVE_STATUS_VOUT,
	.func[20] = PMBUS_HAVE_STATUS_VOUT,
	.func[21] = PMBUS_HAVE_STATUS_VOUT,
	.func[22] = PMBUS_HAVE_STATUS_VOUT,
};

#define MFR_FAN_CONFIG_TSFO		BIT(9)
#define MFR_FAN_CONFIG_TACHO		BIT(8)

static int max31785_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	int rv;
	int i;

	rv = pmbus_do_probe(client, id, &max31785_info);
	if (rv < 0)
		return rv;

	for (i = 0; i < max31785_info.pages; i++) {
		int reg;

		if (!(max31785_info.func[i] & (PMBUS_HAVE_FAN12)))
			continue;

		reg = pmbus_read_word_data(client, i, PMBUS_MFR_FAN_CONFIG);
		if (reg < 0)
			continue;

		/*
		 * XXX: Purely for RFC/testing purposes, don't ramp fans on fan
		 * or temperature sensor fault, or a failure to write
		 * FAN_COMMAND_1 inside a 10s window (watchdog mode).
		 *
		 * The TSFO bit controls both ramping on temp sensor failure
		 * AND whether FAN_COMMAND_1 is in watchdog mode.
		 */
		reg = (reg | MFR_FAN_CONFIG_TSFO | MFR_FAN_CONFIG_TACHO);
		reg &= 0xffff;

		rv = pmbus_write_word_data(client, i, PMBUS_MFR_FAN_CONFIG,
					   reg);
	}

	return 0;
}

static const struct i2c_device_id max31785_id[] = {
	{ "max31785", 0 },
	{ },
};

MODULE_DEVICE_TABLE(i2c, max31785_id);

static struct i2c_driver max31785_driver = {
	.driver = {
		.name = "max31785",
	},
	.probe = max31785_probe,
	.remove = pmbus_do_remove,
	.id_table = max31785_id,
};

module_i2c_driver(max31785_driver);

MODULE_AUTHOR("Andrew Jeffery <andrew@aj.id.au>");
MODULE_DESCRIPTION("PMBus driver for the Maxim MAX31785");
MODULE_LICENSE("GPL");
