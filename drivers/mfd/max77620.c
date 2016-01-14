/*
 * Maxim MAX77620 MFD Driver
 *
 * Copyright (C) 2016 NVIDIA CORPORATION. All rights reserved.
 *
 * Author:
 *		Laxman Dewangan <ldewangan@nvidia.com>
 *		Chaitanya Bandi <bandik@nvidia.com>
 *		Mallikarjun Kasoju <mkasoju@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/mfd/core.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/mfd/max77620.h>

static struct resource gpio_resources[] = {
	{
		.start	= MAX77620_IRQ_TOP_GPIO,
		.end	= MAX77620_IRQ_TOP_GPIO,
		.flags  = IORESOURCE_IRQ,
	}
};

static struct resource thermal_resources[] = {
	{
		.start	= MAX77620_IRQ_LBT_TJALRM1,
		.end	= MAX77620_IRQ_LBT_TJALRM1,
		.flags  = IORESOURCE_IRQ,
	},
	{
		.start	= MAX77620_IRQ_LBT_TJALRM2,
		.end	= MAX77620_IRQ_LBT_TJALRM2,
		.flags  = IORESOURCE_IRQ,
	}
};

static const struct regmap_irq max77620_top_irqs[] = {
	[MAX77620_IRQ_TOP_GLBL] = {
		.mask = MAX77620_IRQ_TOP_GLBL_MASK,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_TOP_SD] = {
		.mask = MAX77620_IRQ_TOP_SD_MASK,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_TOP_LDO] = {
		.mask = MAX77620_IRQ_TOP_LDO_MASK,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_TOP_GPIO] = {
		.mask = MAX77620_IRQ_TOP_GPIO_MASK,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_TOP_RTC] = {
		.mask = MAX77620_IRQ_TOP_RTC_MASK,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_TOP_32K] = {
		.mask = MAX77620_IRQ_TOP_32K_MASK,
		.reg_offset = 0,
	},
	[MAX77620_IRQ_TOP_ONOFF] = {
		.mask = MAX77620_IRQ_TOP_ONOFF_MASK,
		.reg_offset = 0,
	},

	[MAX77620_IRQ_LBT_MBATLOW] = {
		.mask = MAX77620_IRQ_LBM_MASK,
		.reg_offset = 1,
	},
	[MAX77620_IRQ_LBT_TJALRM1] = {
		.mask = MAX77620_IRQ_TJALRM1_MASK,
		.reg_offset = 1,
	},
	[MAX77620_IRQ_LBT_TJALRM2] = {
		.mask = MAX77620_IRQ_TJALRM2_MASK,
		.reg_offset = 1,
	},

};

static const char * const max77620_nverc[] = {
	"Shutdown-pin",
	"System WatchDog Timer",
	"Hard Reset",
	"Junction Temp Overload",
	"Main-Battery Low",
	"Main-Battery overvoltage Lockout",
	"Main-Battery undervoltage Lockout",
	"Reset input",
};

enum max77660_ids {
	MAX77620_PMIC_ID,
	MAX77620_GPIO_ID,
	MAX77620_PINCTRL_ID,
	MAX77620_CLK_ID,
	MAX77620_POWER_OFF_ID,
	MAX77620_WDT_ID,
	MAX77620_THERMAL_ID,
	MAX77620_RTC_ID,
};

#define MAX77620_SUB_MODULE_RES(_name, _id)			\
	[MAX77620_##_id##_ID] = {				\
		.name = "max77620-"#_name,			\
		.num_resources	= ARRAY_SIZE(_name##_resources), \
		.resources	= &_name##_resources[0],	\
		.id = MAX77620_##_id##_ID,			\
	}

#define MAX20024_SUB_MODULE_RES(_name, _id)			\
	[MAX77620_##_id##_ID] = {				\
		.name = "max20024-"#_name,			\
		.num_resources	= ARRAY_SIZE(_name##_resources), \
		.resources	= &_name##_resources[0],	\
		.id = MAX77620_##_id##_ID,			\
	}

#define MAX77620_SUB_MODULE_NO_RES(_name, _id)			\
	[MAX77620_##_id##_ID] = {				\
		.name = "max77620-"#_name,			\
		.id = MAX77620_##_id##_ID,			\
	}

#define MAX20024_SUB_MODULE_NO_RES(_name, _id)			\
	[MAX77620_##_id##_ID] = {				\
		.name = "max20024-"#_name,			\
		.id = MAX77620_##_id##_ID,			\
	}

static struct mfd_cell max77620_children[] = {
	MAX77620_SUB_MODULE_RES(gpio, GPIO),
	MAX77620_SUB_MODULE_NO_RES(pmic, PMIC),
	MAX77620_SUB_MODULE_NO_RES(pinctrl, PINCTRL),
	MAX77620_SUB_MODULE_NO_RES(clk, CLK),
	MAX77620_SUB_MODULE_NO_RES(power-off, POWER_OFF),
	MAX77620_SUB_MODULE_NO_RES(wdt, WDT),
	MAX77620_SUB_MODULE_RES(thermal, THERMAL),
};

static struct mfd_cell max20024_children[] = {
	MAX20024_SUB_MODULE_RES(gpio, GPIO),
	MAX20024_SUB_MODULE_NO_RES(pmic, PMIC),
	MAX20024_SUB_MODULE_NO_RES(pinctrl, PINCTRL),
	MAX20024_SUB_MODULE_NO_RES(clk, CLK),
	MAX20024_SUB_MODULE_NO_RES(power-off, POWER_OFF),
	MAX20024_SUB_MODULE_NO_RES(wdt, WDT),
	MAX20024_SUB_MODULE_RES(thermal, THERMAL),
};

struct max77620_sub_modules {
	struct mfd_cell *cells;
	int ncells;
	u32 id;
};

static const struct max77620_sub_modules max77620_cells = {
	.cells = max77620_children,
	.ncells = ARRAY_SIZE(max77620_children),
	.id = MAX77620,
};

static const struct max77620_sub_modules  max20024_cells = {
	.cells = max20024_children,
	.ncells = ARRAY_SIZE(max20024_children),
	.id = MAX20024,
};

static struct regmap_irq_chip max77620_top_irq_chip = {
	.name = "max77620-top",
	.irqs = max77620_top_irqs,
	.num_irqs = ARRAY_SIZE(max77620_top_irqs),
	.num_regs = 2,
	.status_base = MAX77620_REG_IRQTOP,
	.mask_base = MAX77620_REG_IRQTOPM,
};

static const struct regmap_range max77620_readable_ranges[] = {
	regmap_reg_range(MAX77620_REG_CNFGGLBL1, MAX77620_REG_DVSSD4),
};

static const struct regmap_access_table max77620_readable_table = {
	.yes_ranges = max77620_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(max77620_readable_ranges),
};

static const struct regmap_range max20024_readable_ranges[] = {
	regmap_reg_range(MAX77620_REG_CNFGGLBL1, MAX77620_REG_DVSSD4),
	regmap_reg_range(MAX20024_REG_MAX_ADD, MAX20024_REG_MAX_ADD),
};

static const struct regmap_access_table max20024_readable_table = {
	.yes_ranges = max20024_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(max20024_readable_ranges),
};

static const struct regmap_range max77620_writable_ranges[] = {
	regmap_reg_range(MAX77620_REG_CNFGGLBL1, MAX77620_REG_DVSSD4),
};

static const struct regmap_access_table max77620_writable_table = {
	.yes_ranges = max77620_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(max77620_writable_ranges),
};

static const struct regmap_range max77620_cacheable_ranges[] = {
	regmap_reg_range(MAX77620_REG_SD0_CFG, MAX77620_REG_LDO_CFG3),
	regmap_reg_range(MAX77620_REG_FPS_CFG0, MAX77620_REG_FPS_SD3),
};

static const struct regmap_access_table max77620_volatile_table = {
	.no_ranges = max77620_cacheable_ranges,
	.n_no_ranges = ARRAY_SIZE(max77620_cacheable_ranges),
};

static const struct regmap_config max77620_regmap_config = {
	.name = "power-slave",
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77620_REG_DVSSD4 + 1,
	.cache_type = REGCACHE_RBTREE,
	.rd_table = &max77620_readable_table,
	.wr_table = &max77620_writable_table,
	.volatile_table = &max77620_volatile_table,
};

static const struct regmap_config max20024_regmap_config = {
	.name = "power-slave",
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX20024_REG_MAX_ADD + 1,
	.cache_type = REGCACHE_RBTREE,
	.rd_table = &max20024_readable_table,
	.wr_table = &max77620_writable_table,
	.volatile_table = &max77620_volatile_table,
};

int max77620_irq_get_virq(struct device *dev, int irq)
{
	struct max77620_chip *chip = dev_get_drvdata(dev);

	return regmap_irq_get_virq(chip->top_irq_data, irq);
}
EXPORT_SYMBOL_GPL(max77620_irq_get_virq);

int max77620_reg_write(struct device *dev, unsigned int reg, u8 val)
{
	struct max77620_chip *chip = dev_get_drvdata(dev);

	return regmap_write(chip->rmap, reg, val);
}
EXPORT_SYMBOL_GPL(max77620_reg_write);

int max77620_reg_read(struct device *dev, unsigned int reg, u8 *val)
{
	struct max77620_chip *chip = dev_get_drvdata(dev);
	unsigned int ival;
	int ret;

	ret = regmap_read(chip->rmap, reg, &ival);
	if (ret < 0)
		return ret;
	*val = ival;

	return 0;
}
EXPORT_SYMBOL_GPL(max77620_reg_read);

int max77620_reg_update(struct device *dev, unsigned int reg,
			unsigned int mask, unsigned int val)
{
	struct max77620_chip *chip = dev_get_drvdata(dev);

	return regmap_update_bits(chip->rmap, reg, mask, val);
}
EXPORT_SYMBOL_GPL(max77620_reg_update);

static int max77620_get_fps_period_reg_value(struct max77620_chip *chip,
					     int tperiod)
{
	int base_fps_time = (chip->id == MAX20024) ? 20 : 40;
	int x, i;

	for (i = 0; i < 0x7; ++i) {
		x = base_fps_time * BIT(i);
		if (x >= tperiod)
			return i;
	}

	return i;
}

static int max77620_config_fps(struct max77620_chip *chip,
			       struct device_node *fps_np)
{
	struct device *dev = chip->dev;
	unsigned int mask = 0, config = 0;
	int input_enable = 2;
	u32 pval;
	int tperiod, fps_id;
	int ret;

	ret = of_property_read_u32(fps_np, "reg", &pval);
	if (ret < 0) {
		dev_err(dev, "reg prop missing from node %s\n", fps_np->name);
		return 0;
	}
	if (pval > 2) {
		dev_err(dev, "FPS%u is not supported\n", pval);
		return 0;
	}
	fps_id = pval;

	ret = of_property_read_u32(fps_np,
				   "maxim,active-fps-time-period-us", &pval);
	if (!ret) {
		mask |= MAX77620_FPS_TIME_PERIOD_MASK;
		chip->active_fps_period[fps_id] = min(pval, 5120U);
		tperiod = max77620_get_fps_period_reg_value(
				chip, chip->active_fps_period[fps_id]);
		config |= tperiod << MAX77620_FPS_TIME_PERIOD_SHIFT;
	}

	ret = of_property_read_u32(fps_np,
				   "maxim,suspend-fps-time-period-us", &pval);
	if (!ret)
		chip->suspend_fps_period[fps_id] = min(pval, 5120U);

	ret = of_property_read_u32(fps_np, "maxim,fps-enable-input", &pval);
	if (!ret) {
		if (pval > 2) {
			dev_err(dev, "FPS %d enable-input invalid\n", fps_id);
		} else {
			input_enable = pval;
			mask |= MAX77620_FPS_EN_SRC_MASK;
		}
	}
	config |= (input_enable & 0x3) << MAX77620_FPS_EN_SRC_SHIFT;

	if (input_enable == 2) {
		bool enable_fps = of_property_read_bool(fps_np,
					"maxim,fps-sw-enable");
		mask |= MAX77620_FPS_ENFPS_MASK;
		if (enable_fps)
			config |= 1;
	}

	if (!chip->sleep_enable)
		chip->sleep_enable = of_property_read_bool(fps_np,
					"maxim,enable-sleep");
	if (!chip->enable_global_lpm)
		chip->enable_global_lpm = of_property_read_bool(fps_np,
					"maxim,enable-global-lpm");

	ret = max77620_reg_update(dev, MAX77620_REG_FPS_CFG0 + fps_id,
				  mask, config);
	if (ret < 0) {
		dev_err(dev, "Reg 0x%02x write failed: %d\n",
			MAX77620_REG_FPS_CFG0 + fps_id, ret);
		return ret;
	}

	return 0;
}

static int max77620_initialise_fps(struct max77620_chip *chip,
				   struct device *dev)
{
	struct device_node *fps_np, *fps_child;
	u8 config;
	int fps_id;
	int ret;

	for (fps_id = 0; fps_id < 3; ++fps_id) {
		chip->active_fps_period[fps_id] = -1;
		chip->suspend_fps_period[fps_id] = -1;
	}

	fps_np = of_get_child_by_name(dev->of_node, "fps");
	if (!fps_np)
		goto skip_fps;

	for_each_child_of_node(fps_np, fps_child) {
		ret = max77620_config_fps(chip, fps_child);
		if (ret < 0)
			return ret;
	}

	config = chip->enable_global_lpm ? MAX77620_ONOFFCNFG2_SLP_LPM_MSK : 0;
	ret = max77620_reg_update(chip->dev, MAX77620_REG_ONOFFCNFG2,
				  MAX77620_ONOFFCNFG2_SLP_LPM_MSK, config);
	if (ret < 0) {
		dev_err(dev, "Reg ONOFFCNFG2 update failed: %d\n", ret);
		return ret;
	}

skip_fps:
	/* Enable wake on EN0 pin */
	ret = max77620_reg_update(chip->dev, MAX77620_REG_ONOFFCNFG2,
				  MAX77620_ONOFFCNFG2_WK_EN0,
				  MAX77620_ONOFFCNFG2_WK_EN0);
	if (ret < 0) {
		dev_err(dev, "Reg ONOFFCNFG2 WK_EN0 update failed: %d\n", ret);
		return ret;
	}

	if (!chip->sleep_enable)
		chip->sleep_enable = of_property_read_bool(dev->of_node,
						"maxim,enable-sleep");

	/* For MAX20024, SLPEN will be POR reset if CLRSE is b11 */
	if ((chip->id == MAX20024) && chip->sleep_enable) {
		config = MAX77620_ONOFFCNFG1_SLPEN | MAX20024_ONOFFCNFG1_CLRSE;
		ret = max77620_reg_update(chip->dev, MAX77620_REG_ONOFFCNFG1,
					  config, config);
		if (ret < 0) {
			dev_err(dev, "Reg ONOFFCNFG1 update failed: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int max77620_init_backup_battery_charging(struct max77620_chip *chip,
						 struct device *dev)
{
	struct device_node *bb_node;
	u32 pval;
	u8 config;
	int ret;

	bb_node = of_get_child_by_name(dev->of_node, "backup-battery");
	if (!bb_node) {
		dev_dbg(dev, "Backup battery charging support disabled\n");
		max77620_reg_update(chip->dev, MAX77620_REG_CNFGBBC,
				    MAX77620_CNFGBBC_ENABLE, 0);
		return 0;
	}

	config = MAX77620_CNFGBBC_ENABLE;
	ret = of_property_read_u32(bb_node,
				   "maxim,bb-charging-current-microamp", &pval);
	if (ret < 0)
		pval = 50;
	if (pval <= 50)
		config |= 0 << MAX77620_CNFGBBC_CURRENT_SHIFT;
	else if (pval <= 100)
		config |= 3 << MAX77620_CNFGBBC_CURRENT_SHIFT;
	else if (pval <= 200)
		config |= 0 << MAX77620_CNFGBBC_CURRENT_SHIFT;
	else if (pval <= 400)
		config |= 3 << MAX77620_CNFGBBC_CURRENT_SHIFT;
	else if (pval <= 600)
		config |= 1 << MAX77620_CNFGBBC_CURRENT_SHIFT;
	else
		config |= 2 << MAX77620_CNFGBBC_CURRENT_SHIFT;

	if (pval > 100)
		config |= MAX77620_CNFGBBC_LOW_CURRENT_DISABLE;

	ret = of_property_read_u32(bb_node,
				   "maxim,bb-charging-voltage-microvolt",
				   &pval);
	if (ret < 0)
		pval = 2500000;
	pval /= 1000;
	if (pval <= 2500)
		config |= 0 << MAX77620_CNFGBBC_VOLTAGE_SHIFT;
	else if (pval <= 3000)
		config |= 1 << MAX77620_CNFGBBC_VOLTAGE_SHIFT;
	else if (pval <= 3300)
		config |= 2 << MAX77620_CNFGBBC_VOLTAGE_SHIFT;
	else
		config |= 3 << MAX77620_CNFGBBC_VOLTAGE_SHIFT;

	ret = of_property_read_u32(bb_node,
				   "maxim,bb-output-resister-ohm", &pval);
	if (ret < 0)
		pval = 1000;

	if (pval <= 100)
		config |= 0 << MAX77620_CNFGBBC_RESISTOR_SHIFT;
	else if (pval <= 1000)
		config |= 1 << MAX77620_CNFGBBC_RESISTOR_SHIFT;
	else if (pval <= 3000)
		config |= 2 << MAX77620_CNFGBBC_RESISTOR_SHIFT;
	else if (pval <= 6000)
		config |= 3 << MAX77620_CNFGBBC_RESISTOR_SHIFT;

	ret = max77620_reg_write(dev, MAX77620_REG_CNFGBBC, config);
	if (ret < 0) {
		dev_err(dev, "Reg 0x%02x write failed: %d\n",
			MAX77620_REG_CNFGBBC, ret);
		return ret;
	}

	return 0;
}

static int max77620_init_low_battery_monitor(struct max77620_chip *chip,
					     struct device *dev)
{
	struct device_node *np;
	u8 mask = 0, val = 0;
	u32 pval;
	int ret;

	np = of_get_child_by_name(dev->of_node, "low-battery-monitor");
	if (!np)
		return 0;

	ret = of_property_read_u32(np, "maxim,low-battery-dac", &pval);
	if (!ret) {
		mask |= MAX77620_CNFGGLBL1_LBDAC_EN;
		if (pval)
			val |= MAX77620_CNFGGLBL1_LBDAC_EN;
	}

	ret = of_property_read_u32(np, "maxim,low-battery-shutdown", &pval);
	if (!ret) {
		mask |= MAX77620_CNFGGLBL1_MPPLD;
		if (pval)
			val |= MAX77620_CNFGGLBL1_MPPLD;
	}

	ret = of_property_read_u32(np, "maxim,low-battery-reset", &pval);
	if (!ret) {
		mask |= MAX77620_CNFGGLBL1_LBRSTEN;
		if (pval)
			val |= MAX77620_CNFGGLBL1_LBRSTEN;
	}

	if (mask) {
		ret = max77620_reg_update(dev, MAX77620_REG_CNFGGLBL1,
					  mask, val);
		if (ret < 0)
			dev_err(dev, "Reg CNFGGLBL1 update failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int max77620_initialise_chip(struct max77620_chip *chip,
				    struct device *dev)
{
	struct device_node *np = dev->of_node;
	u32 mrt_time = 0;
	u8 reg_val;
	int ret;

	ret = of_property_read_u32(np, "maxim,hard-power-off-time", &mrt_time);
	if (ret < 0)
		return 0;

	mrt_time = (mrt_time > 12) ? 12 : mrt_time;
	if (mrt_time <= 6)
		reg_val = mrt_time - 2;
	else
		reg_val = (mrt_time - 6) / 2 + 4;

	reg_val <<= MAX77620_ONOFFCNFG1_MRT_SHIFT;

	ret = max77620_reg_update(dev, MAX77620_REG_ONOFFCNFG1,
				  MAX77620_ONOFFCNFG1_MRT_MASK, reg_val);
	if (ret < 0) {
		dev_err(dev, "REG ONOFFCNFG1 update failed: %d\n", ret);
		return ret;
	}

	/* Disable alarm wake to enable sleep from EN input signal */
	ret = max77620_reg_update(dev, MAX77620_REG_ONOFFCNFG2,
				  MAX77620_ONOFFCNFG2_WK_ALARM1, 0);
	if (ret < 0) {
		dev_err(dev, "REG ONOFFCNFG2 update failed: %d\n", ret);
		return ret;
	}

	return ret;
}

static int max77620_read_es_version(struct max77620_chip *chip)
{
	u8 val, cid_val[6];
	int i;
	int ret;

	for (i = MAX77620_REG_CID0; i <= MAX77620_REG_CID5; ++i) {
		ret = max77620_reg_read(chip->dev, i, &val);
		if (ret < 0) {
			dev_err(chip->dev, "CID%d register read failed: %d\n",
				i - MAX77620_REG_CID0, ret);
			return ret;
		}
		dev_dbg(chip->dev, "CID%d: 0x%02x\n",
			i - MAX77620_REG_CID0, val);
		cid_val[i - MAX77620_REG_CID0] = val;
	}

	/* CID4 is OTP Version  and CID5 is ES version */
	dev_info(chip->dev, "PMIC Version OTP:0x%02X and ES:0x%02X\n",
		 cid_val[4], MAX77620_CID5_DIDM(cid_val[5]));

	return ret;
}

static irqreturn_t max77620_mbattlow_irq(int irq, void *data)
{
	struct max77620_chip *max77620 = data;

	dev_dbg(max77620->dev, "MBATTLOW interrupt occurred\n");

	return IRQ_HANDLED;
}

static int max77620_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct device_node *node = client->dev.of_node;
	const struct max77620_sub_modules *children;
	const struct regmap_config *rmap_config = &max77620_regmap_config;
	struct max77620_chip *chip;
	int ret = 0;

	if (!node) {
		dev_err(&client->dev, "Device is not from DT\n");
		return -ENODEV;
	}

	children = of_device_get_match_data(&client->dev);
	if (!children)
		return -ENODEV;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	i2c_set_clientdata(client, chip);
	chip->dev = &client->dev;
	chip->irq_base = -1;
	chip->chip_irq = client->irq;
	chip->id = children->id;
	chip->base_client = client;

	if (chip->id == MAX20024)
		rmap_config = &max20024_regmap_config;

	chip->rmap = devm_regmap_init_i2c(chip->base_client, rmap_config);
	if (IS_ERR(chip->rmap)) {
		ret = PTR_ERR(chip->rmap);
		dev_err(&client->dev, "regmap init failed %d\n", ret);
		return ret;
	}

	mutex_init(&chip->mutex_config);

	ret = max77620_read_es_version(chip);
	if (ret < 0)
		return ret;

	ret = max77620_initialise_chip(chip, &client->dev);
	if (ret < 0)
		return ret;

	ret = regmap_add_irq_chip(chip->rmap, chip->chip_irq,
				  IRQF_ONESHOT | IRQF_SHARED,
				  chip->irq_base, &max77620_top_irq_chip,
				  &chip->top_irq_data);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to add top irq_chip %d\n", ret);
		return ret;
	}

	ret = max77620_initialise_fps(chip, &client->dev);
	if (ret < 0)
		goto fail_free_irq;

	ret = max77620_init_backup_battery_charging(chip, &client->dev);
	if (ret < 0)
		goto fail_free_irq;

	ret = max77620_init_low_battery_monitor(chip, &client->dev);
	if (ret < 0)
		goto fail_free_irq;

	ret =  mfd_add_devices(&client->dev, -1, children->cells,
			       children->ncells, NULL, 0,
			       regmap_irq_get_domain(chip->top_irq_data));
	if (ret < 0) {
		dev_err(&client->dev, "mfd add dev fail %d\n", ret);
		goto fail_free_irq;
	}

	chip->irq_mbattlow = max77620_irq_get_virq(chip->dev,
					MAX77620_IRQ_LBT_MBATLOW);
	if (chip->irq_mbattlow) {
		ret = devm_request_threaded_irq(chip->dev, chip->irq_mbattlow,
						NULL, max77620_mbattlow_irq,
						IRQF_ONESHOT,
						dev_name(chip->dev), chip);
		if (ret < 0)
			dev_err(&client->dev, "request irq %d failed: %d\n",
				chip->irq_mbattlow, ret);
	}

	return 0;

fail_free_irq:
	regmap_del_irq_chip(chip->chip_irq, chip->top_irq_data);

	return ret;
}

static int max77620_remove(struct i2c_client *client)
{
	struct max77620_chip *chip = i2c_get_clientdata(client);

	mfd_remove_devices(chip->dev);
	regmap_del_irq_chip(chip->chip_irq, chip->top_irq_data);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int max77620_set_fps_period(struct max77620_chip *chip,
				   int fps_id, int time_period)
{
	struct device *dev = chip->dev;
	int period = max77620_get_fps_period_reg_value(chip, time_period);
	int ret;

	ret = max77620_reg_update(dev, MAX77620_REG_FPS_CFG0 + fps_id,
				  MAX77620_FPS_TIME_PERIOD_MASK,
				  period << MAX77620_FPS_TIME_PERIOD_SHIFT);
	if (ret < 0) {
		dev_err(dev, "Reg 0x%02x write failed: %d\n",
			MAX77620_REG_FPS_CFG0 + fps_id, ret);
		return ret;
	}

	return 0;
}

static int max77620_i2c_suspend(struct device *dev)
{
	struct max77620_chip *chip = dev_get_drvdata(dev);
	unsigned int config;
	int fps;
	int ret;

	for (fps = 0; fps < 2; ++fps) {
		if (chip->suspend_fps_period[fps] < 0)
			continue;

		ret = max77620_set_fps_period(chip, fps,
					      chip->suspend_fps_period[fps]);
		if (ret < 0)
			dev_err(dev, "FPS%d config failed: %d\n", fps, ret);
	}

	/*
	 * For MAX20024: No need to configure SLPEN on suspend as
	 * it will be configured on Init.
	 */
	if (chip->id == MAX20024)
		goto out;

	config = (chip->sleep_enable) ? MAX77620_ONOFFCNFG1_SLPEN : 0;
	ret = max77620_reg_update(chip->dev, MAX77620_REG_ONOFFCNFG1,
				  MAX77620_ONOFFCNFG1_SLPEN,
				  config);
	if (ret < 0) {
		dev_err(dev, "Reg ONOFFCNFG1 update failed: %d\n", ret);
		return ret;
	}

	/* Disable WK_EN0 */
	ret = max77620_reg_update(chip->dev, MAX77620_REG_ONOFFCNFG2,
				  MAX77620_ONOFFCNFG2_WK_EN0, 0);
	if (ret < 0) {
		dev_err(dev, "Reg ONOFFCNFG2 WK_EN0 update failed: %d\n", ret);
		return ret;
	}

out:
	disable_irq(chip->chip_irq);

	return 0;
}

static int max77620_i2c_resume(struct device *dev)
{
	struct max77620_chip *chip = dev_get_drvdata(dev);
	int ret;
	int fps;

	for (fps = 0; fps < 2; ++fps) {
		if (chip->active_fps_period[fps] < 0)
			continue;

		ret = max77620_set_fps_period(chip, fps,
					      chip->active_fps_period[fps]);
		if (ret < 0)
			dev_err(dev, "FPS%d config failed: %d\n", fps, ret);
	}

	/*
	 * For MAX20024: No need to configure WKEN0 on resume as
	 * it is configured on Init.
	 */
	if (chip->id == MAX20024)
		goto out;

	/* Enable WK_EN0 */
	ret = max77620_reg_update(chip->dev, MAX77620_REG_ONOFFCNFG2,
				  MAX77620_ONOFFCNFG2_WK_EN0,
		MAX77620_ONOFFCNFG2_WK_EN0);
	if (ret < 0) {
		dev_err(dev, "Reg ONOFFCNFG2 WK_EN0 update failed: %d\n", ret);
		return ret;
	}

out:
	enable_irq(chip->chip_irq);

	return 0;
}
#endif

static const struct i2c_device_id max77620_id[] = {
	{"max77620", MAX77620},
	{"max20024", MAX20024},
	{},
};
MODULE_DEVICE_TABLE(i2c, max77620_id);

static const struct of_device_id max77620_of_match[] = {
	{
		.compatible = "maxim,max77620",
		.data = &max77620_cells,
	}, {
		.compatible = "maxim,max20024",
		.data = &max20024_cells,
	}, {
	},
};
MODULE_DEVICE_TABLE(of, max77620_of_match);

static const struct dev_pm_ops max77620_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(max77620_i2c_suspend, max77620_i2c_resume)
};

static struct i2c_driver max77620_driver = {
	.driver = {
		.name = "max77620",
		.pm = &max77620_pm_ops,
		.of_match_table = max77620_of_match,
	},
	.probe = max77620_probe,
	.remove = max77620_remove,
	.id_table = max77620_id,
};

static int __init max77620_init(void)
{
	return i2c_add_driver(&max77620_driver);
}
subsys_initcall(max77620_init);

static void __exit max77620_exit(void)
{
	i2c_del_driver(&max77620_driver);
}
module_exit(max77620_exit);

MODULE_DESCRIPTION("MAX77620/MAX20024 Multi Function Device Core Driver");
MODULE_AUTHOR("Laxman Dewangan <ldewangan@nvidia.com>");
MODULE_AUTHOR("Chaitanya Bandi <bandik@nvidia.com>");
MODULE_AUTHOR("Mallikarjun Kasoju <mkasoju@nvidia.com>");
MODULE_ALIAS("i2c:max77620");
MODULE_LICENSE("GPL v2");
