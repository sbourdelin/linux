/*
 * Copyright (c) 2016, Linaro Limited. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define	SPM_REG_STS_1			0x10
#define	SPM_REG_VCTL			0x14
#define	SPM_REG_PMIC_DATA_0		0x28
#define	SPM_REG_PMIC_DATA_1		0x2c
#define	SPM_REG_RST			0x30

struct saw2_vreg {
	struct device		*dev;
	struct regmap		*regmap;
	struct regulator_desc	rdesc;
	struct regulator_dev	*rdev;
	unsigned int		selector;
};

struct spm_vlevel_data {
	struct saw2_vreg *vreg;
	unsigned int selector;
};

static int saw2_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct saw2_vreg *vreg = rdev_get_drvdata(rdev);

	return regulator_list_voltage_linear_range(rdev, vreg->selector);
}

static void smp_set_vdd(void *data)
{
	struct spm_vlevel_data *vdata = (struct spm_vlevel_data *)data;
	struct saw2_vreg *vreg = vdata->vreg;
	unsigned long sel = vdata->selector;
	u32 val, new_val;
	u32 vctl, data0, data1;
	int timeout_us = 50;

	if (vreg->selector == sel)
		return;

	regmap_read(vreg->regmap, SPM_REG_VCTL, &vctl);
	regmap_read(vreg->regmap, SPM_REG_PMIC_DATA_0, &data0);
	regmap_read(vreg->regmap, SPM_REG_PMIC_DATA_1, &data1);

	/* select the band */
	val = 0x80 | sel;

	vctl &= ~0xff;
	vctl |= val;

	data0 &= ~0xff;
	data0 |= val;

	data1 &= ~0x3f;
	data1 |= val & 0x3f;
	data1 &= ~0x3f0000;
	data1 |= ((val & 0x3f) << 16);

	regmap_write(vreg->regmap, SPM_REG_RST, 1);
	regmap_write(vreg->regmap, SPM_REG_VCTL, vctl);
	regmap_write(vreg->regmap, SPM_REG_PMIC_DATA_0, data0);
	regmap_write(vreg->regmap, SPM_REG_PMIC_DATA_1, data1);

	do {
		regmap_read(vreg->regmap, SPM_REG_STS_1, &new_val);
		new_val &= 0xff;
		if (new_val == val)
			break;
		udelay(1);
	} while (--timeout_us);

	if (!timeout_us) {
		pr_info("%s: Voltage not changed: %#x\n", __func__, new_val);
		return;
	}

	if (sel > vreg->selector) {
		/* PMIC internal slew rate is 1250 uV/us */
		udelay((sel - vreg->selector) * 10);
	}

	vreg->selector = sel;
}

static int saw2_regulator_set_voltage_sel(struct regulator_dev *rdev,
					  unsigned selector)
{
	struct saw2_vreg *vreg = rdev_get_drvdata(rdev);
	struct spm_vlevel_data data;
	int cpu = rdev_get_id(rdev);

	data.vreg = vreg;
	data.selector = selector;

	return smp_call_function_single(cpu, smp_set_vdd, &data, true);
}

static struct regulator_ops saw2_regulator_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.set_voltage_sel = saw2_regulator_set_voltage_sel,
	.get_voltage = saw2_regulator_get_voltage,
};

static struct regulator_desc saw2_regulator = {
	.owner = THIS_MODULE,
	.type = REGULATOR_VOLTAGE,
	.ops  = &saw2_regulator_ops,
	.linear_ranges = (struct regulator_linear_range[]) {
		REGULATOR_LINEAR_RANGE(700000, 0, 56, 12500),
	},
	.n_linear_ranges = 1,
	.n_voltages = 57,
};

static struct saw2_vreg *saw2_get_drv(struct platform_device *pdev,
				      int *vreg_cpu)
{
	struct saw2_vreg *vreg = NULL;
	struct device_node *cpu_node, *saw_node;
	int cpu;
	bool found;

	for_each_possible_cpu(cpu) {
		cpu_node = of_cpu_device_node_get(cpu);
		if (!cpu_node)
			continue;
		saw_node = of_parse_phandle(cpu_node, "qcom,saw", 0);
		found = (saw_node == pdev->dev.of_node->parent);
		of_node_put(saw_node);
		of_node_put(cpu_node);
		if (found)
			break;
	}

	if (found) {
		vreg = devm_kzalloc(&pdev->dev, sizeof(*vreg), GFP_KERNEL);
		if (vreg)
			*vreg_cpu = cpu;
	}

	return vreg;
}

static const struct of_device_id qcom_saw2_regulator_match[] = {
	{ .compatible = "qcom,apq8064-saw2-v1.1-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_saw2_regulator_match);

static int qcom_saw2_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *saw2_np;
	struct saw2_vreg *vreg;
	struct regulator_config config = { };
	int ret = 0, cpu = 0;

	vreg = saw2_get_drv(pdev, &cpu);
	if (!vreg)
		return -EINVAL;

	saw2_np = of_get_parent(np);
	if (!saw2_np)
		return -ENODEV;

	vreg->regmap = syscon_node_to_regmap(saw2_np);
	of_node_put(saw2_np);
	if (IS_ERR(config.regmap))
		return PTR_ERR(config.regmap);

	config.regmap = vreg->regmap;
	config.dev = &pdev->dev;
	config.of_node = np;
	config.driver_data = vreg;

	vreg->rdesc = saw2_regulator;
	vreg->rdesc.id = cpu;
	vreg->rdesc.name = of_get_property(np, "regulator-name", NULL);
	config.init_data = of_get_regulator_init_data(&pdev->dev,
						      pdev->dev.of_node,
						      &vreg->rdesc);

	vreg->rdev = devm_regulator_register(&pdev->dev, &vreg->rdesc, &config);
	if (IS_ERR(vreg->rdev)) {
		ret = PTR_ERR(vreg->rdev);
		dev_err(dev, "error registering SAW2 regulator: %d\n", ret);
		return ret;
	}

	return 0;
}

static struct platform_driver qcom_saw2_regulator_driver = {
	.driver = {
		.name = "qcom-saw2-regulator",
		.of_match_table = qcom_saw2_regulator_match,
	},
	.probe = qcom_saw2_regulator_probe,
};

module_platform_driver(qcom_saw2_regulator_driver);

MODULE_ALIAS("platform:qcom-saw2-regulator");
MODULE_DESCRIPTION("Qualcomm SAW2 regulator driver");
MODULE_AUTHOR("Georgi Djakov <georgi.djakov@linaro.org>");
MODULE_LICENSE("GPL v2");
