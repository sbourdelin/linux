/*
 * Device driver for regulators in hi655x IC
 *
 * Copyright (c) 2016 Hisilicon.
 *
 * Chen Feng <puck.chen@hisilicon.com>
 * Fei  Wang <w.f@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/mfd/hi655x-pmic.h>

struct hi655x_regulator {
	unsigned int disable_reg;
	unsigned int status_reg;
	unsigned int ctrl_mask;
	struct regulator_desc rdesc;
};

/*LDO 2 & LDO 14*/
static const unsigned int ldo2_voltages[] = {
	2500000, 2600000, 2700000, 2800000,
	2900000, 3000000, 3100000, 3200000,
};

/*LDO7 & LDO10*/
static const unsigned int ldo7_voltages[] = {
	1800000, 1850000, 2850000, 2900000,
	3000000, 3100000, 3200000, 3300000,
};

/*LDO13 & LDO15*/
static const unsigned int ldo13_voltages[] = {
	1600000, 1650000, 1700000, 1750000,
	1800000, 1850000, 1900000, 1950000,
};

static const unsigned int ldo17_voltages[] = {
	2500000, 2600000, 2700000, 2800000,
	2900000, 3000000, 3100000, 3200000,
};

static const unsigned int ldo19_voltages[] = {
	1800000, 1850000, 1900000, 1750000,
	2800000, 2850000, 2900000, 3000000,
};

static const unsigned int ldo21_voltages[] = {
	1650000, 1700000, 1750000, 1800000,
	1850000, 1900000, 1950000, 2000000,
};

static const unsigned int ldo22_voltages[] = {
	 900000, 1000000, 1050000, 1100000,
	1150000, 1175000, 1185000, 1200000,
};

enum hi655x_regulator_id {
	hi655x_ldo0,
	hi655x_ldo1,
	hi655x_ldo2,
	hi655x_ldo3,
	hi655x_ldo4,
	hi655x_ldo5,
	hi655x_ldo6,
	hi655x_ldo7,
	hi655x_ldo8,
	hi655x_ldo9,
	hi655x_ldo10,
	hi655x_ldo11,
	hi655x_ldo12,
	hi655x_ldo13,
	hi655x_ldo14,
	hi655x_ldo15,
	hi655x_ldo16,
	hi655x_ldo17,
	hi655x_ldo18,
	hi655x_ldo19,
	hi655x_ldo20,
	hi655x_ldo21,
	hi655x_ldo22,
};

static int hi655x_is_enabled(struct regulator_dev *rdev)
{
	unsigned int value = 0;

	struct hi655x_regulator *regulator = rdev_get_drvdata(rdev);

	regmap_read(rdev->regmap, regulator->status_reg, &value);
	return (value & BIT(regulator->ctrl_mask));
}

static int hi655x_disable(struct regulator_dev *rdev)
{
	int ret = 0;

	struct hi655x_regulator *regulator = rdev_get_drvdata(rdev);

	ret = regmap_write(rdev->regmap, regulator->disable_reg,
			   BIT(regulator->ctrl_mask));
	return ret;
}

static struct regulator_ops hi655x_regulator_ops = {
	.enable = regulator_enable_regmap,
	.disable = hi655x_disable,
	.is_enabled = hi655x_is_enabled,
	.list_voltage = regulator_list_voltage_table,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

#define HI655X_LDO(_id, vreg, vmask, ereg, dreg,                        \
		   sreg, cmask, vtable) {                               \
		.rdesc = {                                              \
			.name           = #_id,                         \
			.ops            = &hi655x_regulator_ops,        \
			.type           = REGULATOR_VOLTAGE,            \
			.id             = hi655x_##_id,                 \
			.owner          = THIS_MODULE,                  \
			.n_voltages     = ARRAY_SIZE(vtable),           \
			.volt_table     = vtable,                       \
			.vsel_reg       = HI655X_BUS_ADDR(vreg),        \
			.vsel_mask      = vmask,                        \
			.enable_reg     = HI655X_BUS_ADDR(ereg),        \
			.enable_mask    = cmask,                        \
		},                                                      \
		.disable_reg = HI655X_BUS_ADDR(dreg),                   \
		.status_reg = HI655X_BUS_ADDR(sreg),                    \
		.ctrl_mask = cmask,                                     \
	}

static struct hi655x_regulator regulators[] = {
	HI655X_LDO(ldo2, 0x72, 0x07, 0x29, 0x2a, 0x2b, 0x01, ldo2_voltages),
	HI655X_LDO(ldo7, 0x78, 0x07, 0x29, 0x2a, 0x2b, 0x06, ldo7_voltages),
	HI655X_LDO(ldo10, 0x78, 0x07, 0x29, 0x2a, 0x2b, 0x01, ldo7_voltages),
	HI655X_LDO(ldo13, 0x7e, 0x07, 0x2c, 0x2d, 0x2e, 0x04, ldo13_voltages),
	HI655X_LDO(ldo14, 0x7f, 0x07, 0x2c, 0x2d, 0x2e, 0x05, ldo2_voltages),
	HI655X_LDO(ldo15, 0x80, 0x07, 0x2c, 0x2d, 0x2e, 0x06, ldo13_voltages),
	HI655X_LDO(ldo17, 0x82, 0x07, 0x2f, 0x30, 0x31, 0x00, ldo17_voltages),
	HI655X_LDO(ldo19, 0x84, 0x07, 0x2f, 0x30, 0x31, 0x02, ldo19_voltages),
	HI655X_LDO(ldo21, 0x86, 0x07, 0x2f, 0x30, 0x31, 0x04, ldo21_voltages),
	HI655X_LDO(ldo22, 0x87, 0x07, 0x2f, 0x30, 0x31, 0x05, ldo22_voltages),
};

static const struct of_device_id of_hi655x_regulator_match_tbl[] = {
	{
		.compatible = "hisilicon,hi655x-regulator",
	},
};
MODULE_DEVICE_TABLE(of, of_hi655x_regulator_match_tbl);

static int hi655x_regulator_probe(struct platform_device *pdev)
{
	unsigned int i;
	struct regulator_dev *rdev;
	struct hi655x_pmic *pmic;
	struct hi655x_regulator *regulator;
	struct regulator_init_data *init_data;
	struct regulator_config config = { };
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;

	pmic = dev_get_drvdata(dev->parent);
	if (!pmic) {
		dev_err(dev, "no pmic in the regulator parent node\n");
		return -ENODEV;
	}

	regulator = devm_kzalloc(dev, sizeof(*regulator), GFP_KERNEL);
	if (!regulator)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(regulators); i++) {
		if (!of_node_cmp(np->name, regulators[i].rdesc.name))
			break;
	}

	if (i == ARRAY_SIZE(regulators)) {
		dev_err(dev, "error regulator %s int dts\n", np->name);
		return -ENODEV;
	}

	regulator = &regulators[i];
	init_data = of_get_regulator_init_data(dev, np,
					       &regulator->rdesc);
	if (!init_data)
		return -EINVAL;

	config.dev = dev;
	config.init_data = init_data;
	config.driver_data = regulator;
	config.regmap = pmic->regmap;
	config.of_node = pdev->dev.of_node;

	rdev = devm_regulator_register(dev, &regulator->rdesc,
				       &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);

	platform_set_drvdata(pdev, regulator);

	return 0;
}

static struct platform_driver hi655x_regulator_driver = {
	.driver = {
		.name	= "hi655x-regulator",
		.of_match_table = of_hi655x_regulator_match_tbl,
	},
	.probe	= hi655x_regulator_probe,
};
module_platform_driver(hi655x_regulator_driver);

MODULE_AUTHOR("Chen Feng <puck.chen@hisilicon.com>");
MODULE_DESCRIPTION("Hisi hi655x PMIC driver");
MODULE_LICENSE("GPL v2");
