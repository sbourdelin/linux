/*
 * This file is the ADC part of of the STM32 DFSDM driver
 *
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
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
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <linux/iio/hw_consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include "stm32-dfsdm.h"

#define DFSDM_TIMEOUT_US 100000
#define DFSDM_TIMEOUT (msecs_to_jiffies(DFSDM_TIMEOUT_US / 1000))

struct stm32_dfsdm_adc {
	struct stm32_dfsdm *dfsdm;
	unsigned int fl_id;
	unsigned int ch_id;

	unsigned int oversamp;

	struct completion completion;

	u32 *buffer;

	/* Hardware consumer structure for Front End IIO */
	struct iio_hw_consumer *hwc;
};

static int stm32_dfsdm_start_conv(struct stm32_dfsdm_adc *adc)
{
	int ret;

	ret = stm32_dfsdm_start_dfsdm(adc->dfsdm);
	if (ret < 0)
		return ret;

	ret = stm32_dfsdm_start_channel(adc->dfsdm, adc->ch_id);
	if (ret < 0)
		goto stop_dfsdm;

	ret = stm32_dfsdm_filter_configure(adc->dfsdm, adc->fl_id, adc->ch_id);
	if (ret < 0)
		goto stop_channels;

	ret = stm32_dfsdm_start_filter(adc->dfsdm, adc->fl_id);
	if (ret < 0)
		goto stop_channels;

	return 0;

stop_channels:
	stm32_dfsdm_stop_channel(adc->dfsdm, adc->ch_id);
stop_dfsdm:
	stm32_dfsdm_stop_dfsdm(adc->dfsdm);

	return ret;
}

static void stm32_dfsdm_stop_conv(struct stm32_dfsdm_adc *adc)
{
	stm32_dfsdm_stop_filter(adc->dfsdm, adc->fl_id);

	stm32_dfsdm_stop_channel(adc->dfsdm, adc->ch_id);

	stm32_dfsdm_stop_dfsdm(adc->dfsdm);
}

static int stm32_dfsdm_single_conv(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan, int *res)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	long timeout;
	int ret;

	reinit_completion(&adc->completion);

	adc->buffer = res;

	/* Unmask IRQ for regular conversion achievement*/
	ret = regmap_update_bits(adc->dfsdm->regmap, DFSDM_CR2(adc->fl_id),
				 DFSDM_CR2_REOCIE_MASK, DFSDM_CR2_REOCIE(1));
	if (ret < 0)
		return ret;

	ret = stm32_dfsdm_start_conv(adc);
	if (ret < 0)
		return ret;

	timeout = wait_for_completion_interruptible_timeout(&adc->completion,
							    DFSDM_TIMEOUT);
	/* Mask IRQ for regular conversion achievement*/
	regmap_update_bits(adc->dfsdm->regmap, DFSDM_CR2(adc->fl_id),
			   DFSDM_CR2_REOCIE_MASK, DFSDM_CR2_REOCIE(0));

	if (timeout == 0) {
		dev_warn(&indio_dev->dev, "Conversion timed out!\n");
		ret = -ETIMEDOUT;
	} else if (timeout < 0) {
		ret = timeout;
	} else {
		dev_dbg(&indio_dev->dev, "Converted val %#x\n", *res);
		ret = IIO_VAL_INT;
	}

	/* Mask IRQ for regular conversion achievement*/
	regmap_update_bits(adc->dfsdm->regmap, DFSDM_CR2(adc->fl_id),
			   DFSDM_CR2_REOCIE_MASK, DFSDM_CR2_REOCIE(0));

	stm32_dfsdm_stop_conv(adc);

	return ret;
}

static int stm32_dfsdm_write_raw(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *chan,
				 int val, int val2, long mask)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	struct stm32_dfsdm_filter *fl = &adc->dfsdm->fl_list[adc->fl_id];
	int ret = -EINVAL;

	if (mask == IIO_CHAN_INFO_OVERSAMPLING_RATIO) {
		ret = stm32_dfsdm_set_osrs(fl, 0, val);
		if (!ret)
			adc->oversamp = val;
	}
	return ret;
}

static int stm32_dfsdm_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan, int *val,
				int *val2, long mask)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_hw_consumer_enable(adc->hwc);
		if (ret < 0) {
			dev_err(&indio_dev->dev,
				"%s: IIO enable failed (channel %d)\n",
				__func__, chan->channel);
			return ret;
		}
		ret = stm32_dfsdm_single_conv(indio_dev, chan, val);
		if (ret < 0) {
			dev_err(&indio_dev->dev,
				"%s: Conversion failed (channel %d)\n",
				__func__, chan->channel);
			return ret;
		}

		iio_hw_consumer_disable(adc->hwc);

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_OVERSAMPLING_RATIO:
		*val = adc->oversamp;

		return IIO_VAL_INT;
	}

	return -EINVAL;
}

static const struct iio_info stm32_dfsdm_info_adc = {
	.read_raw = stm32_dfsdm_read_raw,
	.write_raw = stm32_dfsdm_write_raw,
	.driver_module = THIS_MODULE,
};

static irqreturn_t stm32_dfsdm_irq(int irq, void *arg)
{
	struct stm32_dfsdm_adc *adc = arg;
	struct regmap *regmap = adc->dfsdm->regmap;
	unsigned int status;

	regmap_read(regmap, DFSDM_ISR(adc->fl_id), &status);

	if (status & DFSDM_ISR_REOCF_MASK) {
		/* read the data register clean the IRQ status */
		regmap_read(regmap, DFSDM_RDATAR(adc->fl_id), adc->buffer);
		complete(&adc->completion);
	}
	if (status & DFSDM_ISR_ROVRF_MASK) {
		regmap_update_bits(regmap, DFSDM_ICR(adc->fl_id),
				   DFSDM_ICR_CLRROVRF_MASK,
				   DFSDM_ICR_CLRROVRF_MASK);
	}

	return IRQ_HANDLED;
}

static int stm32_dfsdm_postenable(struct iio_dev *indio_dev)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);

	return stm32_dfsdm_start_conv(adc);
}

static int stm32_dfsdm_predisable(struct iio_dev *indio_dev)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);

	stm32_dfsdm_stop_conv(adc);
	return 0;
}

static const struct iio_buffer_setup_ops stm32_dfsdm_buffer_setup_ops = {
	.postenable = &stm32_dfsdm_postenable,
	.predisable = &stm32_dfsdm_predisable,
};

static int stm32_dfsdm_adc_chan_init_one(struct iio_dev *indio_dev,
					 struct iio_chan_spec *chan,
					 int ch_idx)
{
	struct iio_chan_spec *ch = &chan[ch_idx];
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	int ret;

	ret = stm32_dfsdm_channel_parse_of(adc->dfsdm, indio_dev, chan, ch_idx);

	ch->type = IIO_VOLTAGE;
	ch->indexed = 1;
	ch->scan_index = ch_idx;

	/*
	 * IIO_CHAN_INFO_RAW: used to compute regular conversion
	 * IIO_CHAN_INFO_OVERSAMPLING_RATIO: used to set oversampling
	 */
	ch->info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				 BIT(IIO_CHAN_INFO_OVERSAMPLING_RATIO);

	ch->scan_type.sign = 'u';
	ch->scan_type.realbits = 24;
	ch->scan_type.storagebits = 32;
	adc->ch_id = ch->channel;

	return stm32_dfsdm_chan_configure(adc->dfsdm,
					  &adc->dfsdm->ch_list[ch->channel]);
}

static int stm32_dfsdm_adc_chan_init(struct iio_dev *indio_dev)
{
	struct iio_chan_spec *channels;
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	unsigned int num_ch;
	int ret, chan_idx;

	num_ch = of_property_count_u32_elems(indio_dev->dev.of_node,
					     "st,adc-channels");
	if (num_ch < 0 || num_ch >= adc->dfsdm->num_chs) {
		dev_err(&indio_dev->dev, "Bad st,adc-channels?\n");
		return num_ch < 0 ? num_ch : -EINVAL;
	}

	/*
	 * Number of channel per filter is temporary limited to 1.
	 * Restriction should be cleaned with scan mode
	 */
	if (num_ch > 1) {
		dev_err(&indio_dev->dev, "Multi channel not yet supported\n");
		return -EINVAL;
	}

	/* Bind to SD modulator IIO device */
	adc->hwc = iio_hw_consumer_alloc(&indio_dev->dev);
	if (IS_ERR(adc->hwc))
		return -EPROBE_DEFER;

	channels = devm_kcalloc(&indio_dev->dev, num_ch, sizeof(*channels),
				GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

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
	iio_hw_consumer_free(adc->hwc);
	return ret;
}

static const struct of_device_id stm32_dfsdm_adc_match[] = {
	{ .compatible = "st,stm32-dfsdm-adc"},
	{}
};

static int stm32_dfsdm_adc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct stm32_dfsdm_adc *adc;
	struct device_node *np = dev->of_node;
	struct iio_dev *iio;
	char *name;
	int ret, irq, val;

	iio = devm_iio_device_alloc(dev, sizeof(*adc));
	if (IS_ERR(iio)) {
		dev_err(dev, "%s: Failed to allocate IIO\n", __func__);
		return PTR_ERR(iio);
	}

	adc = iio_priv(iio);
	if (IS_ERR(adc)) {
		dev_err(dev, "%s: Failed to allocate ADC\n", __func__);
		return PTR_ERR(adc);
	}
	adc->dfsdm = dev_get_drvdata(dev->parent);

	iio->dev.parent = dev;
	iio->dev.of_node = np;
	iio->info = &stm32_dfsdm_info_adc;
	iio->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_SOFTWARE;

	platform_set_drvdata(pdev, adc);

	ret = of_property_read_u32(dev->of_node, "reg", &adc->fl_id);
	if (ret != 0) {
		dev_err(dev, "Missing reg property\n");
		return -EINVAL;
	}

	name = kzalloc(sizeof("dfsdm-adc0"), GFP_KERNEL);
	if (!name)
		return -ENOMEM;
	snprintf(name, sizeof("dfsdm-adc0"), "dfsdm-adc%d", adc->fl_id);
	iio->name = name;

	/*
	 * In a first step IRQs generated for channels are not treated.
	 * So IRQ associated to filter instance 0 is dedicated to the Filter 0.
	 */
	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(dev, irq, stm32_dfsdm_irq,
			       0, pdev->name, adc);
	if (ret < 0) {
		dev_err(dev, "Failed to request IRQ\n");
		return ret;
	}

	ret = of_property_read_u32(dev->of_node, "st,filter-order", &val);
	if (ret < 0) {
		dev_err(dev, "Failed to set filter order\n");
		return ret;
	}
	adc->dfsdm->fl_list[adc->fl_id].ford = val;

	ret = of_property_read_u32(dev->of_node, "st,filter0-sync", &val);
	if (!ret)
		adc->dfsdm->fl_list[adc->fl_id].sync_mode = val;

	ret = stm32_dfsdm_adc_chan_init(iio);
	if (ret < 0)
		return ret;

	init_completion(&adc->completion);

	return iio_device_register(iio);
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
