/*
 * This file is part of STM32 DFSDM mfd driver
 *
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Author(s): Arnaud Pouliquen <arnaud.pouliquen@st.com> for STMicroelectronics.
 *
 * License terms: GPL V2.0.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "stm32-dfsdm.h"

struct stm32_dfsdm_dev_data {
	unsigned int num_filters;
	unsigned int num_channels;
	const struct regmap_config *regmap_cfg;
};

#define STM32H7_DFSDM_NUM_FILTERS	4
#define STM32H7_DFSDM_NUM_CHANNELS	8

static bool stm32_dfsdm_volatile_reg(struct device *dev, unsigned int reg)
{
	if (reg < DFSDM_FILTER_BASE_ADR)
		return false;

	/*
	 * Mask is done on register to avoid to list registers of all them
	 * filter instances.
	 */
	switch (reg & DFSDM_FILTER_REG_MASK) {
	case DFSDM_CR1(0) & DFSDM_FILTER_REG_MASK:
	case DFSDM_ISR(0) & DFSDM_FILTER_REG_MASK:
	case DFSDM_JDATAR(0) & DFSDM_FILTER_REG_MASK:
	case DFSDM_RDATAR(0) & DFSDM_FILTER_REG_MASK:
		return true;
	}

	return false;
}

static const struct regmap_config stm32h7_dfsdm_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = sizeof(u32),
	.max_register = 0x2B8,
	.volatile_reg = stm32_dfsdm_volatile_reg,
	.fast_io = true,
};

static const struct stm32_dfsdm_dev_data stm32h7_dfsdm_data = {
	.num_filters = STM32H7_DFSDM_NUM_FILTERS,
	.num_channels = STM32H7_DFSDM_NUM_CHANNELS,
	.regmap_cfg = &stm32h7_dfsdm_regmap_cfg,
};

/**
 * struct dfsdm_priv -  stm32 dfsdm  private data
 * @pdev:		platform device
 * @stm32_dfsdm:	common data exported for all instances
 * @regmap:		register map of the device;
 * @clkout_div:		SPI clkout divider value.
 * @n_active_ch:	atomic active channel counter.
 */
struct dfsdm_priv {
	struct platform_device *pdev;

	struct stm32_dfsdm dfsdm;
	struct regmap *regmap;

	unsigned int clkout_div;
	atomic_t n_active_ch;
};

/**
 * stm32_dfsdm_start_dfsdm - start global dfsdm IP interface.
 *
 * Enable interface if n_active_ch is not null.
 * @dfsdm: Handle used to retrieve dfsdm context.
 */
int stm32_dfsdm_start_dfsdm(struct stm32_dfsdm *dfsdm)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);
	int ret;
	int div = priv->clkout_div;

	if (atomic_inc_return(&priv->n_active_ch) == 1) {
		/* TODO: enable clocks */

		/* Output the SPI CLKOUT (if clkout_div == 0 clok if OFF) */
		ret = regmap_update_bits(priv->regmap, DFSDM_CHCFGR1(0),
					 DFSDM_CHCFGR1_CKOUTDIV_MASK,
					 DFSDM_CHCFGR1_CKOUTDIV(div));
		if (ret < 0)
			return ret;

		/* Global enable of DFSDM interface */
		ret = regmap_update_bits(priv->regmap, DFSDM_CHCFGR1(0),
					 DFSDM_CHCFGR1_DFSDMEN_MASK,
					 DFSDM_CHCFGR1_DFSDMEN(1));
		if (ret < 0)
			return ret;
	}

	dev_dbg(&priv->pdev->dev, "%s: n_active_ch %d\n", __func__,
		atomic_read(&priv->n_active_ch));

	return 0;
}

/**
 * stm32_dfsdm_stop_dfsdm - stop global DFSDM IP interface.
 *
 * Disable interface if n_active_ch is null
 * @dfsdm: Handle used to retrieve dfsdm context.
 */
int stm32_dfsdm_stop_dfsdm(struct stm32_dfsdm *dfsdm)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);
	int ret;

	if (atomic_dec_and_test(&priv->n_active_ch)) {
		/* Global disable of DFSDM interface */
		ret = regmap_update_bits(priv->regmap, DFSDM_CHCFGR1(0),
					 DFSDM_CHCFGR1_DFSDMEN_MASK,
					 DFSDM_CHCFGR1_DFSDMEN(0));
		if (ret < 0)
			return ret;

		/* Stop SPI CLKOUT */
		ret = regmap_update_bits(priv->regmap, DFSDM_CHCFGR1(0),
					 DFSDM_CHCFGR1_CKOUTDIV_MASK,
					 DFSDM_CHCFGR1_CKOUTDIV(0));
		if (ret < 0)
			return ret;

		/* TODO: disable clocks */
	}
	dev_dbg(&priv->pdev->dev, "%s: n_active_ch %d\n", __func__,
		atomic_read(&priv->n_active_ch));

	return 0;
}

static int stm32_dfsdm_parse_of(struct platform_device *pdev,
				struct dfsdm_priv *priv)
{
	struct device_node *node = pdev->dev.of_node;
	struct resource *res;

	if (!node)
		return -EINVAL;

	/* Get resources */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "Failed to get memory resource\n");
		return -ENODEV;
	}
	priv->dfsdm.phys_base = res->start;
	priv->dfsdm.base = devm_ioremap_resource(&pdev->dev, res);

	return 0;
};

static const struct of_device_id stm32_dfsdm_of_match[] = {
	{
		.compatible = "st,stm32h7-dfsdm",
		.data = &stm32h7_dfsdm_data,
	},
	{}
};
MODULE_DEVICE_TABLE(of, stm32_dfsdm_of_match);

static int stm32_dfsdm_remove(struct platform_device *pdev)
{
	of_platform_depopulate(&pdev->dev);

	return 0;
}

static int stm32_dfsdm_probe(struct platform_device *pdev)
{
	struct dfsdm_priv *priv;
	struct device_node *pnode = pdev->dev.of_node;
	const struct of_device_id *of_id;
	const struct stm32_dfsdm_dev_data *dev_data;
	struct stm32_dfsdm *dfsdm;
	int ret, i;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pdev = pdev;

	/* Populate data structure depending on compatibility */
	of_id = of_match_node(stm32_dfsdm_of_match, pnode);
	if (!of_id->data) {
		dev_err(&pdev->dev, "Data associated to device is missing\n");
		return -EINVAL;
	}

	dev_data = (const struct stm32_dfsdm_dev_data *)of_id->data;
	dfsdm = &priv->dfsdm;
	dfsdm->fl_list = devm_kzalloc(&pdev->dev, sizeof(*dfsdm->fl_list),
				      GFP_KERNEL);
	if (!dfsdm->fl_list)
		return -ENOMEM;

	dfsdm->num_fls = dev_data->num_filters;
	dfsdm->ch_list = devm_kzalloc(&pdev->dev, sizeof(*dfsdm->ch_list),
				      GFP_KERNEL);
	if (!dfsdm->ch_list)
		return -ENOMEM;
	dfsdm->num_chs = dev_data->num_channels;
		dev_err(&pdev->dev, "%s: dfsdm->num_ch: %d\n",
			__func__, dfsdm->num_chs);

	ret = stm32_dfsdm_parse_of(pdev, priv);
	if (ret < 0)
		return ret;

	priv->regmap = devm_regmap_init_mmio(&pdev->dev, dfsdm->base,
					    &stm32h7_dfsdm_regmap_cfg);
	if (IS_ERR(priv->regmap)) {
		ret = PTR_ERR(priv->regmap);
		dev_err(&pdev->dev, "%s: Failed to allocate regmap: %d\n",
			__func__, ret);
		return ret;
	}

	for (i = 0; i < STM32H7_DFSDM_NUM_FILTERS; i++) {
		struct stm32_dfsdm_filter *fl = &dfsdm->fl_list[i];

		fl->id = i;
	}

	platform_set_drvdata(pdev, dfsdm);

	return of_platform_populate(pnode, NULL, NULL, &pdev->dev);
}

static struct platform_driver stm32_dfsdm_driver = {
	.probe = stm32_dfsdm_probe,
	.remove = stm32_dfsdm_remove,
	.driver = {
		.name = "stm32-dfsdm",
		.of_match_table = stm32_dfsdm_of_match,
	},
};

module_platform_driver(stm32_dfsdm_driver);

MODULE_AUTHOR("Arnaud Pouliquen <arnaud.pouliquen@st.com>");
MODULE_DESCRIPTION("STMicroelectronics STM32 dfsdm driver");
MODULE_LICENSE("GPL v2");
