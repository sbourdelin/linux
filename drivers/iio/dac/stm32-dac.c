/*
 * This file is part of STM32 DAC driver
 *
 * Copyright (C) 2017, STMicroelectronics - All Rights Reserved
 * Authors: Amelie Delaunay <amelie.delaunay@st.com>
 *	    Fabrice Gasnier <fabrice.gasnier@st.com>
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

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/timer/stm32-timer-trigger.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_event.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "stm32-dac-core.h"

#define STM32_DAC_CHANNEL_1		1
#define STM32_DAC_CHANNEL_2		2
#define STM32_DAC_IS_CHAN_1(ch)		((ch) & STM32_DAC_CHANNEL_1)
/* channel2 shift */
#define STM32_DAC_CHAN2_SHIFT		16

/**
 * struct stm32_dac - private data of DAC driver
 * @common:		reference to DAC common data
 * @wavetype:		waveform generator
 * @swtrig:		Using software trigger
 */
struct stm32_dac {
	struct stm32_dac_common *common;
	u32 wavetype;
	bool swtrig;
};

/**
 * struct stm32_dac_trig_info - DAC trigger info
 * @name: name of the trigger, corresponding to its source
 * @tsel: trigger selection, value to be configured in DAC_CR.TSELx
 */
struct stm32_dac_trig_info {
	const char *name;
	u32 tsel;
};

static const struct stm32_dac_trig_info stm32h7_dac_trinfo[] = {
	{ "swtrig", 0 },
	{ TIM1_TRGO, 1 },
	{ TIM2_TRGO, 2 },
	{ TIM4_TRGO, 3 },
	{ TIM5_TRGO, 4 },
	{ TIM6_TRGO, 5 },
	{ TIM7_TRGO, 6 },
	{ TIM8_TRGO, 7 },
	{},
};

static irqreturn_t stm32_dac_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct stm32_dac *dac = iio_priv(indio_dev);
	int channel = indio_dev->channels[0].channel;

	/* Using software trigger? Then, trigger it now */
	if (dac->swtrig) {
		u32 swtrig;

		if (STM32_DAC_IS_CHAN_1(channel))
			swtrig = STM32_DAC_SWTRIGR_SWTRIG1;
		else
			swtrig = STM32_DAC_SWTRIGR_SWTRIG2;
		regmap_update_bits(dac->common->regmap, STM32_DAC_SWTRIGR,
				   swtrig, swtrig);
	}

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static unsigned int stm32_dac_get_trig_tsel(struct stm32_dac *dac,
					    struct iio_trigger *trig)
{
	unsigned int i;

	/* skip 1st trigger that should be swtrig */
	for (i = 1; stm32h7_dac_trinfo[i].name; i++) {
		/*
		 * Checking both stm32 timer trigger type and trig name
		 * should be safe against arbitrary trigger names.
		 */
		if (is_stm32_timer_trigger(trig) &&
		    !strcmp(stm32h7_dac_trinfo[i].name, trig->name)) {
			return stm32h7_dac_trinfo[i].tsel;
		}
	}

	/* When no trigger has been found, default to software trigger */
	dac->swtrig = true;

	return stm32h7_dac_trinfo[0].tsel;
}

static int stm32_dac_set_trigger(struct iio_dev *indio_dev,
				 struct iio_trigger *trig)
{
	struct stm32_dac *dac = iio_priv(indio_dev);
	int channel = indio_dev->channels[0].channel;
	u32 shift = STM32_DAC_IS_CHAN_1(channel) ? 0 : STM32_DAC_CHAN2_SHIFT;
	u32 val = 0, tsel;
	u32 msk = (STM32H7_DAC_CR_TEN1 | STM32H7_DAC_CR_TSEL1) << shift;

	dac->swtrig = false;
	if (trig) {
		/* select & enable trigger (tsel / ten) */
		tsel = stm32_dac_get_trig_tsel(dac, trig);
		val = tsel << STM32H7_DAC_CR_TSEL1_SHIFT;
		val = (val | STM32H7_DAC_CR_TEN1) << shift;
	}

	if (trig)
		dev_dbg(&indio_dev->dev, "enable trigger: %s\n", trig->name);
	else
		dev_dbg(&indio_dev->dev, "disable trigger\n");

	return regmap_update_bits(dac->common->regmap, STM32_DAC_CR, msk, val);
}

static int stm32_dac_is_enabled(struct iio_dev *indio_dev, int channel)
{
	struct stm32_dac *dac = iio_priv(indio_dev);
	u32 en, val;
	int ret;

	ret = regmap_read(dac->common->regmap, STM32_DAC_CR, &val);
	if (ret < 0)
		return ret;
	if (STM32_DAC_IS_CHAN_1(channel))
		en = FIELD_GET(STM32_DAC_CR_EN1, val);
	else
		en = FIELD_GET(STM32_DAC_CR_EN2, val);

	return !!en;
}

static int stm32_dac_enable(struct iio_dev *indio_dev, int channel)
{
	struct stm32_dac *dac = iio_priv(indio_dev);
	u32 en = STM32_DAC_IS_CHAN_1(channel) ?
		STM32_DAC_CR_EN1 : STM32_DAC_CR_EN2;
	int ret;

	ret = regmap_update_bits(dac->common->regmap, STM32_DAC_CR, en, en);
	if (ret < 0) {
		dev_err(&indio_dev->dev, "Enable failed\n");
		return ret;
	}

	/*
	 * When HFSEL is set, it is not allowed to write the DHRx register
	 * during 8 clock cycles after the ENx bit is set. It is not allowed
	 * to make software/hardware trigger during this period either.
	 */
	if (dac->common->hfsel)
		udelay(1);

	return 0;
}

static int stm32_dac_disable(struct iio_dev *indio_dev, int channel)
{
	struct stm32_dac *dac = iio_priv(indio_dev);
	u32 en = STM32_DAC_IS_CHAN_1(channel) ?
		STM32_DAC_CR_EN1 : STM32_DAC_CR_EN2;
	int ret;

	ret = regmap_update_bits(dac->common->regmap, STM32_DAC_CR, en, 0);
	if (ret)
		dev_err(&indio_dev->dev, "Disable failed\n");

	return ret;
}

static int stm32_dac_get_value(struct stm32_dac *dac, int channel, int *val)
{
	int ret;

	if (STM32_DAC_IS_CHAN_1(channel))
		ret = regmap_read(dac->common->regmap, STM32_DAC_DOR1, val);
	else
		ret = regmap_read(dac->common->regmap, STM32_DAC_DOR2, val);

	return ret ? ret : IIO_VAL_INT;
}

static int stm32_dac_set_value(struct stm32_dac *dac, int channel, int val)
{
	int ret;

	if (STM32_DAC_IS_CHAN_1(channel))
		ret = regmap_write(dac->common->regmap, STM32_DAC_DHR12R1, val);
	else
		ret = regmap_write(dac->common->regmap, STM32_DAC_DHR12R2, val);

	return ret;
}

static int stm32_dac_get_offset(struct stm32_dac *dac, int channel, int *val)
{
	int ret;

	/* Offset is only relevant in waveform generation mode. */
	if (!dac->wavetype) {
		*val = 0;
		return IIO_VAL_INT;
	}

	/*
	 * In waveform generation mode, DC offset in DHR is added to waveform
	 * generator output, then stored to DOR (data output register).
	 * Read offset from DHR.
	 */
	if (STM32_DAC_IS_CHAN_1(channel))
		ret = regmap_read(dac->common->regmap, STM32_DAC_DHR12R1, val);
	else
		ret = regmap_read(dac->common->regmap, STM32_DAC_DHR12R2, val);

	return ret ? ret : IIO_VAL_INT;
}

static int stm32_dac_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct stm32_dac *dac = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		return stm32_dac_get_value(dac, chan->channel, val);
	case IIO_CHAN_INFO_OFFSET:
		return stm32_dac_get_offset(dac, chan->channel, val);
	case IIO_CHAN_INFO_SCALE:
		*val = dac->common->vref_mv;
		*val2 = chan->scan_type.realbits;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static int stm32_dac_write_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int val, int val2, long mask)
{
	struct stm32_dac *dac = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_OFFSET:
		/* Offset only makes sense in waveform generation mode */
		if (dac->wavetype)
			return stm32_dac_set_value(dac, chan->channel, val);
		return -EBUSY;
	case IIO_CHAN_INFO_RAW:
		if (!dac->wavetype)
			return stm32_dac_set_value(dac, chan->channel, val);
		/* raw value is read only in waveform generation mode */
		return -EBUSY;
	default:
		return -EINVAL;
	}
}

static int stm32_dac_debugfs_reg_access(struct iio_dev *indio_dev,
					unsigned reg, unsigned writeval,
					unsigned *readval)
{
	struct stm32_dac *dac = iio_priv(indio_dev);

	if (!readval)
		return regmap_write(dac->common->regmap, reg, writeval);
	else
		return regmap_read(dac->common->regmap, reg, readval);
}

static const struct iio_info stm32_dac_iio_info = {
	.read_raw = stm32_dac_read_raw,
	.write_raw = stm32_dac_write_raw,
	.set_trigger = stm32_dac_set_trigger,
	.debugfs_reg_access = stm32_dac_debugfs_reg_access,
	.driver_module = THIS_MODULE,
};

static const char * const stm32_dac_powerdown_modes[] = {
	"Hi-Z",
	"enable",
};

static int stm32_dac_get_powerdown_mode(struct iio_dev *indio_dev,
					const struct iio_chan_spec *chan)
{
	return stm32_dac_is_enabled(indio_dev, chan->channel);
}

static int stm32_dac_set_powerdown_mode(struct iio_dev *indio_dev,
					const struct iio_chan_spec *chan,
					unsigned int type)
{
	if (type)
		return stm32_dac_enable(indio_dev, chan->channel);
	else
		return stm32_dac_disable(indio_dev, chan->channel);
}

static ssize_t stm32_dac_read_powerdown(struct iio_dev *indio_dev,
					uintptr_t private,
					const struct iio_chan_spec *chan,
					char *buf)
{
	int ret = stm32_dac_is_enabled(indio_dev, chan->channel);

	if (ret < 0)
		return ret;

	return sprintf(buf, "%d\n", ret);
}

static ssize_t stm32_dac_write_powerdown(struct iio_dev *indio_dev,
					 uintptr_t private,
					 const struct iio_chan_spec *chan,
					 const char *buf, size_t len)
{
	unsigned int en;
	int ret;

	ret = kstrtouint(buf, 0, &en);
	if (ret)
		return ret;

	ret = stm32_dac_set_powerdown_mode(indio_dev, chan, en);
	if (ret < 0)
		return ret;

	return len;
}

static const struct iio_enum stm32_dac_powerdown_mode_en = {
	.items = stm32_dac_powerdown_modes,
	.num_items = ARRAY_SIZE(stm32_dac_powerdown_modes),
	.get = stm32_dac_get_powerdown_mode,
	.set = stm32_dac_set_powerdown_mode,
};

/* waveform generator wave selection */
static const char * const stm32_dac_wavetype_desc[] = {
	"flat",
	"noise",
	"triangle",
};

static int stm32_dac_set_wavetype(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  unsigned int wavetype)
{
	struct stm32_dac *dac = iio_priv(indio_dev);
	u32 mask, val;
	int ret;

	/*
	 * Waveform generator requires a trigger to be configured, to increment
	 * or decrement internal counter in case of triangle generator. Noise
	 * generator is a bit different, but also requires a trigger to
	 * generate samples.
	 */
	if (wavetype && !indio_dev->trig)
		dev_dbg(&indio_dev->dev, "Wavegen requires a trigger\n");

	if (STM32_DAC_IS_CHAN_1(chan->channel)) {
		val = FIELD_PREP(STM32_DAC_CR_WAVE1, wavetype);
		mask = STM32_DAC_CR_WAVE1;
	} else {
		val = FIELD_PREP(STM32_DAC_CR_WAVE2, wavetype);
		mask = STM32_DAC_CR_WAVE2;
	}

	ret = regmap_update_bits(dac->common->regmap, STM32_DAC_CR, mask, val);
	if (ret)
		return ret;
	dac->wavetype = wavetype;

	return 0;
}

static int stm32_dac_get_wavetype(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan)
{
	struct stm32_dac *dac = iio_priv(indio_dev);
	u32 val;
	int ret;

	ret = regmap_read(dac->common->regmap, STM32_DAC_CR, &val);
	if (ret < 0)
		return ret;

	if (STM32_DAC_IS_CHAN_1(chan->channel))
		return FIELD_GET(STM32_DAC_CR_WAVE1, val);
	else
		return FIELD_GET(STM32_DAC_CR_WAVE2, val);
}

static const struct iio_enum stm32_dac_wavetype_enum = {
	.items = stm32_dac_wavetype_desc,
	.num_items = ARRAY_SIZE(stm32_dac_wavetype_desc),
	.get = stm32_dac_get_wavetype,
	.set = stm32_dac_set_wavetype,
};

/*
 * waveform generator mamp selection: mask/amplitude
 * - noise: LFSR mask (linear feedback shift register, umasks bit 0, [1:0]...)
 * - triangle: amplitude (equal to 1, 3, 5, 7... 4095)
 */
static const char * const stm32_dac_amplitude_desc[] = {
	"1", "3", "7", "15", "31", "63", "127", "255", "511", "1023", "2047",
	"4095",
};

static int stm32_dac_set_amplitude(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan,
				   unsigned int amplitude)
{
	struct stm32_dac *dac = iio_priv(indio_dev);
	u32 mask, val;

	if (STM32_DAC_IS_CHAN_1(chan->channel)) {
		val = FIELD_PREP(STM32_DAC_CR_MAMP1, amplitude);
		mask = STM32_DAC_CR_MAMP1;
	} else {
		val = FIELD_PREP(STM32_DAC_CR_MAMP2, amplitude);
		mask = STM32_DAC_CR_MAMP2;
	}

	return regmap_update_bits(dac->common->regmap, STM32_DAC_CR, mask, val);
}

static int stm32_dac_get_amplitude(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan)
{
	struct stm32_dac *dac = iio_priv(indio_dev);
	u32 val;
	int ret;

	ret = regmap_read(dac->common->regmap, STM32_DAC_CR, &val);
	if (ret < 0)
		return ret;

	if (STM32_DAC_IS_CHAN_1(chan->channel))
		return FIELD_GET(STM32_DAC_CR_MAMP1, val);
	else
		return FIELD_GET(STM32_DAC_CR_MAMP2, val);
}

static const struct iio_enum stm32_dac_amplitude_enum = {
	.items = stm32_dac_amplitude_desc,
	.num_items = ARRAY_SIZE(stm32_dac_amplitude_desc),
	.get = stm32_dac_get_amplitude,
	.set = stm32_dac_set_amplitude,
};

static const struct iio_chan_spec_ext_info stm32_dac_ext_info[] = {
	{
		.name = "powerdown",
		.read = stm32_dac_read_powerdown,
		.write = stm32_dac_write_powerdown,
		.shared = IIO_SEPARATE,
	},
	IIO_ENUM("powerdown_mode", IIO_SEPARATE, &stm32_dac_powerdown_mode_en),
	IIO_ENUM_AVAILABLE("powerdown_mode", &stm32_dac_powerdown_mode_en),
	IIO_ENUM("wavetype", IIO_SEPARATE, &stm32_dac_wavetype_enum),
	IIO_ENUM_AVAILABLE("wavetype", &stm32_dac_wavetype_enum),
	IIO_ENUM("amplitude", IIO_SEPARATE, &stm32_dac_amplitude_enum),
	IIO_ENUM_AVAILABLE("amplitude", &stm32_dac_amplitude_enum),
	{},
};

#define STM32_DAC_CHANNEL(chan, name) {			\
	.type = IIO_VOLTAGE,				\
	.indexed = 1,					\
	.output = 1,					\
	.channel = chan,				\
	.info_mask_separate =				\
		BIT(IIO_CHAN_INFO_OFFSET) |		\
		BIT(IIO_CHAN_INFO_RAW) |		\
		BIT(IIO_CHAN_INFO_SCALE),		\
	/* scan_index is always 0 as num_channels is 1 */ \
	.scan_type = {					\
		.sign = 'u',				\
		.realbits = 12,				\
		.storagebits = 16,			\
	},						\
	.datasheet_name = name,				\
	.ext_info = stm32_dac_ext_info			\
}

static const struct iio_chan_spec stm32_dac_channels[] = {
	STM32_DAC_CHANNEL(STM32_DAC_CHANNEL_1, "out1"),
	STM32_DAC_CHANNEL(STM32_DAC_CHANNEL_2, "out2"),
};

static int stm32_dac_chan_of_init(struct iio_dev *indio_dev)
{
	struct device_node *np = indio_dev->dev.of_node;
	unsigned int i;
	u32 channel;
	int ret;

	ret = of_property_read_u32(np, "reg", &channel);
	if (ret) {
		dev_err(&indio_dev->dev, "Failed to read reg property\n");
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(stm32_dac_channels); i++) {
		if (stm32_dac_channels[i].channel == channel)
			break;
	}
	if (i >= ARRAY_SIZE(stm32_dac_channels)) {
		dev_err(&indio_dev->dev, "Invalid st,dac-channel\n");
		return -EINVAL;
	}

	indio_dev->channels = &stm32_dac_channels[i];
	/*
	 * Expose only one channel here, as they can be used independently,
	 * with separate trigger. Then separate IIO devices are instantiated
	 * to manage this.
	 */
	indio_dev->num_channels = 1;

	return 0;
};

static int stm32_dac_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct iio_dev *indio_dev;
	struct stm32_dac *dac;
	int ret;

	if (!np)
		return -ENODEV;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*dac));
	if (!indio_dev)
		return -ENOMEM;
	platform_set_drvdata(pdev, indio_dev);

	dac = iio_priv(indio_dev);
	dac->common = dev_get_drvdata(pdev->dev.parent);
	indio_dev->name = dev_name(&pdev->dev);
	indio_dev->dev.parent = &pdev->dev;
	indio_dev->dev.of_node = pdev->dev.of_node;
	indio_dev->info = &stm32_dac_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = stm32_dac_chan_of_init(indio_dev);
	if (ret < 0)
		return ret;

	ret = iio_triggered_event_setup(indio_dev, NULL,
					stm32_dac_trigger_handler);
	if (ret)
		return ret;

	ret = iio_device_register(indio_dev);
	if (ret) {
		iio_triggered_event_cleanup(indio_dev);
		return ret;
	}

	return 0;
}

static int stm32_dac_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);

	iio_triggered_event_cleanup(indio_dev);
	iio_device_unregister(indio_dev);

	return 0;
}

static const struct of_device_id stm32_dac_of_match[] = {
	{ .compatible = "st,stm32-dac", },
	{},
};
MODULE_DEVICE_TABLE(of, stm32_dac_of_match);

static struct platform_driver stm32_dac_driver = {
	.probe = stm32_dac_probe,
	.remove = stm32_dac_remove,
	.driver = {
		.name = "stm32-dac",
		.of_match_table = stm32_dac_of_match,
	},
};
module_platform_driver(stm32_dac_driver);

MODULE_ALIAS("platform:stm32-dac");
MODULE_AUTHOR("Amelie Delaunay <amelie.delaunay@st.com>");
MODULE_DESCRIPTION("STMicroelectronics STM32 DAC driver");
MODULE_LICENSE("GPL v2");
