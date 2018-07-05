// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STMicroelectronics 2018 - All Rights Reserved
 * Author: Philippe Peurichard <philippe.peurichard@st.com>,
 * Pascal Paillet <p.paillet@st.com> for STMicroelectronics.
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mfd/stpmu1.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

/**
 * stpmu1 regulator description
 * @desc: regulator framework description
 * @valid_modes_mask: modes supported by the regulator
 * @valid_ops_mask: ops supported by the regulator
 * @pull_down_reg: pull down register address
 * @mask_reset_reg: mask reset register address
 * @mask_reset_mask: mask rank and mask reset register mask
 * @icc_reg: icc register address
 * @icc_mask: icc register mask
 */
struct stpmu1_regulator_cfg {
	struct regulator_desc desc;

	unsigned int valid_modes_mask;
	unsigned int valid_ops_mask;
	u8 mask_reset_reg;
	u8 mask_reset_mask;
	u8 icc_reg;
	u8 icc_mask;
};

/**
 * stpmu1 regulator data: this structure is used as driver data
 * @regul_id: regulator id
 * @reg_node: DT node of regulator (unused on non-DT platforms)
 * @cfg: stpmu specific regulator description
 * @voltage_ref_reg: Special case for Vref DDR & LDO3 for which voltage
 * depends on Buck2
 * @mask_reset: mask_reset bit value
 * @irq_curlim: current limit interrupt number
 * @regmap: point to parent regmap structure
 */
struct stpmu1_regulator {
	unsigned int regul_id;
	struct device_node *reg_node;
	struct stpmu1_regulator_cfg *cfg;
	struct regulator_dev *voltage_ref_reg;
	u8 mask_reset;
	int irq_curlim;
	struct regmap *regmap;
};

/**
 * struct stpmu1_device_data - contains all regulators data
 * @regulator_table : contains all the regulators
 * @num_regulators: number of regulators used
 */
struct stpmu1_device_data {
	struct stpmu1_regulator *regulator_table;
	int num_regulators;
};

static int stpmu1_set_mode(struct regulator_dev *rdev, unsigned int mode);
static unsigned int stpmu1_get_mode(struct regulator_dev *rdev);
static int stpmu1_set_icc(struct regulator_dev *rdev);
static int stpmu1_set_bypass(struct regulator_dev *rdev, bool enable);
static int stpmu1_regulator_parse_dt(void *driver_data);
static unsigned int stpmu1_regulator_get_max_volt(struct regulator_desc *desc);
static unsigned int stpmu1_map_mode(unsigned int mode);
static int stpmu1_ldo3_list_voltage(struct regulator_dev *rdev,
				    unsigned int sel);
static int stpmu1_ldo3_get_voltage(struct regulator_dev *rdev);
static int stpmu1_fixed_regul_get_voltage(struct regulator_dev *rdev);

enum {
	STPMU1_BUCK1 = 0,
	STPMU1_BUCK2 = 1,
	STPMU1_BUCK3 = 2,
	STPMU1_BUCK4 = 3,
	STPMU1_LDO1 = 4,
	STPMU1_LDO2 = 5,
	STPMU1_LDO3 = 6,
	STPMU1_LDO4 = 7,
	STPMU1_LDO5 = 8,
	STPMU1_LDO6 = 9,
	STPMU1_VREF_DDR = 10,
	STPMU1_BOOST = 11,
	STPMU1_VBUS_OTG = 12,
	STPMU1_SW_OUT = 13,
};

/*
 * PMIC Ramp_delay: Time to settle down after voltage change (unit: uV/us)
 * or enable
 * is 3.6mV/uS +/-60%  -> 2.25mV/uS worst case
 */
#define PMIC_RAMP_SLOPE_UV_PER_US 2250
/* Enable time is 5000V/2250mV/uS */
#define PMIC_ENABLE_TIME_US 2200

struct regulator_linear_range buck1_ranges[] = {
	REGULATOR_LINEAR_RANGE(600000, 0, 30, 25000),
	REGULATOR_LINEAR_RANGE(1350000, 31, 63, 0),
};

struct regulator_linear_range buck2_ranges[] = {
	REGULATOR_LINEAR_RANGE(1000000, 0, 17, 0),
	REGULATOR_LINEAR_RANGE(1050000, 18, 19, 0),
	REGULATOR_LINEAR_RANGE(1100000, 20, 21, 0),
	REGULATOR_LINEAR_RANGE(1150000, 22, 23, 0),
	REGULATOR_LINEAR_RANGE(1200000, 24, 25, 0),
	REGULATOR_LINEAR_RANGE(1250000, 26, 27, 0),
	REGULATOR_LINEAR_RANGE(1300000, 28, 29, 0),
	REGULATOR_LINEAR_RANGE(1350000, 30, 31, 0),
	REGULATOR_LINEAR_RANGE(1400000, 32, 33, 0),
	REGULATOR_LINEAR_RANGE(1450000, 34, 35, 0),
	REGULATOR_LINEAR_RANGE(1500000, 36, 63, 0),
};

struct regulator_linear_range buck3_ranges[] = {
	REGULATOR_LINEAR_RANGE(1000000, 0, 19, 0),
	REGULATOR_LINEAR_RANGE(1100000, 20, 23, 0),
	REGULATOR_LINEAR_RANGE(1200000, 24, 27, 0),
	REGULATOR_LINEAR_RANGE(1300000, 28, 31, 0),
	REGULATOR_LINEAR_RANGE(1400000, 32, 35, 0),
	REGULATOR_LINEAR_RANGE(1500000, 36, 55, 100000),
	REGULATOR_LINEAR_RANGE(3400000, 56, 63, 0),

};

struct regulator_linear_range buck4_ranges[] = {
	REGULATOR_LINEAR_RANGE(600000, 0, 27, 25000),
	REGULATOR_LINEAR_RANGE(1300000, 28, 29, 0),
	REGULATOR_LINEAR_RANGE(1350000, 30, 31, 0),
	REGULATOR_LINEAR_RANGE(1400000, 32, 33, 0),
	REGULATOR_LINEAR_RANGE(1450000, 34, 35, 0),
	REGULATOR_LINEAR_RANGE(1500000, 36, 60, 100000),
	REGULATOR_LINEAR_RANGE(3900000, 61, 63, 0),

};

struct regulator_linear_range ldo1_ranges[] = {
	REGULATOR_LINEAR_RANGE(1700000, 0, 7, 0),
	REGULATOR_LINEAR_RANGE(1700000, 8, 24, 100000),
	REGULATOR_LINEAR_RANGE(3300000, 25, 31, 0),

};

struct regulator_linear_range ldo2_ranges[] = {
	REGULATOR_LINEAR_RANGE(1700000, 0, 7, 0),
	REGULATOR_LINEAR_RANGE(1700000, 8, 24, 100000),
	REGULATOR_LINEAR_RANGE(3300000, 25, 30, 0),

};

struct regulator_linear_range ldo3_ranges[] = {
	/* Special case to allow range to cover lowest value of Buck2/2 */
	REGULATOR_LINEAR_RANGE(500000, 0, 0, 0),
	REGULATOR_LINEAR_RANGE(1700000, 1, 7, 0),
	REGULATOR_LINEAR_RANGE(1700000, 8, 24, 100000),
	/* index 31 is special case when LDO3 is in mode DDR */
	REGULATOR_LINEAR_RANGE(3300000, 25, 31, 0),
};

struct regulator_linear_range ldo5_ranges[] = {
	REGULATOR_LINEAR_RANGE(1700000, 0, 7, 0),
	REGULATOR_LINEAR_RANGE(1700000, 8, 30, 100000),
	REGULATOR_LINEAR_RANGE(3900000, 31, 31, 0),
};

struct regulator_linear_range ldo6_ranges[] = {
	REGULATOR_LINEAR_RANGE(900000, 0, 24, 100000),
	REGULATOR_LINEAR_RANGE(3300000, 25, 31, 0),
};

static struct regulator_ops stpmu1_ldo_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.is_enabled = regulator_is_enabled_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.set_pull_down = regulator_set_pull_down_regmap,
	.set_over_current_protection = stpmu1_set_icc,
};

static struct regulator_ops stpmu1_ldo3_ops = {
	.list_voltage = stpmu1_ldo3_list_voltage,
	.map_voltage = regulator_map_voltage_iterate,
	.is_enabled = regulator_is_enabled_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.get_voltage = stpmu1_ldo3_get_voltage,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.set_pull_down = regulator_set_pull_down_regmap,
	.get_bypass = regulator_get_bypass_regmap,
	.set_bypass = stpmu1_set_bypass,
	.set_over_current_protection = stpmu1_set_icc,
};

static struct regulator_ops stpmu1_ldo4_fixed_regul_ops = {
	.is_enabled = regulator_is_enabled_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.set_pull_down = regulator_set_pull_down_regmap,
	.set_over_current_protection = stpmu1_set_icc,
};

static struct regulator_ops stpmu1_buck_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.is_enabled = regulator_is_enabled_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.set_pull_down = regulator_set_pull_down_regmap,
	.set_mode = stpmu1_set_mode,
	.get_mode = stpmu1_get_mode,
	.set_over_current_protection = stpmu1_set_icc,
};

static struct regulator_ops stpmu1_fixed_regul_ops = {
	.is_enabled = regulator_is_enabled_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.get_voltage = stpmu1_fixed_regul_get_voltage,
	.set_pull_down = regulator_set_pull_down_regmap,
};

static struct regulator_ops stpmu1_switch_regul_ops = {
	.is_enabled = regulator_is_enabled_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.get_voltage = stpmu1_fixed_regul_get_voltage,
	.set_over_current_protection = stpmu1_set_icc,
};

#define REG_LDO(ids, base) { \
	.name = #ids, \
	.id = STPMU1_##ids, \
	.n_voltages = 32, \
	.ops = &stpmu1_ldo_ops, \
	.linear_ranges = base ## _ranges, \
	.n_linear_ranges = ARRAY_SIZE(base ## _ranges), \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.vsel_reg = ids##_ACTIVE_CR, \
	.vsel_mask = LDO_VOLTAGE_MASK, \
	.enable_reg = ids##_ACTIVE_CR, \
	.enable_mask = LDO_ENABLE_MASK, \
	.enable_val = 1, \
	.disable_val = 0, \
	.pull_down_reg = ids##_PULL_DOWN_REG, \
	.pull_down_mask = ids##_PULL_DOWN_MASK, \
	.supply_name = #base, \
}

#define REG_LDO3(ids) { \
	.name = #ids, \
	.id = STPMU1_##ids, \
	.n_voltages = 32, \
	.ops = &stpmu1_ldo3_ops, \
	.linear_ranges = ldo3_ranges, \
	.n_linear_ranges = ARRAY_SIZE(ldo3_ranges), \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.vsel_reg = LDO3_ACTIVE_CR, \
	.vsel_mask = LDO_VOLTAGE_MASK, \
	.enable_reg = LDO3_ACTIVE_CR, \
	.enable_mask = LDO_ENABLE_MASK, \
	.enable_val = 1, \
	.disable_val = 0, \
	.bypass_reg = LDO3_ACTIVE_CR, \
	.bypass_mask = LDO_BYPASS_MASK, \
	.bypass_val_on = LDO_BYPASS_MASK, \
	.bypass_val_off = 0, \
	.pull_down_reg = ids##_PULL_DOWN_REG, \
	.pull_down_mask = ids##_PULL_DOWN_MASK, \
	.supply_name = "ldo3", \
}

#define REG_LDO4(ids) { \
	.name = #ids, \
	.id = STPMU1_##ids, \
	.n_voltages = 1, \
	.ops = &stpmu1_ldo4_fixed_regul_ops, \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.min_uV = 3300000, \
	.fixed_uV = 3300000, \
	.enable_reg = LDO4_ACTIVE_CR, \
	.enable_mask = LDO_ENABLE_MASK, \
	.enable_val = 1, \
	.disable_val = 0, \
	.pull_down_reg = ids##_PULL_DOWN_REG, \
	.pull_down_mask = ids##_PULL_DOWN_MASK, \
	.supply_name = "ldo4", \
}

#define REG_BUCK(ids, base) { \
	.name = #ids, \
	.id = STPMU1_##ids, \
	.ops = &stpmu1_buck_ops, \
	.n_voltages = 64, \
	.linear_ranges = base ## _ranges, \
	.n_linear_ranges = ARRAY_SIZE(base ## _ranges), \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.vsel_reg = ids##_ACTIVE_CR, \
	.vsel_mask = BUCK_VOLTAGE_MASK, \
	.enable_reg = ids##_ACTIVE_CR, \
	.enable_mask = BUCK_ENABLE_MASK, \
	.enable_val = 1, \
	.disable_val = 0, \
	.of_map_mode = stpmu1_map_mode, \
	.pull_down_reg = ids##_PULL_DOWN_REG, \
	.pull_down_mask = ids##_PULL_DOWN_MASK, \
	.supply_name = #base, \
}

#define REG_VREF_DDR(ids, reg) { \
	.name = #ids, \
	.id = STPMU1_##ids, \
	.n_voltages = 1, \
	.ops = &stpmu1_fixed_regul_ops, \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.min_uV = 0, \
	.fixed_uV = 5000000, \
	.enable_reg = (reg), \
	.enable_mask = BUCK_ENABLE_MASK, \
	.enable_val = 1, \
	.disable_val = 0, \
	.of_map_mode = stpmu1_map_mode, \
	.pull_down_reg = ids##_PULL_DOWN_REG, \
	.pull_down_mask = ids##_PULL_DOWN_MASK, \
	.supply_name = NULL, \
}

#define REG_SWITCH(ids, base, reg, mask, val) { \
	.name = #ids, \
	.id = STPMU1_##ids, \
	.n_voltages = 1, \
	.ops = &stpmu1_switch_regul_ops, \
	.type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, \
	.min_uV = 0, \
	.fixed_uV = 5000000, \
	.enable_reg = (reg), \
	.enable_mask = (mask), \
	.enable_val = (val), \
	.disable_val = 0, \
	.of_map_mode = stpmu1_map_mode, \
	.supply_name = #base, \
}

struct stpmu1_regulator_cfg stpmu1_regulator_cfgs[] = {
	[STPMU1_BUCK1] = {
		.desc = REG_BUCK(BUCK1, buck1),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE |
				  REGULATOR_CHANGE_MODE,
		.valid_modes_mask = REGULATOR_MODE_NORMAL |
				    REGULATOR_MODE_STANDBY,
		.icc_reg = BUCKS_ICCTO_CR,
		.icc_mask = BIT(0),
		.mask_reset_reg = BUCKS_MASK_RESET_CR,
		.mask_reset_mask = BIT(0),
	},
	[STPMU1_BUCK2] = {
		.desc = REG_BUCK(BUCK2, buck2),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE |
				  REGULATOR_CHANGE_MODE,
		.valid_modes_mask = REGULATOR_MODE_NORMAL |
				    REGULATOR_MODE_STANDBY,
		.icc_reg = BUCKS_ICCTO_CR,
		.icc_mask = BIT(1),
		.mask_reset_reg = BUCKS_MASK_RESET_CR,
		.mask_reset_mask = BIT(1),
	},
	[STPMU1_BUCK3] = {
		.desc = REG_BUCK(BUCK3, buck3),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE |
				  REGULATOR_CHANGE_MODE,
		.valid_modes_mask = REGULATOR_MODE_NORMAL |
				    REGULATOR_MODE_STANDBY,
		.icc_reg = BUCKS_ICCTO_CR,
		.icc_mask = BIT(2),
		.mask_reset_reg = BUCKS_MASK_RESET_CR,
		.mask_reset_mask = BIT(2),
	},
	[STPMU1_BUCK4] = {
		.desc = REG_BUCK(BUCK4, buck4),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE |
				  REGULATOR_CHANGE_MODE,
		.valid_modes_mask = REGULATOR_MODE_NORMAL |
				    REGULATOR_MODE_STANDBY,
		.icc_reg = BUCKS_ICCTO_CR,
		.icc_mask = BIT(3),
		.mask_reset_reg = BUCKS_MASK_RESET_CR,
		.mask_reset_mask = BIT(3),
	},
	[STPMU1_LDO1] = {
		.desc = REG_LDO(LDO1, ldo1),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.icc_reg = LDOS_ICCTO_CR,
		.icc_mask = BIT(0),
		.mask_reset_reg = LDOS_MASK_RESET_CR,
		.mask_reset_mask = BIT(0),
	},
	[STPMU1_LDO2] = {
		.desc = REG_LDO(LDO2, ldo2),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.icc_reg = LDOS_ICCTO_CR,
		.icc_mask = BIT(1),
		.mask_reset_reg = LDOS_MASK_RESET_CR,
		.mask_reset_mask = BIT(1),
	},
	[STPMU1_LDO3] = {
		.desc = REG_LDO3(LDO3),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE |
				  REGULATOR_CHANGE_BYPASS,
		.icc_reg = LDOS_ICCTO_CR,
		.icc_mask = BIT(2),
		.mask_reset_reg = LDOS_MASK_RESET_CR,
		.mask_reset_mask = BIT(2),
	},
	[STPMU1_LDO4] = {
		.desc = REG_LDO4(LDO4),
		.icc_reg = LDOS_ICCTO_CR,
		.icc_mask = BIT(3),
		.mask_reset_reg = LDOS_MASK_RESET_CR,
		.mask_reset_mask = BIT(3),
	},
	[STPMU1_LDO5] = {
		.desc = REG_LDO(LDO5, ldo5),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.icc_reg = LDOS_ICCTO_CR,
		.icc_mask = BIT(4),
		.mask_reset_reg = LDOS_MASK_RESET_CR,
		.mask_reset_mask = BIT(4),
	},
	[STPMU1_LDO6] = {
		.desc = REG_LDO(LDO6, ldo6),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.icc_reg = LDOS_ICCTO_CR,
		.icc_mask = BIT(5),
		.mask_reset_reg = LDOS_MASK_RESET_CR,
		.mask_reset_mask = BIT(5),
	},
	[STPMU1_VREF_DDR] = {
		.desc = REG_VREF_DDR(VREF_DDR, VREF_DDR_ACTIVE_CR),
		.mask_reset_reg = LDOS_MASK_RESET_CR,
		.mask_reset_mask = BIT(6),
	},
	[STPMU1_BOOST] = {
		.desc = REG_SWITCH(BOOST, boost, BST_SW_CR,
				   BOOST_ENABLED,
				   BOOST_ENABLED),
		.icc_reg = BUCKS_ICCTO_CR,
		.icc_mask = BIT(6),
	},
	[STPMU1_VBUS_OTG] = {
		.desc = REG_SWITCH(VBUS_OTG, pwr_sw1, BST_SW_CR,
				   USBSW_OTG_SWITCH_ENABLED,
				   USBSW_OTG_SWITCH_ENABLED),
		.icc_reg = BUCKS_ICCTO_CR,
		.icc_mask = BIT(4),
	},
	[STPMU1_SW_OUT] = {
		.desc = REG_SWITCH(SW_OUT, pwr_sw2, BST_SW_CR,
				   SWIN_SWOUT_ENABLED,
				   SWIN_SWOUT_ENABLED),
		.icc_reg = BUCKS_ICCTO_CR,
		.icc_mask = BIT(5),
	},
};

#define GET_MINIMUM_VOLTAGE(_regulator_desc_ptr) \
	((_regulator_desc_ptr)->linear_ranges[0].min_uV)

static unsigned int stpmu1_map_mode(unsigned int mode)
{
	return mode == REGULATOR_MODE_STANDBY ?
		REGULATOR_MODE_STANDBY : REGULATOR_MODE_NORMAL;
}

static unsigned int stpmu1_regulator_get_max_volt(struct regulator_desc *desc)
{
	const struct regulator_linear_range *last_range =
		&desc->linear_ranges[desc->n_linear_ranges - 1];

	unsigned int max_voltage =
		last_range->min_uV +
		(last_range->max_sel - last_range->min_sel + 1)
		* last_range->uV_step;

	return max_voltage;
}

static int stpmu1_get_voltage_regmap(struct regulator_dev *rdev)
{
	int selector = 0;

	if (!rdev)
		return -EINVAL;
	selector = regulator_get_voltage_sel_regmap(rdev);
	return regulator_list_voltage_linear_range(rdev, selector);
}

static int stpmu1_ldo3_list_voltage(struct regulator_dev *rdev,
				    unsigned int sel)
{
	struct stpmu1_regulator *regul = rdev_get_drvdata(rdev);

	if (sel == 0)
		return regulator_list_voltage_linear_range(rdev, 1);

	if (sel < 31)
		return regulator_list_voltage_linear_range(rdev, sel);

	if (sel == 31)
		return stpmu1_get_voltage_regmap(regul->voltage_ref_reg) / 2;

	return -EINVAL;
}

static int stpmu1_ldo3_get_voltage(struct regulator_dev *rdev)
{
	int sel = regulator_get_voltage_sel_regmap(rdev);

	if (sel < 0)
		return -EINVAL;

	return stpmu1_ldo3_list_voltage(rdev, (unsigned int)sel);
}

static int stpmu1_fixed_regul_get_voltage(struct regulator_dev *rdev)
{
	struct stpmu1_regulator *regul = rdev_get_drvdata(rdev);
	int id = rdev_get_id(rdev);

	/* VREF_DDR voltage is equal to Buck2/2 */
	if (id == STPMU1_VREF_DDR)
		return stpmu1_get_voltage_regmap(regul->voltage_ref_reg) / 2;

	/* For all other case , rely on fixed value defined by Hw settings */
	return regul->cfg->desc.fixed_uV;
}

static int stpmu1_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct stpmu1_regulator *regul = rdev_get_drvdata(rdev);
	unsigned int hplp;

	/* The low power mode will be set for NORMAL/RUN registers */
	switch (mode) {
	case REGULATOR_MODE_NORMAL:
		hplp = 0;
		break;
	case REGULATOR_MODE_STANDBY:
		hplp = 1;
		break;
	default:
		return -EINVAL;
	}

	return  regmap_update_bits(regul->regmap, regul->cfg->desc.enable_reg,
				   BUCK_HPLP_ENABLE_MASK,
				   hplp << BUCK_HPLP_SHIFT);
}

static unsigned int stpmu1_get_mode(struct regulator_dev *rdev)
{
	struct stpmu1_regulator *regul = rdev_get_drvdata(rdev);
	int ret;
	unsigned int val;

	ret = regmap_read(regul->regmap, regul->cfg->desc.enable_reg, &val);
	if (ret < 0)
		return ret;

	val &= BUCK_HPLP_ENABLE_MASK;
	val >>= BUCK_HPLP_SHIFT;

	return val ? REGULATOR_MODE_STANDBY : REGULATOR_MODE_NORMAL;
}

static int stpmu1_set_icc(struct regulator_dev *rdev)
{
	struct stpmu1_regulator *regul = rdev_get_drvdata(rdev);

	/* enable switch off in case of over current */
	return regmap_update_bits(regul->regmap, regul->cfg->icc_reg,
				  regul->cfg->icc_mask, regul->cfg->icc_mask);
}

static int stpmu1_set_bypass(struct regulator_dev *rdev, bool enable)
{
	struct stpmu1_regulator *regul = rdev_get_drvdata(rdev);
	int ret;
	unsigned int val;

	if (enable)
		val = regul->cfg->desc.bypass_val_on;
	else
		val = regul->cfg->desc.bypass_val_off;

	ret = regmap_update_bits(regul->regmap, regul->cfg->desc.bypass_reg,
				 regul->cfg->desc.bypass_mask, val);

	return ret;
}

static irqreturn_t stpmu1_curlim_irq_handler(int irq, void *data)
{
	struct regulator_dev *rdev = (struct regulator_dev *)data;

	mutex_lock(&rdev->mutex);

	/* Send an overcurrent notification */
	regulator_notifier_call_chain(rdev,
				      REGULATOR_EVENT_OVER_CURRENT,
				      NULL);

	mutex_unlock(&rdev->mutex);

	return IRQ_HANDLED;
}

static int stpmu1_regulator_init(struct platform_device *pdev,
				 struct regulator_dev *rdev)
{
	struct stpmu1_regulator *regul = rdev_get_drvdata(rdev);
	int ret = 0;

	/* set mask reset */
	if (regul->mask_reset && regul->cfg->mask_reset_reg != 0) {
		ret = regmap_update_bits(regul->regmap,
					 regul->cfg->mask_reset_reg,
					 regul->cfg->mask_reset_mask,
					 regul->cfg->mask_reset_mask);
		if (ret) {
			dev_err(&pdev->dev, "set mask reset failed\n");
			return ret;
		}
	}

	/* setup an irq handler for over-current detection */
	if (regul->irq_curlim >= 0) {
		ret = devm_request_threaded_irq(&pdev->dev,
						regul->irq_curlim, NULL,
						stpmu1_curlim_irq_handler,
						IRQF_ONESHOT | IRQF_SHARED,
						pdev->name, rdev);
		if (ret) {
			dev_err(&pdev->dev, "Request IRQ failed\n");
			return ret;
		}
	}
	return 0;
}

#define MATCH(_name, _id) \
	[STPMU1_##_id] = { \
		.name = #_name, \
		.desc = &stpmu1_regulator_cfgs[STPMU1_##_id].desc, \
	}

static struct of_regulator_match stpmu1_regulators_matches[] = {
	MATCH(buck1, BUCK1),
	MATCH(buck2, BUCK2),
	MATCH(buck3, BUCK3),
	MATCH(buck4, BUCK4),
	MATCH(ldo1, LDO1),
	MATCH(ldo2, LDO2),
	MATCH(ldo3, LDO3),
	MATCH(ldo4, LDO4),
	MATCH(ldo5, LDO5),
	MATCH(ldo6, LDO6),
	MATCH(vref_ddr, VREF_DDR),
	MATCH(boost, BOOST),
	MATCH(pwr_sw1, VBUS_OTG),
	MATCH(pwr_sw2, SW_OUT),
};

static inline struct device_node *match_of_node(struct platform_device *pdev,
						int index)
{
	if (!stpmu1_regulators_matches[index].of_node)
		dev_info(&pdev->dev,
			 "DT node not found for regulator %i\n\r", index);

	return stpmu1_regulators_matches[index].of_node;
}

static int stpmu1_regulator_parse_dt(void *driver_data)
{
	struct stpmu1_regulator *regul =
		(struct stpmu1_regulator *)driver_data;

	if (!regul)
		return -EINVAL;

	if (of_get_property(regul->reg_node, "st,mask_reset", NULL))
		regul->mask_reset = 1;

	regul->irq_curlim = of_irq_get(regul->reg_node, 0);

	return 0;
}

static void update_regulator_constraints(int index,
					 struct regulator_init_data *init_data)
{
	struct stpmu1_regulator_cfg *cfg = &stpmu1_regulator_cfgs[index];
	struct regulator_desc *desc = &cfg->desc;

	init_data->constraints.valid_ops_mask |=
		cfg->valid_ops_mask;
	init_data->constraints.valid_modes_mask |=
		cfg->valid_modes_mask;

	/*
	 * if all constraints are not specified in DT , ensure Hw
	 * constraints are met
	 */
	if (desc->n_voltages > 1) {
		if (!init_data->constraints.min_uV)
			init_data->constraints.min_uV =
				GET_MINIMUM_VOLTAGE(desc);
		if (!init_data->constraints.max_uV)
			init_data->constraints.max_uV =
			stpmu1_regulator_get_max_volt(desc);
	}

	if (!init_data->constraints.ramp_delay)
		init_data->constraints.ramp_delay = PMIC_RAMP_SLOPE_UV_PER_US;

	if (!init_data->constraints.enable_time)
		init_data->constraints.enable_time = PMIC_ENABLE_TIME_US;
}

static struct
regulator_dev *stpmu1_regulator_register(struct platform_device *pdev, int id,
					 struct regulator_init_data *init_data,
					 struct stpmu1_regulator *regul,
					 struct regulator_dev *buck2)
{
	struct stpmu1_dev *pmic_dev = dev_get_drvdata(pdev->dev.parent);
	struct regulator_dev *rdev;
	struct regulator_config config = {};

	config.dev = &pdev->dev;
	config.init_data = init_data;
	config.of_node = match_of_node(pdev, id);
	config.regmap = pmic_dev->regmap;
	config.driver_data = regul;

	regul->regul_id = id;
	regul->reg_node = config.of_node;
	regul->cfg = &stpmu1_regulator_cfgs[id];
	regul->regmap = pmic_dev->regmap;

	/* LDO3 and VREF_DDR can use buck2 as reference voltage */
	if (regul->regul_id == STPMU1_LDO3 ||
	    regul->regul_id == STPMU1_VREF_DDR) {
		if (!buck2) {
			dev_err(&pdev->dev,
				"Error in PMIC regulator settings: Buck2 is not defined prior to LDO3 or VREF_DDR regulators\n"
				);
			return ERR_PTR(-EINVAL);
		}
		regul->voltage_ref_reg = buck2;
	}

	rdev = devm_regulator_register(&pdev->dev, &regul->cfg->desc, &config);
	if (IS_ERR(rdev)) {
		dev_err(&pdev->dev, "failed to register %s regulator\n",
			regul->cfg->desc.name);
	}

	return rdev;
}

static int stpmu1_regulator_probe(struct platform_device *pdev)
{
	struct stpmu1_dev *pmic_dev = dev_get_drvdata(pdev->dev.parent);
	struct stpmu1_device_data *ddata;
	struct regulator_dev *rdev;
	struct stpmu1_regulator *regul;
	struct regulator_init_data *init_data;
	struct device_node *np;
	int i, ret;
	struct regulator_dev *buck2_rdev = NULL;

	ddata = devm_kzalloc(&pdev->dev, sizeof(struct stpmu1_device_data),
			     GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);

	/* disable icc for all reguls */
	ret = regmap_update_bits(pmic_dev->regmap, BUCKS_ICCTO_CR,
				 BUCKS_ICCTO_CR_REG_MASK, 0);
	if (ret == 0)
		ret = regmap_update_bits(pmic_dev->regmap, LDOS_ICCTO_CR,
					 LDOS_ICCTO_CR_REG_MASK, 0);
	/* reset pulldown value */
	if (ret == 0)
		ret = regmap_update_bits(pmic_dev->regmap, BUCKS_PD_CR,
					 BUCKS_PD_CR_REG_MASK, 0);
	if (ret == 0)
		ret = regmap_update_bits(pmic_dev->regmap, LDO14_PD_CR,
					 LDO1234_PULL_DOWN_REGISTER_MASK, 0);
	if (ret == 0)
		ret = regmap_update_bits(pmic_dev->regmap, LDO56_VREF_PD_CR,
					 LDO56_VREF_PD_CR_REG_MASK, 0);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to update stpmu1 register %d\n", ret);
		return ret;
	}

	np = pdev->dev.of_node;
	if (!np) {
		dev_err(&pdev->dev, "regulators node not found\n");
		return -EINVAL;
	}

	ret = of_regulator_match(&pdev->dev, np,
				 stpmu1_regulators_matches,
				 ARRAY_SIZE(stpmu1_regulators_matches));
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Error in PMIC regulator device tree node");
		return ret;
	}
	ddata->num_regulators = ret;

	dev_dbg(&pdev->dev, "%d regulator(s) found in DT\n",
		ddata->num_regulators);

	regul = devm_kzalloc(&pdev->dev, ddata->num_regulators *
			     sizeof(struct stpmu1_regulator),
			     GFP_KERNEL);
	if (!regul)
		return -ENOMEM;

	ddata->regulator_table = regul;

	/* Register all defined (from DT) regulators to Regulator Framework */
	for (i = 0; i < ARRAY_SIZE(stpmu1_regulator_cfgs); i++) {
		/* Parse DT & find regulators to register */
		init_data = stpmu1_regulators_matches[i].init_data;
		if (init_data) {
			init_data->regulator_init = &stpmu1_regulator_parse_dt;

			update_regulator_constraints(i, init_data);

			rdev = stpmu1_regulator_register(pdev, i, init_data,
							 regul, buck2_rdev);
			if (IS_ERR(rdev))
				return PTR_ERR(rdev);

			ret = stpmu1_regulator_init(pdev, rdev);
			if (ret) {
				dev_err(&pdev->dev,
					"failed to initialize regulator %d\n",
					ret);
				return ret;
			}

			if (regul->regul_id == STPMU1_BUCK2)
				buck2_rdev = rdev;

			regul++;
		}
	}

	dev_dbg(&pdev->dev, "stpmu1_regulator driver probed\n");

	return 0;
}

static const struct of_device_id of_pmic_regulator_match[] = {
	{ .compatible = "st,stpmu1-regulators" },
	{ },
};

MODULE_DEVICE_TABLE(of, of_pmic_regulator_match);

static struct platform_driver stpmu1_regulator_driver = {
	.driver = {
		.name = "stpmu1-regulator",
		.of_match_table = of_match_ptr(of_pmic_regulator_match),
	},
	.probe = stpmu1_regulator_probe,
};

module_platform_driver(stpmu1_regulator_driver);

MODULE_DESCRIPTION("STPMU1 PMIC voltage regulator driver");
MODULE_AUTHOR("<philippe.peurichard@st.com>");
MODULE_LICENSE("GPL");
