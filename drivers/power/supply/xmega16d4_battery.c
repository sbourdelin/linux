/*
 * Battery monitor driver for SL50 Toby Churchill SBS Batteries
 *
 * Copyright (c) 2017, Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#define SBS_MEMORY_MAP_SIZE		128
/* Charging voltage, 2 bytes */
#define SBS_CHARGING_VOLTAGE		0x0a
/* Design voltage, 2 bytes */
#define SBS_DESIGN_VOLTAGE		0x0c
/* Fast charging current, 2 bytes */
#define SBS_FAST_CHARGING_CURRENT	0x0e
/* Max T, Low T, 2 bytes */
#define SBS_MAX_LOW_TEMPERATURE		0x10
/* Pack capacity, 2 bytes */
#define SBS_PACK_CAPACITY		0x12
/* Serial number, 2 bytes */
#define SBS_SERIAL_NUMBER		0x18
/* Manufacturer name, 16 bytes */
#define SBS_MANUFACTURER_NAME		0x20
/* Model name, 16 bytes */
#define SBS_MODEL_NAME			0x30
/* Device chemistry, 5 bytes */
#define SBS_DEVICE_CHEMISTRY		0x40
/* Cycle count, 2 bytes */
#define SBS_CYCLE_COUNT			0x50
/* Voltage now, 2 bytes */
#define SBS_VOLTAGE_NOW			0x70
/* Current now, 2 bytes */
#define SBS_CURRENT_NOW			0x72
/* Battery Status, 2 bytes */
#define SBS_BATTERY_STATUS		0x74
# define BATTERY_STATUS_CHARGING	0
# define BATTERY_STATUS_DISCHARGING	BIT(6)
# define BATTERY_STATUS_FULLY_CHARGED	BIT(5)
/* State of charge in percentage, 1 byte */
#define SBS_STATE_OF_CHARGE		0x76

/* MM SIZE + START(u16) + CHECKSUM(u16) */
#define SPI_MSG_LENGTH		(SBS_MEMORY_MAP_SIZE + 4)
#define SPI_MSG_DATA_BP		2
/* MSB checksum byte position */
#define SPI_MSG_CSUM_BP		(2 + SBS_MEMORY_MAP_SIZE)
#define SPI_MSG_START_TOKEN	0xb00b

struct xmega16d4_battery_data {
	struct spi_device *spi;
	struct power_supply *bat;

	struct mutex		work_lock; /* protect work data */
	struct delayed_work	bat_work;

	u8 map[SBS_MEMORY_MAP_SIZE];

	char model_name[16];
	char manufacturer_name[16];
	char serial_number[5];

	int technology;
	int voltage_uV;			/* units of uV */
	int current_uA;			/* units of uA */
	int rated_capacity;		/* units of µAh */
	int cycle_count;
	int rem_capacity;		/* percentage */
	int life_sec;			/* units of seconds */
	int status;			/* state of charge */
};

#define MAX_KEYLENGTH 256
struct battery_property_map {
	int value;
	char const *key;
};

static struct battery_property_map map_technology[] = {
	{ POWER_SUPPLY_TECHNOLOGY_NiMH, "NiMH" },
	{ POWER_SUPPLY_TECHNOLOGY_LION, "LION" },
	{ POWER_SUPPLY_TECHNOLOGY_LIPO, "LIPO" },
	{ POWER_SUPPLY_TECHNOLOGY_LiFe, "LiFe" },
	{ POWER_SUPPLY_TECHNOLOGY_NiCd, "NiCd" },
	{ POWER_SUPPLY_TECHNOLOGY_LiMn, "LiMn" },
	{ -1,				NULL   },
};

static int map_get_value(struct battery_property_map *map, const char *key,
			 int def_val)
{
	char buf[MAX_KEYLENGTH];
	int cr;

	strncpy(buf, key, MAX_KEYLENGTH);
	buf[MAX_KEYLENGTH - 1] = '\0';

	cr = strnlen(buf, MAX_KEYLENGTH) - 1;
	if (buf[cr] == '\n')
		buf[cr] = '\0';

	while (map->key) {
		if (strncasecmp(map->key, buf, MAX_KEYLENGTH) == 0)
			return map->value;
		map++;
	}

	return def_val;
}

static int xmega16d4_battery_read_status(struct xmega16d4_battery_data *data)
{
	int i;
	int csum = 0;
	u8 buf[SBS_MEMORY_MAP_SIZE], technology[5];
	unsigned int uval;
	int sval;
	struct spi_device *spi = data->spi;

	for (i = 0; i < SBS_MEMORY_MAP_SIZE; i++) {
		spi_write(spi, &i, 1);
		spi_read(spi, &buf[i], 1);
	}

	print_hex_dump(KERN_DEBUG, ": ", DUMP_PREFIX_OFFSET, 16, 1,
		       buf, SBS_MEMORY_MAP_SIZE, false);

	/* Calculate the data checksum */
	for (i = 0; i < SBS_MEMORY_MAP_SIZE - 2; i++)
		csum += buf[i];

	/* Verify the checksum */
	uval = (buf[SBS_MEMORY_MAP_SIZE - 2] << 8) |
		buf[SBS_MEMORY_MAP_SIZE - 1];
	if (csum != uval) {
		dev_dbg(&spi->dev,
			"message received with invalid checksum (%d != %d)\n",
			csum, uval);
		return -ENOMSG;
	}

	/* Update memory map with the new data */
	memcpy(data->map, buf, SBS_MEMORY_MAP_SIZE);

	strncpy(data->model_name, &data->map[SBS_MODEL_NAME], 16);

	strncpy(data->manufacturer_name, &data->map[SBS_MANUFACTURER_NAME],
		16);

	strncpy(technology, &data->map[SBS_DEVICE_CHEMISTRY], 5);

	uval = (u16)((data->map[SBS_SERIAL_NUMBER + 1] << 8) |
		data->map[SBS_SERIAL_NUMBER]);
	snprintf(data->serial_number, ARRAY_SIZE(data->serial_number), "%04d",
		 uval);

	data->technology = map_get_value(map_technology, technology,
					 POWER_SUPPLY_TECHNOLOGY_UNKNOWN);

	data->voltage_uV = (u16)((data->map[SBS_VOLTAGE_NOW + 1] << 8) |
			    data->map[SBS_VOLTAGE_NOW]);
	data->voltage_uV *= 1000;	/* convert from mV to uV */

	sval = (s16)((data->map[SBS_CURRENT_NOW + 1] << 8) |
		data->map[SBS_CURRENT_NOW]);
	data->current_uA = sval;
	data->current_uA *= 1000;	/* convert from mA to uA */

	data->rated_capacity = (u16)((data->map[SBS_PACK_CAPACITY + 1] << 8) |
				data->map[SBS_PACK_CAPACITY]);
	data->rated_capacity *= 1000;	/* convert from mAh to uAh */

	uval = (u16)((data->map[SBS_BATTERY_STATUS + 1] << 8) |
		data->map[SBS_BATTERY_STATUS]);
	if (uval == BATTERY_STATUS_CHARGING)
		data->status = POWER_SUPPLY_STATUS_CHARGING;
	else if (uval == BATTERY_STATUS_DISCHARGING)
		data->status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (uval == BATTERY_STATUS_FULLY_CHARGED)
		data->status = POWER_SUPPLY_STATUS_FULL;
	else
		data->status = POWER_SUPPLY_STATUS_UNKNOWN;

	data->cycle_count = (u16)((data->map[SBS_CYCLE_COUNT + 1] << 8) |
			     data->map[SBS_CYCLE_COUNT]);

	data->rem_capacity = data->map[SBS_STATE_OF_CHARGE];

	uval = (data->rem_capacity * (data->rated_capacity / 1000)) / 100;
	if (data->current_uA)
		data->life_sec = (3600l * uval) / (data->current_uA / 1000);

	return 0;
}

static void xmega16d4_battery_work(struct work_struct *work)
{
	struct xmega16d4_battery_data *data = container_of(work,
		struct xmega16d4_battery_data, bat_work.work);

	/* Update values */
	mutex_lock(&data->work_lock);
	xmega16d4_battery_read_status(data);
	mutex_unlock(&data->work_lock);

	schedule_delayed_work(&data->bat_work, HZ * 60);
}

static int xmega16d4_battery_get_property(struct power_supply *psy,
					  enum power_supply_property psp,
					  union power_supply_propval *val)
{
	struct xmega16d4_battery_data *data = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = data->status;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = data->voltage_uV;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = data->current_uA;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = data->rated_capacity;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		val->intval = data->life_sec;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = data->rem_capacity;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = data->technology;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = data->model_name;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = data->manufacturer_name;
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = data->serial_number;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static enum power_supply_property xmega16d4_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	/* Properties of type `const char *' */
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static const struct power_supply_desc xmega16d4_battery_desc = {
	.name			= "battery-monitor",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= xmega16d4_battery_props,
	.num_properties		= ARRAY_SIZE(xmega16d4_battery_props),
	.get_property		= xmega16d4_battery_get_property,
};

static int xmega16d4_battery_probe(struct spi_device *spi)
{
	struct xmega16d4_battery_data *data;
	struct power_supply_config psy_cfg = {};

	data = devm_kzalloc(&spi->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->spi = spi;
	psy_cfg.of_node = spi->dev.of_node;
	psy_cfg.drv_data = data;

	mutex_init(&data->work_lock);

	INIT_DELAYED_WORK(&data->bat_work, xmega16d4_battery_work);

	spi_set_drvdata(spi, data);

	/* Get initial status */
	if (xmega16d4_battery_read_status(data))
		return -ENODEV;

	data->bat = devm_power_supply_register(&spi->dev,
					       &xmega16d4_battery_desc,
					       &psy_cfg);
	if (IS_ERR(data->bat))
		return PTR_ERR(data->bat);

	schedule_delayed_work(&data->bat_work, 0);

	return 0;
}

static int xmega16d4_battery_remove(struct spi_device *spi)
{
	struct xmega16d4_battery_data *data = spi_get_drvdata(spi);

	cancel_delayed_work_sync(&data->bat_work);

	return 0;
}

static const struct of_device_id xmega16d4_battery_of_match[] = {
	{ .compatible = "tcl,xmega16d4-battery", },
	{ /* sentinel */ },
};

static struct spi_driver xmega16d4_battery_driver = {
	.driver = {
		.name		= "xmega16d4-battery",
		.owner		= THIS_MODULE,
		.of_match_table = of_match_ptr(xmega16d4_battery_of_match),
	},
	.probe	= xmega16d4_battery_probe,
	.remove	= xmega16d4_battery_remove,
};
module_spi_driver(xmega16d4_battery_driver);

MODULE_ALIAS("spi:xmega16d4-battery");

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Enric Balletbo Serra <enric.balletbo@collabora.com>");
MODULE_DESCRIPTION("xmega16d4 battery monitor driver");
