/*
 * tps65023-regulator.c
 *
 * Supports TPS65023 Regulator
 *
 * Copyright (C) 2009 Texas Instrument Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any kind,
 * whether express or implied; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/regulator/of_regulator.h>

/* Register definitions */
#define	TPS65023_REG_VERSION		0
#define	TPS65023_REG_PGOODZ		1
#define	TPS65023_REG_MASK		2
#define	TPS65023_REG_REG_CTRL		3
#define	TPS65023_REG_CON_CTRL		4
#define	TPS65023_REG_CON_CTRL2		5
#define	TPS65023_REG_DEF_CORE		6
#define	TPS65023_REG_DEFSLEW		7
#define	TPS65023_REG_LDO_CTRL		8

/* PGOODZ bitfields */
#define	TPS65023_PGOODZ_PWRFAILZ	BIT(7)
#define	TPS65023_PGOODZ_LOWBATTZ	BIT(6)
#define	TPS65023_PGOODZ_VDCDC1		BIT(5)
#define	TPS65023_PGOODZ_VDCDC2		BIT(4)
#define	TPS65023_PGOODZ_VDCDC3		BIT(3)
#define	TPS65023_PGOODZ_LDO2		BIT(2)
#define	TPS65023_PGOODZ_LDO1		BIT(1)

/* MASK bitfields */
#define	TPS65023_MASK_PWRFAILZ		BIT(7)
#define	TPS65023_MASK_LOWBATTZ		BIT(6)
#define	TPS65023_MASK_VDCDC1		BIT(5)
#define	TPS65023_MASK_VDCDC2		BIT(4)
#define	TPS65023_MASK_VDCDC3		BIT(3)
#define	TPS65023_MASK_LDO2		BIT(2)
#define	TPS65023_MASK_LDO1		BIT(1)

/* REG_CTRL bitfields */
#define TPS65023_REG_CTRL_VDCDC1_EN	BIT(5)
#define TPS65023_REG_CTRL_VDCDC2_EN	BIT(4)
#define TPS65023_REG_CTRL_VDCDC3_EN	BIT(3)
#define TPS65023_REG_CTRL_LDO2_EN	BIT(2)
#define TPS65023_REG_CTRL_LDO1_EN	BIT(1)

/* REG_CTRL2 bitfields */
#define TPS65023_REG_CTRL2_GO		BIT(7)
#define TPS65023_REG_CTRL2_CORE_ADJ	BIT(6)
#define TPS65023_REG_CTRL2_DCDC2	BIT(2)
#define TPS65023_REG_CTRL2_DCDC1	BIT(1)
#define TPS65023_REG_CTRL2_DCDC3	BIT(0)

/* Number of step-down converters available */
#define TPS65023_NUM_DCDC		3
/* Number of LDO voltage regulators  available */
#define TPS65023_NUM_LDO		2
/* Number of total regulators available */
#define TPS65023_NUM_REGULATOR	(TPS65023_NUM_DCDC + TPS65023_NUM_LDO)

/* DCDCs */
#define TPS65023_DCDC_1			0
#define TPS65023_DCDC_2			1
#define TPS65023_DCDC_3			2
/* LDOs */
#define TPS65023_LDO_1			3
#define TPS65023_LDO_2			4

#define TPS65023_MAX_REG_ID		TPS65023_LDO_2

#define TPS65023_REGULATOR(_name, _id, _of_match, _ops, _n, _vr, _vm, _em, \
			   _t) \
	{						\
		.name		= _name,		\
		.id		= _id,			\
		.of_match       = of_match_ptr(_of_match),    \
		.regulators_node = of_match_ptr("regulators"), \
		.ops		= &_ops,		\
		.n_voltages	= _n,			\
		.type		= REGULATOR_VOLTAGE,	\
		.owner		= THIS_MODULE,		\
		.vsel_reg	= _vr,			\
		.vsel_mask	= _vm,			\
		.enable_reg	= TPS65023_REG_REG_CTRL,\
		.enable_mask	= _em,			\
		.volt_table	= _t,			\
	}						\

#define TPS65023_REGULATOR_DCDX(_name, _id, _of_match, _ops, _n, _vr, _vm, \
				_em, _ar, _ab, _t) \
	{						\
		.name		= _name,		\
		.id		= _id,			\
		.of_match       = of_match_ptr(_of_match),    \
		.regulators_node = of_match_ptr("regulators"), \
		.ops		= &_ops,		\
		.n_voltages	= _n,			\
		.type		= REGULATOR_VOLTAGE,	\
		.owner		= THIS_MODULE,		\
		.vsel_reg	= _vr,			\
		.vsel_mask	= _vm,			\
		.enable_reg	= TPS65023_REG_REG_CTRL,\
		.enable_mask	= _em,			\
		.apply_reg	= _ar,			\
		.apply_bit	= _ab,			\
		.volt_table	= _t,			\
	}						\

/* Supported voltage values for regulators */
static const unsigned int VCORE_VSEL_table[] = {
	800000, 825000, 850000, 875000,
	900000, 925000, 950000, 975000,
	1000000, 1025000, 1050000, 1075000,
	1100000, 1125000, 1150000, 1175000,
	1200000, 1225000, 1250000, 1275000,
	1300000, 1325000, 1350000, 1375000,
	1400000, 1425000, 1450000, 1475000,
	1500000, 1525000, 1550000, 1600000,
};

static const unsigned int DCDC_FIXED_3300000_VSEL_table[] = {
	3300000,
};

static const unsigned int DCDC_FIXED_1800000_VSEL_table[] = {
	1800000,
};

/* Supported voltage values for LDO regulators for tps65020 */
static const unsigned int TPS65020_LDO_VSEL_table[] = {
	1000000, 1050000, 1100000, 1300000,
	1800000, 2500000, 3000000, 3300000,
};

/* Supported voltage values for LDO regulators
 * for tps65021 and tps65023 */
static const unsigned int TPS65023_LDO1_VSEL_table[] = {
	1000000, 1100000, 1300000, 1800000,
	2200000, 2600000, 2800000, 3150000,
};

static const unsigned int TPS65023_LDO2_VSEL_table[] = {
	1050000, 1200000, 1300000, 1800000,
	2500000, 2800000, 3000000, 3300000,
};

/* PMIC details */
struct tps_pmic {
	struct regulator_desc desc[TPS65023_NUM_REGULATOR];
	struct regulator_dev *rdev[TPS65023_NUM_REGULATOR];
	struct regmap *regmap;
	u8 core_regulator;
};

enum tps6502x_id {
	TPS65020,
	TPS65021,
	TPS65023,
};

/* Struct passed as driver data */
struct tps_driver_data {
	enum tps6502x_id id;
	const struct regulator_desc *regulators;
	u8 core_regulator;
};

static int tps65023_dcdc_get_voltage_sel(struct regulator_dev *dev)
{
	struct tps_pmic *tps = rdev_get_drvdata(dev);
	int dcdc = rdev_get_id(dev);

	if (dcdc < TPS65023_DCDC_1 || dcdc > TPS65023_DCDC_3)
		return -EINVAL;

	if (dcdc != tps->core_regulator)
		return 0;

	return regulator_get_voltage_sel_regmap(dev);
}

static int tps65023_dcdc_set_voltage_sel(struct regulator_dev *dev,
					 unsigned selector)
{
	struct tps_pmic *tps = rdev_get_drvdata(dev);
	int dcdc = rdev_get_id(dev);

	if (dcdc != tps->core_regulator)
		return -EINVAL;

	return regulator_set_voltage_sel_regmap(dev, selector);
}

/* Operations permitted on VDCDCx */
static const struct regulator_ops tps65023_dcdc_ops = {
	.is_enabled = regulator_is_enabled_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.get_voltage_sel = tps65023_dcdc_get_voltage_sel,
	.set_voltage_sel = tps65023_dcdc_set_voltage_sel,
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_ascend,
};

/* Operations permitted on LDOx */
static const struct regulator_ops tps65023_ldo_ops = {
	.is_enabled = regulator_is_enabled_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_ascend,
};

static const struct regmap_config tps65023_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static const struct regulator_desc regulators_65020[] = {
	TPS65023_REGULATOR_DCDX("DCDC1", TPS65023_DCDC_1, "dcdc1",
			   tps65023_dcdc_ops,
			   ARRAY_SIZE(DCDC_FIXED_3300000_VSEL_table),
			   TPS65023_REG_DEF_CORE,
			   ARRAY_SIZE(DCDC_FIXED_3300000_VSEL_table) - 1,
			   1 << (TPS65023_NUM_REGULATOR - 0),
			   TPS65023_REG_CON_CTRL2, TPS65023_REG_CTRL2_GO,
			   DCDC_FIXED_3300000_VSEL_table),
	TPS65023_REGULATOR_DCDX("DCDC2", TPS65023_DCDC_2, "dcdc2",
			   tps65023_dcdc_ops,
			   ARRAY_SIZE(DCDC_FIXED_1800000_VSEL_table),
			   TPS65023_REG_DEF_CORE,
			   ARRAY_SIZE(DCDC_FIXED_1800000_VSEL_table) - 1,
			   1 << (TPS65023_NUM_REGULATOR - 1),
			   TPS65023_REG_CON_CTRL2, TPS65023_REG_CTRL2_GO,
			   DCDC_FIXED_1800000_VSEL_table),
	TPS65023_REGULATOR_DCDX("DCDC3", TPS65023_DCDC_3, "dcdc3",
			   tps65023_dcdc_ops, ARRAY_SIZE(VCORE_VSEL_table),
			   TPS65023_REG_DEF_CORE,
			   ARRAY_SIZE(VCORE_VSEL_table) - 1,
			   1 << (TPS65023_NUM_REGULATOR - 2),
			   TPS65023_REG_CON_CTRL2, TPS65023_REG_CTRL2_GO,
			   VCORE_VSEL_table),
	TPS65023_REGULATOR("LDO1", TPS65023_LDO_1, "ldo1",
			   tps65023_ldo_ops,
			   ARRAY_SIZE(TPS65020_LDO_VSEL_table),
			   TPS65023_REG_LDO_CTRL,
			   0x07, 1 << 1, TPS65020_LDO_VSEL_table),
	TPS65023_REGULATOR("LDO2", TPS65023_LDO_2, "ldo2",
			   tps65023_ldo_ops,
			   ARRAY_SIZE(TPS65020_LDO_VSEL_table),
			   TPS65023_REG_LDO_CTRL,
			   0x70, 1 << 2, TPS65020_LDO_VSEL_table),
};

static const struct regulator_desc regulators_65021[] = {
	TPS65023_REGULATOR_DCDX("DCDC1", TPS65023_DCDC_1, "dcdc1",
			   tps65023_dcdc_ops,
			   ARRAY_SIZE(DCDC_FIXED_3300000_VSEL_table),
			   TPS65023_REG_DEF_CORE,
			   ARRAY_SIZE(DCDC_FIXED_3300000_VSEL_table) - 1,
			   1 << (TPS65023_NUM_REGULATOR - 0),
			   TPS65023_REG_CON_CTRL2, TPS65023_REG_CTRL2_GO,
			   DCDC_FIXED_3300000_VSEL_table),
	TPS65023_REGULATOR_DCDX("DCDC2", TPS65023_DCDC_2, "dcdc2",
			   tps65023_dcdc_ops,
			   ARRAY_SIZE(DCDC_FIXED_1800000_VSEL_table),
			   TPS65023_REG_DEF_CORE,
			   ARRAY_SIZE(DCDC_FIXED_1800000_VSEL_table) - 1,
			   1 << (TPS65023_NUM_REGULATOR - 1),
			   TPS65023_REG_CON_CTRL2, TPS65023_REG_CTRL2_GO,
			   DCDC_FIXED_1800000_VSEL_table),
	TPS65023_REGULATOR_DCDX("DCDC3", TPS65023_DCDC_3, "dcdc3",
			   tps65023_dcdc_ops, ARRAY_SIZE(VCORE_VSEL_table),
			   TPS65023_REG_DEF_CORE,
			   ARRAY_SIZE(VCORE_VSEL_table) - 1,
			   1 << (TPS65023_NUM_REGULATOR - 2),
			   TPS65023_REG_CON_CTRL2, TPS65023_REG_CTRL2_GO,
			   VCORE_VSEL_table),
	TPS65023_REGULATOR("LDO1", TPS65023_LDO_1, "ldo1",
			   tps65023_ldo_ops,
			   ARRAY_SIZE(TPS65023_LDO1_VSEL_table),
			   TPS65023_REG_LDO_CTRL,
			   0x07, 1 << 1, TPS65023_LDO1_VSEL_table),
	TPS65023_REGULATOR("LDO2", TPS65023_LDO_2, "ldo2",
			   tps65023_ldo_ops,
			   ARRAY_SIZE(TPS65023_LDO2_VSEL_table),
			   TPS65023_REG_LDO_CTRL,
			   0x70, 1 << 2, TPS65023_LDO2_VSEL_table),
};

static const struct regulator_desc regulators_65023[] = {
	TPS65023_REGULATOR_DCDX("DCDC1", TPS65023_DCDC_1, "dcdc1",
			   tps65023_dcdc_ops,
			   ARRAY_SIZE(VCORE_VSEL_table),
			   TPS65023_REG_DEF_CORE,
			   ARRAY_SIZE(VCORE_VSEL_table) - 1,
			   1 << (TPS65023_NUM_REGULATOR - 0),
			   TPS65023_REG_CON_CTRL2, TPS65023_REG_CTRL2_GO,
			   VCORE_VSEL_table),
	TPS65023_REGULATOR_DCDX("DCDC2", TPS65023_DCDC_2, "dcdc2",
			   tps65023_dcdc_ops,
			   ARRAY_SIZE(DCDC_FIXED_3300000_VSEL_table),
			   TPS65023_REG_DEF_CORE,
			   ARRAY_SIZE(DCDC_FIXED_3300000_VSEL_table) - 1,
			   1 << (TPS65023_NUM_REGULATOR - 1),
			   TPS65023_REG_CON_CTRL2, TPS65023_REG_CTRL2_GO,
			   DCDC_FIXED_3300000_VSEL_table),
	TPS65023_REGULATOR_DCDX("DCDC3", TPS65023_DCDC_3, "dcdc3",
			   tps65023_dcdc_ops,
			   ARRAY_SIZE(DCDC_FIXED_1800000_VSEL_table),
			   TPS65023_REG_DEF_CORE,
			   ARRAY_SIZE(DCDC_FIXED_1800000_VSEL_table) - 1,
			   1 << (TPS65023_NUM_REGULATOR - 2),
			   TPS65023_REG_CON_CTRL2, TPS65023_REG_CTRL2_GO,
			   DCDC_FIXED_1800000_VSEL_table),
	TPS65023_REGULATOR("LDO1", TPS65023_LDO_1, "ldo1",
			   tps65023_ldo_ops,
			   ARRAY_SIZE(TPS65023_LDO1_VSEL_table),
			   TPS65023_REG_LDO_CTRL,
			   0x07, 1 << 1, TPS65023_LDO1_VSEL_table),
	TPS65023_REGULATOR("LDO2", TPS65023_LDO_2, "ldo2",
			   tps65023_ldo_ops,
			   ARRAY_SIZE(TPS65023_LDO2_VSEL_table),
			   TPS65023_REG_LDO_CTRL,
			   0x70, 1 << 2, TPS65023_LDO2_VSEL_table),
};

static int tps_65023_probe(struct i2c_client *client,
				     const struct i2c_device_id *id)
{
	const struct tps_driver_data *drv_data = (void *)id->driver_data;
	struct regulator_config config = { };
	struct regulator_init_data *init_data;
	struct regulator_dev *rdev;
	struct tps_pmic *tps;
	int i;
	int error;

	/**
	 * init_data points to array of regulator_init structures
	 * coming from the board-evm file.
	 */
	init_data = dev_get_platdata(&client->dev);

	tps = devm_kzalloc(&client->dev, sizeof(*tps), GFP_KERNEL);
	if (!tps)
		return -ENOMEM;

	tps->regmap = devm_regmap_init_i2c(client, &tps65023_regmap_config);
	if (IS_ERR(tps->regmap)) {
		error = PTR_ERR(tps->regmap);
		dev_err(&client->dev, "Failed to allocate register map: %d\n",
			error);
		return error;
	}

	/* common for all regulators */
	tps->core_regulator = drv_data->core_regulator;

	for (i = 0; i < TPS65023_NUM_REGULATOR; i++) {
		/* Register the regulators */
		config.dev = &client->dev;
		if (init_data)
			config.init_data = &init_data[i];

		config.driver_data = tps;
		config.regmap = tps->regmap;

		rdev = devm_regulator_register(&client->dev,
					       &drv_data->regulators[i],
					       &config);
		if (IS_ERR(rdev)) {
			dev_err(&client->dev, "failed to register %s regulator\n",
				drv_data->regulators[i].name);
			return PTR_ERR(rdev);
		}
		/* Save regulator for cleanup */
		tps->rdev[i] = rdev;
	}

	i2c_set_clientdata(client, tps);

	/* Enable setting output voltage by I2C */
	regmap_update_bits(tps->regmap, TPS65023_REG_CON_CTRL2,
					TPS65023_REG_CTRL2_CORE_ADJ,
					TPS65023_REG_CTRL2_CORE_ADJ);

	return 0;
}

static struct tps_driver_data tps65020_drv_data = {
	.id = TPS65020,
	.regulators = regulators_65020,
	.core_regulator = TPS65023_DCDC_3,
};

static struct tps_driver_data tps65021_drv_data = {
	.id = TPS65021,
	.regulators = regulators_65021,
	.core_regulator = TPS65023_DCDC_3,
};

static struct tps_driver_data tps65023_drv_data = {
	.id = TPS65023,
	.regulators = regulators_65023,
	.core_regulator = TPS65023_DCDC_1,
};

static const struct i2c_device_id tps_65023_id[] = {
	{.name = "tps65023",
	.driver_data = (unsigned long) &tps65023_drv_data},
	{.name = "tps65021",
	.driver_data = (unsigned long) &tps65021_drv_data,},
	{.name = "tps65020",
	.driver_data = (unsigned long) &tps65020_drv_data},
	{ },
};

MODULE_DEVICE_TABLE(i2c, tps_65023_id);

#if defined(CONFIG_OF)
static const struct of_device_id tps6502x_of_match[] = {
	 { .compatible = "ti,tps65023", .data = (void *)&tps65023_drv_data},
	 { .compatible = "ti,tps65021", .data = (void *)&tps65021_drv_data},
	 { .compatible = "ti,tps65020", .data = (void *)&tps65020_drv_data},
	{},
};
MODULE_DEVICE_TABLE(of, tps6502x_of_match);
#endif

static struct i2c_driver tps_65023_i2c_driver = {
	.driver = {
		.name = "tps65023",
		.of_match_table = of_match_ptr(tps6502x_of_match),
	},
	.probe = tps_65023_probe,
	.id_table = tps_65023_id,
};

static int __init tps_65023_init(void)
{
	return i2c_add_driver(&tps_65023_i2c_driver);
}
subsys_initcall(tps_65023_init);

static void __exit tps_65023_cleanup(void)
{
	i2c_del_driver(&tps_65023_i2c_driver);
}
module_exit(tps_65023_cleanup);

MODULE_AUTHOR("Texas Instruments");
MODULE_DESCRIPTION("TPS65023 voltage regulator driver");
MODULE_LICENSE("GPL v2");
