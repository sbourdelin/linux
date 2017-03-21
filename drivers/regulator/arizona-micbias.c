/*
 * arizona-micbias.c  --  Microphone bias supplies for Arizona devices
 *
 * Copyright 2017 Cirrus Logic Inc.
 *
 * Author: Charles Keepax <ckeepax@opensource.wolfsonmicro.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>
#include <sound/soc.h>

#include <linux/mfd/arizona/core.h>
#include <linux/mfd/arizona/pdata.h>
#include <linux/mfd/arizona/registers.h>

#define ARIZONA_MICBIAS_MAX_NAME 10
#define ARIZONA_MICBIAS_MAX_SELECTOR 0xD

struct arizona_micbias_priv {
	int id;
	char name[ARIZONA_MICBIAS_MAX_NAME];

	struct regulator_dev *regulator;
	struct arizona *arizona;

	struct regulator_consumer_supply supply;
	struct regulator_init_data *init_data;
	struct regulator_desc desc;
};

static const struct regulator_ops arizona_micbias_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_bypass = regulator_get_bypass_regmap,
	.set_bypass = regulator_set_bypass_regmap,
	.set_soft_start = regulator_set_soft_start_regmap,
	.set_pull_down = regulator_set_pull_down_regmap,
};

static const struct regulator_desc arizona_micbias_desc_tmpl = {
	.supply_name = "MICVDD",
	.type = REGULATOR_VOLTAGE,
	.ops = &arizona_micbias_ops,

	.min_uV = 1500000,
	.uV_step = 100000,
	.n_voltages = ARIZONA_MICBIAS_MAX_SELECTOR + 1,

	.vsel_reg = ARIZONA_MIC_BIAS_CTRL_1,
	.vsel_mask = ARIZONA_MICB1_LVL_MASK,
	.enable_reg = ARIZONA_MIC_BIAS_CTRL_1,
	.enable_mask = ARIZONA_MICB1_ENA,
	.bypass_reg = ARIZONA_MIC_BIAS_CTRL_1,
	.bypass_mask = ARIZONA_MICB1_BYPASS,
	.soft_start_reg = ARIZONA_MIC_BIAS_CTRL_1,
	.soft_start_mask = ARIZONA_MICB1_RATE,
	.pull_down_reg = ARIZONA_MIC_BIAS_CTRL_1,
	.pull_down_mask = ARIZONA_MICB1_DISCH,

	.owner = THIS_MODULE,
};

static const struct regulator_init_data arizona_micbias_tmpl = {
	.constraints = {
		.valid_ops_mask = REGULATOR_CHANGE_STATUS |
				  REGULATOR_CHANGE_VOLTAGE |
				  REGULATOR_CHANGE_BYPASS,
		.min_uV = 1500000,
		.max_uV = 2800000,
	},
};

static int arizona_micbias_of_get_pdata(struct regulator_config *config)
{
	struct arizona_micbias_priv *micbias = config->driver_data;
	struct arizona *arizona = micbias->arizona;
	struct arizona_micbias *pdata = &arizona->pdata.micbias[micbias->id];
	struct device_node *np;

	np = of_get_child_by_name(arizona->dev->of_node, micbias->name);
	if (np) {
		config->of_node = np;

		micbias->init_data = of_get_regulator_init_data(arizona->dev,
								np,
								&micbias->desc);

		pdata->ext_cap = of_property_read_bool(np, "wlf,ext-cap");
	}

	return 0;
}

static int arizona_micbias_probe(struct platform_device *pdev)
{
	struct arizona *arizona = dev_get_drvdata(pdev->dev.parent);
	int id = pdev->id;
	struct arizona_micbias *pdata = &arizona->pdata.micbias[id];
	struct arizona_micbias_priv *micbias;
	struct regulator_config config = { };
	struct regulation_constraints *constraints;
	unsigned int val;
	int ret;

	micbias = devm_kzalloc(&pdev->dev, sizeof(*micbias), GFP_KERNEL);
	if (micbias == NULL)
		return -ENOMEM;

	micbias->arizona = arizona;
	micbias->id = id;
	snprintf(micbias->name, sizeof(micbias->name), "MICBIAS%d", id + 1);

	micbias->desc = arizona_micbias_desc_tmpl;
	micbias->desc.name = micbias->name;
	micbias->desc.vsel_reg += id;
	micbias->desc.enable_reg += id;
	micbias->desc.bypass_reg += id;
	micbias->desc.soft_start_reg += id;
	micbias->desc.pull_down_reg += id;

	micbias->supply.supply = micbias->name;
	micbias->supply.dev_name = dev_name(arizona->dev);

	config.dev = arizona->dev;
	config.regmap = arizona->regmap;
	config.driver_data = micbias;

	if (IS_ENABLED(CONFIG_OF)) {
		if (!dev_get_platdata(arizona->dev)) {
			ret = arizona_micbias_of_get_pdata(&config);
			if (ret < 0)
				return ret;
		}
	}

	if (!micbias->init_data) {
		micbias->init_data = devm_kmemdup(&pdev->dev,
						  &arizona_micbias_tmpl,
						  sizeof(arizona_micbias_tmpl),
						  GFP_KERNEL);
		if (!micbias->init_data)
			return -ENOMEM;
	}

	micbias->init_data->consumer_supplies = &micbias->supply;
	micbias->init_data->num_consumer_supplies = 1;

	config.init_data = micbias->init_data;
	constraints = &micbias->init_data->constraints;

	if (pdata->mV) {
		constraints->min_uV = pdata->mV * 1000;
		constraints->max_uV = pdata->mV * 1000;
	}

	if (pdata->soft_start)
		constraints->soft_start = true;

	if (pdata->bypass)
		constraints->valid_ops_mask |= REGULATOR_CHANGE_BYPASS;

	if (pdata->discharge)
		constraints->pull_down = true;

	if (pdata->ext_cap)
		val = ARIZONA_MICB1_EXT_CAP;
	else
		val = 0;

	/*
	 * The core expects the regulator to have pull_down (discharge) and
	 * bypass disabled by default so clear those here, whilst we set the
	 * external cap.
	 */
	regmap_update_bits(arizona->regmap, ARIZONA_MIC_BIAS_CTRL_1 + id,
			   ARIZONA_MICB1_EXT_CAP | ARIZONA_MICB1_DISCH |
			   ARIZONA_MICB1_BYPASS, val);

	micbias->regulator = devm_regulator_register(&pdev->dev,
						     &micbias->desc,
						     &config);

	of_node_put(config.of_node);

	if (IS_ERR(micbias->regulator)) {
		ret = PTR_ERR(micbias->regulator);
		dev_err(arizona->dev, "Failed to register %s supply: %d\n",
			micbias->name, ret);
		return ret;
	}

	return 0;
}

static struct platform_driver arizona_micbias_driver = {
	.probe = arizona_micbias_probe,
	.driver		= {
		.name	= "arizona-micbias",
	},
};

module_platform_driver(arizona_micbias_driver);

/* Module information */
MODULE_AUTHOR("Charles Keepax <ckeepax@opensource.wolfsonmicro.com>");
MODULE_DESCRIPTION("Arizona microphone bias supply driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:arizona-micbias");
