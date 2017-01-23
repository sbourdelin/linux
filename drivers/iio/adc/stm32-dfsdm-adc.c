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

#include <linux/irq_work.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/iio/iio.h>

#include <linux/mfd/stm32-dfsdm.h>

#define DFSDM_ADC_MAX_RESOLUTION 24
#define DFSDM_ADC_STORAGE_BITS   32

#define DFSDM_MAX_CH_OFFSET BIT(24)
#define DFSDM_MAX_CH_SHIFT 24

#define DFSDM_TIMEOUT_US 100000
#define DFSDM_TIMEOUT (msecs_to_jiffies(DFSDM_TIMEOUT_US / 1000))

#define CH_ID_FROM_IDX(i) (adc->inputs[i].id)
#define CH_CFG_FROM_IDX(i) (&adc->inputs_cfg[i])

struct stm32_dfsdm_adc {
	struct device *dev;
	struct stm32_dfsdm *dfsdm;
	struct list_head adc_list;

	/* Filter */
	unsigned int fl_id;
	struct stm32_dfsdm_sinc_filter sinc;
	unsigned int int_oversampling;

	/* Channels */
	struct stm32_dfsdm_channel *inputs;
	struct stm32_dfsdm_ch_cfg *inputs_cfg;

	/* Raw mode*/
	struct completion completion;
	struct stm32_dfsdm_regular reg_params;
	u32 *buffer;
};

static const char * const stm32_dfsdm_adc_sinc_order[] = {
	[0] = "FastSinc",
	[1] = "Sinc1",
	[2] = "Sinc2",
	[3] = "Sinc3",
	[4] = "Sinc4",
	[5] = "Sinc5",
};

static inline const struct iio_chan_spec *get_ch_from_id(
					struct iio_dev *indio_dev, int ch_id)
{
	int i;

	for (i = 0; i < indio_dev->num_channels; i++) {
		if (ch_id == indio_dev->channels[i].channel)
			return &indio_dev->channels[i];
	}

	return NULL;
}

/*
 * Filter attributes
 */

static int stm32_dfsdm_adc_set_sinc(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    unsigned int val)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);

	dev_dbg(&indio_dev->dev, "%s: %s\n", __func__,
		stm32_dfsdm_adc_sinc_order[adc->sinc.order]);

	adc->sinc.order = val;

	return 0;
}

static int stm32_dfsdm_adc_get_sinc(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);

	dev_dbg(&indio_dev->dev, "%s: %s\n", __func__,
		stm32_dfsdm_adc_sinc_order[adc->sinc.order]);

	return adc->sinc.order;
}

static const struct iio_enum stm32_dfsdm_adc_fl_sinc_order = {
	.items = stm32_dfsdm_adc_sinc_order,
	.num_items = ARRAY_SIZE(stm32_dfsdm_adc_sinc_order),
	.get = stm32_dfsdm_adc_get_sinc,
	.set = stm32_dfsdm_adc_set_sinc,
};

static ssize_t stm32_dfsdm_adc_get_int_os(struct iio_dev *indio_dev,
					  uintptr_t priv,
					  const struct iio_chan_spec *chan,
					  char *buf)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", adc->int_oversampling);
}

static ssize_t stm32_dfsdm_adc_set_int_os(struct iio_dev *indio_dev,
					  uintptr_t priv,
					  const struct iio_chan_spec *chan,
					  const char *buf, size_t len)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	int ret, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	if ((!val) || (val > DFSDM_MAX_INT_OVERSAMPLING)) {
		dev_err(&indio_dev->dev, "invalid oversampling (0 or > %#x)",
			DFSDM_MAX_INT_OVERSAMPLING);
		return -EINVAL;
	}
	adc->int_oversampling = val;

	return len;
}

static ssize_t stm32_dfsdm_adc_get_fl_os(struct iio_dev *indio_dev,
					 uintptr_t priv,
					 const struct iio_chan_spec *chan,
					 char *buf)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", adc->sinc.oversampling);
}

static ssize_t stm32_dfsdm_adc_set_fl_os(struct iio_dev *indio_dev,
					 uintptr_t priv,
					const struct iio_chan_spec *chan,
					const char *buf, size_t len)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	int ret, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	if ((!val) || (val > DFSDM_MAX_FL_OVERSAMPLING)) {
		dev_err(&indio_dev->dev, "invalid oversampling (0 or > %#x)",
			DFSDM_MAX_FL_OVERSAMPLING);
		return -EINVAL;
	}
	adc->sinc.oversampling = val;

	return len;
}

/*
 * Data bit shifting attribute
 */
static ssize_t stm32_dfsdm_adc_get_shift(struct iio_dev *indio_dev,
					 uintptr_t priv,
					 const struct iio_chan_spec *chan,
					 char *buf)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	struct stm32_dfsdm_ch_cfg *ch_cfg = CH_CFG_FROM_IDX(chan->scan_index);

	return snprintf(buf, PAGE_SIZE, "%d\n", ch_cfg->right_bit_shift);
}

static ssize_t stm32_dfsdm_adc_set_shift(struct iio_dev *indio_dev,
					 uintptr_t priv,
					 const struct iio_chan_spec *chan,
					 const char *buf, size_t len)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	struct stm32_dfsdm_ch_cfg *ch_cfg = CH_CFG_FROM_IDX(chan->scan_index);
	int ret, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	if (val > DFSDM_MAX_CH_SHIFT) {
		dev_err(&indio_dev->dev, "invalid shift value (> %#x)",
			DFSDM_MAX_CH_SHIFT);
		return -EINVAL;
	}
	ch_cfg->right_bit_shift = val;

	return len;
}

static const struct iio_chan_spec_ext_info stm32_dfsdm_adc_ext_info[] = {
	/* sinc_filter_order: Configure Sinc filter order */
	IIO_ENUM("sinc_filter_order", IIO_SHARED_BY_TYPE,
		 &stm32_dfsdm_adc_fl_sinc_order),
	IIO_ENUM_AVAILABLE("sinc_filter_order", &stm32_dfsdm_adc_fl_sinc_order),
	/* filter oversampling: Post filter oversampling ratio */
	{
		.name = "sinc_filter_oversampling_ratio",
		.shared = IIO_SHARED_BY_TYPE,
		.read = stm32_dfsdm_adc_get_fl_os,
		.write = stm32_dfsdm_adc_set_fl_os,
	},
	/* data_right_bit_shift : Filter output data shifting */
	{
		.name = "data_right_bit_shift",
		.shared = IIO_SEPARATE,
		.read = stm32_dfsdm_adc_get_shift,
		.write = stm32_dfsdm_adc_set_shift,
	},

	/*
	 * averaging_length : Mean windows of data from filter.
	 * Defines how many filter data will be summed to one data output
	 */
	{
		.name = "integrator_oversampling",
		.shared = IIO_SHARED_BY_TYPE,
		.read = stm32_dfsdm_adc_get_int_os,
		.write = stm32_dfsdm_adc_set_int_os,
	},
	{},
};

/*
 * Filter event routine called under IRQ context
 */
static void stm32_dfsdm_event_cb(struct stm32_dfsdm *dfsdm, int flt_id,
				 enum stm32_dfsdm_events ev, unsigned int param,
				 void *context)
{
	struct stm32_dfsdm_adc *adc = context;
	unsigned int ch_id;

	dev_dbg(adc->dev, "%s:\n", __func__);

	switch (ev) {
	case DFSDM_EVENT_REG_EOC:
		stm32_dfsdm_read_fl_conv(adc->dfsdm, flt_id, adc->buffer,
					 &ch_id, DFSDM_FILTER_REG_CONV);
		complete(&adc->completion);
		break;
	case DFSDM_EVENT_REG_XRUN:
		dev_err(adc->dev, "%s: underrun detected for filter %d\n",
			__func__, flt_id);
		break;
	default:
		dev_err(adc->dev, "%s: event %#x not implemented\n",
			__func__, ev);
		break;
	}
}

static inline void stm32_dfsdm_adc_fl_config(struct stm32_dfsdm_adc *adc,
					     u32 channel_mask,
					     struct stm32_dfsdm_filter *filter)
{
	dev_dbg(adc->dev, "%s:\n", __func__);

	filter->event.cb = stm32_dfsdm_event_cb;
	filter->event.context = adc;

	filter->sinc_params = adc->sinc;

	filter->int_oversampling = adc->int_oversampling;
}

static int stm32_dfsdm_adc_start_raw_conv(struct stm32_dfsdm_adc *adc,
					  const struct iio_chan_spec *chan)
{
	struct stm32_dfsdm_filter filter;
	struct stm32_dfsdm_ch_cfg *ch_cfg = CH_CFG_FROM_IDX(chan->scan_index);
	unsigned int ch_id = CH_ID_FROM_IDX(chan->scan_index);
	int ret;

	dev_dbg(adc->dev, "%s:\n", __func__);

	memset(&filter, 0, sizeof(filter));
	filter.reg_params = &adc->reg_params;

	if (!filter.reg_params)
		return -ENOMEM;

	filter.reg_params->ch_src = ch_id;

	stm32_dfsdm_adc_fl_config(adc, BIT(ch_id), &filter);

	ret = stm32_dfsdm_configure_filter(adc->dfsdm, adc->fl_id, &filter);
	if (ret < 0) {
		dev_err(adc->dev, "Failed to configure filter\n");
		return ret;
	}

	ret = stm32_dfsdm_start_channel(adc->dfsdm, ch_id, ch_cfg);
	if (ret < 0)
		return ret;

	stm32_dfsdm_start_filter(adc->dfsdm, adc->fl_id, DFSDM_FILTER_REG_CONV);

	return 0;
}

static void stm32_dfsdm_adc_stop_raw_conv(struct stm32_dfsdm_adc *adc,
					  const struct iio_chan_spec *chan)
{
	unsigned int ch_id = CH_ID_FROM_IDX(chan->scan_index);

	dev_dbg(adc->dev, "%s:\n", __func__);

	stm32_dfsdm_stop_filter(adc->dfsdm, adc->fl_id);
	stm32_dfsdm_stop_channel(adc->dfsdm, ch_id);
}

static int stm32_dfsdm_single_conv(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan,
				   u32 *result)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	long timeout;
	int ret;

	dev_dbg(&indio_dev->dev, "%s:\n", __func__);

	reinit_completion(&adc->completion);

	ret = stm32_dfsdm_register_fl_event(adc->dfsdm, adc->fl_id,
					    DFSDM_EVENT_REG_EOC, 0);
	if (ret < 0) {
		dev_err(&indio_dev->dev, "Failed to register event\n");
		return ret;
	}

	adc->buffer = result;
	ret = stm32_dfsdm_adc_start_raw_conv(adc, chan);
	if (ret) {
		dev_err(&indio_dev->dev, "Failed to start conversion\n");
		goto free_event;
	}

	timeout = wait_for_completion_interruptible_timeout(&adc->completion,
							    DFSDM_TIMEOUT);
	if (timeout == 0) {
		dev_warn(&indio_dev->dev, "Conversion timed out!\n");
		ret = -ETIMEDOUT;
	} else if (timeout < 0) {
		ret = timeout;
	} else {
		dev_dbg(&indio_dev->dev, "converted val %#x\n", *result);
		ret = IIO_VAL_INT;
	}

	stm32_dfsdm_adc_stop_raw_conv(adc, chan);

free_event:
	adc->buffer = NULL;
	stm32_dfsdm_unregister_fl_event(adc->dfsdm, adc->fl_id,
					DFSDM_EVENT_REG_EOC, 0);

	return ret;
}

static int stm32_dfsdm_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan, int *val,
				int *val2, long mask)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	struct stm32_dfsdm_ch_cfg *ch_cfg = CH_CFG_FROM_IDX(chan->scan_index);
	int ret = -EINVAL;

	dev_dbg(&indio_dev->dev, "%s channel %d\n", __func__, chan->channel);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = stm32_dfsdm_single_conv(indio_dev, chan, val);
		if (!ret)
			ret = IIO_VAL_INT;
		break;
	case IIO_CHAN_INFO_OFFSET:
		*val = ch_cfg->offset;
		ret = IIO_VAL_INT;
		break;
	}

	return ret;
}

static int stm32_dfsdm_write_raw(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *chan, int val,
				 int val2, long mask)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	struct stm32_dfsdm_ch_cfg *ch_cfg = CH_CFG_FROM_IDX(chan->scan_index);

	dev_dbg(&indio_dev->dev, "%s channel%d", __func__, chan->channel);

	switch (mask) {
	case IIO_CHAN_INFO_OFFSET:
		if (val > DFSDM_MAX_CH_OFFSET) {
			dev_err(&indio_dev->dev, "invalid offset (> %#lx)",
				DFSDM_MAX_CH_OFFSET);
			return -EINVAL;
		}
		ch_cfg->offset = val;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct iio_info stm32_dfsdm_iio_info = {
	.read_raw = stm32_dfsdm_read_raw,
	.write_raw = stm32_dfsdm_write_raw,
	.driver_module = THIS_MODULE,
};

static int stm32_dfsdm_adc_chan_init_one(struct iio_dev *indio_dev,
					 struct iio_chan_spec *chan,
					 int chan_idx)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	struct stm32_dfsdm_channel *dfsdm_ch = &adc->inputs[chan_idx];
	struct iio_chan_spec *ch = &chan[chan_idx];
	int ret;
	unsigned int alt_ch = 0;

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

	ch->extend_name = ch->datasheet_name;
	ch->type = IIO_VOLTAGE;
	ch->indexed = 1;
	ch->scan_index = chan_idx;
	ch->info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				 BIT(IIO_CHAN_INFO_OFFSET);
	ch->scan_type.sign = 'u';
	ch->scan_type.realbits = DFSDM_ADC_MAX_RESOLUTION;
	ch->scan_type.storagebits = DFSDM_ADC_STORAGE_BITS;
	ch->scan_type.shift = 8;

	ch->ext_info = stm32_dfsdm_adc_ext_info;

	of_property_read_u32_index(indio_dev->dev.of_node, "st,adc-alt-channel",
				   chan_idx, &alt_ch);
	/* Select the previous channel if alternate field is defined*/
	if (alt_ch) {
		if (!ch->channel)
			ch->channel = adc->dfsdm->max_channels;
		ch->channel -= 1;
		dfsdm_ch->serial_if.pins = DFSDM_CHANNEL_NEXT_CHANNEL_PINS;
	} else {
		dfsdm_ch->serial_if.pins = DFSDM_CHANNEL_SAME_CHANNEL_PINS;
	}
	dfsdm_ch->id = ch->channel;

	dfsdm_ch->type.DataPacking = DFSDM_CHANNEL_STANDARD_MODE;

	dfsdm_ch->type.source = DFSDM_CHANNEL_EXTERNAL_INPUTS;
	ret = of_property_read_u32_index(indio_dev->dev.of_node,
					 "st,adc-channel-types",
					 chan_idx, &dfsdm_ch->serial_if.type);
	if (ret < 0)
		dfsdm_ch->serial_if.type = DFSDM_CHANNEL_SPI_RISING;

	ret = of_property_read_u32_index(indio_dev->dev.of_node,
					 "st,adc-channel-clk-src",
					 chan_idx,
					 &dfsdm_ch->serial_if.spi_clk);

	if ((dfsdm_ch->serial_if.type == DFSDM_CHANNEL_MANCHESTER_RISING)  ||
	    (dfsdm_ch->serial_if.type == DFSDM_CHANNEL_MANCHESTER_FALLING) ||
	    (ret < 0))
		dfsdm_ch->serial_if.spi_clk = DFSDM_CHANNEL_SPI_CLOCK_INTERNAL;

	return stm32_dfsdm_get_channel(adc->dfsdm, dfsdm_ch);
}

static int stm32_dfsdm_adc_chan_init(struct iio_dev *indio_dev)
{
	struct stm32_dfsdm_adc *adc = iio_priv(indio_dev);
	unsigned int num_ch;
	struct iio_chan_spec *channels;
	int ret, chan_idx;

	num_ch = of_property_count_strings(indio_dev->dev.of_node,
					   "st,adc-channel-names");

	channels = devm_kcalloc(&indio_dev->dev, num_ch, sizeof(*channels),
				GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	adc->inputs = devm_kcalloc(&indio_dev->dev, num_ch,
				   sizeof(*adc->inputs), GFP_KERNEL);
	if (!adc->inputs)
		return -ENOMEM;

	adc->inputs_cfg = devm_kcalloc(&indio_dev->dev, num_ch,
				       sizeof(*adc->inputs_cfg), GFP_KERNEL);
	if (!adc->inputs_cfg)
		return -ENOMEM;

	for (chan_idx = 0; chan_idx < num_ch; chan_idx++) {
		ret = stm32_dfsdm_adc_chan_init_one(indio_dev, channels,
						    chan_idx);
		if (ret < 0)
			goto ch_error;
	}

	indio_dev->num_channels = num_ch;
	indio_dev->channels = channels;

	return 0;

ch_error:
	for (chan_idx--; chan_idx >= 0; chan_idx--)
		stm32_dfsdm_release_channel(adc->dfsdm,
					    adc->inputs[chan_idx].id);

	return ret;
}

static int stm32_dfsdm_adc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct stm32_dfsdm_adc *adc;
	struct device_node *np = pdev->dev.of_node;
	struct iio_dev *indio_dev;
	int ret, i;

	if (!np)
		return -ENODEV;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (IS_ERR(indio_dev)) {
		dev_err(dev, "%s: failed to allocate iio", __func__);
		return PTR_ERR(indio_dev);
	}

	indio_dev->name = np->name;
	indio_dev->dev.parent = dev;
	indio_dev->dev.of_node = np;
	indio_dev->info = &stm32_dfsdm_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	adc = iio_priv(indio_dev);
	if (IS_ERR(adc)) {
		dev_err(dev, "%s: failed to allocate adc", __func__);
		return PTR_ERR(adc);
	}

	if (of_property_read_u32(np, "reg", &adc->fl_id)) {
		dev_err(&pdev->dev, "missing reg property\n");
		return -EINVAL;
	}

	adc->dev = &indio_dev->dev;
	adc->dfsdm = dev_get_drvdata(pdev->dev.parent);

	ret = stm32_dfsdm_adc_chan_init(indio_dev);
	if (ret < 0) {
		dev_err(dev, "iio channels init failed\n");
		return ret;
	}

	ret = stm32_dfsdm_get_filter(adc->dfsdm, adc->fl_id);
	if (ret < 0)
		goto get_fl_err;

	adc->int_oversampling = DFSDM_MIN_INT_OVERSAMPLING;
	adc->sinc.oversampling = DFSDM_MIN_FL_OVERSAMPLING;

	init_completion(&adc->completion);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret) {
		dev_err(adc->dev, "failed to register iio device\n");
		goto register_err;
	}

	platform_set_drvdata(pdev, adc);

	return 0;

register_err:
	stm32_dfsdm_release_filter(adc->dfsdm, adc->fl_id);

get_fl_err:
	for (i = 0; i < indio_dev->num_channels; i++)
		stm32_dfsdm_release_channel(adc->dfsdm, adc->inputs[i].id);

	return ret;
}

static int stm32_dfsdm_adc_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct stm32_dfsdm_adc *adc = platform_get_drvdata(pdev);
	int i;

	indio_dev = iio_priv_to_dev(adc);
	for (i = 0; i < indio_dev->num_channels; i++)
		stm32_dfsdm_release_channel(adc->dfsdm, adc->inputs[i].id);
	stm32_dfsdm_release_filter(adc->dfsdm, adc->fl_id);

	return 0;
}

static const struct of_device_id stm32_dfsdm_adc_match[] = {
	{ .compatible = "st,stm32-dfsdm-adc"},
	{}
};

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
