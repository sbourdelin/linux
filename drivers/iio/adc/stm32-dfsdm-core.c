/*
 * This file is part the core part STM32 DFSDM driver
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
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <linux/iio/trigger.h>
#include <linux/iio/sysfs.h>

#include "stm32-dfsdm.h"

struct stm32_dfsdm_dev_data {
	unsigned int num_filters;
	unsigned int num_channels;
	const struct regmap_config *regmap_cfg;
};

#define STM32H7_DFSDM_NUM_FILTERS	4
#define STM32H7_DFSDM_NUM_CHANNELS	8

#define DFSDM_MAX_INT_OVERSAMPLING 256

#define DFSDM_MAX_FL_OVERSAMPLING 1024

#define DFSDM_MAX_RES BIT(31)
#define DFSDM_DATA_RES BIT(23)

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

struct dfsdm_priv {
	struct platform_device *pdev; /* platform device*/

	struct stm32_dfsdm dfsdm; /* common data exported for all instances */

	unsigned int spi_clk_out_div; /* SPI clkout divider value */
	atomic_t n_active_ch;	/* number of current active channels */

	/* Clock */
	struct clk *clk; /* DFSDM clock */
	struct clk *aclk; /* audio clock */
};

/**
 * stm32_dfsdm_set_osrs - compute filter parameters.
 *
 * Enable interface if n_active_ch is not null.
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fast: Fast mode enabled or disabled
 * @oversamp: Expected oversampling between filtered sample and SD input stream
 */
int stm32_dfsdm_set_osrs(struct stm32_dfsdm_filter *fl, unsigned int fast,
			 unsigned int oversamp)
{
	unsigned int i, d, fosr, iosr;
	u64 res;
	s64 delta;
	unsigned int m = 1;	/* multiplication factor */
	unsigned int p = fl->ford;	/* filter order (ford) */

	pr_debug("%s: Requested oversampling: %d\n",  __func__, oversamp);
	/*
	 * This function tries to compute filter oversampling and integrator
	 * oversampling, base on oversampling ratio requested by user.
	 *
	 * Decimation d depends on the filter order and the oversampling ratios.
	 * ford: filter order
	 * fosr: filter over sampling ratio
	 * iosr: integrator over sampling ratio
	 */
	if (fl->ford == DFSDM_FASTSINC_ORDER) {
		m = 2;
		p = 2;
	}

	/*
	 * Looks for filter and integrator oversampling ratios which allows
	 * to reach 24 bits data output resolution.
	 * Leave at once if exact resolution if reached.
	 * Otherwise the higher resolution below 32 bits is kept.
	 */
	for (fosr = 1; fosr <= DFSDM_MAX_FL_OVERSAMPLING; fosr++) {
		for (iosr = 1; iosr <= DFSDM_MAX_INT_OVERSAMPLING; iosr++) {
			if (fast)
				d = fosr * iosr;
			else if (fl->ford == DFSDM_FASTSINC_ORDER)
				d = fosr * (iosr + 3) + 2;
			else
				d = fosr * (iosr - 1 + p) + p;

			if (d > oversamp)
				break;
			else if (d != oversamp)
				continue;
			/*
			 * Check resolution (limited to signed 32 bits)
			 *   res <= 2^31
			 * Sincx filters:
			 *   res = m * fosr^p x iosr (with m=1, p=ford)
			 * FastSinc filter
			 *   res = m * fosr^p x iosr (with m=2, p=2)
			 */
			res = fosr;
			for (i = p - 1; i > 0; i--) {
				res = res * (u64)fosr;
				if (res > DFSDM_MAX_RES)
					break;
			}
			if (res > DFSDM_MAX_RES)
				continue;
			res = res * (u64)m * (u64)iosr;
			if (res > DFSDM_MAX_RES)
				continue;

			delta = res - DFSDM_DATA_RES;

			if (res >= fl->res) {
				fl->res = res;
				fl->fosr = fosr;
				fl->iosr = iosr;
				fl->fast = fast;
				pr_debug("%s: fosr = %d, iosr = %d\n",
					 __func__, fl->fosr, fl->iosr);
			}

			if (!delta)
				return 0;
		}
	}

	if (!fl->fosr)
		return -EINVAL;

	return 0;
}

/**
 * stm32_dfsdm_start_dfsdm - start global dfsdm IP interface.
 *
 * Enable interface if n_active_ch is not null.
 * @dfsdm: Handle used to retrieve dfsdm context.
 */
int stm32_dfsdm_start_dfsdm(struct stm32_dfsdm *dfsdm)
{
	struct dfsdm_priv *priv = container_of(dfsdm, struct dfsdm_priv, dfsdm);
	struct device *dev = &priv->pdev->dev;
	unsigned int clk_div = priv->spi_clk_out_div;
	int ret;

	if (atomic_inc_return(&priv->n_active_ch) == 1) {
		/* Enable clocks */
		ret = clk_prepare_enable(priv->clk);
		if (ret < 0) {
			dev_err(dev, "Failed to start clock\n");
			return ret;
		}
		if (priv->aclk) {
			ret = clk_prepare_enable(priv->aclk);
			if (ret < 0) {
				dev_err(dev, "Failed to start audio clock\n");
				goto disable_clk;
			}
		}

		/* Output the SPI CLKOUT (if clk_div == 0 clock if OFF) */
		ret = regmap_update_bits(dfsdm->regmap, DFSDM_CHCFGR1(0),
					 DFSDM_CHCFGR1_CKOUTDIV_MASK,
					 DFSDM_CHCFGR1_CKOUTDIV(clk_div));
		if (ret < 0)
			goto disable_aclk;

		/* Global enable of DFSDM interface */
		ret = regmap_update_bits(dfsdm->regmap, DFSDM_CHCFGR1(0),
					 DFSDM_CHCFGR1_DFSDMEN_MASK,
					 DFSDM_CHCFGR1_DFSDMEN(1));
		if (ret < 0)
			goto disable_aclk;
	}

	dev_dbg(dev, "%s: n_active_ch %d\n", __func__,
		atomic_read(&priv->n_active_ch));

	return 0;

disable_aclk:
	clk_disable_unprepare(priv->aclk);
disable_clk:
	clk_disable_unprepare(priv->clk);

	return ret;
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
		ret = regmap_update_bits(dfsdm->regmap, DFSDM_CHCFGR1(0),
					 DFSDM_CHCFGR1_DFSDMEN_MASK,
					 DFSDM_CHCFGR1_DFSDMEN(0));
		if (ret < 0)
			return ret;

		/* Stop SPI CLKOUT */
		ret = regmap_update_bits(dfsdm->regmap, DFSDM_CHCFGR1(0),
					 DFSDM_CHCFGR1_CKOUTDIV_MASK,
					 DFSDM_CHCFGR1_CKOUTDIV(0));
		if (ret < 0)
			return ret;

		/* Disable clocks */
		clk_disable_unprepare(priv->clk);
		if (priv->aclk)
			clk_disable_unprepare(priv->aclk);
	}
	dev_dbg(&priv->pdev->dev, "%s: n_active_ch %d\n", __func__,
		atomic_read(&priv->n_active_ch));

	return 0;
}

/**
 * stm32_dfsdm_start_channel
 *	Start DFSDM IP channels and associated interface.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @ch_id: Channel index.
 */
int stm32_dfsdm_start_channel(struct stm32_dfsdm *dfsdm, unsigned int ch_id)
{
	return regmap_update_bits(dfsdm->regmap, DFSDM_CHCFGR1(ch_id),
				  DFSDM_CHCFGR1_CHEN_MASK,
				  DFSDM_CHCFGR1_CHEN(1));
}

/**
 * stm32_dfsdm_stop_channel
 *	Stop DFSDM IP channels and associated interface.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @ch_id: Channel index.
 */
void stm32_dfsdm_stop_channel(struct stm32_dfsdm *dfsdm, unsigned int ch_id)
{
	regmap_update_bits(dfsdm->regmap, DFSDM_CHCFGR1(ch_id),
			   DFSDM_CHCFGR1_CHEN_MASK,
			   DFSDM_CHCFGR1_CHEN(0));
}

/**
 * stm32_dfsdm_chan_configure
 *	Configure DFSDM IP channels and associated interface.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @ch_id: channel index.
 */
int stm32_dfsdm_chan_configure(struct stm32_dfsdm *dfsdm,
			       struct stm32_dfsdm_channel *ch)
{
	unsigned int id = ch->id;
	struct regmap *regmap = dfsdm->regmap;
	int ret;

	ret = regmap_update_bits(regmap, DFSDM_CHCFGR1(id),
				 DFSDM_CHCFGR1_SITP_MASK,
				 DFSDM_CHCFGR1_SITP(ch->type));
	if (ret < 0)
		return ret;
	ret = regmap_update_bits(regmap, DFSDM_CHCFGR1(id),
				 DFSDM_CHCFGR1_SPICKSEL_MASK,
				 DFSDM_CHCFGR1_SPICKSEL(ch->src));
	if (ret < 0)
		return ret;
	return regmap_update_bits(regmap, DFSDM_CHCFGR1(id),
				  DFSDM_CHCFGR1_CHINSEL_MASK,
				  DFSDM_CHCFGR1_CHINSEL(ch->alt_si));
}

/**
 * stm32_dfsdm_start_filter - Start DFSDM IP filter conversion.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter index.
 */
int stm32_dfsdm_start_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id)
{
	int ret;

	/* Enable filter */
	ret = regmap_update_bits(dfsdm->regmap, DFSDM_CR1(fl_id),
				 DFSDM_CR1_DFEN_MASK, DFSDM_CR1_DFEN(1));
	if (ret < 0)
		return ret;

	/* Start conversion */
	return regmap_update_bits(dfsdm->regmap, DFSDM_CR1(fl_id),
				  DFSDM_CR1_RSWSTART_MASK,
				  DFSDM_CR1_RSWSTART(1));
}

/**
 * stm32_dfsdm_stop_filter - Stop DFSDM IP filter conversion.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: Filter index.
 */
void stm32_dfsdm_stop_filter(struct stm32_dfsdm *dfsdm, unsigned int fl_id)
{
	/* Mask IRQ for regular conversion achievement*/
	regmap_update_bits(dfsdm->regmap, DFSDM_CR2(fl_id),
			   DFSDM_CR2_REOCIE_MASK, DFSDM_CR2_REOCIE(0));
	/* Disable conversion */
	regmap_update_bits(dfsdm->regmap, DFSDM_CR1(fl_id),
			   DFSDM_CR1_DFEN_MASK, DFSDM_CR1_DFEN(0));
}

/**
 * stm32_dfsdm_filter_configure - Configure DFSDM IP filter and associate it
 *	to channel.
 *
 * @dfsdm: Handle used to retrieve dfsdm context.
 * @fl_id: channel index.
 * @fl_id: Filter index.
 */
int stm32_dfsdm_filter_configure(struct stm32_dfsdm *dfsdm, unsigned int fl_id,
				 unsigned int ch_id)
{
	struct regmap *regmap = dfsdm->regmap;
	struct stm32_dfsdm_filter *fl = &dfsdm->fl_list[fl_id];
	int ret;

	/* Average integrator oversampling */
	ret = regmap_update_bits(regmap, DFSDM_FCR(fl_id), DFSDM_FCR_IOSR_MASK,
				 DFSDM_FCR_IOSR(fl->iosr));

	/* Filter order and Oversampling */
	if (!ret)
		ret = regmap_update_bits(regmap, DFSDM_FCR(fl_id),
					 DFSDM_FCR_FOSR_MASK,
					 DFSDM_FCR_FOSR(fl->fosr));

	if (!ret)
		ret = regmap_update_bits(regmap, DFSDM_FCR(fl_id),
					 DFSDM_FCR_FORD_MASK,
					 DFSDM_FCR_FORD(fl->ford));

	/* If only one channel no scan mode supported for the moment */
	ret = regmap_update_bits(regmap, DFSDM_CR1(fl_id),
				 DFSDM_CR1_RCH_MASK,
				 DFSDM_CR1_RCH(ch_id));

	return regmap_update_bits(regmap, DFSDM_CR1(fl_id),
					 DFSDM_CR1_RSYNC_MASK,
			  DFSDM_CR1_RSYNC(fl->sync_mode));
}

static const struct iio_trigger_ops dfsdm_trigger_ops = {
	.owner = THIS_MODULE,
};

static int stm32_dfsdm_setup_spi_trigger(struct platform_device *pdev,
					 struct stm32_dfsdm *dfsdm)
{
	/*
	 * To be able to use buffer consumer interface a trigger is needed.
	 * As conversion are trigged by PDM samples from SPI bus, that makes
	 * sense to define the serial interface ( SPI or manchester) as
	 * trigger source.
	 */

	struct iio_trigger *trig;
	int ret;

	trig = devm_iio_trigger_alloc(&pdev->dev, DFSDM_SPI_TRIGGER_NAME);
	if (!trig)
		return -ENOMEM;

	trig->dev.parent = pdev->dev.parent;
	trig->ops = &dfsdm_trigger_ops;

	iio_trigger_set_drvdata(trig, dfsdm);

	ret = devm_iio_trigger_register(&pdev->dev, trig);
	if (ret)
		return ret;

	return 0;
}

int stm32_dfsdm_channel_parse_of(struct stm32_dfsdm *dfsdm,
				 struct iio_dev *indio_dev,
				 struct iio_chan_spec *chan, int chan_idx)
{
	struct iio_chan_spec *ch = &chan[chan_idx];
	struct stm32_dfsdm_channel *df_ch;
	const char *of_str;
	int ret, val;

	ret = of_property_read_u32_index(indio_dev->dev.of_node,
					 "st,adc-channels", chan_idx,
					 &ch->channel);
	if (ret < 0) {
		dev_err(&indio_dev->dev,
			" Error parsing 'st,adc-channels' for idx %d\n",
			chan_idx);
		return ret;
	}

	ret = of_property_read_string_index(indio_dev->dev.of_node,
					    "st,adc-channel-names", chan_idx,
					    &ch->datasheet_name);
	if (ret < 0) {
		dev_err(&indio_dev->dev,
			" Error parsing 'st,adc-channel-names' for idx %d\n",
			chan_idx);
		return ret;
	}

	df_ch =  &dfsdm->ch_list[ch->channel];
	df_ch->id = ch->channel;
	ret = of_property_read_string_index(indio_dev->dev.of_node,
					    "st,adc-channel-types", chan_idx,
					    &of_str);
	val  = stm32_dfsdm_str2val(of_str, stm32_dfsdm_chan_type);
	if (ret < 0 || val < 0)
		df_ch->type = 0;
	else
		df_ch->type = val;

	ret = of_property_read_string_index(indio_dev->dev.of_node,
					    "st,adc-channel-clk-src", chan_idx,
					    &of_str);
	val  = stm32_dfsdm_str2val(of_str, stm32_dfsdm_chan_src);
	if (ret < 0 || val < 0)
		df_ch->src = 0;
	else
		df_ch->src = val;

	ret = of_property_read_u32_index(indio_dev->dev.of_node,
					 "st,adc-alt-channel", chan_idx,
					 &df_ch->alt_si);
	if (ret < 0)
		df_ch->alt_si = 0;

	return 0;
}

static int stm32_dfsdm_parse_of(struct platform_device *pdev,
				struct dfsdm_priv *priv)
{
	struct device_node *node = pdev->dev.of_node;
	struct resource *res;
	unsigned long clk_freq;
	unsigned int spi_freq, rem;
	int ret;

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

	/* Source clock */
	priv->clk = devm_clk_get(&pdev->dev, "dfsdm");
	if (IS_ERR(priv->clk)) {
		dev_err(&pdev->dev, "No stm32_dfsdm_clk clock found\n");
		return -EINVAL;
	}

	priv->aclk = devm_clk_get(&pdev->dev, "audio");
	if (IS_ERR(priv->aclk))
		priv->aclk = NULL;

	if (priv->aclk)
		clk_freq = clk_get_rate(priv->aclk);
	else
		clk_freq = clk_get_rate(priv->clk);

	/* SPI clock freq */
	ret = of_property_read_u32(pdev->dev.of_node, "spi-max-frequency",
				   &spi_freq);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to get spi-max-frequency\n");
		return ret;
	}

	priv->spi_clk_out_div = div_u64_rem(clk_freq, spi_freq, &rem) - 1;
	priv->dfsdm.spi_master_freq = spi_freq;

	if (rem) {
		dev_warn(&pdev->dev, "SPI clock not accurate\n");
		dev_warn(&pdev->dev, "%ld = %d * %d + %d\n",
			 clk_freq, spi_freq, priv->spi_clk_out_div + 1, rem);
	}

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
	dfsdm->fl_list = devm_kcalloc(&pdev->dev, dev_data->num_filters,
				      sizeof(*dfsdm->fl_list), GFP_KERNEL);
	if (!dfsdm->fl_list)
		return -ENOMEM;

	dfsdm->num_fls = dev_data->num_filters;
	dfsdm->ch_list = devm_kcalloc(&pdev->dev, dev_data->num_channels,
				      sizeof(*dfsdm->ch_list),
				      GFP_KERNEL);
	if (!dfsdm->ch_list)
		return -ENOMEM;
	dfsdm->num_chs = dev_data->num_channels;

	ret = stm32_dfsdm_parse_of(pdev, priv);
	if (ret < 0)
		return ret;

	dfsdm->regmap = devm_regmap_init_mmio(&pdev->dev, dfsdm->base,
					    &stm32h7_dfsdm_regmap_cfg);
	if (IS_ERR(dfsdm->regmap)) {
		ret = PTR_ERR(dfsdm->regmap);
		dev_err(&pdev->dev, "%s: Failed to allocate regmap: %d\n",
			__func__, ret);
		return ret;
	}

	for (i = 0; i < STM32H7_DFSDM_NUM_FILTERS; i++) {
		struct stm32_dfsdm_filter *fl = &dfsdm->fl_list[i];

		fl->id = i;
	}

	platform_set_drvdata(pdev, dfsdm);

	ret = stm32_dfsdm_setup_spi_trigger(pdev, dfsdm);
	if (ret < 0)
		return ret;

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
