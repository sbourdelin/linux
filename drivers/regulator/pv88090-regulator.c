/*
 * pv88090-regulator.c - Regulator device driver for PV88090
 * Copyright (C) 2015  Powerventure Semiconductor Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regmap.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/regulator/of_regulator.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/regulator/pv88090.h>
#include "pv88090-regulator.h"

/* PV88090 REGULATOR IDs */
enum {
	/* BUCKs */
	PV88090_ID_BUCK1,
	PV88090_ID_BUCK2,
	PV88090_ID_BUCK3,

	/* LDOs */
	PV88090_ID_LDO1,
	PV88090_ID_LDO2,
};

struct pv88090_regulator {
	struct regulator_desc desc;
	/* Current limiting */
	unsigned	n_current_limits;
	const int	*current_limits;
	unsigned int limit_mask;
	unsigned int conf;		/* buck configuration register */
};

struct pv88090 {
	struct device *dev;
	struct regmap *regmap;
	struct pv88090_pdata *pdata;
	struct regulator_dev *rdev[PV88090_MAX_REGULATORS];
};

static const struct regmap_config pv88090_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

/* Current limits array (in uA) for BUCK1, BUCK2, BUCK3.
 *  Entry indexes corresponds to register values.
 */

static const int pv88090_buck1_limits[] = {
	 220000,  440000,  660000,  880000, 1100000, 1320000, 1540000, 1760000,
	1980000, 2200000, 2420000, 2640000, 2860000, 3080000, 3300000, 3520000,
	3740000, 3960000, 4180000, 4400000, 4620000, 4840000, 5060000, 5280000,
	5500000, 5720000, 5940000, 6160000, 6380000, 6600000, 6820000, 7040000
};

static const int pv88090_buck2_limits[] = {
	1496000, 2393000, 3291000, 4189000
};

static const int pv88090_buck3_limits[] = {
	1496000, 2393000, 3291000, 4189000
};

static unsigned int pv88090_buck_get_mode(struct regulator_dev *rdev)
{
	struct pv88090_regulator *info = rdev_get_drvdata(rdev);
	unsigned int data;
	int ret, mode = 0;

	ret = regmap_read(rdev->regmap, info->conf, &data);
	if (ret < 0)
		return ret;

	switch (data & PV88090_BUCK1_MODE_MASK) {
	case PV88090_BUCK_MODE_SYNC:
		mode = REGULATOR_MODE_FAST;
		break;
	case PV88090_BUCK_MODE_AUTO:
		mode = REGULATOR_MODE_NORMAL;
		break;
	case PV88090_BUCK_MODE_SLEEP:
		mode = REGULATOR_MODE_STANDBY;
		break;
	}

	return mode;
}

static int pv88090_buck_set_mode(struct regulator_dev *rdev,
					unsigned int mode)
{
	struct pv88090_regulator *info = rdev_get_drvdata(rdev);
	int val = 0;

	switch (mode) {
	case REGULATOR_MODE_FAST:
		val = PV88090_BUCK_MODE_SYNC;
		break;
	case REGULATOR_MODE_NORMAL:
		val = PV88090_BUCK_MODE_AUTO;
		break;
	case REGULATOR_MODE_STANDBY:
		val = PV88090_BUCK_MODE_SLEEP;
		break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(rdev->regmap, info->conf,
					PV88090_BUCK1_MODE_MASK, val);
}

static int pv88090_set_current_limit(struct regulator_dev *rdev, int min,
				    int max)
{
	struct pv88090_regulator *info = rdev_get_drvdata(rdev);
	int i;

	/* search for closest to maximum */
	for (i = info->n_current_limits; i >= 0; i--) {
		if (min <= info->current_limits[i]
			&& max >= info->current_limits[i]) {
			return regmap_update_bits(rdev->regmap,
				info->conf,
				info->limit_mask,
				i << PV88090_BUCK1_ILIM_SHIFT);
		}
	}

	return -EINVAL;
}

static int pv88090_get_current_limit(struct regulator_dev *rdev)
{
	struct pv88090_regulator *info = rdev_get_drvdata(rdev);
	unsigned int data;
	int ret;

	ret = regmap_read(rdev->regmap, info->conf, &data);
	if (ret < 0)
		return ret;

	data = (data & info->limit_mask) >> PV88090_BUCK1_ILIM_SHIFT;
	return info->current_limits[data];
}

static struct regulator_ops pv88090_buck_ops = {
	.get_mode = pv88090_buck_get_mode,
	.set_mode = pv88090_buck_set_mode,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.set_current_limit = pv88090_set_current_limit,
	.get_current_limit = pv88090_get_current_limit,
};

static struct regulator_ops pv88090_ldo_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.list_voltage = regulator_list_voltage_linear,
};

#define PV88090_BUCK(chip, regl_name, min, step, max, limits_array) \
{\
	.desc.id = chip##_ID_##regl_name,\
	.desc.name = __stringify(chip##_##regl_name),\
	.desc.type = REGULATOR_VOLTAGE,\
	.desc.owner = THIS_MODULE,\
	.desc.ops = &pv88090_buck_ops,\
	.desc.min_uV = min, \
	.desc.uV_step = step, \
	.desc.n_voltages = ((max) - (min))/(step) + 1, \
	.desc.enable_reg = PV88090_REG_##regl_name##_CONF0, \
	.desc.enable_mask = PV88090_##regl_name##_EN, \
	.desc.vsel_reg = PV88090_REG_##regl_name##_CONF0, \
	.desc.vsel_mask = PV88090_V##regl_name##_MASK, \
	.current_limits = limits_array, \
	.n_current_limits = ARRAY_SIZE(limits_array), \
	.limit_mask = PV88090_##regl_name##_ILIM_MASK, \
	.conf = PV88090_REG_##regl_name##_CONF1, \
}

#define PV88090_LDO(chip, regl_name, min, step, max) \
{\
	.desc.id = chip##_ID_##regl_name,\
	.desc.name = __stringify(chip##_##regl_name),\
	.desc.type = REGULATOR_VOLTAGE,\
	.desc.owner = THIS_MODULE,\
	.desc.ops = &pv88090_ldo_ops,\
	.desc.min_uV = min, \
	.desc.uV_step = step, \
	.desc.n_voltages = ((max) - (min))/(step) + 1, \
	.desc.enable_reg = PV88090_REG_##regl_name##_CONT, \
	.desc.enable_mask = PV88090_##regl_name##_EN, \
	.desc.vsel_reg = PV88090_REG_##regl_name##_CONT, \
	.desc.vsel_mask = PV88090_V##regl_name##_MASK, \
}

static const struct pv88090_regulator pv88090_regulator_info[] = {
	PV88090_BUCK(PV88090, BUCK1, 600000, 6250, 1393750,
		pv88090_buck1_limits),
	PV88090_BUCK(PV88090, BUCK2, 600000, 6250, 1393750,
		pv88090_buck2_limits),
	PV88090_BUCK(PV88090, BUCK3, 1400000, 6250, 2193750,
		pv88090_buck3_limits),
	PV88090_LDO(PV88090, LDO1, 1200000, 50000, 4350000),
	PV88090_LDO(PV88090, LDO2,  650000, 25000, 2225000),
};

#ifdef CONFIG_OF
static struct of_regulator_match pv88090_matches[] = {
	[PV88090_ID_BUCK1] = { .name = "BUCK1" },
	[PV88090_ID_BUCK2] = { .name = "BUCK2" },
	[PV88090_ID_BUCK3] = { .name = "BUCK3" },
	[PV88090_ID_LDO1] = { .name = "LDO1" },
	[PV88090_ID_LDO2] = { .name = "LDO2" },
};

static struct pv88090_pdata *pv88090_parse_regulators_dt(
		struct device *dev)
{
	struct pv88090_pdata *pdata;
	struct device_node *node;
	int i, num, n;

	node = of_get_child_by_name(dev->of_node, "regulators");
	if (!node) {
		dev_err(dev, "regulators node not found\n");
		return ERR_PTR(-ENODEV);
	}

	num = of_regulator_match(dev, node, pv88090_matches,
				 ARRAY_SIZE(pv88090_matches));
	of_node_put(node);
	if (num < 0) {
		dev_err(dev, "Failed to match regulators\n");
		return ERR_PTR(-EINVAL);
	}

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->num_regulator = num;

	n = 0;
	for (i = 0; i < ARRAY_SIZE(pv88090_matches); i++) {
		if (!pv88090_matches[i].init_data)
			continue;

		if (i < PV88090_ID_LDO1) {
			pv88090_matches[i].init_data->constraints
				.valid_modes_mask
				|=	REGULATOR_MODE_FAST
				| REGULATOR_MODE_NORMAL
				| REGULATOR_MODE_STANDBY;
			pv88090_matches[i].init_data->constraints.valid_ops_mask
				|=	REGULATOR_CHANGE_MODE;
		}

		pdata->init_data[n] = pv88090_matches[i].init_data;
		pdata->reg_node[n] = pv88090_matches[i].of_node;
		n++;
	};

	return pdata;
}
#else
static struct pv88090_pdata *pv88090_parse_regulators_dt(
		struct device *dev)
{
	return ERR_PTR(-ENODEV);
}
#endif

static irqreturn_t pv88090_irq_handler(int irq, void *data)
{
	struct pv88090 *chip = data;
	int i, reg_val, err, ret = IRQ_NONE;

	err = regmap_read(chip->regmap, PV88090_REG_EVENT_A, &reg_val);
	if (err < 0)
		goto error_i2c;

	if (reg_val & PV88090_E_VDD_FLT) {
		for (i = 0; i < PV88090_MAX_REGULATORS; i++) {
			if (chip->rdev[i] != NULL) {
				regulator_notifier_call_chain(chip->rdev[i],
					REGULATOR_EVENT_UNDER_VOLTAGE,
					NULL);
			}
		}

		err = regmap_update_bits(chip->regmap, PV88090_REG_EVENT_A,
			PV88090_E_VDD_FLT, PV88090_E_VDD_FLT);
		if (err < 0)
			goto error_i2c;

		ret = IRQ_HANDLED;
	}

	if (reg_val & PV88090_E_OVER_TEMP) {
		for (i = 0; i < PV88090_MAX_REGULATORS; i++) {
			if (chip->rdev[i] != NULL) {
				regulator_notifier_call_chain(chip->rdev[i],
					REGULATOR_EVENT_OVER_TEMP,
					NULL);
			}
		}

		err = regmap_update_bits(chip->regmap, PV88090_REG_EVENT_A,
			PV88090_E_OVER_TEMP, PV88090_E_OVER_TEMP);
		if (err < 0)
			goto error_i2c;

		ret = IRQ_HANDLED;
	}

	return ret;

error_i2c:
	dev_err(chip->dev, "I2C error : %d\n", err);
	return IRQ_NONE;
}

static int pv88090_regulator_init(struct pv88090 *chip)
{
	struct regulator_config config = { };
	int i;

	for (i = 0; i < chip->pdata->num_regulator; i++) {
		config.init_data = chip->pdata->init_data[i];
		config.dev = chip->dev;
		config.driver_data = (void *)&pv88090_regulator_info[i];
		config.regmap = chip->regmap;
		config.of_node = chip->pdata->reg_node[i];
		chip->rdev[i] = regulator_register(
			&pv88090_regulator_info[i].desc, &config);
		if (IS_ERR(chip->rdev[i])) {
			dev_err(chip->dev,
				"Failed to register PV88090 regulator\n");
			return PTR_ERR(chip->rdev[i]);
		}
	}

	return 0;
}

/*
 * I2C driver interface functions
 */
static int pv88090_i2c_probe(struct i2c_client *i2c,
		const struct i2c_device_id *id)
{
	struct pv88090 *chip;
	int error, ret;

	chip = devm_kzalloc(&i2c->dev, sizeof(struct pv88090), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &i2c->dev;
	chip->regmap = devm_regmap_init_i2c(i2c, &pv88090_regmap_config);
	if (IS_ERR(chip->regmap)) {
		error = PTR_ERR(chip->regmap);
		dev_err(chip->dev, "Failed to allocate register map: %d\n",
			error);
		return error;
	}

	i2c_set_clientdata(i2c, chip);

	chip->pdata = i2c->dev.platform_data;

	if (!chip->pdata)
		chip->pdata = pv88090_parse_regulators_dt(chip->dev);

	if (IS_ERR(chip->pdata)) {
		dev_err(chip->dev, "No regulators defined for the platform\n");
		return PTR_ERR(chip->pdata);
	}

	if (i2c->irq != 0) {
		ret = regmap_write(chip->regmap, PV88090_REG_MASK_A, 0xFF);
		if (ret < 0) {
			dev_err(chip->dev,
				"Failed to mask A reg: %d\n", ret);
			return ret;
		}
		regmap_write(chip->regmap, PV88090_REG_MASK_B, 0xFF);
		if (ret < 0) {
			dev_err(chip->dev,
				"Failed to mask B reg: %d\n", ret);
			return ret;
		}

		ret = request_threaded_irq(i2c->irq, NULL,
					pv88090_irq_handler,
					IRQF_TRIGGER_LOW|IRQF_ONESHOT,
					"pv88090", chip);
		if (ret != 0) {
			dev_err(chip->dev, "Failed to request IRQ: %d\n",
				i2c->irq);
			return ret;
		}

		ret = regmap_update_bits(chip->regmap, PV88090_REG_MASK_A,
			PV88090_M_VDD_FLT | PV88090_M_OVER_TEMP, 0);
		if (ret < 0) {
			dev_err(chip->dev,
				"Failed to update mask reg: %d\n", ret);
			return ret;
		}

	} else {
		dev_warn(chip->dev, "No IRQ configured\n");
	}

	ret = pv88090_regulator_init(chip);

	if (ret < 0)
		dev_err(chip->dev, "Failed to initialize regulator: %d\n", ret);

	return ret;
}

static const struct i2c_device_id pv88090_i2c_id[] = {
	{"pv88090", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, pv88090_i2c_id);

#ifdef CONFIG_OF
static const struct of_device_id pv88090_dt_ids[] = {
	{ .compatible = "pvs,pv88090", .data = &pv88090_i2c_id[0] },
	{},
};
MODULE_DEVICE_TABLE(of, pv88090_dt_ids);
#endif

static struct i2c_driver pv88090_regulator_driver = {
	.driver = {
		.name = "pv88090",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(pv88090_dt_ids),
	},
	.probe = pv88090_i2c_probe,
	.id_table = pv88090_i2c_id,
};

module_i2c_driver(pv88090_regulator_driver);

MODULE_AUTHOR("James Ban <James.Ban.opensource@diasemi.com>");
MODULE_DESCRIPTION("Regulator device driver for Powerventure PV88090");
MODULE_LICENSE("GPL");
