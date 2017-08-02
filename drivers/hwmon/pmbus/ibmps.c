/*
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>

#include "pmbus.h"

#define IBMPS_MFR_FAN_FAULT			BIT(0)
#define IBMPS_MFR_THERMAL_FAULT			BIT(1)
#define IBMPS_MFR_OV_FAULT			BIT(2)
#define IBMPS_MFR_UV_FAULT			BIT(3)
#define IBMPS_MFR_PS_KILL			BIT(4)
#define IBMPS_MFR_OC_FAULT			BIT(5)
#define IBMPS_MFR_VAUX_FAULT			BIT(6)
#define IBMPS_MFR_CURRENT_SHARE_WARNING		BIT(7)

static int ibmps_read_word_data(struct i2c_client *client, int page, int reg);

static int ibmps_read_byte_data(struct i2c_client *client, int page, int reg)
{
	int rc, mfr;

	switch (reg) {
	case PMBUS_STATUS_BYTE:
	case PMBUS_STATUS_WORD:
		rc = ibmps_read_word_data(client, page, PMBUS_STATUS_WORD);
		break;
	case PMBUS_STATUS_VOUT:
	case PMBUS_STATUS_IOUT:
	case PMBUS_STATUS_TEMPERATURE:
	case PMBUS_STATUS_FAN_12:
		rc = pmbus_read_byte_data(client, page, reg);
		if (rc < 0)
			return rc;

		mfr = pmbus_read_byte_data(client, page,
					   PMBUS_STATUS_MFR_SPECIFIC);
		if (mfr < 0)
			return rc;

		if (reg == PMBUS_STATUS_FAN_12) {
			if (mfr & IBMPS_MFR_FAN_FAULT)
				rc |= PB_FAN_FAN1_FAULT;
		} else if (reg == PMBUS_STATUS_TEMPERATURE) {
			if (mfr & IBMPS_MFR_THERMAL_FAULT)
				rc |= PB_TEMP_OT_FAULT;
		} else if (reg == PMBUS_STATUS_VOUT) {
			if (mfr & (IBMPS_MFR_OV_FAULT | IBMPS_MFR_VAUX_FAULT))
				rc |= PB_VOLTAGE_OV_FAULT;
			if (mfr & IBMPS_MFR_UV_FAULT)
				rc |= PB_VOLTAGE_UV_FAULT;
		} else if (reg == PMBUS_STATUS_IOUT) {
			if (mfr & IBMPS_MFR_OC_FAULT)
				rc |= PB_IOUT_OC_FAULT;
			if (mfr & IBMPS_MFR_CURRENT_SHARE_WARNING)
				rc |= PB_CURRENT_SHARE_FAULT;
		}
		break;
	default:
		if (reg >= PMBUS_VIRT_BASE)
			return -ENXIO;

		rc = pmbus_read_byte_data(client, page, reg);
		break;
	}

	return rc;
}

static int ibmps_read_word_data(struct i2c_client *client, int page, int reg)
{
	int rc, mfr;

	switch (reg) {
	case PMBUS_STATUS_BYTE:
	case PMBUS_STATUS_WORD:
		rc = pmbus_read_word_data(client, page, PMBUS_STATUS_WORD);
		if (rc < 0)
			return rc;

		mfr = pmbus_read_byte_data(client, page,
					   PMBUS_STATUS_MFR_SPECIFIC);
		if (mfr < 0)
			return rc;

		if (mfr & IBMPS_MFR_PS_KILL)
			rc |= PB_STATUS_OFF;

		if (mfr)
			rc |= PB_STATUS_WORD_MFR;
		break;
	default:
		if (reg >= PMBUS_VIRT_BASE)
			return -ENXIO;

		rc = pmbus_read_word_data(client, page, reg);
		if (rc < 0)
			rc = ibmps_read_byte_data(client, page, reg);
		break;
	}

	return rc;
}

static struct pmbus_driver_info ibmps_info = {
	.pages = 1,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_IOUT |
		PMBUS_HAVE_PIN | PMBUS_HAVE_FAN12 | PMBUS_HAVE_TEMP |
		PMBUS_HAVE_TEMP2 | PMBUS_HAVE_TEMP3 | PMBUS_HAVE_STATUS_VOUT |
		PMBUS_HAVE_STATUS_IOUT | PMBUS_HAVE_STATUS_INPUT |
		PMBUS_HAVE_STATUS_TEMP | PMBUS_HAVE_STATUS_FAN12,
	.read_byte_data = ibmps_read_byte_data,
	.read_word_data = ibmps_read_word_data,
};

static int ibmps_probe(struct i2c_client *client,
		       const struct i2c_device_id *id)
{
	return pmbus_do_probe(client, id, &ibmps_info);
}

static int ibmps_remove(struct i2c_client *client)
{
	return pmbus_do_remove(client);
}

enum chips { witherspoon };

static const struct i2c_device_id ibmps_id[] = {
	{ "witherspoon", witherspoon },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ibmps_id);

static const struct of_device_id ibmps_of_match[] = {
	{ .compatible = "ibm,ibmps" },
	{}
};
MODULE_DEVICE_TABLE(of, p8_i2c_occ_of_match);

static struct i2c_driver ibmps_driver = {
	.driver = {
		.name = "ibmps",
		.of_match_table = ibmps_of_match,
	},
	.probe = ibmps_probe,
	.remove = ibmps_remove,
	.id_table = ibmps_id,
};

module_i2c_driver(ibmps_driver);

MODULE_AUTHOR("Eddie James");
MODULE_DESCRIPTION("PMBus driver for IBM power supply");
MODULE_LICENSE("GPL");
