/*
 * Device driver for regulators in hi655x IC
 *
 * Copyright (c) 2015 Hisilicon.
 *
 * Fei Wang <w.f@huawei.com>
 * Chen Feng <puck.chen@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/slab.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/regulator/hi655x-regulator.h>
#include <linux/mfd/hi655x-pmic.h>
#include <linux/regmap.h>
#include <linux/bitops.h>

static int hi655x_is_enabled(struct regulator_dev *rdev)
{
	unsigned int value = 0;

	struct hi655x_regulator *regulator = rdev_get_drvdata(rdev);
	struct hi655x_regulator_ctrl_regs *ctrl_regs = &regulator->ctrl_regs;

	regmap_read(rdev->regmap, ctrl_regs->status_reg, &value);
	return (value & BIT(regulator->ctrl_mask));
}

static int hi655x_enable(struct regulator_dev *rdev)
{
	int ret = 0;
	struct hi655x_regulator *regulator = rdev_get_drvdata(rdev);
	struct hi655x_regulator_ctrl_regs *ctrl_regs = &regulator->ctrl_regs;

	ret = regmap_update_bits(rdev->regmap, ctrl_regs->enable_reg,
				 regulator->ctrl_mask, regulator->ctrl_mask);
	return ret;
}

static int hi655x_disable(struct regulator_dev *rdev)
{
	int ret = 0;
	struct hi655x_regulator *regulator = rdev_get_drvdata(rdev);

	if (!regulator) {
		pr_err("get driver data error!\n");
		return -ENODEV;
	}
	struct hi655x_regulator_ctrl_regs *ctrl_regs = &regulator->ctrl_regs;

	ret = regmap_update_bits(rdev->regmap, ctrl_regs->disable_reg,
				 regulator->ctrl_mask, regulator->ctrl_mask);
	return ret;
}

static int hi655x_get_voltage(struct regulator_dev *rdev)
{
	unsigned int value = 0;
	struct hi655x_regulator *regulator = rdev_get_drvdata(rdev);

	if (!regulator) {
		pr_err("get driver data error!\n");
		return -ENODEV;
	}
	struct hi655x_regulator_vset_regs *vset_regs = &regulator->vset_regs;

	regmap_read(rdev->regmap, vset_regs->vset_reg, &value);

	return regulator->vset_table[value];
}

static int hi655x_set_voltage(struct regulator_dev *rdev,
			      int min_uV, int max_uV, unsigned *selector)
{
	int i = 0;
	int ret = 0;
	int vol = 0;
	struct hi655x_regulator *regulator = rdev_get_drvdata(rdev);

	if (!regulator) {
		pr_err("get driver data error!\n");
		return -ENODEV;
	}

	struct hi655x_regulator_vset_regs *vset_regs = &regulator->vset_regs;

	/**
	 * search the matched vol and get its index
	 */
	for (i = 0; i < regulator->vol_numb; i++) {
		vol = regulator->vset_table[i];
		if ((vol >= min_uV) && (vol <= max_uV))
			break;
	}

	if (i == regulator->vol_numb)
		return -1;

	regmap_update_bits(rdev->regmap, vset_regs->vset_reg,
			   regulator->vset_mask, i);
	*selector = i;

	return ret;
}

static unsigned int hi655x_map_mode(unsigned int mode)
{
	/* hi655x pmic on hi6220 SoC only support normal mode */
	if (mode == REGULATOR_MODE_NORMAL)
		return REGULATOR_MODE_NORMAL;
	else
		return -EINVAL;
}

static int hi655x_set_mode(struct regulator_dev *rdev,
			   unsigned int mode)

{
	if (mode == REGULATOR_MODE_NORMAL)
		return 0;
	else
		return -EINVAL;
}

static struct regulator_ops hi655x_regulator_ops = {
	.is_enabled = hi655x_is_enabled,
	.enable = hi655x_enable,
	.disable = hi655x_disable,
	.list_voltage = regulator_list_voltage_table,
	.get_voltage = hi655x_get_voltage,
	.set_voltage = hi655x_set_voltage,
	.set_mode = hi655x_set_mode,
};

static const struct of_device_id of_hi655x_regulator_match_tbl[] = {
	{
		.compatible = "hisilicon,hi655x-regulator-pmic",
	},
};
MODULE_DEVICE_TABLE(of, of_hi655x_regulator_match_tbl);

/**
 * get the hi655x specific data from dt node.
 */
static void of_get_hi655x_ctr(struct hi655x_regulator *regulator,
			      struct device *dev, struct device_node *np)
{
	unsigned int *vset_table = NULL;

	of_property_read_u32_array(np, "regulator-ctrl-regs",
				   (u32 *)&regulator->ctrl_regs, 0x3);
	of_property_read_u32(np, "regulator-ctrl-mask", &regulator->ctrl_mask);
	of_property_read_u32(np, "regulator-vset-regs",
			     (u32 *)&regulator->vset_regs);
	of_property_read_u32(np, "regulator-vset-mask", &regulator->vset_mask);
	of_property_read_u32(np, "regulator-n-vol", &regulator->vol_numb);
	of_property_read_u32(np, "regulator-off-on-delay",
			     &regulator->rdesc.off_on_delay);

	vset_table = devm_kzalloc(dev, regulator->vol_numb * sizeof(int),
				  GFP_KERNEL);

	of_property_read_u32_array(np, "regulator-vset-table",
				   vset_table,
				   regulator->vol_numb);
	regulator->vset_table = vset_table;
	regulator->rdesc.volt_table = vset_table;
	regulator->rdesc.n_voltages = regulator->vol_numb;
}

static int hi655x_regulator_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct hi655x_regulator *regulator;
	struct hi655x_pmic *pmic;
	struct regulator_init_data *init_data;
	struct regulator_config config = { };
	struct device_node *np = pdev->dev.of_node;

	pmic = dev_get_drvdata(pdev->dev.parent);
	if (!pmic) {
		pr_err("no pmic in the regulator parent node\n");
		return -ENODEV;
	}

	regulator = devm_kzalloc(&pdev->dev, sizeof(*regulator), GFP_KERNEL);
	if (!regulator)
		return -ENOMEM;
	of_get_hi655x_ctr(regulator, &pdev->dev, np);

	regulator->rdesc.name = dev_name(&pdev->dev);
	regulator->rdesc.type = REGULATOR_VOLTAGE;
	regulator->rdesc.owner = THIS_MODULE;
	regulator->rdesc.of_map_mode = hi655x_map_mode;
	regulator->rdesc.ops = &hi655x_regulator_ops;
	init_data = of_get_regulator_init_data(&pdev->dev, pdev->dev.of_node,
					       &regulator->rdesc);
	if (!init_data) {
		pr_err("get init data from dts error!\n");
		return -EINVAL;
	}
	config.dev = &pdev->dev;
	config.init_data = init_data;
	config.driver_data = regulator;
	config.regmap = pmic->regmap;

	regulator->regdev = devm_regulator_register(&pdev->dev,
						    &regulator->rdesc,
						    &config);
	if (IS_ERR(regulator->regdev)) {
		pr_err("register regulator to system error!\n");
		return PTR_ERR(regulator->regdev);
	}

	platform_set_drvdata(pdev, regulator);
	return 0;
}

static struct platform_driver hi655x_regulator_driver = {
	.driver = {
		.name	= "hi655x_regulator",
		.of_match_table = of_hi655x_regulator_match_tbl,
	},
	.probe	= hi655x_regulator_probe,
};
module_platform_driver(hi655x_regulator_driver);

MODULE_AUTHOR("Chen Feng <puck.chen@hisilicon.com>");
MODULE_DESCRIPTION("Hisi hi655x regulator driver");
MODULE_LICENSE("GPL v2");
