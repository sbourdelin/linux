/*
 * Intel CHT Whiskey Cove Fuel Gauge driver
 * Copyright (C) 2017 Hans de Goede <hdegoede@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Cherrytrail Whiskey Cove devices have 2 functional blocks which interact
 * with the battery.
 *
 * 1) The fuel-gauge which is build into the Whiskey Cove PMIC, but has its
 * own i2c bus and i2c client addresses separately from the rest of the PMIC.
 * That block is what this driver is for.
 *
 * 2) An external charger IC, which is connected to the SMBUS controller
 * which is part of the rest of the Whiskey Cove PMIC, mfd/intel_cht_wc.c
 * registers a platform device for the SMBUS controller and
 * i2c/busses/i2c-cht-wc.c contains the i2c-adapter driver for this.
 *
 * However we want to present this as a single power_supply device to
 * userspace. So this driver offers a callback to get the fuel-gauge
 * power_supply properties, which gets passed to the external charger
 * driver via i2c_board_info when i2c-cht-wc.c calls i2c_new_device().
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/intel_soc_pmic.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/power/cht_wc_fuel_gauge.h>
#include <linux/slab.h>

#define REG_CHARGE_NOW		0x05
#define REG_VOLTAGE_NOW		0x09
#define REG_CURRENT_NOW		0x0a
#define REG_CURRENT_AVG		0x0b
#define REG_CHARGE_FULL		0x10
#define REG_CHARGE_DESIGN	0x18
#define REG_VOLTAGE_AVG		0x19
#define REG_VOLTAGE_OCV		0x1b /* Only updated during charging */

#define CHT_WC_FG_PTYPE		4

struct cht_wc_fg_data {
	struct device *dev;
	struct i2c_client *client;
};

static DEFINE_MUTEX(cht_wc_fg_mutex);
static struct cht_wc_fg_data *cht_wc_fg;

static int cht_wc_fg_read(struct cht_wc_fg_data *fg, u8 reg,
			  union power_supply_propval *val, int scale,
			  int sign_extend)
{
	int ret;

	ret = i2c_smbus_read_word_data(fg->client, reg);
	if (ret < 0)
		return ret;

	if (sign_extend)
		ret = sign_extend32(ret, 15);

	val->intval = ret * scale;

	return 0;
}

int cht_wc_fg_get_property(enum power_supply_property prop,
			   union power_supply_propval *val)
{
	int ret = 0;

	mutex_lock(&cht_wc_fg_mutex);

	if (!cht_wc_fg) {
		ret = -ENXIO;
		goto out_unlock;
	}

	switch (prop) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = cht_wc_fg_read(cht_wc_fg, REG_VOLTAGE_NOW, val, 75, 0);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		ret = cht_wc_fg_read(cht_wc_fg, REG_VOLTAGE_AVG, val, 75, 0);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		ret = cht_wc_fg_read(cht_wc_fg, REG_VOLTAGE_OCV, val, 75, 0);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = cht_wc_fg_read(cht_wc_fg, REG_CURRENT_NOW, val, 150, 1);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		ret = cht_wc_fg_read(cht_wc_fg, REG_CURRENT_AVG, val, 150, 1);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = cht_wc_fg_read(cht_wc_fg, REG_CHARGE_DESIGN, val, 500, 0);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = cht_wc_fg_read(cht_wc_fg, REG_CHARGE_FULL, val, 500, 0);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = cht_wc_fg_read(cht_wc_fg, REG_CHARGE_NOW, val, 500, 0);
		break;
	default:
		ret = -ENODATA;
	}
out_unlock:
	mutex_unlock(&cht_wc_fg_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(cht_wc_fg_get_property);

static int cht_wc_fg_probe(struct i2c_client *client,
			const struct i2c_device_id *i2c_id)
{
	struct device *dev = &client->dev;
	struct cht_wc_fg_data *fg;
	acpi_status status;
	unsigned long long ptyp;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	status = acpi_evaluate_integer(ACPI_HANDLE(dev), "PTYP", NULL, &ptyp);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Failed to get PTYPE\n");
		return -ENODEV;
	}

	/*
	 * The same ACPI HID is used with different PMICs check PTYP to
	 * ensure that we are dealing with a Whiskey Cove PMIC.
	 */
	if (ptyp != CHT_WC_FG_PTYPE)
		return -ENODEV;

	fg->dev = dev;
	/*
	 * The current resource settings table for the fuel gauge contains
	 * multiple i2c devices on 2 different i2c-busses. The one we actually
	 * want is the second resource (index 1).
	 */
	fg->client = i2c_acpi_new_device(dev, 1);
	if (!fg->client)
		return -EPROBE_DEFER;

	i2c_set_clientdata(client, fg);

	mutex_lock(&cht_wc_fg_mutex);
	cht_wc_fg = fg;
	mutex_unlock(&cht_wc_fg_mutex);

	return 0;
}

static int cht_wc_fg_remove(struct i2c_client *i2c)
{
	struct cht_wc_fg_data *fg = i2c_get_clientdata(i2c);

	mutex_lock(&cht_wc_fg_mutex);
	cht_wc_fg = NULL;
	mutex_unlock(&cht_wc_fg_mutex);

	i2c_unregister_device(fg->client);

	return 0;
}

static const struct i2c_device_id cht_wc_fg_i2c_id[] = {
	{ }
};
MODULE_DEVICE_TABLE(i2c, cht_wc_fg_i2c_id);

static const struct acpi_device_id cht_wc_fg_acpi_ids[] = {
	{ "INT33FE", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, cht_wc_fg_acpi_ids);

static struct i2c_driver cht_wc_fg_driver = {
	.driver	= {
		.name	= "CHT Whiskey Cove PMIC Fuel Gauge",
		.acpi_match_table = ACPI_PTR(cht_wc_fg_acpi_ids),
	},
	.probe = cht_wc_fg_probe,
	.remove = cht_wc_fg_remove,
	.id_table = cht_wc_fg_i2c_id,
	.irq_index = 1,
};

module_i2c_driver(cht_wc_fg_driver);

MODULE_DESCRIPTION("Intel CHT Whiskey Cove PMIC Fuel Gauge driver");
MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_LICENSE("GPL");
