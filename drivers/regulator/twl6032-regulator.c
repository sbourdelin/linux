/*
 * TWL6032 regulator driver
 * Copyright (C) 2016 Nicolae Rosia <nicolae.rosia@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/irq.h>
#include <linux/mfd/twl-core.h>

/* TWL6032 register offsets */
#define TWL6032_VREG_TRANS 1
#define TWL6032_VREG_STATE 2
#define TWL6032_VREG_VOLTAGE 3

#define TWL6032_LDO_MIN_MV 1000
#define TWL6032_LDO_MAX_MV 3300

/* TWL6030 LDO register values for CFG_TRANS */
#define TWL6032_CFG_TRANS_STATE_MASK 0x03
#define TWL6032_CFG_TRANS_STATE_OFF 0x00
#define TWL6032_CFG_TRANS_STATE_AUTO 0x01
#define TWL6032_CFG_TRANS_SLEEP_SHIFT 2

#define TWL6032_CFG_STATE_MASK 0x03
#define TWL6032_CFG_STATE_OFF 0x00
#define TWL6032_CFG_STATE_ON 0x01
#define TWL6032_CFG_STATE_OFF2 0x02
#define TWL6032_CFG_STATE_SLEEP 0x03

static const char *rdev_get_name(struct regulator_dev *rdev)
{
	if (rdev->constraints && rdev->constraints->name)
		return rdev->constraints->name;
	else if (rdev->desc->name)
		return rdev->desc->name;
	else
		return "";
}

struct twl6032_regulator_info {
	u8 base;
	unsigned int min_mV;
	struct regulator_desc desc;
};

struct twl6032_regulator {
	struct twl6032_regulator_info *info;
};

static int twl6032_set_trans_state(struct regulator_dev *rdev, u8 shift, u8 val)
{
	struct twl6032_regulator *twl6032_reg = rdev_get_drvdata(rdev);
	struct twl6032_regulator_info *info = twl6032_reg->info;
	unsigned int state;
	u8 mask;
	int ret;

	/* Read CFG_TRANS register of TWL6030 */
	ret = regmap_read(rdev->regmap, info->base + TWL6032_VREG_TRANS,
			  &state);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s %s: regmap_read: %d\n",
			rdev_get_name(rdev), __func__, ret);
		return ret;
	}

	mask = TWL6032_CFG_TRANS_STATE_MASK << shift;
	val = (val << shift) & mask;

	/* If value is already set, no need to write to reg */
	if (val == (state & mask))
		return 0;

	state &= ~mask;
	state |= val;

	return regmap_write(rdev->regmap, info->base + TWL6032_VREG_TRANS,
			    state);
}

static int
twl6032_ldo_list_voltage(struct regulator_dev *rdev, unsigned int sel)
{
	int ret;

	switch (sel) {
	case 0:
		ret = 0;
		break;
	case 1 ... 24:
		/* Linear mapping from 00000001 to 00011000:
		 * Absolute voltage value = 1.0 V + 0.1 V × (sel – 00000001)
		 */
		ret = (TWL6032_LDO_MIN_MV + 100 * (sel - 1)) * 1000;
		break;
	case 25 ... 30:
		ret = -EINVAL;
		break;
	case 31:
		ret = 2750000;
		break;
	default:
		ret = -EINVAL;
	}

	dev_dbg(&rdev->dev, "%s %s: sel: %d, mV: %d\n", rdev_get_name(rdev),
		__func__, sel, ret);

	return ret;
}

static int
twl6032_ldo_set_voltage_sel(struct regulator_dev *rdev, unsigned int sel)
{
	struct twl6032_regulator *twl6032_reg = rdev_get_drvdata(rdev);
	struct twl6032_regulator_info *info = twl6032_reg->info;
	int ret;

	dev_dbg(&rdev->dev, "%s %s: sel: 0x%02X\n", rdev_get_name(rdev),
		__func__, sel);

	ret = regmap_write(rdev->regmap, info->base + TWL6032_VREG_VOLTAGE,
			   sel);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s %s: regmap_write: %d\n",
			rdev_get_name(rdev), __func__, ret);
		return ret;
	}

	return 0;
}

static int twl6032_ldo_get_voltage_sel(struct regulator_dev *rdev)
{
	struct twl6032_regulator *twl6032_reg = rdev_get_drvdata(rdev);
	struct twl6032_regulator_info *info = twl6032_reg->info;
	unsigned int val;
	int ret;

	ret = regmap_read(rdev->regmap, info->base + TWL6032_VREG_VOLTAGE,
			  &val);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s %s: regmap_read: %d\n",
			rdev_get_name(rdev), __func__, ret);
		return ret;
	}

	dev_dbg(&rdev->dev, "%s %s: vsel: 0x%02X\n", rdev_get_name(rdev),
		__func__, val);

	return val;
}

static int twl6032_ldo_enable(struct regulator_dev *rdev)
{
	struct twl6032_regulator *twl6032_reg = rdev_get_drvdata(rdev);
	struct twl6032_regulator_info *info = twl6032_reg->info;
	int ret;

	dev_dbg(&rdev->dev, "%s %s\n", rdev_get_name(rdev), __func__);

	ret = regmap_write(rdev->regmap, info->base + TWL6032_VREG_STATE,
			   TWL6032_CFG_STATE_ON);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s %s: regmap_write: %d\n",
			rdev_get_name(rdev), __func__, ret);
		return ret;
	}

	ret = twl6032_set_trans_state(rdev, TWL6032_CFG_TRANS_SLEEP_SHIFT,
				      TWL6032_CFG_TRANS_STATE_AUTO);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s %s: twl6032_set_trans_state: %d\n",
			rdev_get_name(rdev), __func__, ret);
		return ret;
	}

	return 0;
}

static int twl6032_ldo_disable(struct regulator_dev *rdev)
{
	struct twl6032_regulator *twl6032_reg = rdev_get_drvdata(rdev);
	struct twl6032_regulator_info *info = twl6032_reg->info;
	int ret;

	dev_dbg(&rdev->dev, "%s %s\n", rdev_get_name(rdev), __func__);

	ret = regmap_write(rdev->regmap, info->base + TWL6032_VREG_STATE,
			   TWL6032_CFG_STATE_OFF);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s %s: regmap_write: %d\n",
			rdev_get_name(rdev), __func__, ret);
		return ret;
	}

	ret = twl6032_set_trans_state(rdev, TWL6032_CFG_TRANS_SLEEP_SHIFT,
				      TWL6032_CFG_TRANS_STATE_OFF);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s %s: twl6032_set_trans_state: %d\n",
			rdev_get_name(rdev), __func__, ret);
		return ret;
	}

	return 0;
}

static int twl6032_ldo_is_enabled(struct regulator_dev *rdev)
{
	struct twl6032_regulator *twl6032_reg = rdev_get_drvdata(rdev);
	struct twl6032_regulator_info *info = twl6032_reg->info;
	int ret;
	unsigned int val;

	ret = regmap_read(rdev->regmap, info->base + TWL6032_VREG_STATE, &val);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s regmap_read: %d\n", __func__, ret);
		return ret;
	}

	dev_dbg(&rdev->dev, "%s %s: val: 0x%02X, val-masked: 0x%02X, ret: %d\n",
		rdev_get_name(rdev), __func__,
		val, val & TWL6032_CFG_STATE_MASK,
		(val & TWL6032_CFG_STATE_MASK) == TWL6032_CFG_STATE_ON);

	val &= TWL6032_CFG_STATE_MASK;

	return val == TWL6032_CFG_STATE_ON;
}

static int twl6032_ldo_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	struct twl6032_regulator *twl6032_reg = rdev_get_drvdata(rdev);
	struct twl6032_regulator_info *info = twl6032_reg->info;
	unsigned int val = 0;
	int ret;

	dev_dbg(&rdev->dev, "%s %s: mode: 0x%02X\n", rdev_get_name(rdev),
		__func__, mode);

	switch (mode) {
	case REGULATOR_MODE_NORMAL:
		val |= TWL6032_CFG_STATE_ON;
		break;
	case REGULATOR_MODE_STANDBY:
		val |= TWL6032_CFG_STATE_SLEEP;
		break;

	default:
		return -EINVAL;
	}

	ret = regmap_write(rdev->regmap, info->base + TWL6032_VREG_STATE, val);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s %s: regmap_write: %d\n",
			rdev_get_name(rdev), __func__, ret);
		return ret;
	}

	return 0;
}

static int twl6032_ldo_get_status(struct regulator_dev *rdev)
{
	struct twl6032_regulator *twl6032_reg = rdev_get_drvdata(rdev);
	struct twl6032_regulator_info *info = twl6032_reg->info;
	int ret;
	unsigned int val;

	ret = regmap_read(rdev->regmap, info->base + TWL6032_VREG_STATE, &val);
	if (ret < 0) {
		dev_err(&rdev->dev, "%s %s: regmap_read: %d\n",
			rdev_get_name(rdev), __func__, ret);
		return ret;
	}

	dev_dbg(&rdev->dev, "%s %s: val: 0x%02X, val-with-mask: 0x%02X\n",
		rdev_get_name(rdev), __func__,
		val, val & TWL6032_CFG_STATE_MASK);

	val &= TWL6032_CFG_STATE_MASK;

	switch (val) {
	case TWL6032_CFG_STATE_ON:
		return REGULATOR_STATUS_NORMAL;

	case TWL6032_CFG_STATE_SLEEP:
		return REGULATOR_STATUS_STANDBY;

	case TWL6032_CFG_STATE_OFF:
	case TWL6032_CFG_STATE_OFF2:
	default:
		break;
	}

	return REGULATOR_STATUS_OFF;
}

static int twl6032_ldo_suspend_enable(struct regulator_dev *rdev)
{
	return twl6032_set_trans_state(rdev, TWL6032_CFG_TRANS_SLEEP_SHIFT,
				       TWL6032_CFG_TRANS_STATE_AUTO);
}

static int twl6032_ldo_suspend_disable(struct regulator_dev *rdev)
{
	return twl6032_set_trans_state(rdev, TWL6032_CFG_TRANS_SLEEP_SHIFT,
				       TWL6032_CFG_TRANS_STATE_OFF);
}

static int
twl6032_fixed_list_voltage(struct regulator_dev *rdev, unsigned int sel)
{
	struct twl6032_regulator *twl6032_reg = rdev_get_drvdata(rdev);
	struct twl6032_regulator_info *info = twl6032_reg->info;

	return info->min_mV * 1000; /* mV to V */
}

static int twl6032_fixed_get_voltage(struct regulator_dev *rdev)
{
	struct twl6032_regulator *twl6032_reg = rdev_get_drvdata(rdev);
	struct twl6032_regulator_info *info = twl6032_reg->info;

	return info->min_mV * 1000; /* mV to V */
}

static const struct regulator_ops twl6032_ldo_ops = {
	.list_voltage = twl6032_ldo_list_voltage,
	.set_voltage_sel = twl6032_ldo_set_voltage_sel,
	.get_voltage_sel = twl6032_ldo_get_voltage_sel,
	.enable = twl6032_ldo_enable,
	.disable = twl6032_ldo_disable,
	.is_enabled = twl6032_ldo_is_enabled,
	.set_mode = twl6032_ldo_set_mode,
	.get_status = twl6032_ldo_get_status,
	.set_suspend_enable = twl6032_ldo_suspend_enable,
	.set_suspend_disable =  twl6032_ldo_suspend_disable,
};

static const struct regulator_ops twl6032_fixed_ops = {
	.list_voltage = twl6032_fixed_list_voltage,
	.get_voltage = twl6032_fixed_get_voltage,
	.enable = twl6032_ldo_enable,
	.disable = twl6032_ldo_disable,
	.is_enabled = twl6032_ldo_is_enabled,
	.set_mode = twl6032_ldo_set_mode,
	.get_status = twl6032_ldo_get_status,
	.set_suspend_enable = twl6032_ldo_suspend_enable,
	.set_suspend_disable =  twl6032_ldo_suspend_disable,
};

#define TWL6032_LDO_REG_VOLTAGES \
	((TWL6032_LDO_MAX_MV - TWL6032_LDO_MIN_MV) / 100 + 1)
#define TWL6032_LDO_REG(_id, _reg) \
{ \
	.base = _reg, \
	.desc = { \
		.name = "twl6032-reg-" # _id, \
		.n_voltages = TWL6032_LDO_REG_VOLTAGES, \
		.ops = &twl6032_ldo_ops, \
		.type = REGULATOR_VOLTAGE, \
		.owner = THIS_MODULE, \
	}, \
}

#define TWL6032_FIXED_REG(_id, _reg, _min_mV) \
{ \
	.base = _reg, \
	.min_mV = _min_mV, \
	.desc = { \
		.name = "twl6032-reg-" # _id, \
		.n_voltages = 1, \
		.ops = &twl6032_fixed_ops, \
		.type = REGULATOR_VOLTAGE, \
		.owner = THIS_MODULE, \
	}, \
}

#define TWL6032_RESOURCE_REG(_id, _reg) \
{ \
	.base = _reg, \
	.desc = { \
		.name = "twl6032-reg-" # _id, \
		.ops = &twl6032_ldo_ops, \
		.type = REGULATOR_VOLTAGE, \
		.owner = THIS_MODULE, \
	}, \
}

static struct twl6032_regulator_info twl6032_ldo_reg_info[] = {
	TWL6032_LDO_REG(LDO1, 0x9C),
	TWL6032_LDO_REG(LDO2, 0x84),
	TWL6032_LDO_REG(LDO3, 0x8C),
	TWL6032_LDO_REG(LDO4, 0x88),
	TWL6032_LDO_REG(LDO5, 0x98),
	TWL6032_LDO_REG(LDO6, 0x90),
	TWL6032_LDO_REG(LDO7, 0xA4),
	TWL6032_LDO_REG(LDOLN, 0x94),
	TWL6032_LDO_REG(LDOUSB, 0xA0),
};

static struct twl6032_regulator_info twl6032_fixed_reg_info[] = {
	TWL6032_FIXED_REG(VANA, 0x80, 2100),
};

static struct of_regulator_match
twl6032_ldo_reg_matches[] = {
	{ .name = "LDO1", },
	{ .name = "LDO2", },
	{ .name = "LDO3", },
	{ .name = "LDO4", },
	{ .name = "LDO5", },
	{ .name = "LDO6", },
	{ .name = "LDO7", },
	{ .name = "LDOLN" },
	{ .name = "LDOUSB" }
};

static struct of_regulator_match
twl6032_fixed_reg_matches[] = {
	{ .name = "VANA", },
};

#define TWL6032_LDO_REG_NUM ARRAY_SIZE(twl6032_ldo_reg_matches)
#define TWL6032_FIXED_REG_NUM ARRAY_SIZE(twl6032_fixed_reg_matches)

struct twl6032_regulator_priv {
	struct twl6032_regulator ldo_regulators[TWL6032_LDO_REG_NUM];
	struct twl6032_regulator fixed_regulators[TWL6032_FIXED_REG_NUM];
};

static int twl6032_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *regulators;
	struct of_regulator_match *match;
	struct twlcore *twl = dev_get_drvdata(pdev->dev.parent);
	struct twl6032_regulator_priv *priv;
	struct regulator_config config = {
		.dev = &pdev->dev,
	};
	struct regulator_dev *rdev;
	int ret, i;

	if (!dev->of_node) {
		dev_err(&pdev->dev, "no DT info\n");
		return -EINVAL;
	}

	regulators = of_get_child_by_name(dev->of_node, "regulators");
	if (!regulators) {
		dev_err(dev, "regulator node not found\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	/* LDO regulators parsing */
	ret = of_regulator_match(dev, regulators, twl6032_ldo_reg_matches,
				 TWL6032_LDO_REG_NUM);
	of_node_put(regulators);
	if (ret < 0) {
		dev_err(dev, "error parsing LDO reg init data: %d\n", ret);
		return ret;
	}

	for (i = 0; i < TWL6032_LDO_REG_NUM; i++) {
		struct twl6032_regulator *twl6032_reg;

		match = &twl6032_ldo_reg_matches[i];
		if (!match->of_node)
			continue;

		twl6032_reg = &priv->ldo_regulators[i];
		twl6032_reg->info = &twl6032_ldo_reg_info[i];

		config.init_data = match->init_data;
		config.driver_data = &priv->ldo_regulators[i];
		config.regmap = twl->twl_modules[0].regmap;
		config.of_node = match->of_node;

		rdev = devm_regulator_register(dev, &twl6032_reg->info->desc,
					       &config);
		if (IS_ERR(rdev)) {
			ret = PTR_ERR(rdev);
			dev_err(dev, "failed to register regulator %s: %d\n",
				twl6032_reg->info->desc.name, ret);
			return ret;
		}
	}

	/* Fixed regulators parsing */
	ret = of_regulator_match(dev, regulators, twl6032_fixed_reg_matches,
				 TWL6032_FIXED_REG_NUM);
	of_node_put(regulators);
	if (ret < 0) {
		dev_err(dev, "error parsing fixed reg init data: %d\n", ret);
		return ret;
	}

	for (i = 0; i < TWL6032_FIXED_REG_NUM; i++) {
		struct twl6032_regulator *twl6032_reg;

		match = &twl6032_fixed_reg_matches[i];
		if (!match->of_node)
			continue;

		twl6032_reg = &priv->fixed_regulators[i];
		twl6032_reg->info = &twl6032_fixed_reg_info[i];

		config.init_data = match->init_data;
		config.driver_data = &priv->fixed_regulators[i];
		config.regmap = twl->twl_modules[0].regmap;
		config.of_node = match->of_node;

		rdev = devm_regulator_register(dev, &twl6032_reg->info->desc,
					       &config);
		if (IS_ERR(rdev)) {
			ret = PTR_ERR(rdev);
			dev_err(dev, "failed to register regulator %s: %d\n",
				twl6032_reg->info->desc.name, ret);
			return ret;
		}
	}

	return 0;
}

static int twl6032_regulator_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id twl6032_dt_match[] = {
	{ .compatible = "ti,twl6032-regulator" },
	{ /* last entry */ }
};

MODULE_DEVICE_TABLE(platform, twl6032_regulator_driver_ids);

static struct platform_driver twl6032_regulator_driver = {
	.driver = {
		.name = "twl6032-regulator",
		.of_match_table = twl6032_dt_match,
	},
	.probe = twl6032_regulator_probe,
	.remove = twl6032_regulator_remove,
};

static int __init twl6032_regulator_init(void)
{
	return platform_driver_register(&twl6032_regulator_driver);
}
subsys_initcall(twl6032_regulator_init);

static void __exit twl6032_regulator_exit(void)
{
	platform_driver_unregister(&twl6032_regulator_driver);
}
module_exit(twl6032_regulator_exit);

MODULE_AUTHOR("Nicolae Rosia <nicolae.rosia@gmail.com>");
MODULE_DESCRIPTION("TI TWL6032 Regulator Driver");
MODULE_LICENSE("GPL v2");
