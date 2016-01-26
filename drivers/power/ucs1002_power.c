/*
 * Driver for UCS1002 Programmable USB Port Power Controller
 *
 * Copyright (C) 2016 Zodiac Inflight Innovations
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
 *
 */
#include <linux/freezer.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define POLL_INTERVAL		(HZ * 2)

/* UCS1002 Registers */
#define UCS1002_REG_CURRENT_MEASUREMENT	0x00

/*
 * The Total Accumulated Charge registers store the total accumulated charge
 * delivered from the VS source to a portable device. The total value is
 * calculated using four registers, from 01h to 04h. The bit weighting of
 * the registers is given in mA/hrs.
 */
#define UCS1002_REG_TOTAL_ACC_CHARGE	0x01

/* Other Status Register */
#define UCS1020_REG_OTHER_STATUS	0x0f
#  define F_ALERT_PIN			BIT(5)
#  define F_ADET_PIN			BIT(4)
#  define F_CHG_ACT			BIT(3)
#  define F_EM_ACT			BIT(2)
#  define F_EM_STEP_MASK		0x03

/* Interrupt Status */
#define UCS1002_REG_INTERRUPT_STATUS	0x10
#  define F_DISCHARGE_ERR		BIT(6)
#  define F_RESET			BIT(5)
#  define F_MIN_KEEP_OUT		BIT(4)
#  define F_TSD				BIT(3)
#  define F_OVER_VOLT			BIT(2)
#  define F_BACK_VOLT			BIT(1)
#  define F_OVER_ILIM			BIT(0)

/* Pin Status Register */
#define UCS1002_REG_PIN_STATUS		0x14
#  define UCS1002_PWR_STATE_MASK	0x03
#  define F_PWR_EN_PIN			BIT(6)
#  define F_M2_PIN			BIT(5)
#  define F_M1_PIN			BIT(4)
#  define F_EM_EN_PIN			BIT(3)
#  define F_SEL_PIN			BIT(2)
#  define F_ACTIVE_MODE_MASK		0x38
#  define F_ACTIVE_MODE_SHIFT		3

/* General Configuration Register */
#define UCS1002_REG_GENERAL_CFG	0x15
#  define F_ALERT_MASK			BIT(6)
#  define F_ALERT_LINK			BIT(5)
#  define F_DISCHARGE			BIT(4)
#  define F_RATION_EN			BIT(3)
#  define F_RATION_RST			BIT(2)
#  define F_RATION_BEH_MASK		0x03
#  define F_RATION_BEH_REPORT		0x00
#  define F_RATION_BEH_REPORT_DISCON	0x01
#  define F_RATION_BEH_DISCON_SLEEP	0x02
#  define F_RATION_BEH_IGNORE		0x03

/* Emulation Configuration Register */
#define UCS1002_REG_EMU_CFG		0x16

/* Switch Configuration Register */
#define UCS1002_REG_SWITCH_CFG		0x17
#  define F_PIN_IGNORE			BIT(7)
#  define F_EM_EN_SET			BIT(5)
#  define F_M2_SET			BIT(4)
#  define F_M1_SET			BIT(3)
#  define F_S0_SET			BIT(2)
#  define F_PWR_EN_SET			BIT(1)
#  define F_LATCH_SET			BIT(0)
#  define V_SET_ACTIVE_MODE_MASK	0x38
#  define V_SET_ACTIVE_MODE_PASSTHROUGH	F_M2_SET
#  define V_SET_ACTIVE_MODE_DEDICATED	F_EM_EN_SET
#  define V_SET_ACTIVE_MODE_BC12_DCP	(F_M2_SET | F_EM_EN_SET)
#  define V_SET_ACTIVE_MODE_BC12_SDP	F_M1_SET
#  define V_SET_ACTIVE_MODE_BC12_CDP	(F_M1_SET | F_M2_SET | F_EM_EN_SET)

/* Current Limit Register */
#define UCS1002_REG_ILIMIT		0x19
#  define UCS1002_ILIM_SW_MASK		0x07

/* High-speed Switch Configuration Register */
#define UCS1002_REG_HS_SWITCH_CFG	0x25

/* Custom Emulation Configuration Register */
#define UCS1002_REG_CUSTOM_EMU_CFG_BASE	0x40
#define V_CUSTOM_EMU_CFG_NREGS		12

/* Custom Current Limiting Behavior Config */
#define UCS1002_REG_CUSTOM_ILIMIT_CFG	0x51

/* Product ID */
#define UCS1002_REG_PRODUCT_ID		0xfd
#  define UCS1002_PRODUCT_ID		0x4e

/* Manufacture name */
#define UCS1002_MANUFACTURER		"SMSC"

/* Number of registers to set a custom profile */
#define UCS1002_PROFILE_NREGS		17

struct ucs1002_platform_data {
	struct gpio_desc *gpiod_em;
	struct gpio_desc *gpiod_m1;
	struct gpio_desc *gpiod_m2;
	struct gpio_desc *gpiod_pwr;
};

struct ucs1002_info {
	struct power_supply *charger;
	struct i2c_client *client;
	struct regmap *regmap;
	struct ucs1002_platform_data *pdata;
	struct task_struct *poll_task;

	bool curr_alarm;
	bool enabled;
	bool present;
	/* Interrupts */
	int irq_a_det;
	int irq_alert;
};

static const struct regmap_config ucs1002_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static enum power_supply_property ucs1002_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_PRESENT, /* the presence of PED */
	POWER_SUPPLY_PROP_MANUFACTURER,
};

/*
 * Iterate through each element of the 'map' array until an element whose value
 * is equal to 'value' is found. Return the index of the respective element or
 * -EINVAL if no such element is found.
 */
static int ucs1002_find_idx(int value, const int *map, int map_size)
{
	int idx;

	for (idx = 0; idx < map_size; idx++)
		if (value == map[idx])
			return idx;

	return -EINVAL;
}

static int ucs1002_power_enable(struct ucs1002_info *info, bool enable)
{
	int ret, regval;
	unsigned int val;

	/* Read the polarity setting determined by the SEL pin */
	ret = regmap_read(info->regmap, UCS1002_REG_PIN_STATUS, &regval);
	if (ret < 0)
		return ret;

	if (regval & F_SEL_PIN)
		val = enable ? F_PWR_EN_SET : 0;
	else
		val = enable ? 0 : F_PWR_EN_SET;

	if (info->pdata) {
		ret = regmap_update_bits(info->regmap, UCS1002_REG_SWITCH_CFG,
					 F_PWR_EN_SET, val);
		if (ret < 0)
			return ret;
	} else {
		gpiod_set_value_cansleep(info->pdata->gpiod_pwr, val ? 0 : 1);
	}

	info->enabled = enable;

	return 0;
}

static int ucs1002_get_online(struct ucs1002_info *info,
			      union power_supply_propval *val)
{
	int ret, regval;

	ret = regmap_read(info->regmap, UCS1020_REG_OTHER_STATUS, &regval);
	if (ret < 0)
		return -EINVAL;

	val->intval = (regval & F_CHG_ACT) ? 1 : 0;

	return 0;
}

/*
 * To fit within 32 bits some values are rounded (uA/h)
 *
 * For Total Accumulated Charge Middle Low Byte register, addr 03h, byte 2
 *
 *   B0: 0.01084 mA/h rounded to 11 uA/h
 *   B1: 0.02169 mA/h rounded to 22 uA/h
 *   B2: 0.04340 mA/h rounded to 43 uA/h
 *   B3: 0.08676 mA/h rounded to 87 uA/h
 *   B4: 0.17350 mA/h rounded to 173 uÁ/h
 *
 * For Total Accumulated Charge Low Byte register, addr 04h, byte 3
 *
 *   B6: 0.00271 mA/h rounded to 3 uA/h
 *   B7: 0.005422 mA/h rounded to 5 uA/h
 */
static const u32 ucs1002_charge_byte_values[4][8] = {
	[0] = {
		710700, 1421000, 2843000, 5685000, 11371000, 22742000,
		45484000, 90968000
	      },
	[1] = {
		2776, 5552, 11105, 22210, 44420, 88840, 177700, 355400
	      },
	[2] = {
		11, 22, 43, 87, 173, 347, 694, 1388
	      },
	[3] = {
		0, 0, 0, 0, 0, 0, 3, 5
	      }
};

static int ucs1002_get_charge(struct ucs1002_info *info,
			      union power_supply_propval *val)
{
	int i, j, ret, regval;
	unsigned int total = 0;

	ret = regmap_read(info->regmap, UCS1002_REG_GENERAL_CFG, &regval);
	if (ret < 0)
		return ret;

	for (i = 0; i < 4; i++) {
		ret = regmap_read(info->regmap,
				  UCS1002_REG_TOTAL_ACC_CHARGE + i,
				  &regval);
		if (ret < 0)
			return -EINVAL;

		for (j = 0; j < 8; j++)
			if (regval & BIT(j))
				total += ucs1002_charge_byte_values[i][j];
	}

	val->intval = total;

	return 0;
}

/*
 * The Current Measurement register stores the measured current value
 * delivered to the portable device. The range is from 9.76 mA to 2.5 A.
 * Following values are in uA.
 */
static const u32 ucs1002_current_measurement_values[] = {
	9760, 19500, 39000, 78100, 156200, 312300, 624600,
	1249300
};

static int ucs1002_get_current(struct ucs1002_info *info,
			       union power_supply_propval *val)
{
	int n, ret, regval;
	unsigned int total = 0;

	ret = regmap_read(info->regmap, UCS1002_REG_CURRENT_MEASUREMENT,
			  &regval);
	if (ret < 0)
		return -EINVAL;

	for (n = 0; n < ARRAY_SIZE(ucs1002_current_measurement_values); n++)
		if (regval & BIT(n))
			total += ucs1002_current_measurement_values[n];

	val->intval = total;

	return 0;
}

/*
 * The Current Limit register stores the maximum current used by the port
 * switch. The range is from 500mA to 2.5 A. Following values are in uA.
 */
static const u32 ucs1002_current_limit_values[] = {
	500000, 900000, 1000000, 1200000, 1500000, 1800000, 2000000, 2500000
};

static int ucs1002_get_max_current(struct ucs1002_info *info,
				   union power_supply_propval *val)
{
	int ret, regval;

	ret = regmap_read(info->regmap, UCS1002_REG_ILIMIT, &regval);
	if (ret < 0)
		return ret;

	regval &= UCS1002_ILIM_SW_MASK;
	val->intval = ucs1002_current_limit_values[regval];

	return 0;
}

static int ucs1002_set_max_current(struct ucs1002_info *info, u32 val)
{
	int ret, idx;

	idx = ucs1002_find_idx(val, ucs1002_current_limit_values,
			       ARRAY_SIZE(ucs1002_current_limit_values));
	if (idx < 0) {
		dev_err(&info->client->dev,
			"%d is an invalid max current value\n", val);
		return -EINVAL;
	}

	ret = regmap_write(info->regmap, UCS1002_REG_ILIMIT, idx);
	if (ret < 0)
		return ret;

	return 0;
}

static int ucs1002_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct ucs1002_info *info = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		return ucs1002_get_online(info, val);
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		return ucs1002_get_charge(info, val);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return ucs1002_get_current(info, val);
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		return ucs1002_get_max_current(info, val);
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = info->present ? 1 : 0;
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = UCS1002_MANUFACTURER;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ucs1002_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct ucs1002_info *info = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		return ucs1002_set_max_current(info, val->intval);
	default:
		return -EINVAL;
	}
}

static int ucs1002_property_is_writeable(struct power_supply *psy,
					 enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		return true;
	default:
		return false;
	}
}

static const struct power_supply_desc ucs1002_charger_desc = {
	.name			= "ucs1002",
	.type			= POWER_SUPPLY_TYPE_MAINS,
	.get_property		= ucs1002_get_property,
	.set_property		= ucs1002_set_property,
	.property_is_writeable	= ucs1002_property_is_writeable,
	.properties		= ucs1002_props,
	.num_properties		= ARRAY_SIZE(ucs1002_props),
};

static ssize_t ucs1002_sysfs_show_curr_alarm(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct ucs1002_info *info = power_supply_get_drvdata(psy);

	return scnprintf(buf, PAGE_SIZE, "%d\n", info->curr_alarm);
}

static ssize_t ucs1002_sysfs_show_active_mode(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	int ret, regval;
	struct power_supply *psy = dev_get_drvdata(dev);
	struct ucs1002_info *info = power_supply_get_drvdata(psy);

	ret = regmap_read(info->regmap, UCS1002_REG_PIN_STATUS, &regval);
	if (ret < 0)
		return -EINVAL;

	regval &= F_ACTIVE_MODE_MASK;
	regval = regval >> F_ACTIVE_MODE_SHIFT;

	switch (regval) {
	/* Dedicated Charger Emulation Cycle */
	case 1:
	case 3:
		return scnprintf(buf, PAGE_SIZE, "%s\n", "dedicated");
	/* Data Pass-through */
	case 4:
	case 6:
		return scnprintf(buf, PAGE_SIZE, "%s\n", "pass-through");
	/* BC1.2 SDP */
	case 2:
		return scnprintf(buf, PAGE_SIZE, "%s\n", "BC1.2-SDP");
	/* BC1.2 DCP */
	case 5:
		return scnprintf(buf, PAGE_SIZE, "%s\n", "BC1.2-DCP");
	/* BC1.2 CDP */
	case 7:
		return scnprintf(buf, PAGE_SIZE, "%s\n", "BC1.2-CDP");
	default:
		return scnprintf(buf, PAGE_SIZE, "%s\n", "unknown");
	};
}

static ssize_t ucs1002_sysfs_set_active_mode(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct ucs1002_info *info = power_supply_get_drvdata(psy);
	int mode, ret = 0;

	if (strncmp(buf, "dedicated", 9) == 0)
		mode = V_SET_ACTIVE_MODE_DEDICATED;
	else if (strncmp(buf, "pass-through", 12) == 0)
		mode = V_SET_ACTIVE_MODE_PASSTHROUGH;
	else if (strncmp(buf, "BC1.2-DCP", 9) == 0)
		mode = V_SET_ACTIVE_MODE_BC12_DCP;
	else if (strncmp(buf, "BC1.2-SDP", 9) == 0)
		mode = V_SET_ACTIVE_MODE_BC12_SDP;
	else if (strncmp(buf, "BC1.2-CDP", 9) == 0)
		mode = V_SET_ACTIVE_MODE_BC12_CDP;
	else
		return -EINVAL;

	if (info->pdata) {
		gpiod_set_value_cansleep(info->pdata->gpiod_em,
					 (mode & F_EM_EN_SET) ? 1 : 0);
		gpiod_set_value_cansleep(info->pdata->gpiod_m1,
					 (mode & F_M1_SET) ? 1 : 0);
		gpiod_set_value_cansleep(info->pdata->gpiod_m2,
					 (mode & F_M2_SET) ? 1 : 0);
	} else {
		ret = regmap_update_bits(info->regmap, UCS1002_REG_SWITCH_CFG,
					 V_SET_ACTIVE_MODE_MASK, mode);
		if (ret < 0)
			return ret;
	}

	return count;
}

static ssize_t ucs1002_sysfs_show_enabled(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct ucs1002_info *info = power_supply_get_drvdata(psy);

	return scnprintf(buf, PAGE_SIZE, "%d\n", info->enabled);
}

static ssize_t ucs1002_sysfs_set_enabled(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct ucs1002_info *info = power_supply_get_drvdata(psy);
	long val;
	int ret;

	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;

	ret = ucs1002_power_enable(info, val ? true : false);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t ucs1002_sysfs_show_profile(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct ucs1002_info *info = power_supply_get_drvdata(psy);
	int idx, regval, ret, len = 0;

	/*
	 * Read Custom Emulation Profile
	 */

	/* read registers 40h-4Ch (Custom Emulation Configuration) */
	for (idx = 0; idx < V_CUSTOM_EMU_CFG_NREGS; idx++) {
		ret = regmap_read(info->regmap,
				  UCS1002_REG_CUSTOM_EMU_CFG_BASE + idx,
				  &regval);
		if (ret < 0)
			return ret;

		len += scnprintf(&buf[len], PAGE_SIZE, "%02x ", regval);
	}

	/* read register 16h (Emulation Configuration) */
	ret = regmap_read(info->regmap, UCS1002_REG_EMU_CFG, &regval);
	if (ret < 0)
		return ret;

	len += scnprintf(&buf[len], PAGE_SIZE, "%02x ", regval);

	/* read register 19h (Current Limit) */
	ret = regmap_read(info->regmap, UCS1002_REG_ILIMIT, &regval);
	if (ret < 0)
		return ret;

	len += scnprintf(&buf[len], PAGE_SIZE, "%02x ", regval);

	/* read register 25h (High-speed Switch Configuration) */
	ret = regmap_read(info->regmap, UCS1002_REG_HS_SWITCH_CFG, &regval);
	if (ret < 0)
		return ret;

	len += scnprintf(&buf[len], PAGE_SIZE, "%02x ", regval);

	/* read register 51h (Custom Current Limiting Behavior Config) */
	ret = regmap_read(info->regmap, UCS1002_REG_CUSTOM_ILIMIT_CFG,
			  &regval);
	if (ret < 0)
		return ret;

	len += scnprintf(&buf[len], PAGE_SIZE, "%02x\n", regval);

	return len;
}

static ssize_t ucs1002_sysfs_set_profile(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct ucs1002_info *info = power_supply_get_drvdata(psy);
	const char *delim = " ";
	char profile_data[256];
	char *token, *str_ptr;
	int regval[UCS1002_PROFILE_NREGS];
	int idx = 0, ret;

	strncpy(profile_data, buf, 256);
	str_ptr = &profile_data[0];

	while (str_ptr && (idx < UCS1002_PROFILE_NREGS)) {
		token = strsep(&str_ptr, delim);
		if (kstrtoint(token, 0, &regval[idx])) {
			dev_dbg(dev, "failed to convert %s to integer\n",
				token);
			return -EINVAL;
		}
		idx++;
	}

	if (idx != UCS1002_PROFILE_NREGS) {
		dev_dbg(dev, "failed to set emulation profile (%d)\n", idx);
		return -EINVAL;
	}

	/*
	 * Write Custom Emulation Profile
	 */

	/* write registers 40h-4Ch (Custom Emulation Configuration) */
	for (idx = 0; idx < V_CUSTOM_EMU_CFG_NREGS; idx++) {
		ret = regmap_write(info->regmap,
				   UCS1002_REG_CUSTOM_EMU_CFG_BASE + idx,
				   regval[idx]);
		if (ret < 0)
			return ret;
	}

	/* write register 16h (Emulation Configuration) */
	ret = regmap_write(info->regmap, UCS1002_REG_EMU_CFG,
			   regval[idx++]);
	if (ret < 0)
		return ret;

	/* write register 19h (Current Limit) */
	ret = regmap_write(info->regmap, UCS1002_REG_ILIMIT, regval[idx++]);
	if (ret < 0)
		return ret;

	/* write register 25h (High-speed Switch Configuration) */
	ret = regmap_write(info->regmap, UCS1002_REG_HS_SWITCH_CFG,
			   regval[idx++]);
	if (ret < 0)
		return ret;

	/* write register 51h (Custom Current Limiting Behavior Config) */
	ret = regmap_write(info->regmap, UCS1002_REG_CUSTOM_ILIMIT_CFG,
			   regval[idx]);
	if (ret < 0)
		return ret;

	return count;
}

const char * const ucs1002_pwr_state_values[] = {
	"sleep", "detect", "active", "error"
};

static ssize_t ucs1002_sysfs_show_state(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	int ret, regval;
	struct power_supply *psy = dev_get_drvdata(dev);
	struct ucs1002_info *info = power_supply_get_drvdata(psy);

	ret = regmap_read(info->regmap, UCS1002_REG_PIN_STATUS, &regval);
	if (ret < 0)
		return -EINVAL;

	regval &= UCS1002_PWR_STATE_MASK;

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 ucs1002_pwr_state_values[regval]);
}

static DEVICE_ATTR(curr_alarm, S_IRUGO, ucs1002_sysfs_show_curr_alarm, NULL);
static DEVICE_ATTR(enabled, S_IWUSR | S_IRUGO, ucs1002_sysfs_show_enabled,
		   ucs1002_sysfs_set_enabled);
static DEVICE_ATTR(mode, S_IWUSR | S_IRUGO, ucs1002_sysfs_show_active_mode,
		   ucs1002_sysfs_set_active_mode);
static DEVICE_ATTR(profile, S_IWUSR | S_IRUGO, ucs1002_sysfs_show_profile,
		   ucs1002_sysfs_set_profile);
static DEVICE_ATTR(state, S_IRUGO, ucs1002_sysfs_show_state, NULL);

static struct attribute *ucs1002_specific_attr[] = {
	&dev_attr_curr_alarm.attr,
	&dev_attr_enabled.attr,
	&dev_attr_mode.attr,
	&dev_attr_profile.attr,
	&dev_attr_state.attr,
	NULL,
};

static const struct attribute_group ucs1002_attr_group = {
	.attrs = ucs1002_specific_attr,
};

static irqreturn_t ucs1002_charger_irq(int irq, void *data)
{
	int ret, regval;
	bool present;
	struct ucs1002_info *info = data;

	present = info->present;

	ret = regmap_read(info->regmap, UCS1020_REG_OTHER_STATUS, &regval);
	if (ret < 0)
		return IRQ_HANDLED;

	/* update attached status */
	info->present = (regval & F_ADET_PIN) ? true : false;

	/* notify the change */
	if (present != info->present)
		power_supply_changed(info->charger);

	return IRQ_HANDLED;
}

static irqreturn_t ucs1002_alert_irq(int irq, void *data)
{
	int ret, regval;
	struct ucs1002_info *info = data;

	ret = regmap_read(info->regmap, UCS1002_REG_INTERRUPT_STATUS, &regval);
	if (ret < 0)
		return IRQ_HANDLED;

	/* update current alarm status */
	info->curr_alarm = (regval & F_OVER_ILIM) ? true : false;

	/* over current alarm */
	if (regval & F_OVER_ILIM)
		power_supply_changed(info->charger);

	return IRQ_HANDLED;
}

static int ucs1002_poll_task(void *data)
{
	set_freezable();

	while (!kthread_should_stop()) {
		schedule_timeout_interruptible(POLL_INTERVAL);
		try_to_freeze();
		ucs1002_charger_irq(-1, data);
		ucs1002_alert_irq(-1, data);
	}
	return 0;
}

static int ucs1002_probe(struct i2c_client *client,
			 const struct i2c_device_id *dev_id)
{
	int ret, regval;
	u32 property;
	struct ucs1002_info *info;
	struct device *dev = &client->dev;
	struct power_supply_config ucs1002_charger_config = {};

	info = devm_kzalloc(&client->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->regmap = devm_regmap_init_i2c(client, &ucs1002_regmap_config);
	if (IS_ERR(info->regmap)) {
		ret = PTR_ERR(info->regmap);
		dev_err(dev, "regmap initialization failed: %d\n", ret);
		return ret;
	}

	info->client = client;
	info->curr_alarm = false;
	info->enabled = false;
	info->present = false;

	info->irq_a_det = irq_of_parse_and_map(dev->of_node, 0);
	info->irq_alert = irq_of_parse_and_map(dev->of_node, 1);

	i2c_set_clientdata(client, info);

	ucs1002_charger_config.of_node = dev->of_node;
	ucs1002_charger_config.drv_data = info;

	ret = regmap_read(info->regmap, UCS1002_REG_PRODUCT_ID, &regval);
	if (ret < 0)
		return ret;

	if (regval != UCS1002_PRODUCT_ID) {
		dev_err(dev,
			"Product ID does not match (0x%02x != 0x%02x)\n",
			regval, UCS1002_PRODUCT_ID);
		return -ENODEV;
	}

	dev_info(dev, "registered with product id 0x%02x\n",
		 UCS1002_PRODUCT_ID);

	/* Enable charge rationing by default */
	ret = regmap_update_bits(info->regmap, UCS1002_REG_GENERAL_CFG,
				 F_RATION_EN, F_RATION_EN);
	if (ret < 0)
		return ret;

	info->charger = devm_power_supply_register(dev, &ucs1002_charger_desc,
						   &ucs1002_charger_config);
	if (IS_ERR(info->charger)) {
		dev_err(dev, "failed to register power supply\n");
		return PTR_ERR(info->charger);
	}

	ret = sysfs_create_group(&info->charger->dev.kobj, &ucs1002_attr_group);
	if (ret < 0) {
		dev_err(dev, "can't create sysfs entries\n");
		return ret;
	}

	/* Optional properties */

	if (device_property_read_bool(dev, "microchip,pin-ignore")) {
		dev_dbg(dev, "set active mode selection through i2c\n");
		/*
		 * Ignore the M1, M2, PWR_EN, and EM_EN pin states. Set active
		 * mode selection to Dedicated Charger Emulation Cycle.
		 *
		 * #M1    #M2    EM_EN
		 *  0      0       1   - Dedicated Charger Emulation Cycle
		 *
		 */
		ret = regmap_update_bits(info->regmap, UCS1002_REG_SWITCH_CFG,
					 F_PIN_IGNORE | F_EM_EN_SET | F_M2_SET |
					 F_M1_SET, F_PIN_IGNORE | F_EM_EN_SET);
		if (ret < 0)
			return ret;
	} else {
		dev_dbg(dev, "set active mode selection through pins\n");
		/*
		 * PIN_IGNORE mode not set, so EM, M1 and M2 pins must be
		 * defined
		 */
		info->pdata = devm_kzalloc(dev,
					sizeof(struct ucs1002_platform_data),
					GFP_KERNEL);
		if (!info->pdata)
			return -ENOMEM;

		/* gpio for chip EM_EN pin */
		info->pdata->gpiod_em = devm_gpiod_get(dev, "em",
						       GPIOD_OUT_HIGH);
		if (IS_ERR(info->pdata->gpiod_em)) {
			dev_err(dev, "unable to claim EM_EN gpio\n");
			return PTR_ERR(info->pdata->gpiod_em);
		}

		/* Read the polarity setting determined by the SEL pin */
		ret = regmap_read(info->regmap, UCS1002_REG_PIN_STATUS,
				  &regval);
		if (ret < 0)
			return ret;

		/* gpio for chip PWR_EN pin - power off */
		info->pdata->gpiod_pwr = devm_gpiod_get(dev, "pwr",
							(regval & F_SEL_PIN) ?
							GPIOD_OUT_LOW :
							GPIOD_OUT_HIGH);
		if (IS_ERR(info->pdata->gpiod_pwr)) {
			dev_err(dev, "unable to claim PWR_EN gpio\n");
			return PTR_ERR(info->pdata->gpiod_pwr);
		}

		/* gpio for chip M1 pin */
		info->pdata->gpiod_m1 = devm_gpiod_get(dev, "m1",
						       GPIOD_OUT_LOW);
		if (IS_ERR(info->pdata->gpiod_m1)) {
			dev_err(dev, "unable to claim M1 gpio\n");
			return PTR_ERR(info->pdata->gpiod_m1);
		}

		/* gpio for chip M2 pin */
		info->pdata->gpiod_m2 = devm_gpiod_get(dev, "m2",
						       GPIOD_OUT_LOW);
		if (IS_ERR(info->pdata->gpiod_m2)) {
			dev_err(dev, "unable to claim EM_EN gpio\n");
			return PTR_ERR(info->pdata->gpiod_m2);
		}
	}

	/*
	 * The current limit is based on the resistor on the COMM_SEL / ILIM
	 * pin and this value cannot be changed to be higher than hardware
	 * set value. If the property is not set, the value set by hardware is
	 * the default.
	 */
	ret = device_property_read_u32(dev, "microchip,current-limit",
				       &property);
	if (ret == 0)
		ucs1002_set_max_current(info, property);

	/* Turn on the port power switch */
	ucs1002_power_enable(info, true);

	if (info->irq_a_det && info->irq_alert) {
		ret = devm_request_threaded_irq(dev, info->irq_a_det, NULL,
						ucs1002_charger_irq,
						IRQF_TRIGGER_FALLING |
						IRQF_TRIGGER_RISING |
						IRQF_ONESHOT,
						"ucs1002-a_det", info);
		if (ret) {
			dev_err(dev, "failed to request A_DET threaded irq\n");
			return ret;
		}

		ret = devm_request_threaded_irq(dev, info->irq_alert, NULL,
						ucs1002_alert_irq,
						IRQF_TRIGGER_FALLING |
						IRQF_TRIGGER_RISING |
						IRQF_ONESHOT,
						"ucs1002-alert", info);
		if (ret) {
			dev_err(dev, "failed to request ALERT threaded irq\n");
			return ret;
		}
	} else {
		dev_warn(dev, "no IRQ support, using polling mode\n");
		info->poll_task = kthread_run(ucs1002_poll_task, info,
					      "kucs1002");
		if (IS_ERR(info->poll_task)) {
			ret = PTR_ERR(info->poll_task);
			dev_err(dev, "unable to run kthread err (%d)\n", ret);
			return ret;
		}
	}

	return 0;
}

static int ucs1002_remove(struct i2c_client *client)
{
	struct ucs1002_info *info = i2c_get_clientdata(client);

	if (info->poll_task)
		kthread_stop(info->poll_task);

	sysfs_remove_group(&info->charger->dev.kobj, &ucs1002_attr_group);

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id ucs1002_of_match[] = {
	{ .compatible = "microchip,ucs1002", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ucs1002_of_match);
#endif

static const struct i2c_device_id ucs1002_ids[] = {
	{"ucs1002", 0},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i2c, ucs1002_ids);

static struct i2c_driver ucs1002_driver = {
	.driver = {
		   .name = "ucs1002",
		   .of_match_table = of_match_ptr(ucs1002_of_match),
	},
	.probe = ucs1002_probe,
	.remove = ucs1002_remove,
	.id_table = ucs1002_ids,
};
module_i2c_driver(ucs1002_driver);

MODULE_DESCRIPTION("Microchip UCS1002 Programmable USB Port Power Controller");
MODULE_AUTHOR("Enric Balletbo Serra <enric.balletbo@collabora.com>");
MODULE_LICENSE("GPL v2");
