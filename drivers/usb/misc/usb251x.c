/*
 * drivers/usb/misc/usb251x.c
 *
 * driver for SMSC USB251X USB Hub
 *
 * Authors: Rick Bronson <rick@efn.org>
 *          Fabien Lahoudere <fabien.lahoudere@collabora.co.uk>
 *
 * Register initialization is based on code examples provided by Philips
 * Copyright (c) 2005 Koninklijke Philips Electronics N.V.
 *
 * This file is licensed under
 * the terms of the GNU General Public License version 2. This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/usb.h>
#include <linux/of_device.h>
#include <linux/types.h>

/* registers */
#define USB251X_VENDOR_ID_LSB 0x00
#define USB251X_VENDOR_ID_MSB 0x01
#define USB251X_PRODUCT_ID_LSB 0x02
#define USB251X_PRODUCT_ID_MSB 0x03
#define USB251X_DEVICE_ID_LSB 0x04
#define USB251X_DEVICE_ID_MSB 0x05
#define USB251X_CONFIGURATION_DATA_BYTE_1 0x06
#define USB251X_CONFIGURATION_DATA_BYTE_2 0x07
#define USB251X_CONFIGURATION_DATA_BYTE_3 0x08
#define USB251X_NON_REMOVABLE_DEVICES 0x09
#define USB251X_PORT_DISABLE_SELF 0x0A
#define USB251X_PORT_DISABLE_BUS 0x0B
#define USB251X_MAX_POWER_SELF 0x0C
#define USB251X_MAX_POWER_BUS 0x0D
#define USB251X_HUB_CONTROLLER_MAX_CURRENT_SELF 0x0E
#define USB251X_HUB_CONTROLLER_MAX_CURRENT_BUS 0x0F
#define USB251X_POWER_ON_TIME 0x10
#define USB251X_LANGUAGE_ID_HIGH 0x11
#define USB251X_LANGUAGE_ID_LOW 0x12
#define USB251X_MANUFACTURER_STRING_LENGTH 0x13
#define USB251X_PRODUCT_STRING_LENGTH 0x14
#define USB251X_SERIAL_STRING_LENGTH 0x15
#define USB251X_MANUFACTURER_STRING 0x16
#define USB251X_PRODUCT_STRING 0x54
#define USB251X_SERIAL_STRING 0x92
#define USB251X_BATTERY_CHARGING_ENABLE 0xD0
#define USB251X_BOOST_UP 0xF6
#define USB251X_BOOST_X 0xF8
#define USB251X_PORT_SWAP 0xFA
#define USB251X_PORT_MAP_12 0xFB
#define USB251X_PORT_MAP_34 0xFC
#define USB251X_PORT_MAP_56 0xFD
#define USB251X_PORT_MAP_7 0xFE
#define USB251X_STATUS_COMMAND 0xFF

#define USB251X_ADDR_SZ 256	/* address space of device */
#define USB251X_I2C_WRITE_SIZE 16
#define USB251X_I2C_NAME "usb251x"

/* The platform data */
struct usb251x_platform_data {
	unsigned char *init_table;	/* table to init part */
};

#define DRIVER_DESC "SMSC USB 2.0 Hi-Speed Hub Controller"

static unsigned char default_init_table[USB251X_ADDR_SZ] = {
	0x24, 0x04, 0x14, 0x25, 0xa0, 0x0b, 0x9b, 0x20,	/* 00 - 07 */
	0x02, 0x00, 0x00, 0x00, 0x01, 0x32, 0x01, 0x32,	/* 08 - 0F */
	0x32, 0x00, 0x00, 4, 30, 0x00, 'S', 0x00,	/* 10 - 17 */
	'M', 0x00, 'S', 0x00, 'C', 0x00, 0x00, 0x00,	/* 18 - 1F */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 20 - 27 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 28 - 2F */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 30 - 37 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 38 - 3F */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 40 - 47 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 48 - 4F */
	0x00, 0x00, 0x00, 0x00, 'U', 0x00, 'S', 0x00,	/* 50 - 57 */
	'B', 0x00, ' ', 0x00, '2', 0x00, '.', 0x00,	/* 58 - 5F */
	'0', 0x00, ' ', 0x00, 'H', 0x00, 'i', 0x00,	/* 60 - 67 */
	'-', 0x00, 'S', 0x00, 'p', 0x00, 'e', 0x00,	/* 68 - 6F */
	'e', 0x00, 'd', 0x00, ' ', 0x00, 'H', 0x00,	/* 70 - 77 */
	'u', 0x00, 'b', 0x00, ' ', 0x00, 'C', 0x00,	/* 78 - 7F */
	'o', 0x00, 'n', 0x00, 't', 0x00, 'r', 0x00,	/* 80 - 87 */
	'o', 0x00, 'l', 0x00, 'l', 0x00, 'e', 0x00,	/* 88 - 8F */
	'r', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 90 - 97 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* 98 - 9F */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* A0 - A7 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* A8 - AF */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* B0 - B7 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* B8 - BF */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* C0 - C7 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* C8 - CF */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* D0 - D7 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* D8 - DF */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* E0 - E7 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* E8 - EF */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* F0 - F7 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00	/* F8 - FF */
};

/* USB251X only supports i2c block writes 16+1 bytes at a time */
static int usb251x_configure(struct i2c_client *client,
			     unsigned char *init_table)
{
	int cntr, ret = 0;
	unsigned char i2cwritebuffer[USB251X_I2C_WRITE_SIZE + 1];

	for (cntr = 0; cntr < USB251X_ADDR_SZ / USB251X_I2C_WRITE_SIZE; cntr++) {
		i2cwritebuffer[0] = USB251X_I2C_WRITE_SIZE;
		memcpy(&i2cwritebuffer[1],
		       &init_table[cntr * USB251X_I2C_WRITE_SIZE],
		       USB251X_I2C_WRITE_SIZE);
		ret =
		    i2c_smbus_write_i2c_block_data(client,
						   cntr *
						   USB251X_I2C_WRITE_SIZE,
						   USB251X_I2C_WRITE_SIZE + 1,
						   &i2cwritebuffer[0]);
		if (ret < 0) {
			dev_err(&client->dev, "failed writings to 0x%02x\n",
				cntr * USB251X_I2C_WRITE_SIZE);
			return ret;
		}
	}
	return ret;
}

static void usb251x_set_config_from_of(const struct device_node *node,
				       unsigned char *table,
				       const char *pname, u8 offset)
{
	int ret;
	unsigned char value;

	ret = of_property_read_u8(node, pname, &value);
	if (ret == 0)
		table[offset] = value;
}

static int usb251x_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct usb251x_platform_data *pdata = client->dev.platform_data;
	struct device *dev = &client->dev;

	dev_info(dev, DRIVER_DESC " " USB251X_I2C_NAME "\n");
	if (usb_disabled()) {
		dev_err(dev, "USB is required to be enabled\n.");
		return -ENODEV;
	}

	if (dev->of_node) {
		if (!pdata) {
			pdata =
			    kmalloc(sizeof(struct usb251x_platform_data),
				    GFP_KERNEL);
			if (!pdata)
				return -ENOMEM;
		}
		if (!pdata->init_table) {
			pdata->init_table =
			    kmalloc(sizeof(unsigned char) * USB251X_ADDR_SZ,
				    GFP_KERNEL);
			if (!pdata->init_table) {
				kfree(pdata);
				return -ENOMEM;
			}
		}
		memcpy(pdata->init_table, default_init_table, USB251X_ADDR_SZ);

		/* set configuration data 1 */
		usb251x_set_config_from_of(dev->of_node,
					   pdata->init_table,
					   "smsc,usb251x-cfg-data1",
					   USB251X_CONFIGURATION_DATA_BYTE_1);
		/* set configuration data 2 */
		usb251x_set_config_from_of(dev->of_node,
					   pdata->init_table,
					   "smsc,usb251x-cfg-data2",
					   USB251X_CONFIGURATION_DATA_BYTE_2);
		/* set configuration data 3 */
		usb251x_set_config_from_of(dev->of_node,
					   pdata->init_table,
					   "smsc,usb251x-cfg-data3",
					   USB251X_CONFIGURATION_DATA_BYTE_3);
		/* set port mapping for ports 1 and 2 */
		usb251x_set_config_from_of(dev->of_node,
					   pdata->init_table,
					   "smsc,usb251x-portmap12",
					   USB251X_PORT_MAP_12);
		/* set port mapping for ports 3 and 4 */
		usb251x_set_config_from_of(dev->of_node,
					   pdata->init_table,
					   "smsc,usb251x-portmap34",
					   USB251X_PORT_MAP_34);
		/* set port mapping for ports 5 and 6 */
		usb251x_set_config_from_of(dev->of_node,
					   pdata->init_table,
					   "smsc,usb251x-portmap56",
					   USB251X_PORT_MAP_56);
		/* set port mapping for port 7 */
		usb251x_set_config_from_of(dev->of_node,
					   pdata->init_table,
					   "smsc,usb251x-portmap7",
					   USB251X_PORT_MAP_7);
		/* set SMBus configuration */
		usb251x_set_config_from_of(dev->of_node,
					   pdata->init_table,
					   "smsc,usb251x-status-command",
					   USB251X_STATUS_COMMAND);
	} else {
		dev_err(dev, "initialization data required.\n");
		return -EINVAL;
	}

	return usb251x_configure(client, pdata->init_table);
}

static int usb251x_resume(struct device *dev)
{
	const struct usb251x_platform_data *pdata = dev->platform_data;
	struct i2c_client *client = to_i2c_client(dev);

	return usb251x_configure(client, pdata->init_table);
}

#ifdef CONFIG_OF
static const struct of_device_id usb251x_dt_ids[] = {
	{.compatible = "smsc,usb251x",},
	{}
};

MODULE_DEVICE_TABLE(of, usb251x_dt_ids);
#endif

static const struct i2c_device_id usb251x_id[] = {
	{USB251X_I2C_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, usb251x_id);

static const struct dev_pm_ops usb251x_pm_ops = {
	.resume = usb251x_resume,
};

static struct i2c_driver usb251x_driver = {
	.driver = {
		   .name = USB251X_I2C_NAME,
		   .pm = &usb251x_pm_ops,
		   },
	.probe = usb251x_probe,
	.id_table = usb251x_id,
};

module_i2c_driver(usb251x_driver);
MODULE_LICENSE("GPL");
