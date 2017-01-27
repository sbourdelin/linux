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

#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

#include "linux/i2c/arcxcnn.h"

#define ARCXCNN_CMD		0x00	/* Command Register */
#define ARCXCNN_CMD_STDBY	0x80	/*   I2C Standby */
#define ARCXCNN_CMD_RESET	0x40	/*   Reset */
#define ARCXCNN_CMD_BOOST	0x10	/*   Boost */
#define ARCXCNN_CMD_OVP_MASK	0x0C	/*   --- Over Voltage Threshold */
#define ARCXCNN_CMD_OVP_XXV	0x0C	/*   <rsvrd> Over Voltage Threshold */
#define ARCXCNN_CMD_OVP_20V	0x08	/*   20v Over Voltage Threshold */
#define ARCXCNN_CMD_OVP_24V	0x04	/*   24v Over Voltage Threshold */
#define ARCXCNN_CMD_OVP_31V	0x00	/*   31.4v Over Voltage Threshold */
#define ARCXCNN_CMD_EXT_COMP	0x01	/*   part (0) or full (1) ext. comp */

#define ARCXCNN_CONFIG		0x01	/* Configuration */
#define ARCXCNN_STATUS1		0x02	/* Status 1 */
#define ARCXCNN_STATUS2		0x03	/* Status 2 */
#define ARCXCNN_FADECTRL	0x04	/* Fading Control */
#define ARCXCNN_ILED_CONFIG	0x05	/* ILED Configuration */
#define ARCXCNN_ILED_DIM_PWM	0x00	/*   config dim mode pwm */
#define ARCXCNN_ILED_DIM_INT	0x04	/*   config dim mode internal */
#define ARCXCNN_LEDEN		0x06	/* LED Enable Register */
#define ARCXCNN_LEDEN_ISETEXT	0x80	/*   Full-scale current set extern */
#define ARCXCNN_LEDEN_MASK	0x3F	/*   LED string enables mask */
#define ARCXCNN_LEDEN_BITS	0x06	/*   Bits of LED string enables */
#define ARCXCNN_LEDEN_LED1	0x01
#define ARCXCNN_LEDEN_LED2	0x02
#define ARCXCNN_LEDEN_LED3	0x04
#define ARCXCNN_LEDEN_LED4	0x08
#define ARCXCNN_LEDEN_LED5	0x10
#define ARCXCNN_LEDEN_LED6	0x20

#define ARCXCNN_WLED_ISET_LSB	0x07	/* LED ISET LSB (in upper nibble) */
#define ARCXCNN_WLED_ISET_LSB_SHIFT 0x04  /* ISET LSB Left Shift */
#define ARCXCNN_WLED_ISET_MSB	0x08	/* LED ISET MSB (8 bits) */

#define ARCXCNN_DIMFREQ		0x09
#define ARCXCNN_COMP_CONFIG	0x0A
#define ARCXCNN_FILT_CONFIG	0x0B
#define ARCXCNN_IMAXTUNE	0x0C
#define ARCXCNN_ID_MSB		0x1E
#define ARCXCNN_ID_LSB		0x1F

#define MAX_BRIGHTNESS		4095

static int s_no_reset_on_remove;
module_param_named(noreset, s_no_reset_on_remove, int, 0644);
MODULE_PARM_DESC(noreset, "No reset on module removal");

static int s_ibright = 60;
module_param_named(ibright, s_ibright, int, 0644);
MODULE_PARM_DESC(ibright, "Initial brightness (when no plat data)");

static int s_ileden = ARCXCNN_LEDEN_MASK;
module_param_named(ileden, s_ileden, int, 0644);
MODULE_PARM_DESC(ileden, "Initial LED String Enables (when no plat data)");

struct arcxcnn {
	char chipname[64];
	struct i2c_client *client;
	struct backlight_device *bl;
	struct device *dev;
	struct arcxcnn_platform_data *pdata;
};

static int arcxcnn_update_bit(struct arcxcnn *lp, u8 reg, u8 mask, u8 data)
{
	int ret;
	u8 tmp;

	ret = i2c_smbus_read_byte_data(lp->client, reg);
	if (ret < 0) {
		dev_err(lp->dev, "failed to read 0x%.2x\n", reg);
		return ret;
	}

	tmp = (u8)ret;
	tmp &= ~mask;
	tmp |= data & mask;

	return i2c_smbus_write_byte_data(lp->client, reg, tmp);
}

static int arcxcnn_set_brightness(struct arcxcnn *lp, u32 brightness)
{
	int ret;
	u8 val;

	/* lower nibble of brightness goes in upper nibble of LSB register */
	val = (brightness & 0xF) << ARCXCNN_WLED_ISET_LSB_SHIFT;
	ret = i2c_smbus_write_byte_data(lp->client, ARCXCNN_WLED_ISET_LSB, val);
	if (ret < 0)
		return ret;

	/* remaining 8 bits of brightness go in MSB register */
	val = (brightness >> 4);
	ret = i2c_smbus_write_byte_data(lp->client, ARCXCNN_WLED_ISET_MSB, val);

	return ret;
}

static int arcxcnn_bl_update_status(struct backlight_device *bl)
{
	struct arcxcnn *lp = bl_get_data(bl);
	u32 brightness = bl->props.brightness;

	if (bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	arcxcnn_set_brightness(lp, brightness);

	/* set power-on/off/save modes */
	arcxcnn_update_bit(lp, ARCXCNN_CMD, ARCXCNN_CMD_STDBY,
		(bl->props.power == 0) ? 0 : ARCXCNN_CMD_STDBY);

	return 0;
}

static const struct backlight_ops arcxcnn_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = arcxcnn_bl_update_status,
};

static int arcxcnn_backlight_register(struct arcxcnn *lp)
{
	struct backlight_properties *props;
	const char *name = lp->pdata->name ? : "arctic_bl";

	props = devm_kzalloc(lp->dev, sizeof(*props), GFP_KERNEL);
	if (!props)
		return -ENOMEM;

	props->type = BACKLIGHT_PLATFORM;
	props->max_brightness = MAX_BRIGHTNESS;

	if (lp->pdata->initial_brightness > props->max_brightness)
		lp->pdata->initial_brightness = props->max_brightness;

	props->brightness = lp->pdata->initial_brightness;

	lp->bl = devm_backlight_device_register(lp->dev, name, lp->dev, lp,
				       &arcxcnn_bl_ops, props);
	if (IS_ERR(lp->bl))
		return PTR_ERR(lp->bl);

	return 0;
}

static ssize_t arcxcnn_chip_id_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct arcxcnn *lp = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%s\n", lp->chipname);
}

static ssize_t arcxcnn_leden_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct arcxcnn *lp = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%02X\n", lp->pdata->leden);
}

static ssize_t arcxcnn_leden_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct arcxcnn *lp = dev_get_drvdata(dev);
	unsigned long leden;

	if (kstrtoul(buf, 0, &leden))
		return 0;

	if (leden != lp->pdata->leden) {
		/* don't allow 0 for leden, use power to turn all off */
		if (leden == 0)
			return -EINVAL;
		lp->pdata->leden = leden & ARCXCNN_LEDEN_MASK;
		arcxcnn_update_bit(lp, ARCXCNN_LEDEN,
			ARCXCNN_LEDEN_MASK, lp->pdata->leden);
	}

	return len;
}

static DEVICE_ATTR(chip_id, 0444, arcxcnn_chip_id_show, NULL);
static DEVICE_ATTR(leden, 0664, arcxcnn_leden_show, arcxcnn_leden_store);

static struct attribute *arcxcnn_attributes[] = {
	&dev_attr_chip_id.attr,
	&dev_attr_leden.attr,
	NULL,
};

static const struct attribute_group arcxcnn_attr_group = {
	.attrs = arcxcnn_attributes,
};

static void arcxcnn_parse_dt(struct arcxcnn *lp)
{
	struct device *dev = lp->dev;
	struct device_node *node = dev->of_node;
	u32 prog_val, num_entry, entry, sources[ARCXCNN_LEDEN_BITS];
	int ret;

	/* device tree entry isn't required, defaults are OK */
	if (!node)
		return;

	ret = of_property_read_string(node, "label", &lp->pdata->name);
	if (ret < 0)
		lp->pdata->name = NULL;

	ret = of_property_read_u32(node, "default-brightness", &prog_val);
	if (ret == 0)
		lp->pdata->initial_brightness = prog_val;

	ret = of_property_read_u32(node, "arcticsand,led-config-0", &prog_val);
	if (ret == 0)
		lp->pdata->led_config_0 = (u8)prog_val;

	ret = of_property_read_u32(node, "arcticsand,led-config-1", &prog_val);
	if (ret == 0)
		lp->pdata->led_config_1 = (u8)prog_val;

	ret = of_property_read_u32(node, "arcticsand,dim-freq", &prog_val);
	if (ret == 0)
		lp->pdata->dim_freq = (u8)prog_val;

	ret = of_property_read_u32(node, "arcticsand,comp-config", &prog_val);
	if (ret == 0)
		lp->pdata->comp_config = (u8)prog_val;

	ret = of_property_read_u32(node, "arcticsand,filter-config", &prog_val);
	if (ret == 0)
		lp->pdata->filter_config = (u8)prog_val;

	ret = of_property_read_u32(node, "arcticsand,trim-config", &prog_val);
	if (ret == 0)
		lp->pdata->trim_config = (u8)prog_val;

	ret = of_property_count_u32_elems(node, "led-sources");
	if (ret < 0) {
		lp->pdata->leden = ARCXCNN_LEDEN_MASK; /* all on is default */
	} else {
		num_entry = ret;
		if (num_entry > ARCXCNN_LEDEN_BITS)
			num_entry = ARCXCNN_LEDEN_BITS;

		ret = of_property_read_u32_array(node, "led-sources", sources,
					num_entry);
		if (ret < 0) {
			dev_err(dev, "led-sources node is invalid.\n");
			return;
		}

		lp->pdata->leden = 0;

		/* for each enable in source, set bit in led enable */
		for (entry = 0; entry < num_entry; entry++) {
			u8 onbit = 1 << sources[entry];

			lp->pdata->leden |= onbit;
		}
	}
}

static int arcxcnn_probe(struct i2c_client *cl, const struct i2c_device_id *id)
{
	struct arcxcnn *lp;
	int ret;
	u8 regval;
	u16 chipid;

	if (!i2c_check_functionality(cl->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	lp = devm_kzalloc(&cl->dev, sizeof(*lp), GFP_KERNEL);
	if (!lp)
		return -ENOMEM;

	lp->client = cl;
	lp->dev = &cl->dev;
	lp->pdata = dev_get_platdata(&cl->dev);

	/* reset the device */
	i2c_smbus_write_byte_data(lp->client,
		ARCXCNN_CMD, ARCXCNN_CMD_RESET);

	/* read device ID */
	regval = i2c_smbus_read_byte_data(lp->client, ARCXCNN_ID_MSB);
	chipid = regval;
	chipid <<= 8;

	regval = i2c_smbus_read_byte_data(lp->client, ARCXCNN_ID_LSB);
	chipid |= regval;

	snprintf(lp->chipname, sizeof(lp->chipname),
		"%s-%04X", id->name, chipid);

	if (!lp->pdata) {
		lp->pdata = devm_kzalloc(lp->dev,
				sizeof(*lp->pdata), GFP_KERNEL);
		if (!lp->pdata)
			return -ENOMEM;

		/* Setup defaults based on power-on defaults */
		lp->pdata->name = NULL;
		lp->pdata->initial_brightness = s_ibright;
		lp->pdata->leden = s_ileden;

		lp->pdata->led_config_0 = i2c_smbus_read_byte_data(
			lp->client, ARCXCNN_FADECTRL);

		lp->pdata->led_config_1 = i2c_smbus_read_byte_data(
			lp->client, ARCXCNN_ILED_CONFIG);
		/* insure dim mode is not default pwm */
		lp->pdata->led_config_1 |= ARCXCNN_ILED_DIM_INT;

		lp->pdata->dim_freq = i2c_smbus_read_byte_data(
			lp->client, ARCXCNN_DIMFREQ);

		lp->pdata->comp_config = i2c_smbus_read_byte_data(
			lp->client, ARCXCNN_COMP_CONFIG);

		lp->pdata->filter_config = i2c_smbus_read_byte_data(
			lp->client, ARCXCNN_FILT_CONFIG);

		lp->pdata->trim_config = i2c_smbus_read_byte_data(
			lp->client, ARCXCNN_IMAXTUNE);

		if (IS_ENABLED(CONFIG_OF))
			arcxcnn_parse_dt(lp);
	}

	i2c_set_clientdata(cl, lp);

	/* constrain settings to what is possible */
	if (lp->pdata->initial_brightness > MAX_BRIGHTNESS)
		lp->pdata->initial_brightness = MAX_BRIGHTNESS;

	/* set initial brightness */
	arcxcnn_set_brightness(lp, lp->pdata->initial_brightness);

	/* set other register values directly */
	i2c_smbus_write_byte_data(lp->client, ARCXCNN_FADECTRL,
		lp->pdata->led_config_0);
	i2c_smbus_write_byte_data(lp->client, ARCXCNN_ILED_CONFIG,
		lp->pdata->led_config_1);
	i2c_smbus_write_byte_data(lp->client, ARCXCNN_DIMFREQ,
		lp->pdata->dim_freq);
	i2c_smbus_write_byte_data(lp->client, ARCXCNN_COMP_CONFIG,
		lp->pdata->comp_config);
	i2c_smbus_write_byte_data(lp->client, ARCXCNN_FILT_CONFIG,
		lp->pdata->filter_config);
	i2c_smbus_write_byte_data(lp->client, ARCXCNN_IMAXTUNE,
		lp->pdata->trim_config);

	/* set initial LED Enables */
	arcxcnn_update_bit(lp, ARCXCNN_LEDEN,
		ARCXCNN_LEDEN_MASK, lp->pdata->leden);

	ret = arcxcnn_backlight_register(lp);
	if (ret) {
		dev_err(lp->dev,
			"failed to register backlight. err: %d\n", ret);
		return ret;
	}

	ret = sysfs_create_group(&lp->dev->kobj, &arcxcnn_attr_group);
	if (ret) {
		dev_err(lp->dev, "failed to register sysfs. err: %d\n", ret);
		return ret;
	}

	backlight_update_status(lp->bl);

	return 0;
}

static int arcxcnn_remove(struct i2c_client *cl)
{
	struct arcxcnn *lp = i2c_get_clientdata(cl);

	if (!s_no_reset_on_remove) {
		/* disable all strings */
		i2c_smbus_write_byte_data(lp->client,
			ARCXCNN_LEDEN, 0x00);
		/* reset the device */
		i2c_smbus_write_byte_data(lp->client,
			ARCXCNN_CMD, ARCXCNN_CMD_RESET);
	}
	lp->bl->props.brightness = 0;

	backlight_update_status(lp->bl);

	sysfs_remove_group(&lp->dev->kobj, &arcxcnn_attr_group);

	return 0;
}

static const struct of_device_id arcxcnn_dt_ids[] = {
	{ .compatible = "arc,arc2c0608" },
	{ }
};
MODULE_DEVICE_TABLE(of, arcxcnn_dt_ids);

static const struct i2c_device_id arcxcnn_ids[] = {
	{"arc2c0608", ARC2C0608},
	{ }
};
MODULE_DEVICE_TABLE(i2c, arcxcnn_ids);

static struct i2c_driver arcxcnn_driver = {
	.driver = {
		.name = "arcxcnn_bl",
		.of_match_table = of_match_ptr(arcxcnn_dt_ids),
	},
	.probe = arcxcnn_probe,
	.remove = arcxcnn_remove,
	.id_table = arcxcnn_ids,
};
module_i2c_driver(arcxcnn_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Brian Dodge <bdodge@arcticsand.com>");
MODULE_DESCRIPTION("ARCXCNN Backlight driver");
