// SPDX-License-Identifier: GPL-1.0+
/*
 * Driver for BQ2416X Li-Ion Battery Charger
 *
 * Copyright (C) 2015 Verifone, Inc.
 * Wojciech Ziemba <wojciech.ziemba@verifone.com>
 *
 * Copyright (C) 2018 EFE GmbH
 * Karsten Müller <mueller.k@efe-gmbh.de>
 * Jens Renner <renner@efe-gmbh.de>
 *
 * The bq2416x series is a 2.5A, Dual-Input, Single-Cell Switched-Mode
 * Li-Ion Battery Charger with Power Path Management and I2C Interface.
 *
 * This driver was tested on BQ24160.
 *
 * Datasheets:
 * http://www.ti.com/product/bq24160
 * http://www.ti.com/product/bq24160a
 * http://www.ti.com/product/bq24161
 * http://www.ti.com/product/bq24161b
 * http://www.ti.com/product/bq24163
 * http://www.ti.com/product/bq24168
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/pm_runtime.h>
#include <linux/hwmon-sysfs.h>
#include <linux/regmap.h>
#include <linux/power_supply.h>

/* Get the value of bitfield */
#define BF_GET(_y, _mask) (((_y) & _mask) >> (__builtin_ffs((int) _mask) - 1))
/* Shift the value of bitfield. Mask based */
#define BF_SHIFT(_x, _mask) ((_x) << (__builtin_ffs((int) _mask) - 1))
/* Watchdog timer. 10 seconds in reserve */
#define BQ2416X_WATCHDOG_TIMER	(30 - 10)
/* Register numbers */
#define BQ2416X_REG_STATUS	0x00
#define BQ2416X_REG_SUP_STATUS	0x01
#define BQ2416X_REG_CONTROL	0x02
#define BQ2416X_REG_BAT_VOLT	0x03
#define BQ2416X_REG_VENDOR	0x04
#define BQ2416X_REG_TERM	0x05
#define BQ2416X_REG_DPM		0x06
#define BQ2416X_REG_NTC		0x07
#define BQ2416X_REG_MAX		0x08

/* status/control register */
#define BQ2416X_REG_STATUS_TMR_RST_MASK	BIT(7)
#define BQ2416X_REG_STATUS_STAT_MASK		(BIT(6) | BIT(5) | BIT(4))
#define BQ2416X_REG_STATUS_SUPPLY_SEL_MASK	BIT(3)
#define BQ2416X_REG_STATUS_FAULT_MASK		(BIT(2) | BIT(1) | BIT(0))

/* battery/supply status register */
#define BQ2416X_REG_SUP_STATUS_INSTAT_MASK	(BIT(7) | BIT(6))
#define BQ2416X_REG_SUP_STATUS_USBSTAT_MASK	(BIT(5) | BIT(4))
#define BQ2416X_REG_SUP_STATUS_OTG_LOCK_MASK	BIT(3)
#define BQ2416X_REG_SUP_STATUS_BATSTAT_MASK	(BIT(2) | BIT(1))
#define BQ2416X_REG_SUP_STATUS_EN_NOBATOP_MASK	BIT(0)

/* control register */
#define BQ2416X_REG_CONTROL_RESET_MASK		BIT(7)
#define BQ2416X_REG_CONTROL_USB_CURR_LIM_MASK	(BIT(6) | BIT(5) | BIT(4))
#define BQ2416X_REG_CONTROL_EN_STAT_MASK	BIT(3)
#define BQ2416X_REG_CONTROL_TE_MASK		BIT(2)
#define BQ2416X_REG_CONTROL_CE_MASK		BIT(1)
#define BQ2416X_REG_CONTROL_HZ_MODE_MASK	BIT(0)

/* control/battery voltage register */
#define BQ2416X_REG_BAT_VOLT_MASK		(BIT(7) | BIT(6) | BIT(5) | \
						BIT(4) | BIT(3) | BIT(2))
#define BQ2416X_REG_BAT_VOLT_IN_CURR_LIM_MASK	BIT(1)
#define BQ2416X_REG_BAT_VOLT_USB_DETECT_MASK	BIT(0)

/* vendor/part/revision register */
#define BQ2416X_REG_VENDOR_REV_MASK		(BIT(2) | BIT(1) | BIT(0))
#define BQ2416X_REG_VENDOR_CODE_MASK		(BIT(7) | BIT(6) | BIT(5))

/* battery termination fast charge current register */
#define BQ2416X_REG_TERM_CHRG_CURR_MASK	(BIT(7) | BIT(6) | BIT(5) | \
						BIT(4) | BIT(3))
#define BQ2416X_REG_TERM_TERM_CURR_MASK	(BIT(2) | BIT(1) | BIT(0))

/* VIN-DPM voltage/DPPM status register */
#define BQ2416X_REG_DPM_MINSYS_STATUS_MASK	BIT(7)
#define BQ2416X_REG_DPM_STATUS_MASK		BIT(6)
#define BQ2416X_REG_DPM_USB_VOLT_MASK		(BIT(5) | BIT(4) | BIT(3))
#define BQ2416X_REG_DPM_IN_VOLT_MASK		(BIT(2) | BIT(1) | BIT(0))

/* Safety timer/NTC monitor register */
#define BQ2416X_REG_NTC_TMRX2_MASK		BIT(7)
#define BQ2416X_REG_NTC_TMR_MASK		(BIT(6) | BIT(5))
#define BQ2416X_REG_NTC_TS_EN_MASK		BIT(3)
#define BQ2416X_REG_NTC_TS_FAULT_MASK		(BIT(2) | BIT(1))
#define BQ2416X_REG_NTC_LOW_CHARGE_MASK	BIT(0)

/* Charge voltage [uV] */
#define BQ2416X_CHARGE_VOLTAGE_MIN		3500000
#define BQ2416X_CHARGE_VOLTAGE_MAX		4440000
#define BQ2416X_CHARGE_VOLTAGE_STEP		20000

/* IN current limit */
#define BQ2416X_IN_CURR_LIM_1500		0
#define BQ2416X_IN_CURR_LIM_2500		1

/* Charge current [uA] */
#define BQ2416X_CHARGE_CURRENT_MIN		550000
#define BQ2416X_CHARGE_CURRENT_MAX		2500000
#define BQ2416X_CHARGE_CURRENT_STEP		75000

/* Charge termination current in uA */
#define BQ2416X_CHARGE_TERM_CURRENT_MIN		50000
#define BQ2416X_CHARGE_TERM_CURRENT_MAX		400000
#define BQ2416X_CHARGE_TERM_CURRENT_STEP	50000

/* USB DPM voltage [uV] */
#define BQ2416X_DPM_USB_VOLTAGE_MIN		4200000
#define BQ2416X_DPM_USB_VOLTAGE_MAX		4760000
#define BQ2416X_DPM_USB_VOLTAGE_STEP		80000

/* IN DPM voltage [uV] */
#define BQ2416X_DPM_IN_VOLTAGE_MIN		4200000
#define BQ2416X_DPM_IN_VOLTAGE_MAX		4760000
#define BQ2416X_DPM_IN_VOLTAGE_STEP		80000

/* Supported chips */
enum  bq2416x_type {
	BQ24160 = 0,
	BQ24160A,
	BQ24161,
	BQ24161B,
	BQ24163,
	BQ24168,
};

/* Charger status */
enum {
	STAT_NO_VALID_SOURCE = 0,
	STAT_IN_READY,
	STAT_USB_READY,
	STAT_CHARGING_FROM_IN,
	STAT_CHARGING_FROM_USB,
	STAT_CHARGE_DONE,
	STAT_NA,
	STAT_FAULT,
};

/* Charger status to power subsys status map */
static const int bq2416x_charge_status[] = {
	[STAT_NO_VALID_SOURCE] = POWER_SUPPLY_STATUS_NOT_CHARGING,
	[STAT_IN_READY] = POWER_SUPPLY_STATUS_NOT_CHARGING,
	[STAT_USB_READY] = POWER_SUPPLY_STATUS_NOT_CHARGING,
	[STAT_CHARGING_FROM_IN] = POWER_SUPPLY_STATUS_CHARGING,
	[STAT_CHARGING_FROM_USB] = POWER_SUPPLY_STATUS_CHARGING,
	[STAT_CHARGE_DONE] = POWER_SUPPLY_STATUS_FULL,
	[STAT_NA] = POWER_SUPPLY_STATUS_UNKNOWN,
	[STAT_FAULT] = POWER_SUPPLY_STATUS_NOT_CHARGING,
};

/* Charger fault */
enum {
	FAULT_NORMAL = 0,
	FAULT_THERMAL_SHUTDOWN,
	FAULT_BATT_TEMP_FAULT,
	FAULT_WDOG_TIMER_EXPIRED,
	FAULT_SAFETY_TIMER_EXPIRED,
	FAULT_IN_SUPPLY_FAULT,
	FAULT_USB_SUPPLY_FAULT,
	FAULT_BATTERY_FAULT,
};

/* Charger fault to power subsys fault map */
static const struct {
	const char * const str;
	const int id;
} bq2416x_charge_fault[] = {
	[FAULT_NORMAL] = {"Normal", POWER_SUPPLY_HEALTH_GOOD},
	[FAULT_THERMAL_SHUTDOWN] = {"Thermal shutdown",
				POWER_SUPPLY_HEALTH_OVERHEAT},
	[FAULT_BATT_TEMP_FAULT] = {"Battery temp fault",
				POWER_SUPPLY_HEALTH_OVERHEAT},
	[FAULT_WDOG_TIMER_EXPIRED] = {"Watchdog timer expired",
				POWER_SUPPLY_HEALTH_WATCHDOG_TIMER_EXPIRE},
	[FAULT_SAFETY_TIMER_EXPIRED] = {"Safety timer expired",
				POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE},
	[FAULT_IN_SUPPLY_FAULT] = {"IN Supply fault",
				POWER_SUPPLY_HEALTH_UNSPEC_FAILURE},
	[FAULT_USB_SUPPLY_FAULT] = {"USB Supply fault",
				POWER_SUPPLY_HEALTH_UNSPEC_FAILURE},
	[FAULT_BATTERY_FAULT] = {"Battery fault", POWER_SUPPLY_HEALTH_DEAD},
};

/* IN(Wall) source status */
enum {
	INSTAT_NORMAL = 0,
	INSTAT_SUPPLY_OVP,
	INSTAT_WEAK_SOURCE_CONNECTED,
	INSTAT_FAULTY_ADAPTER,
};

/* IN(Wall) source status to string map */
static const char * const bq2416x_in_status[] = {
	[INSTAT_NORMAL] = "Normal",
	[INSTAT_SUPPLY_OVP] = "OVP",
	[INSTAT_WEAK_SOURCE_CONNECTED] = "Weak source",
	[INSTAT_FAULTY_ADAPTER] = "Faulty adapter",
};

/* Battery status */
enum {
	BATSTAT_BATTERY_PRESENT = 0,
	BATSTAT_BATTERY_OVP,
	BATSTAT_BATTERY_NOT_PRESENT,
	BATSTAT_BATTERY_NA,
};

/* IN(Wall) source limit */
enum in_curr_lim {
	IN_CURR_LIM_1500MA = 0,
	IN_CURR_LIM_2500MA,
};

/*
 * USB source current limit
 * AUTO as auto detection (BQ24160/0A/3) or limit of default mode
 */
enum usb_curr_lim {
	USB_CURR_LIM_AUTO   = -EINVAL,
	USB_CURR_LIM_100MA  = 0,
	USB_CURR_LIM_150MA  = 1,
	USB_CURR_LIM_500MA  = 2,
	USB_CURR_LIM_800MA  = 3,
	USB_CURR_LIM_900MA  = 4,
	USB_CURR_LIM_1500MA = 5,
};

/* Safety timer settings */
enum safe_tmr {
	TMR_27MIN = 0,
	TMR_6H,
	TMR_9H,
	TMR_OFF,
};

static const int bq2416x_usb_curr_lim[] = {
	[USB_CURR_LIM_100MA]	= 100000,
	[USB_CURR_LIM_150MA]	= 150000,
	[USB_CURR_LIM_500MA]	= 500000,
	[USB_CURR_LIM_800MA]	= 800000,
	[USB_CURR_LIM_900MA]	= 900000,
	[USB_CURR_LIM_1500MA]	= 1500000,
};

static const int bq24160_in_lim[] = {
	[IN_CURR_LIM_1500MA] = 1500000,
	[IN_CURR_LIM_2500MA] = 2500000,
};

/* External NTC Monitoring(TS) fault */
enum {
	TS_FAULT_NORMAL,
	TS_FAULT_COLD_HOT,
	TS_FAULT_COOL,
	TS_FAULT_WARM,
};

static const char * const bq2416x_ts_fault[] = {
	[TS_FAULT_NORMAL] = "normal",
	[TS_FAULT_COLD_HOT] = "cold/hot (charge suspended)",
	[TS_FAULT_COOL] = "cool (half current charge)",
	[TS_FAULT_WARM] = "warm (voltage reduced)",
};

/* Firmware response: chip revision */
enum {
	VENDOR_REV_10 = 0,
	VENDOR_REV_11,
	VENDOR_REV_20,
	VENDOR_REV_21,
	VENDOR_REV_22,
	VENDOR_REV_23,
};

static const char * const bq2416x_revision[] = {
	[VENDOR_REV_10] = "1.0",
	[VENDOR_REV_11] = "1.1",
	[VENDOR_REV_20] = "2.0",
	[VENDOR_REV_21] = "2.1",
	[VENDOR_REV_22] = "2.2",
	[VENDOR_REV_23] = "2.3",
};

/**
 * struct bq2416x_initdata - Config data for bq2416x chip.
 * It contains default board voltages and currents.
 * @charge_voltage: charge voltage in [uV]
 * @charge_current: charge current in [uA]
 * @in_curr_limit: Current limit for IN source. Enum 1.5A or 2.5A
 * @usb_curr_limit: Current limit for USB source.
 *  Enum 100mA - 1500mA or AUTO for using auto detection
 * @curr_term_en: enable charge termination by current
 * @nobat_mode: enable no battery operation mode
 *  (charge disable, curr_term = off, nobat_op = 1)
 * @term_current: charge termination current in [uA]
 * @usb_dpm_voltage: USB DPM voltage [uV]
 * @in_dpm_voltage: IN DPM voltage [uV]
 * @stat_pin_en: status pin enable
 * @safety_timer: safety timer enum: 27min, 6h, 9h, off.
 */
struct bq2416x_initdata {
	int charge_voltage;
	int charge_current;
	enum in_curr_lim in_curr_limit;
	enum usb_curr_lim usb_curr_limit;
	int curr_term_en;
	int nobat_mode;
	int term_current;
	int usb_dpm_voltage;
	int in_dpm_voltage;
	int stat_pin_en;
	enum safe_tmr safety_timer;
};

/* Default: no battery mode, battery can be specified in DT */
const struct bq2416x_initdata initdefault = {
	.charge_voltage		= 4200000,
	.charge_current		= 0,
	.in_curr_limit		= IN_CURR_LIM_1500MA,
	.usb_curr_limit		= USB_CURR_LIM_AUTO,
	.curr_term_en		= 0,
	.nobat_mode		= 1,
	.term_current		= 0,
	.usb_dpm_voltage	= 4200000,
	.in_dpm_voltage		= 4200000,
	.stat_pin_en		= 1,
	.safety_timer		= TMR_9H
};

/**
 * struct bq2416x_priv - this device's private data
 * @dev: this device
 * @regmap: register map for bq2416x
 * @pdata: platform data
 * @psy: power-supply class for this device
 * @watchdog: watchdog worker
 * @model: model of this device
 * @name: the name of this device instance
 * @idr: the id of this chip
 */
struct bq2416x_priv {
	struct device *dev;
	struct regmap *regmap;
	struct bq2416x_initdata idata;
	struct power_supply *psy;
	struct power_supply_desc psy_desc;
	struct delayed_work watchdog;
	u8 fault_reg;
	u8 stat_reg;
	char *model;
	char *name;
	int idr;
};

/* each registered chip must have a unique id */
static DEFINE_IDR(bq2416x_idr);
static DEFINE_MUTEX(bq2416x_idr_mutex);

/*
 * Return the index in 'tbl' of greatest value that is less than or equal to
 * 'val'.  The index range returned is 0 to 'tbl_size' - 1.  Assumes that
 * the values in 'tbl' are sorted from smallest to largest and 'tbl_size'
 * is less than 2^8.
 */
static u8 bq2416x_find_idx(const int tbl[], int tbl_size, int v)
{
	int i;

	for (i = 1; i < tbl_size; i++)
		if (v < tbl[i])
			break;

	return i - 1;
}

/*
 * Converts value to its regulation binary representation.
 */
static inline unsigned int conv2bit_repr(unsigned int val, unsigned int min,
					unsigned int max, unsigned int step)
{
	return (clamp_val(val, min, max) - min) / step;
}

/*
 * Regmap callbacks and configuration
 * All registers except vendor register are writeable and should not be cached.
 */
static bool bq2416x_writeable(struct device *dev, unsigned int reg)
{
	return !(reg == BQ2416X_REG_VENDOR);
}

static struct regmap_config bq2416x_i2c_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.writeable_reg = bq2416x_writeable,
	.volatile_reg = bq2416x_writeable,
	.cache_type = REGCACHE_NONE,
	.max_register = BQ2416X_REG_MAX,
};

/* Power-supply class callbacks and configuration */
static int bq2416x_property_is_writeable(struct power_supply *psy,
					enum power_supply_property psp)
{
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = 1;
		break;
	default:
		ret = 0;
	}

	return ret;
}

static enum power_supply_property bq2416x_power_supply_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_SCOPE
};

static int bq2416x_set_status(struct bq2416x_priv *bq2416x, int status)
{
	unsigned int charge_disable;
	int ret;

	if (status == POWER_SUPPLY_STATUS_CHARGING)
		charge_disable = 0;
	else if (status == POWER_SUPPLY_STATUS_NOT_CHARGING)
		charge_disable = BQ2416X_REG_CONTROL_CE_MASK;
	else
		return -EINVAL;

	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_CONTROL,
			BQ2416X_REG_CONTROL_RESET_MASK |
			BQ2416X_REG_CONTROL_CE_MASK,
			charge_disable);

	return ret;
}

static int bq2416x_get_status(struct bq2416x_priv *bq2416x, int *status)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_STATUS, &reg_val);
	if (unlikely(ret))
		return ret;

	reg_val = BF_GET(reg_val, BQ2416X_REG_STATUS_STAT_MASK);
	*status = bq2416x_charge_status[reg_val];

	return ret;
}

static int bq2416x_get_charge_type(struct bq2416x_priv *bq2416x,
					int *charge_type)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_STATUS, &reg_val);
	if (unlikely(ret))
		return ret;

	reg_val = BF_GET(reg_val, BQ2416X_REG_STATUS_STAT_MASK);
	if (bq2416x_charge_status[reg_val] != POWER_SUPPLY_STATUS_CHARGING)
		*charge_type = POWER_SUPPLY_CHARGE_TYPE_NONE;
	else {
		ret = regmap_read(bq2416x->regmap, BQ2416X_REG_NTC, &reg_val);
		if (unlikely(ret))
			return ret;

		if (reg_val & BQ2416X_REG_NTC_LOW_CHARGE_MASK)
			*charge_type = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		else
			*charge_type = POWER_SUPPLY_CHARGE_TYPE_FAST;
	}

	return ret;
}

static int bq2416x_set_charge_type(struct bq2416x_priv *bq2416x,
					int type)
{
	int ret;
	unsigned int charge_disable;
	unsigned int low_charge;

	switch (type) {
	case POWER_SUPPLY_CHARGE_TYPE_NONE:
		charge_disable = BQ2416X_REG_CONTROL_CE_MASK;
		low_charge = 0;
		break;
	case POWER_SUPPLY_CHARGE_TYPE_TRICKLE:
		charge_disable = 0;
		low_charge = BQ2416X_REG_NTC_LOW_CHARGE_MASK;
		break;
	case POWER_SUPPLY_CHARGE_TYPE_FAST:
		charge_disable = 0;
		low_charge = 0;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(bq2416x->regmap,
				BQ2416X_REG_CONTROL,
				BQ2416X_REG_CONTROL_RESET_MASK |
				BQ2416X_REG_CONTROL_CE_MASK,
				charge_disable);
	if (unlikely(ret))
		return ret;

	ret = regmap_update_bits(bq2416x->regmap,
				BQ2416X_REG_NTC,
				BQ2416X_REG_NTC_LOW_CHARGE_MASK,
				low_charge);

	return ret;
}

static int bq2416x_get_bat_present(struct bq2416x_priv *bq2416x, int *present)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_SUP_STATUS, &reg_val);
	if (unlikely(ret))
		return ret;
	reg_val = BF_GET(reg_val, BQ2416X_REG_SUP_STATUS_BATSTAT_MASK);

	*present = (reg_val == BATSTAT_BATTERY_PRESENT) ||
			(reg_val == BATSTAT_BATTERY_OVP);

	return ret;
}

static int bq2416x_get_online(struct bq2416x_priv *bq2416x, int *online)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_STATUS, &reg_val);
	if (unlikely(ret))
		return ret;

	reg_val = BF_GET(reg_val, BQ2416X_REG_STATUS_STAT_MASK);
	*online = ((reg_val > STAT_NO_VALID_SOURCE) && (reg_val < STAT_NA));

	return ret;
}

static int bq2416x_get_charge_current(struct bq2416x_priv *bq2416x,
					int *curr)
{
	int ret;
	unsigned int low_charge;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_TERM, curr);
	if (unlikely(ret))
		return ret;

	*curr = BF_GET(*curr, BQ2416X_REG_TERM_CHRG_CURR_MASK) *
		BQ2416X_CHARGE_CURRENT_STEP +
		BQ2416X_CHARGE_CURRENT_MIN;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_NTC, &low_charge);
	if (unlikely(ret))
		return ret;

	/* halve the current value if in low_charge state */
	*curr >>= low_charge & BQ2416X_REG_NTC_LOW_CHARGE_MASK;

	return ret;
}

static int bq2416x_set_charge_current(struct bq2416x_priv *bq2416x, int curr)
{
	int ret;
	unsigned int reg_bits;

	reg_bits = conv2bit_repr(curr, BQ2416X_CHARGE_CURRENT_MIN,
			BQ2416X_CHARGE_CURRENT_MAX,
			BQ2416X_CHARGE_CURRENT_STEP);

	ret = regmap_update_bits(bq2416x->regmap,
				BQ2416X_REG_TERM,
				BQ2416X_REG_TERM_CHRG_CURR_MASK,
				BF_SHIFT(reg_bits,
				BQ2416X_REG_TERM_CHRG_CURR_MASK));
	if (unlikely(ret))
		return ret;

	/* unset low charge */
	ret = regmap_update_bits(bq2416x->regmap,
				BQ2416X_REG_NTC,
				BQ2416X_REG_NTC_LOW_CHARGE_MASK,
				0);
	return ret;
}

static int bq2416x_get_charge_voltage(struct bq2416x_priv *bq2416x,
					int *voltage)
{
	int ret;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_BAT_VOLT, voltage);
	if (unlikely(ret))
		return ret;

	*voltage = BF_GET(*voltage, BQ2416X_REG_BAT_VOLT_MASK) *
		BQ2416X_CHARGE_VOLTAGE_STEP +
		BQ2416X_CHARGE_VOLTAGE_MIN;

	return ret;
}

static int bq2416x_set_charge_voltage(struct bq2416x_priv *bq2416x,
					int voltage)
{
	int ret;
	unsigned int reg_bits;

	reg_bits = conv2bit_repr(voltage, BQ2416X_CHARGE_VOLTAGE_MIN,
			BQ2416X_CHARGE_VOLTAGE_MAX,
			BQ2416X_CHARGE_VOLTAGE_STEP);

	ret = regmap_update_bits(bq2416x->regmap,
				BQ2416X_REG_BAT_VOLT,
				BQ2416X_REG_BAT_VOLT_MASK,
				BF_SHIFT(reg_bits, BQ2416X_REG_BAT_VOLT_MASK));
	return ret;
}

static int bq2416x_get_term_current(struct bq2416x_priv *bq2416x,
					unsigned int *term_current)
{
	int term_curr;
	int ret;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_TERM, &term_curr);
	if (unlikely(ret))
		return ret;

	*term_current = BF_GET(term_curr, BQ2416X_REG_TERM_TERM_CURR_MASK) *
			BQ2416X_CHARGE_TERM_CURRENT_STEP +
			BQ2416X_CHARGE_TERM_CURRENT_MIN;
	return ret;
}

static int bq2416x_set_term_current(struct bq2416x_priv *bq2416x,
					int term_curr)
{
	int ret;
	unsigned int reg_bits;

	reg_bits = conv2bit_repr(term_curr, BQ2416X_CHARGE_TERM_CURRENT_MIN,
			BQ2416X_CHARGE_TERM_CURRENT_MAX,
			BQ2416X_CHARGE_TERM_CURRENT_STEP);

	ret = regmap_update_bits(bq2416x->regmap,
				BQ2416X_REG_TERM,
				BQ2416X_REG_TERM_TERM_CURR_MASK,
				BF_SHIFT(reg_bits,
				BQ2416X_REG_TERM_TERM_CURR_MASK));
	if (unlikely(ret))
		return ret;

	ret = regmap_update_bits(bq2416x->regmap,
				BQ2416X_REG_NTC,
				BQ2416X_REG_NTC_LOW_CHARGE_MASK,
				0);
	return ret;
}

static int bq2416x_set_usb_dpm_voltage(struct bq2416x_priv *bq2416x,
					int dpm_volt)
{
	int ret;
	unsigned int reg_bits;

	reg_bits = conv2bit_repr(dpm_volt, BQ2416X_DPM_USB_VOLTAGE_MIN,
			BQ2416X_DPM_USB_VOLTAGE_MAX,
			BQ2416X_DPM_USB_VOLTAGE_STEP);

	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_DPM,
			BQ2416X_REG_DPM_USB_VOLT_MASK,
			BF_SHIFT(reg_bits, BQ2416X_REG_DPM_USB_VOLT_MASK));

	return ret;
}

static int bq2416x_set_in_dpm_voltage(struct bq2416x_priv *bq2416x,
					int dpm_volt)
{
	int ret;
	unsigned int reg_bits;

	reg_bits = conv2bit_repr(dpm_volt, BQ2416X_DPM_IN_VOLTAGE_MIN,
			BQ2416X_DPM_IN_VOLTAGE_MAX,
			BQ2416X_DPM_IN_VOLTAGE_STEP);

	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_DPM,
			BQ2416X_REG_DPM_IN_VOLT_MASK,
			BF_SHIFT(reg_bits, BQ2416X_REG_DPM_IN_VOLT_MASK));
	return ret;
}

/*
 * Check (and clear) fault bits, detect new fault status and save last fault.
 * It should be called before reading status register to not lose any faults.
 */
static int bq2416x_check_fault(struct bq2416x_priv *bq2416x, bool klog_en)
{
	int ret, reg_val, fault;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_STATUS, &reg_val);
	if (ret)
		return ret;

	fault = BF_GET(reg_val, BQ2416X_REG_STATUS_FAULT_MASK);

	if (fault && (fault != bq2416x->fault_reg)) {
		if (klog_en)
			dev_warn(bq2416x->dev, "%s",
					bq2416x_charge_fault[fault].str);
		bq2416x->fault_reg = fault;
	}
	return 0;
}

/*
 * USB current detection, only available for bq24160/0A/3.
 * No effect on other chips.
 */
static int bq2416x_current_detection(struct bq2416x_priv *bq2416x)
{
	int ret;

	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_BAT_VOLT,
			BQ2416X_REG_BAT_VOLT_USB_DETECT_MASK,
			BQ2416X_REG_BAT_VOLT_USB_DETECT_MASK);
	dev_dbg(bq2416x->dev, "Current detection");
	return ret;
}

static int bq2416x_reset_watchdog_tmr(struct bq2416x_priv *bq2416x)
{
	int ret;

	/* update of watchdog bit would clear fault bits */
	bq2416x_check_fault(bq2416x, 1);
	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_STATUS,
			BQ2416X_REG_STATUS_TMR_RST_MASK,
			BQ2416X_REG_STATUS_TMR_RST_MASK);
	if (unlikely(ret))
		dev_err(bq2416x->dev, "Can't reset watchdog timer\n");

	return ret;
}

/*
 * Check and returns last detected fault and reset fault state.
 * (No additional IRQ gets triggered in case of a persistent fault).
 * State 'good' only is returned if no fault occurred since last call.
 */
static int bq2416x_get_health(struct bq2416x_priv *bq2416x, int *health)
{
	bq2416x_check_fault(bq2416x, 0);
	*health = bq2416x_charge_fault[bq2416x->fault_reg].id;
	bq2416x->fault_reg = 0;
	return 0;
}

static int bq2416x_configure(struct bq2416x_priv *bq2416x)
{
	struct bq2416x_initdata *idata = &bq2416x->idata;
	int ret;
	unsigned int mask, bits;

	ret = bq2416x_reset_watchdog_tmr(bq2416x);
	if (unlikely(ret))
		return ret;

	/* sequence of register writes important for nobat operation */
	mask =  BQ2416X_REG_CONTROL_RESET_MASK |
			BQ2416X_REG_CONTROL_EN_STAT_MASK |
			BQ2416X_REG_CONTROL_CE_MASK;

	bits =  BF_SHIFT(idata->stat_pin_en,
			BQ2416X_REG_CONTROL_EN_STAT_MASK) |
			BF_SHIFT(idata->nobat_mode,
			BQ2416X_REG_CONTROL_CE_MASK);

	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_CONTROL,
			mask,
			bits);
	if (unlikely(ret))
		return ret;

	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_CONTROL,
			BQ2416X_REG_CONTROL_RESET_MASK |
			BQ2416X_REG_CONTROL_TE_MASK,
			BF_SHIFT(idata->curr_term_en,
				BQ2416X_REG_CONTROL_TE_MASK));
	if (unlikely(ret))
		return ret;

	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_SUP_STATUS,
			BQ2416X_REG_SUP_STATUS_EN_NOBATOP_MASK,
			BF_SHIFT(idata->nobat_mode,
				BQ2416X_REG_SUP_STATUS_EN_NOBATOP_MASK));
	if (unlikely(ret))
		return ret;

	ret = bq2416x_set_charge_voltage(bq2416x, idata->charge_voltage);
	if (unlikely(ret))
		return ret;

	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_BAT_VOLT,
			BQ2416X_REG_BAT_VOLT_IN_CURR_LIM_MASK,
			BF_SHIFT(idata->in_curr_limit,
				BQ2416X_REG_BAT_VOLT_IN_CURR_LIM_MASK));
	if (unlikely(ret))
		return ret;

	if (idata->usb_curr_limit != USB_CURR_LIM_AUTO) {
		ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_CONTROL, BQ2416X_REG_CONTROL_RESET_MASK |
			BQ2416X_REG_CONTROL_USB_CURR_LIM_MASK,
			BF_SHIFT(idata->usb_curr_limit,
				BQ2416X_REG_CONTROL_USB_CURR_LIM_MASK));
		if (unlikely(ret))
			return ret;
	}

	ret = bq2416x_set_charge_current(bq2416x, idata->charge_current);
	if (unlikely(ret))
		return ret;

	ret = bq2416x_set_term_current(bq2416x, idata->term_current);
	if (unlikely(ret))
		return ret;

	ret = bq2416x_set_usb_dpm_voltage(bq2416x, idata->usb_dpm_voltage);
	if (unlikely(ret))
		return ret;

	ret = bq2416x_set_in_dpm_voltage(bq2416x, idata->in_dpm_voltage);
	if (unlikely(ret))
		return ret;

	mask = BQ2416X_REG_NTC_TMR_MASK | BQ2416X_REG_NTC_TS_EN_MASK;
	bits = BF_SHIFT(idata->safety_timer, BQ2416X_REG_NTC_TMR_MASK);

	/* disable battery temp monitor, if no battery defined */
	if (!idata->nobat_mode)
		bits |= BQ2416X_REG_NTC_TS_EN_MASK;

	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_NTC, mask, bits);

	return ret;
}

/*
 * Status pin interrupt handler. It sends uevent upon charger status change
 */
static irqreturn_t bq2416x_thread_irq(int irq, void *priv)
{
	struct bq2416x_priv *bq2416x = priv;

	dev_dbg(bq2416x->dev, "IRQ");
	bq2416x_check_fault(bq2416x, 1);
	if (bq2416x->idata.usb_curr_limit == -EINVAL)
		bq2416x_current_detection(bq2416x);

	/* Give registers some time */
	msleep(300);

	power_supply_changed(bq2416x->psy);

	return IRQ_HANDLED;
}

/*
 * Worker for watchdog timer reset.
 */
static void bq2416x_watchdog_work(struct work_struct *work)
{
	struct bq2416x_priv *bq2416x = container_of(work, struct bq2416x_priv,
						 watchdog.work);

	pm_runtime_get_sync(bq2416x->dev);
	bq2416x_reset_watchdog_tmr(bq2416x);
	pm_runtime_put_sync(bq2416x->dev);

	schedule_delayed_work(&bq2416x->watchdog, BQ2416X_WATCHDOG_TIMER * HZ);
}

/* power-supply class property callbacks */

static int bq2416x_psy_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret = 0;

	val->intval = POWER_SUPPLY_STATUS_UNKNOWN;

	pm_runtime_get_sync(bq2416x->dev);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = bq2416x_get_status(bq2416x, &val->intval);
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = bq2416x->model;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Texas Instruments";
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		ret = bq2416x_get_bat_present(bq2416x, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = bq2416x_get_charge_type(bq2416x, &val->intval);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = bq2416x_get_health(bq2416x, &val->intval);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		ret = bq2416x_get_online(bq2416x, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = bq2416x_get_charge_current(bq2416x, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = BQ2416X_CHARGE_CURRENT_MAX;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = bq2416x_get_charge_voltage(bq2416x, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		val->intval = BQ2416X_CHARGE_VOLTAGE_MAX;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = bq2416x_get_term_current(bq2416x, &val->intval);
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	default:
		return -EINVAL;
	}

	pm_runtime_put_sync(bq2416x->dev);
	return ret;
}

static int bq2416x_psy_set_property(struct power_supply *psy,
					enum power_supply_property psp,
					const union power_supply_propval *val)
{
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;

	pm_runtime_get_sync(bq2416x->dev);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = bq2416x_set_status(bq2416x, val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = bq2416x_set_charge_type(bq2416x, val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = bq2416x_set_charge_current(bq2416x, val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = bq2416x_set_charge_voltage(bq2416x, val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		ret = bq2416x_set_term_current(bq2416x, val->intval);
		break;
	default:
		ret = -EINVAL;
	}

	pm_runtime_put_sync(bq2416x->dev);
	return ret;
}

/* device attributes callbacks */


static ssize_t supply_sel_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;
	unsigned int val;
	const char *str;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_STATUS, &val);
	if (unlikely(ret != 0))
		return ret;
	if (val & BQ2416X_REG_STATUS_SUPPLY_SEL_MASK)
		str = "usb";
	else
		str = "in";

	return sprintf(buf, "%s\n", str);
}

static ssize_t supply_sel_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;

	if (strncmp(buf, "usb", 3) == 0)
		ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_STATUS,
			BQ2416X_REG_STATUS_SUPPLY_SEL_MASK,
			BQ2416X_REG_STATUS_SUPPLY_SEL_MASK);
	else if (strncmp(buf, "in", 2) == 0)
		ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_STATUS,
			BQ2416X_REG_STATUS_SUPPLY_SEL_MASK,
			0);
	else
		ret = -EINVAL;

	if (ret)
		return ret;

	return count;
}

static ssize_t supply_status_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;
	unsigned int val;
	const char *str;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_SUP_STATUS, &val);
	if (unlikely(ret != 0))
		return ret;

	if (strcmp(attr->attr.name, "in_status") == 0) {
		val = BF_GET(val, BQ2416X_REG_SUP_STATUS_INSTAT_MASK);
		str = bq2416x_in_status[val];
	} else if (strcmp(attr->attr.name, "usb_status") == 0) {
		val = BF_GET(val, BQ2416X_REG_SUP_STATUS_USBSTAT_MASK);
		str = bq2416x_in_status[val];
	} else
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%s\n", str);
}

static ssize_t in_current_limit_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;
	unsigned int limit;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_BAT_VOLT, &limit);
	if (unlikely(ret))
		return ret;

	limit = BF_GET(limit, BQ2416X_REG_BAT_VOLT_IN_CURR_LIM_MASK);

	return scnprintf(buf, PAGE_SIZE, "%d\n", bq24160_in_lim[limit]);

}

static ssize_t in_current_limit_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;
	unsigned int reg_bits, limit;

	ret = kstrtouint(buf, 0, &limit);
	if (unlikely(ret))
		return -EINVAL;

	if (limit < 2500000)
		reg_bits = BQ2416X_IN_CURR_LIM_1500;
	else
		reg_bits = BQ2416X_IN_CURR_LIM_2500;

	ret = regmap_update_bits(bq2416x->regmap,
				BQ2416X_REG_BAT_VOLT,
				BQ2416X_REG_BAT_VOLT_IN_CURR_LIM_MASK,
				BF_SHIFT(reg_bits,
				BQ2416X_REG_BAT_VOLT_IN_CURR_LIM_MASK));
	if (unlikely(ret))
		return ret;

	return count;
}

static ssize_t usb_current_limit_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;
	unsigned int limit;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_CONTROL, &limit);
	if (unlikely(ret != 0))
		return ret;

	limit = BF_GET(limit, BQ2416X_REG_CONTROL_USB_CURR_LIM_MASK);

	return scnprintf(buf, PAGE_SIZE, "%d\n", bq2416x_usb_curr_lim[limit]);
}

static ssize_t usb_current_limit_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;
	unsigned int curr, reg_bits;

	ret = kstrtouint(buf, 0, &curr);
	if (unlikely(ret))
		return -EINVAL;

	reg_bits = bq2416x_find_idx(bq2416x_usb_curr_lim,
				ARRAY_SIZE(bq2416x_usb_curr_lim), curr);

	ret = regmap_update_bits(bq2416x->regmap,
			BQ2416X_REG_CONTROL, BQ2416X_REG_CONTROL_RESET_MASK |
			BQ2416X_REG_CONTROL_USB_CURR_LIM_MASK,
			BF_SHIFT(reg_bits,
			BQ2416X_REG_CONTROL_USB_CURR_LIM_MASK));
	if (ret)
		return ret;

	return count;
}

static ssize_t dpm_voltage_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;
	unsigned int reg_val, dpm_volt;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_DPM, &reg_val);
	if (unlikely(ret != 0))
		return ret;

	if (strcmp(attr->attr.name, "usb_dpm_voltage") == 0)
		dpm_volt = BF_GET(reg_val, BQ2416X_REG_DPM_USB_VOLT_MASK);
	else if (strcmp(attr->attr.name, "in_dpm_voltage") == 0)
		dpm_volt = BF_GET(reg_val, BQ2416X_REG_DPM_IN_VOLT_MASK);
	else
		return -EINVAL;

	dpm_volt = dpm_volt * BQ2416X_DPM_IN_VOLTAGE_STEP +
		   BQ2416X_DPM_IN_VOLTAGE_MIN;

	return scnprintf(buf, PAGE_SIZE, "%d\n", dpm_volt);
}

static ssize_t dpm_voltage_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf,
					size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;
	unsigned int dpm_volt;

	ret = kstrtouint(buf, 0, &dpm_volt);
	if (unlikely(ret))
		return -EINVAL;

	if (strcmp(attr->attr.name, "usb_dpm_voltage") == 0)
		ret = bq2416x_set_usb_dpm_voltage(bq2416x, dpm_volt);
	else if (strcmp(attr->attr.name, "in_dpm_voltage") == 0)
		ret = bq2416x_set_in_dpm_voltage(bq2416x, dpm_volt);
	else
		ret =  -EINVAL;

	if (ret)
		return ret;

	return count;

}

static ssize_t ts_fault_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	int ret;
	unsigned int val;
	const char *str;

	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_NTC, &val);
	if (unlikely(ret != 0))
		return ret;

	val = BF_GET(val, BQ2416X_REG_NTC_TS_FAULT_MASK);
	str = bq2416x_ts_fault[val];

	return scnprintf(buf, PAGE_SIZE, "%s\n", str);
}

static ssize_t sysfs_bit_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	int ret;
	unsigned int reg_val;
	unsigned int reg = sattr->nr, mask = sattr->index;

	ret = regmap_read(bq2416x->regmap, reg, &reg_val);
	if (unlikely(ret != 0))
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%d\n", !!(reg_val & mask));
}

static ssize_t sysfs_bit_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct bq2416x_priv *bq2416x = power_supply_get_drvdata(psy);
	struct sensor_device_attribute_2 *sattr = to_sensor_dev_attr_2(attr);
	int ret;
	unsigned int bits;
	unsigned int reg = sattr->nr, mask = sattr->index;

	if (strncmp(buf, "1", 1) == 0)
		bits = mask;
	else if (strncmp(buf, "0", 1) == 0)
		bits = 0;
	else
		return -EINVAL;

	/* clear reset bit before writeback */
	if (reg == BQ2416X_REG_CONTROL)
		mask |= BQ2416X_REG_CONTROL_RESET_MASK;

	ret = regmap_update_bits(bq2416x->regmap,
			reg,
			mask,
			bits);
	if (unlikely(ret))
		return ret;

	return count;
}

#define BIT_DEVICE_ATTR(_name, _mode, _reg, _bit)			\
	SENSOR_DEVICE_ATTR_2(_name, _mode, sysfs_bit_show,	\
	sysfs_bit_store, _reg, _bit)

static DEVICE_ATTR_RW(supply_sel);
static DEVICE_ATTR(in_status, 0444,
			supply_status_show, NULL);
static DEVICE_ATTR(usb_status, 0444,
			supply_status_show, NULL);
static DEVICE_ATTR_RW(in_current_limit);
static DEVICE_ATTR_RW(usb_current_limit);
static BIT_DEVICE_ATTR(high_impedance_enable, 0644,
			BQ2416X_REG_CONTROL,
			BQ2416X_REG_CONTROL_HZ_MODE_MASK);
static BIT_DEVICE_ATTR(dpm_status, 0444,
			BQ2416X_REG_DPM,
			BQ2416X_REG_DPM_STATUS_MASK);
static DEVICE_ATTR(usb_dpm_voltage, 0644,
			dpm_voltage_show,
			dpm_voltage_store);
static DEVICE_ATTR(in_dpm_voltage, 0644,
			dpm_voltage_show,
			dpm_voltage_store);
static DEVICE_ATTR_RO(ts_fault);

static struct attribute *bq2416x_sysfs_attributes[] = {
	&dev_attr_supply_sel.attr,
	&dev_attr_in_status.attr,
	&dev_attr_usb_status.attr,
	&dev_attr_in_current_limit.attr,
	&dev_attr_usb_current_limit.attr,
	&sensor_dev_attr_high_impedance_enable.dev_attr.attr,
	&sensor_dev_attr_dpm_status.dev_attr.attr,
	&dev_attr_usb_dpm_voltage.attr,
	&dev_attr_in_dpm_voltage.attr,
	&dev_attr_ts_fault.attr,
	NULL,
};

static const struct attribute_group bq2416x_sysfs_attr_group = {
	.attrs = bq2416x_sysfs_attributes,
};

static const struct of_device_id bq2416x_of_match[] = {
	{ .compatible = "ti,bq24160" },
	{ .compatible = "ti,bq24160a" },
	{ .compatible = "ti,bq24161" },
	{ .compatible = "ti,bq24161b" },
	{ .compatible = "ti,bq24163" },
	{ .compatible = "ti,bq24168" },
	{},
};
MODULE_DEVICE_TABLE(of, bq2416x_of_match);

static char *bq2416x_charger_supplied_to[] = {
	"main-battery",
};

/*
 * If battery present set charging options
 * otherwise stay in no battery modus
 * (ce = off, curr_term = off, nobat_op = 1)
 */
static void bq2416x_idata_from_of(struct bq2416x_priv *bq2416x)
{
	struct bq2416x_initdata *idata = &bq2416x->idata;
	struct power_supply_battery_info info = {};
	int ret, v;
	unsigned int prop;

	*idata = initdefault;

	if (!power_supply_get_battery_info(bq2416x->psy, &info)) {
		idata->nobat_mode = 0;
		v = info.charge_term_current_ua;
		if (v  != -EINVAL) {
			idata->term_current = v;
			idata->curr_term_en = 1;
		}

		v = info.constant_charge_current_max_ua;
		idata->charge_current = v;

		v = info.constant_charge_voltage_max_uv;
		idata->charge_voltage = v;
	}

	ret = device_property_read_u32(bq2416x->dev,
				"ti,in-current-limit-microamp", &prop);
	if (!ret && prop >= 2500000)
		idata->in_curr_limit = BQ2416X_IN_CURR_LIM_2500;

	ret = device_property_read_u32(bq2416x->dev,
				"ti,usb-current-limit-microamp", &prop);
	if (!ret)
		idata->usb_curr_limit = bq2416x_find_idx(bq2416x_usb_curr_lim,
					ARRAY_SIZE(bq2416x_usb_curr_lim), prop);

	ret = device_property_read_u32(bq2416x->dev,
				"ti,usb-dpm-voltage-microvolt", &prop);
	if (!ret)
		idata->usb_dpm_voltage = prop;

	ret = device_property_read_u32(bq2416x->dev,
				"ti,in-dpm-voltage-microvolt", &prop);
	if (!ret)
		idata->in_dpm_voltage = prop;
}

/* power management op */

static int __maybe_unused bq2416x_suspend(struct device *dev)
{
	struct bq2416x_priv *bq2416x = dev_get_drvdata(dev);

	cancel_delayed_work(&bq2416x->watchdog);

	pm_runtime_get_sync(bq2416x->dev);
	bq2416x_set_charge_type(bq2416x, POWER_SUPPLY_CHARGE_TYPE_NONE);
	pm_runtime_put_sync(bq2416x->dev);

	return 0;
}

static int __maybe_unused bq2416x_resume(struct device *dev)
{
	struct bq2416x_priv *bq2416x = dev_get_drvdata(dev);

	pm_runtime_get_sync(bq2416x->dev);
	bq2416x_reset_watchdog_tmr(bq2416x);
	bq2416x_configure(bq2416x);
	pm_runtime_put_sync(bq2416x->dev);

	schedule_delayed_work(&bq2416x->watchdog, BQ2416X_WATCHDOG_TIMER * HZ);

	power_supply_changed(bq2416x->psy);

	return 0;
}

static SIMPLE_DEV_PM_OPS(bq2416x_pm_ops, bq2416x_suspend, bq2416x_resume);

static int bq2416x_device_init(struct bq2416x_priv *bq2416x)
{
	int ret;
	unsigned int vendor_reg, vendor_code, revision;
	struct power_supply_config psy_cfg = { .drv_data = bq2416x };

	dev_set_drvdata(bq2416x->dev, bq2416x);

	pm_runtime_get_sync(bq2416x->dev);
	ret = regmap_read(bq2416x->regmap, BQ2416X_REG_VENDOR, &vendor_reg);
	if (unlikely(ret)) {
		dev_err(bq2416x->dev, "Can't read vendor code\n");
		return ret;
	}
	pm_runtime_put_sync(bq2416x->dev);

	vendor_code = BF_GET(vendor_reg, BQ2416X_REG_VENDOR_CODE_MASK);
	revision = BF_GET(vendor_reg, BQ2416X_REG_VENDOR_REV_MASK);

	dev_info(bq2416x->dev, "Found BQ2416X, code: 0x%02x rev: %s\n",
			vendor_code, bq2416x_revision[revision]);

	bq2416x->psy_desc.name = bq2416x->name;
	bq2416x->psy_desc.type = POWER_SUPPLY_TYPE_USB;
	bq2416x->psy_desc.properties = bq2416x_power_supply_props;
	bq2416x->psy_desc.get_property = bq2416x_psy_get_property;
	bq2416x->psy_desc.set_property = bq2416x_psy_set_property;
	bq2416x->psy_desc.num_properties =
					ARRAY_SIZE(bq2416x_power_supply_props);
	psy_cfg.of_node = bq2416x->dev->of_node;
	psy_cfg.supplied_to = bq2416x_charger_supplied_to;
	psy_cfg.num_supplicants = ARRAY_SIZE(bq2416x_charger_supplied_to);
	bq2416x->psy_desc.property_is_writeable = bq2416x_property_is_writeable;

	bq2416x->psy = power_supply_register(bq2416x->dev, &bq2416x->psy_desc,
						&psy_cfg);
	if (unlikely(IS_ERR(bq2416x->psy))) {
		dev_err(bq2416x->dev, "Can't register power supply\n");
		return PTR_ERR(bq2416x->psy);
	}

	if (bq2416x->dev->of_node)
		bq2416x_idata_from_of(bq2416x);
	return ret;
}

int bq2416x_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(i2c->dev.parent);
	struct bq2416x_priv *bq2416x;
	int ret, idr;
	char *model, *name;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&i2c->dev, "No support for SMBUS_BYTE_DATA\n");
		return -ENODEV;
	}

	/* Get id for the new charger device */
	mutex_lock(&bq2416x_idr_mutex);
	idr = idr_alloc(&bq2416x_idr, i2c, 0, 0, GFP_KERNEL);
	mutex_unlock(&bq2416x_idr_mutex);

	if (IS_ERR_VALUE((unsigned long) idr))
		return idr;

	model = devm_kzalloc(&i2c->dev, strlen(id->name), GFP_KERNEL);
	if (unlikely(!model)) {
		dev_err(&i2c->dev, "Failed to allocate name\n");
		ret = -ENOMEM;
		goto err_rel_id;
	}
	strncpy(model, id->name, strlen(id->name));

	bq2416x = devm_kzalloc(&i2c->dev, sizeof(*bq2416x), GFP_KERNEL);
	if (unlikely(!bq2416x)) {
		dev_err(&i2c->dev, "Failed to allocate private data\n");
		ret = -ENOMEM;
		goto err_rel_id;
	}

	bq2416x->regmap = devm_regmap_init_i2c(i2c, &bq2416x_i2c_regmap);
	if (IS_ERR(bq2416x->regmap)) {
		ret = PTR_ERR(bq2416x->regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		goto err_rel_id;
	}

	name = kasprintf(GFP_KERNEL, "%s-%d", id->name, idr);
	if (unlikely(!name)) {
		dev_err(&i2c->dev, "Failed to allocate device name\n");
		ret = -ENOMEM;
		goto err_rel_id;
	}

	pm_runtime_enable(&i2c->dev);
	pm_runtime_resume(&i2c->dev);

	bq2416x->dev  = &i2c->dev;
	bq2416x->idr  = idr;
	bq2416x->model = model;
	bq2416x->name  = name;

	ret = bq2416x_device_init(bq2416x);
	if (ret)
		goto err_free_name;

	ret = bq2416x_configure(bq2416x);
	if (unlikely(ret)) {
		dev_err(bq2416x->dev, "Initial configuration failed\n");
		goto err_unregister_psy;
	}

	ret = devm_request_threaded_irq(&i2c->dev, i2c->irq, NULL,
				bq2416x_thread_irq, IRQF_TRIGGER_RISING |
				IRQF_ONESHOT, "bq2416xinterrupt", bq2416x);
	if (ret) {
		dev_err(&i2c->dev, "Can't request IRQ\n");
		goto err_unregister_psy;
	}

	ret = sysfs_create_group(&bq2416x->psy->dev.kobj,
			&bq2416x_sysfs_attr_group);
	if (unlikely(ret)) {
		dev_err(bq2416x->dev, "Can't create sysfs entries\n");
		goto err_unregister_psy;
	}

	INIT_DELAYED_WORK(&bq2416x->watchdog, bq2416x_watchdog_work);
	schedule_delayed_work(&bq2416x->watchdog, BQ2416X_WATCHDOG_TIMER * HZ);

	return 0;

err_unregister_psy:
	power_supply_unregister(bq2416x->psy);
err_free_name:
	pm_runtime_disable(&i2c->dev);
	kfree(name);
err_rel_id:
	mutex_lock(&bq2416x_idr_mutex);
	idr_remove(&bq2416x_idr, idr);
	mutex_unlock(&bq2416x_idr_mutex);

	return ret;
}

static int bq2416x_i2c_remove(struct i2c_client *i2c)
{
	struct bq2416x_priv *bq2416x = i2c_get_clientdata(i2c);

	cancel_delayed_work_sync(&bq2416x->watchdog);
	sysfs_remove_group(&bq2416x->psy->dev.kobj, &bq2416x_sysfs_attr_group);
	power_supply_unregister(bq2416x->psy);
	pm_runtime_disable(bq2416x->dev);

	mutex_lock(&bq2416x_idr_mutex);
	idr_remove(&bq2416x_idr, bq2416x->idr);
	mutex_unlock(&bq2416x_idr_mutex);

	kfree(bq2416x->name);

	return 0;
}

static const struct i2c_device_id  bq2416x_i2c_id[] = {
	{ "bq24160",  BQ24160 },
	{ "bq24160a", BQ24160A },
	{ "bq24161",  BQ24161 },
	{ "bq24161b", BQ24161B },
	{ "bq24163",  BQ24163 },
	{ "bq24168",  BQ24168 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bq2416x_i2c_id);

static struct i2c_driver bq2416x_i2c_driver = {
	.driver = {
		.name	= "bq2416x-charger",
		.of_match_table = of_match_ptr(bq2416x_of_match),
		.pm     = &bq2416x_pm_ops,
	},
	.probe		= bq2416x_i2c_probe,
	.remove		= bq2416x_i2c_remove,
	.id_table	= bq2416x_i2c_id,
};

module_i2c_driver(bq2416x_i2c_driver);

MODULE_DESCRIPTION("TI BQ2416x battery charger driver");
MODULE_AUTHOR("Karsten Mueller <mueller.k@efe-gmbh.de>");
MODULE_LICENSE("GPL");
