/*
 * This file is part of STM32 DFSDM ADC driver
 *
 * Copyright (C) 2016, STMicroelectronics - All Rights Reserved
 * Author: Arnaud Pouliquen <arnaud.pouliquen@st.com>.
 *
 * License type: GPLv2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/iio/hw_consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include <sound/stm32-adfsdm.h>

#include "stm32-dfsdm.h"

enum stm32_dfsdm_mode {
	DFSDM_ADC, /* ADC mode, access through IIO ABI */
	DFSDM_AUDIO /* Audio mode, access through ASoC ABI */
};

struct stm32_dfsdm_adc {
	struct stm32_dfsdm *common;

	unsigned int fl_id;
	unsigned int oversamp;
	unsigned int clk_freq;

	enum stm32_dfsdm_mode mode;
	struct platform_device *audio_pdev;

	void (*overrun_cb)(void *context);
	void *cb_context;

	/* Hardware consumer structure for Front End iio */
	struct iio_hw_consumer *hwc;
};

static const enum stm32_dfsdm_mode stm32_dfsdm_data_adc = DFSDM_ADC;
static const enum stm32_dfsdm_mode stm32_dfsdm_data_audio = DFSDM_AUDIO;

struct stm32_dfsdm_adc_devdata {
	enum stm32_dfsdm_mode mode;
	const struct iio_info *info;
};

static int stm32_dfsdm_set_osrs(struct stm32_dfsdm_adc *adc, bool fast,
				unsigned int oversamp)
{
	/*
	 * TODO
	 * This function tries to compute filter oversampling and integrator
	 * oversampling, base on oversampling ratio requested by user.
	 */

	return 0;
};

static int stm32_dfsdm_single_conv(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan, int *res)
{
	/* TODO: Perform conversion instead of sending fake value */
	dev_dbg(&indio_dev->dev, "%s\n", __func__);

	*res = chan->channel + 0xFFFF00;
	return 0;
}

static int stm32_dfsdm_write_raw(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *chan,
				 int val, int val2, long mask)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		ret = stm32_dfsdm_set_osrs(adc, 0, val);
		if (!ret)
			adc->oversamp = val;
		break;
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (adc->mode == DFSDM_AUDIO)
			ret = stm32_dfsdm_set_osrs(adc, 0, val);
		else
			ret = -EINVAL;
		break;

	default:
		ret = -EINVAL;
	}

	return ret;
}

static int stm32_dfsdm_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan, int *val,
				int *val2, long mask)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	int ret;

	dev_dbg(&indio_dev->dev, "%s\n", __func__);
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (adc->hwc) {
			ret = iio_hw_consumer_enable(adc->hwc);
			if (ret < 0) {
				dev_err(&indio_dev->dev,
					"%s: iio enable failed (channel %d)\n",
					__func__, chan->channel);
				return ret;
			}
		}
		ret = stm32_dfsdm_single_conv(indio_dev, chan, val);
		if (ret < 0) {
			dev_err(&indio_dev->dev,
				"%s: conversion failed (channel %d)\n",
				__func__, chan->channel);
			return ret;
		}

		if (adc->hwc)
			iio_hw_consumer_disable(adc->hwc);

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		*val = adc->oversamp;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = DIV_ROUND_CLOSEST(adc->clk_freq, adc->oversamp);

		return IIO_VAL_INT;
	}

	return -EINVAL;
}

static const struct iio_info stm32_dfsdm_info_adc = {
	.read_raw = stm32_dfsdm_read_raw,
	.write_raw = stm32_dfsdm_write_raw,
	.driver_module = THIS_MODULE,
};

static const struct iio_info stm32_dfsdm_info_audio = {
	.read_raw = stm32_dfsdm_read_raw,
	.write_raw = stm32_dfsdm_write_raw,
	.driver_module = THIS_MODULE,
};

const struct stm32_dfsdm_adc_devdata stm32_dfsdm_devdata_adc = {
	.mode = DFSDM_ADC,
	.info = &stm32_dfsdm_info_adc,
};

const struct stm32_dfsdm_adc_devdata stm32_dfsdm_devdata_audio = {
	.mode = DFSDM_AUDIO,
	.info = &stm32_dfsdm_info_audio,
};

static irqreturn_t stm32_dfsdm_irq(int irq, void *arg)
{
	/* TODO */
	return IRQ_HANDLED;
}

static void stm32_dfsdm_set_sysclk(struct stm32_dfsdm_adc *adc,
				   unsigned int freq)
{
	struct iio_dev *iio = iio_priv_to_dev(adc);

	dev_dbg(&iio->dev, "%s:\n", __func__);

	adc->clk_freq = freq;
};

	/* Set expected audio sampling rate */
static int stm32_dfsdm_set_hwparam(struct stm32_dfsdm_adc *adc,
				   struct stm32_dfsdm_hw_param *params)
{
	struct iio_dev *iio = iio_priv_to_dev(adc);

	dev_dbg(&iio->dev, "%s for rate %d\n", __func__, params->rate);

	return stm32_dfsdm_set_osrs(adc, 0, params->rate);
};

	/* Called when ASoC starts an audio stream setup. */
static int stm32_dfsdm_audio_startup(struct stm32_dfsdm_adc *adc)
{
	struct iio_dev *iio = iio_priv_to_dev(adc);

	dev_dbg(&iio->dev, "%s\n", __func__);

	return 0;
};

	/* Shuts down the audio stream. */
static void stm32_dfsdm_audio_shutdown(struct stm32_dfsdm_adc *adc)
{
	struct iio_dev *iio = iio_priv_to_dev(adc);

	dev_dbg(&iio->dev, "%s\n", __func__);
};

	/*
	 * Provides DMA source physicla addr to allow ALsa to handle DMA
	 * transfers.
	 */
static dma_addr_t stm32_dfsdm_get_dma_source(struct stm32_dfsdm_adc *adc)
{
	struct iio_dev *iio = iio_priv_to_dev(adc);

	dev_dbg(&iio->dev, "%s\n", __func__);

	return (dma_addr_t)(adc->common->phys_base + DFSDM_RDATAR(adc->fl_id));
};

/* Register callback to treat underrun and overrun issues */
static void stm32_dfsdm_register_xrun_cb(struct stm32_dfsdm_adc *adc,
					 void (*overrun_cb)(void *context),
					 void *context)
{
	struct iio_dev *iio = iio_priv_to_dev(adc);

	dev_dbg(&iio->dev, "%s\n", __func__);
	adc->overrun_cb = overrun_cb;
	adc->cb_context = context;
};

const struct stm32_adfsdm_codec_ops stm32_dfsdm_audio_ops = {
	.set_sysclk = stm32_dfsdm_set_sysclk,
	.set_hwparam = stm32_dfsdm_set_hwparam,
	.audio_startup = stm32_dfsdm_audio_startup,
	.audio_shutdown = stm32_dfsdm_audio_shutdown,
	.register_xrun_cb = stm32_dfsdm_register_xrun_cb,
	.get_dma_source = stm32_dfsdm_get_dma_source
};

static int stm32_dfsdm_adc_chan_init_one(struct iio_dev *indio_dev,
					 struct iio_chan_spec *chan,
					 int chan_idx)
{
	struct iio_chan_spec *ch = &chan[chan_idx];
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	int ret;

	dev_dbg(&indio_dev->dev, "%s:\n", __func__);
	ret = of_property_read_u32_index(indio_dev->dev.of_node,
					 "st,adc-channels", chan_idx,
					 &ch->channel);
	if (ret < 0) {
		dev_err(&indio_dev->dev,
			" error parsing 'st,adc-channels' for idx %d\n",
			chan_idx);
		return ret;
	}

	ret = of_property_read_string_index(indio_dev->dev.of_node,
					    "st,adc-channel-names", chan_idx,
					    &ch->datasheet_name);
	if (ret < 0) {
		dev_err(&indio_dev->dev,
			" error parsing 'st,adc-channel-names' for idx %d\n",
			chan_idx);
		return ret;
	}

	ch->type = IIO_VOLTAGE;
	ch->indexed = 1;
	ch->scan_index = chan_idx;
	if (adc->mode == DFSDM_ADC) {
		/*
		 * IIO_CHAN_INFO_RAW: used to compute regular conversion
		 * IIO_CHAN_INFO_SAMP_FREQ: used to indicate sampling frequency
		 * IIO_CHAN_INFO_OVERSAMPLING_RATIO: used set oversampling
		 */
		ch->info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
					 BIT(IIO_CHAN_INFO_SAMP_FREQ) |
					 BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO);
	}

	ch->scan_type.sign = 'u';
	ch->scan_type.realbits = 24;
	ch->scan_type.storagebits = 32;

	return 0;
}

static int stm32_dfsdm_adc_chan_init(struct iio_dev *indio_dev)
{
	struct iio_chan_spec *channels;
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	unsigned int num_ch;
	int ret, chan_idx;

	num_ch = of_property_count_u32_elems(indio_dev->dev.of_node,
					     "st,adc-channels");
	if (num_ch < 0 || num_ch >= adc->common->num_chs) {
		dev_err(&indio_dev->dev, "Bad st,adc-channels?\n");
		return num_ch < 0 ? num_ch : -EINVAL;
	}

	channels = devm_kcalloc(&indio_dev->dev, num_ch, sizeof(*channels),
				GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	if (adc->mode == DFSDM_ADC) {
		/*
		 * Bind to sd modulator iio device for ADC only.
		 * For Audio the PDM microphone will be handled by ASoC
		 */
		adc->hwc = iio_hw_consumer_alloc(&indio_dev->dev);
		if (IS_ERR(adc->hwc)) {
			dev_err(&indio_dev->dev, "no backend found\n");
			return PTR_ERR(adc->hwc);
		}
	}

	for (chan_idx = 0; chan_idx < num_ch; chan_idx++) {
		ret = stm32_dfsdm_adc_chan_init_one(indio_dev, channels,
						    chan_idx);
		if (ret < 0)
			goto free_hwc;
	}

	indio_dev->num_channels = num_ch;
	indio_dev->channels = channels;

	return 0;

free_hwc:
	if (adc->hwc)
		iio_hw_consumer_free(adc->hwc);
	return ret;
}

static const struct of_device_id stm32_dfsdm_adc_match[] = {
	{ .compatible = "st,stm32-dfsdm-adc",
	  .data = &stm32_dfsdm_devdata_adc},
	{ .compatible = "st,stm32-dfsdm-pdm",
	  .data = &stm32_dfsdm_devdata_audio},
	{}
};

static int stm32_dfsdm_adc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct stm32_dfsdm_adc *adc;
	const struct of_device_id *of_id;
	struct device_node *np = dev->of_node;
	const struct stm32_dfsdm_adc_devdata *devdata;
	struct iio_dev *iio;
	int ret, irq;

	dev_dbg(dev, "%s:\n", __func__);

	iio = devm_iio_device_alloc(dev, sizeof(*adc));
	if (IS_ERR(iio)) {
		dev_err(dev, "%s: failed to allocate iio", __func__);
		return PTR_ERR(iio);
	}

	adc = iio_priv(iio);
	if (IS_ERR(adc)) {
		dev_err(dev, "%s: failed to allocate adc", __func__);
		return PTR_ERR(adc);
	}
	adc->common = dev_get_drvdata(dev->parent);

	/* Populate data structure depending on compatibility */
	of_id = of_match_node(stm32_dfsdm_adc_match, np);
	if (!of_id->data) {
		dev_err(&pdev->dev, "Data associated to device is missing\n");
		return -EINVAL;
	}

	devdata = (const struct stm32_dfsdm_adc_devdata *)of_id->data;
	adc->mode = devdata->mode;

	iio->name = np->name;
	iio->dev.parent = dev;
	iio->dev.of_node = np;
	iio->info = devdata->info;
	iio->modes = INDIO_DIRECT_MODE;

	platform_set_drvdata(pdev, adc);

	ret = of_property_read_u32(dev->of_node, "reg", &adc->fl_id);
	if (ret != 0) {
		dev_err(dev, "missing reg property\n");
		return -EINVAL;
	}

	/*
	 * In a first step IRQs generated for channels are not treated.
	 * So IRQ associated to filter instance 0 is dedicated to the Filter 0.
	 * In a second step IRQ domain should be used for filter 0 when feature
	 * like Watchdog, clock absence detection,... will be integrated.
	 */
	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(dev, irq, stm32_dfsdm_irq,
			       0, pdev->name, adc);
	if (ret < 0) {
		dev_err(dev, "failed to request IRQ\n");
		return ret;
	}

	ret = stm32_dfsdm_adc_chan_init(iio);
	if (ret < 0)
		return ret;

	ret = iio_device_register(iio);
	if (ret) {
		dev_err(dev, "failed to register iio device\n");
		return ret;
	}

	if (adc->mode == DFSDM_AUDIO) {
		struct stm32_adfsdm_pdata dai_data = {
			.ops = &stm32_dfsdm_audio_ops,
			.adc = adc,
		};

		adc->audio_pdev = platform_device_register_data(
						dev, STM32_ADFSDM_DRV_NAME,
						PLATFORM_DEVID_AUTO,
						&dai_data, sizeof(dai_data));

		if (IS_ERR(adc->audio_pdev))
			return PTR_ERR(adc->audio_pdev);
	}

	return 0;
}

static int stm32_dfsdm_adc_remove(struct platform_device *pdev)
{
	struct stm32_dfsdm_adc *adc = platform_get_drvdata(pdev);
	struct iio_dev *iio = iio_priv_to_dev(adc);

	iio_device_unregister(iio);

	return 0;
}

static struct platform_driver stm32_dfsdm_adc_driver = {
	.driver = {
		.name = "stm32-dfsdm-adc",
		.of_match_table = stm32_dfsdm_adc_match,
	},
	.probe = stm32_dfsdm_adc_probe,
	.remove = stm32_dfsdm_adc_remove,
};
module_platform_driver(stm32_dfsdm_adc_driver);

MODULE_DESCRIPTION("STM32 sigma delta ADC");
MODULE_AUTHOR("Arnaud Pouliquen <arnaud.pouliquen@st.com>");
MODULE_LICENSE("GPL v2");
