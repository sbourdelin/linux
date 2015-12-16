/*
 *  MEN 14F021P00 Board Management Controller (BMC) MFD Core Driver.
 *
 *  Copyright (C) 2014 MEN Mikro Elektronik Nuernberg GmbH
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>

#define BMC_CMD_WDT_EXIT_PROD	0x18
#define BMC_CMD_WDT_PROD_STAT	0x19
#define BMC_CMD_REV_MAJOR	0x80
#define BMC_CMD_REV_MINOR	0x81
#define BMC_CMD_REV_MAIN	0x82
#define BMC_CMD_SLOT_ADDRESS	0x8c
#define BMC_CMD_HW_VARIANT	0x8f
#define BMC_CMD_PWRCYCL_CNT	0x93
#define BMC_CMD_OP_HRS_CNT	0x94

static struct mfd_cell menf21bmc_cell[] = {
	{ .name = "menf21bmc_wdt", },
	{ .name = "menf21bmc_led", },
	{ .name = "menf21bmc_hwmon", }
};

static ssize_t menf21bmc_mode_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	int val;

	val = i2c_smbus_read_byte_data(client, BMC_CMD_WDT_PROD_STAT);
	if (val < 0)
		return val;

	return sprintf(buf, "%d\n", val);
}

static ssize_t menf21bmc_mode_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t size)
{
	struct i2c_client *client = to_i2c_client(dev);
	unsigned long mode_val;
	int ret;

	if (kstrtoul(buf, 0, &mode_val))
		return -EINVAL;

	/*
	 * We cannot set the production mode (0).
	 * This is the default mode. If exited once,
	 * it cannot be set anymore.
	 */
	if (!mode_val)
		return -EINVAL;

	ret = i2c_smbus_write_byte(client, BMC_CMD_WDT_EXIT_PROD);
	if (ret < 0)
		return ret;

	return size;
}

static ssize_t menf21bmc_hw_variant_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	int val;

	val = i2c_smbus_read_word_data(client, BMC_CMD_HW_VARIANT);
	if (val < 0)
		return val;

	return sprintf(buf, "0x%04x\n", val);

}

static ssize_t menf21bmc_hw_variant_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t size)
{
	struct i2c_client *client = to_i2c_client(dev);
	unsigned long hw_variant;
	int ret;

	if (kstrtoul(buf, 0, &hw_variant))
		return -EINVAL;

	if (hw_variant < 0 || hw_variant > 0xffff)
		return -EINVAL;

	ret = i2c_smbus_write_word_data(client, BMC_CMD_HW_VARIANT,
					hw_variant);
	if (ret < 0)
		return ret;

	return size;

}

static ssize_t menf21bmc_pwrcycl_cnt_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	int val;

	val = i2c_smbus_read_word_data(client, BMC_CMD_PWRCYCL_CNT);
	if (val < 0)
		return val;

	return sprintf(buf, "%d\n", val);
}

static ssize_t menf21bmc_op_hrs_cnt_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	int val;

	val = i2c_smbus_read_word_data(client, BMC_CMD_OP_HRS_CNT);
	if (val < 0)
		return val;

	return sprintf(buf, "%d\n", val);
}

static ssize_t menf21bmc_slot_address_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	int val;

	val = i2c_smbus_read_byte_data(client, BMC_CMD_SLOT_ADDRESS);
	if (val < 0)
		return val;

	return sprintf(buf, "%d\n", val);
}

static DEVICE_ATTR(mode, S_IRUGO | S_IWUSR, menf21bmc_mode_show,
		   menf21bmc_mode_store);
static DEVICE_ATTR(hw_variant, S_IRUGO | S_IWUSR, menf21bmc_hw_variant_show,
		   menf21bmc_hw_variant_store);
static DEVICE_ATTR(pwrcycl_cnt, S_IRUGO, menf21bmc_pwrcycl_cnt_show, NULL);
static DEVICE_ATTR(op_hrs_cnt, S_IRUGO, menf21bmc_op_hrs_cnt_show, NULL);
static DEVICE_ATTR(slot_address, S_IRUGO, menf21bmc_slot_address_show, NULL);

static struct attribute *menf21bmc_attributes[] = {
	&dev_attr_mode.attr,
	&dev_attr_hw_variant.attr,
	&dev_attr_pwrcycl_cnt.attr,
	&dev_attr_op_hrs_cnt.attr,
	&dev_attr_slot_address.attr,
	NULL
};

static const struct attribute_group menf21bmc_attr_group = {
	.attrs = menf21bmc_attributes,
};

static int
menf21bmc_probe(struct i2c_client *client, const struct i2c_device_id *ids)
{
	int rev_major, rev_minor, rev_main;
	int ret;

	ret = i2c_check_functionality(client->adapter,
				      I2C_FUNC_SMBUS_BYTE_DATA |
				      I2C_FUNC_SMBUS_WORD_DATA |
				      I2C_FUNC_SMBUS_BYTE);
	if (!ret)
		return -ENODEV;

	rev_major = i2c_smbus_read_word_data(client, BMC_CMD_REV_MAJOR);
	if (rev_major < 0) {
		dev_err(&client->dev, "failed to get BMC major revision\n");
		return rev_major;
	}

	rev_minor = i2c_smbus_read_word_data(client, BMC_CMD_REV_MINOR);
	if (rev_minor < 0) {
		dev_err(&client->dev, "failed to get BMC minor revision\n");
		return rev_minor;
	}

	rev_main = i2c_smbus_read_word_data(client, BMC_CMD_REV_MAIN);
	if (rev_main < 0) {
		dev_err(&client->dev, "failed to get BMC main revision\n");
		return rev_main;
	}

	dev_info(&client->dev, "FW Revision: %02d.%02d.%02d\n",
		 rev_major, rev_minor, rev_main);

	ret = sysfs_create_group(&client->dev.kobj, &menf21bmc_attr_group);
	if (ret)
		return ret;

	ret = mfd_add_devices(&client->dev, 0, menf21bmc_cell,
			      ARRAY_SIZE(menf21bmc_cell), NULL, 0, NULL);
	if (ret < 0) {
		dev_err(&client->dev, "failed to add BMC sub-devices\n");
		sysfs_remove_group(&client->dev.kobj, &menf21bmc_attr_group);
		return ret;
	}

	return 0;
}

static int menf21bmc_remove(struct i2c_client *client)
{
	sysfs_remove_group(&client->dev.kobj, &menf21bmc_attr_group);
	mfd_remove_devices(&client->dev);

	return 0;
}

static const struct i2c_device_id menf21bmc_id_table[] = {
	{ "menf21bmc" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, menf21bmc_id_table);

static struct i2c_driver menf21bmc_driver = {
	.driver.name	= "menf21bmc",
	.id_table	= menf21bmc_id_table,
	.probe		= menf21bmc_probe,
	.remove		= menf21bmc_remove,
};

module_i2c_driver(menf21bmc_driver);

MODULE_DESCRIPTION("MEN 14F021P00 BMC mfd core driver");
MODULE_AUTHOR("Andreas Werner <andreas.werner@men.de>");
MODULE_LICENSE("GPL v2");
