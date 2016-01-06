/*
 * Atmel ADC driver for SAMA5D2 devices and later.
 *
 * Copyright (C) 2015 Atmel,
 *               2015 Ludovic Desroches <ludovic.desroches@atmel.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/regulator/consumer.h>

#define AT91_ADC8XX_CR		0x00			/* Control Register */
#define		AT91_ADC8XX_CR_SWRST		BIT(0)		/* Software Reset */
#define		AT91_ADC8XX_CR_START		BIT(1)		/* Start Conversion */
#define		AT91_ADC8XX_CR_TSCALIB		BIT(2)		/* Touchscreen Calibration */
#define		AT91_ADC8XX_CR_CMPRST		BIT(4)		/* Comparison Restart */
#define AT91_ADC8XX_MR		0x04			/* Mode Register */
#define		AT91_ADC8XX_MR_TRGSEL(v)	((v) << 1)	/* Trigger Selection */
#define			AT91_ADC8XX_MR_TRGSEL_TRIG0	0		/* ADTRG */
#define			AT91_ADC8XX_MR_TRGSEL_TRIG1	1		/* TIOA0 */
#define			AT91_ADC8XX_MR_TRGSEL_TRIG2	2		/* TIOA1 */
#define			AT91_ADC8XX_MR_TRGSEL_TRIG3	3		/* TIOA2 */
#define			AT91_ADC8XX_MR_TRGSEL_TRIG4	4		/* PWM event line 0 */
#define			AT91_ADC8XX_MR_TRGSEL_TRIG5	5		/* PWM event line 1 */
#define			AT91_ADC8XX_MR_TRGSEL_TRIG6	6		/* TIOA3 */
#define			AT91_ADC8XX_MR_TRGSEL_TRIG7	7		/* RTCOUT0 */
#define		AT91_ADC8XX_MR_SLEEP		BIT(5)		/* Sleep Mode */
#define		AT91_ADC8XX_MR_FWUP		BIT(6)		/* Fast Wake Up */
#define		AT91_ADC8XX_MR_PRESCAL(v)	((v) << AT91_ADC8XX_MR_PRESCAL_OFFSET)	/* Prescaler Rate Selection */
#define			AT91_ADC8XX_MR_PRESCAL_OFFSET	8
#define			AT91_ADC8XX_MR_PRESCAL_MAX	0xff
#define		AT91_ADC8XX_MR_STARTUP(v)	((v) << 16)	/* Startup Time */
#define		AT91_ADC8XX_MR_ANACH		BIT(23)		/* Analog Change */
#define		AT91_ADC8XX_MR_TRACKTIM(v)	((v) << 24)	/* Tracking Time */
#define			AT91_ADC8XX_MR_TRACKTIM_MAX	0xff
#define		AT91_ADC8XX_MR_TRANSFER(v)	((v) << 28)	/* Transfer Time */
#define			AT91_ADC8XX_MR_TRANSFER_MAX	0x3
#define		AT91_ADC8XX_MR_USEQ		BIT(31)		/* Use Sequence Enable */
#define AT91_ADC8XX_SEQR1	0x08			/* Channel Sequence Register 1 */
#define AT91_ADC8XX_SEQR2	0x0c			/* Channel Sequence Register 2 */
#define AT91_ADC8XX_CHER	0x10			/* Channel Enable Register */
#define AT91_ADC8XX_CHDR	0x14			/* Channel Disable Register */
#define AT91_ADC8XX_CHSR	0x18			/* Channel Status Register */
#define AT91_ADC8XX_LCDR	0x20			/* Last Converted Data Register */
#define AT91_ADC8XX_IER		0x24			/* Interrupt Enable Register */
#define AT91_ADC8XX_IDR		0x28			/* Interrupt Disable Register */
#define AT91_ADC8XX_IMR		0x2c			/* Interrupt Mask Register */
#define AT91_ADC8XX_ISR		0x30			/* Interrupt Status Register */
#define AT91_ADC8XX_LCTMR	0x34			/* Last Channel Trigger Mode Register */
#define AT91_ADC8XX_LCCWR	0x38			/* Last Channel Compare Window Register */
#define AT91_ADC8XX_OVER	0x3c			/* Overrun Status Register */
#define AT91_ADC8XX_EMR		0x40			/* Extended Mode Register */
#define AT91_ADC8XX_CWR		0x44			/* Compare Window Register */
#define AT91_ADC8XX_CGR		0x48			/* Channel Gain Register */
#define AT91_ADC8XX_COR		0x4c			/* Channel Offset Register */
#define AT91_ADC8XX_CDR0	0x50			/* Channel Data Register 0 */
#define AT91_ADC8XX_ACR		0x94			/* Analog Control Register */
#define AT91_ADC8XX_TSMR	0xb0			/* Touchscreen Mode Register */
#define AT91_ADC8XX_XPOSR	0xb4			/* Touchscreen X Position Register */
#define AT91_ADC8XX_YPOSR	0xb8			/* Touchscreen Y Position Register */
#define AT91_ADC8XX_PRESSR	0xbc			/* Touchscreen Pressure Register */
#define AT91_ADC8XX_TRGR	0xc0			/* Trigger Register */
#define AT91_ADC8XX_COSR	0xd0			/* Correction Select Register */
#define AT91_ADC8XX_CVR		0xd4			/* Correction Value Register */
#define AT91_ADC8XX_CECR	0xd8			/* Channel Error Correction Register */
#define AT91_ADC8XX_WPMR	0xe4			/* Write Protection Mode Register */
#define AT91_ADC8XX_WPSR	0xe8			/* Write Protection Status Register */
#define AT91_ADC8XX_VERSION	0xfc			/* Version Register */

#define AT91_AT91_ADC8XX_CHAN(num, addr)				\
	{								\
		.type = IIO_VOLTAGE,					\
		.channel = num,						\
		.address = addr,					\
		.scan_type = {						\
			.sign = 'u',					\
			.realbits = 12,					\
		},							\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
		.datasheet_name = "CH"#num,				\
		.indexed = 1,						\
	}

#define at91_adc_readl(st, reg)		readl_relaxed(st->base + reg)
#define at91_adc_writel(st, reg, val)	writel_relaxed(val, st->base + reg)

struct at91_adc_soc_info {
	unsigned			startup_time;
	unsigned			min_sample_rate;
	unsigned			max_sample_rate;
};

struct at91_adc_state {
	void __iomem			*base;
	int				irq;
	struct clk			*per_clk;
	struct regulator		*reg;
	struct regulator		*vref;
	u32				vref_uv;
	const struct iio_chan_spec	*chan;
	bool				conversion_done;
	u32				conversion_value;
	struct at91_adc_soc_info	soc_info;
	wait_queue_head_t		wq_data_available;
	struct mutex			lock;
};

static const struct iio_chan_spec at91_adc_channels[] = {
	AT91_AT91_ADC8XX_CHAN(0, 0x50),
	AT91_AT91_ADC8XX_CHAN(1, 0x54),
	AT91_AT91_ADC8XX_CHAN(2, 0x58),
	AT91_AT91_ADC8XX_CHAN(3, 0x5c),
	AT91_AT91_ADC8XX_CHAN(4, 0x60),
	AT91_AT91_ADC8XX_CHAN(5, 0x64),
	AT91_AT91_ADC8XX_CHAN(6, 0x68),
	AT91_AT91_ADC8XX_CHAN(7, 0x6c),
	AT91_AT91_ADC8XX_CHAN(8, 0x70),
	AT91_AT91_ADC8XX_CHAN(9, 0x74),
	AT91_AT91_ADC8XX_CHAN(10, 0x78),
	AT91_AT91_ADC8XX_CHAN(11, 0x7c),
};

static unsigned at91_adc_startup_time(unsigned startup_time_min,
				      unsigned adc_clk_khz)
{
	const unsigned startup_lookup[] = {
		  0,   8,  16,  24,
		 64,  80,  96, 112,
		512, 576, 640, 704,
		768, 832, 896, 960
		};
	unsigned ticks_min, i;

	/*
	 * Since the adc frequency is checked before, there is no reason
	 * to not meet the startup time constraint.
	 */

	ticks_min = startup_time_min * adc_clk_khz / 1000;
	for (i = 0; i < ARRAY_SIZE(startup_lookup); i++)
		if (startup_lookup[i] > ticks_min)
			break;

	return i;
}

static void at91_adc_setup_samp_freq(struct at91_adc_state *st, unsigned freq)
{
	struct iio_dev *indio_dev = iio_priv_to_dev(st);
	unsigned f_per, prescal, startup;

	f_per = clk_get_rate(st->per_clk);
	prescal = (f_per / (2 * freq)) - 1;

	startup = at91_adc_startup_time(st->soc_info.startup_time,
					freq / 1000);

	at91_adc_writel(st, AT91_ADC8XX_MR,
			AT91_ADC8XX_MR_TRANSFER(2)
			| AT91_ADC8XX_MR_STARTUP(startup)
			| AT91_ADC8XX_MR_PRESCAL(prescal));

	dev_dbg(&indio_dev->dev, "freq: %u, startup: %u, prescal: %u\n",
		freq, startup, prescal);
}

static ssize_t at91_adc_show_samp_freq(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct at91_adc_state *st = iio_priv(dev_to_iio_dev(dev));
	unsigned f_adc, f_per = clk_get_rate(st->per_clk);
	unsigned mr, prescal;

	mr = at91_adc_readl(st, AT91_ADC8XX_MR);
	prescal = (mr >> AT91_ADC8XX_MR_PRESCAL_OFFSET)
		  & AT91_ADC8XX_MR_PRESCAL_MAX;
	f_adc = f_per / (2 * (prescal + 1));

	return sprintf(buf, "%d\n", f_adc);
}

static ssize_t at91_adc_store_samp_freq(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct at91_adc_state *st = iio_priv(dev_to_iio_dev(dev));
	unsigned val;
	int ret;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (val < st->soc_info.min_sample_rate ||
	    val > st->soc_info.max_sample_rate)
		return -EINVAL;

	at91_adc_setup_samp_freq(st, val);

	return len;
}

static IIO_DEV_ATTR_SAMP_FREQ(S_IRUGO | S_IWUSR,
			      at91_adc_show_samp_freq,
			      at91_adc_store_samp_freq);

static struct attribute *at91_adc_event_attributes[] = {
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	NULL,
};

static struct attribute_group at91_adc_event_attribute_group = {
	.attrs = at91_adc_event_attributes,
};

static irqreturn_t at91_adc_interrupt(int irq, void *private)
{
	struct iio_dev *indio = private;
	struct at91_adc_state *st = iio_priv(indio);
	u32 status = at91_adc_readl(st, AT91_ADC8XX_ISR);

	status &= at91_adc_readl(st, AT91_ADC8XX_IMR);
	if (status & 0xffff) {
		st->conversion_value = at91_adc_readl(st, st->chan->address);
		st->conversion_done = true;
		wake_up_interruptible(&st->wq_data_available);
	}

	return IRQ_HANDLED;
}

static int at91_adc_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct at91_adc_state *st = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&st->lock);

		st->chan = chan;

		at91_adc_writel(st, AT91_ADC8XX_CHER, BIT(chan->channel));
		at91_adc_writel(st, AT91_ADC8XX_IER, BIT(chan->channel));
		at91_adc_writel(st, AT91_ADC8XX_CR, AT91_ADC8XX_CR_START);

		ret = wait_event_interruptible_timeout(st->wq_data_available,
						       st->conversion_done,
						       msecs_to_jiffies(1000));
		if (ret == 0)
			ret = -ETIMEDOUT;

		if (ret > 0) {
			*val = st->conversion_value;
			ret = IIO_VAL_INT;
			st->conversion_done = false;
		}

		at91_adc_writel(st, AT91_ADC8XX_IDR, BIT(chan->channel));
		at91_adc_writel(st, AT91_ADC8XX_CHDR, BIT(chan->channel));

		mutex_unlock(&st->lock);
		return ret;

	case IIO_CHAN_INFO_SCALE:
		*val = st->vref_uv / 1000;
		*val2 = chan->scan_type.realbits;
		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

static const struct iio_info at91_adc_info = {
	.read_raw = &at91_adc_read_raw,
	.driver_module = THIS_MODULE,
	.event_attrs = &at91_adc_event_attribute_group,
};

static int at91_adc_probe(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct at91_adc_state *st;
	struct resource	*res;
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev,
					  sizeof(struct at91_adc_state));
	if (!indio_dev)
		return -ENOMEM;

	indio_dev->dev.parent = &pdev->dev;
	indio_dev->name = dev_name(&pdev->dev);
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &at91_adc_info;
	indio_dev->channels = at91_adc_channels;
	indio_dev->num_channels = ARRAY_SIZE(at91_adc_channels);

	st = iio_priv(indio_dev);

	ret = of_property_read_u32(pdev->dev.of_node, "atmel,min-sample-rate",
				   &st->soc_info.min_sample_rate);
	if (ret) {
		dev_err(&pdev->dev,
			"invalid or missing value for atmel,min-sample-rate\n");
		return ret;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "atmel,max-sample-rate",
				   &st->soc_info.max_sample_rate);
	if (ret) {
		dev_err(&pdev->dev,
			"invalid or missing value for atmel,max-sample-rate\n");
		return ret;
	}

	ret = of_property_read_u32(pdev->dev.of_node, "atmel,startup-time-ms",
				   &st->soc_info.startup_time);
	if (ret) {
		dev_err(&pdev->dev,
			"invalid or missing value for atmel,startup-time-ms\n");
		return ret;
	}

	init_waitqueue_head(&st->wq_data_available);
	mutex_init(&st->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	st->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(st->base))
		return PTR_ERR(st->base);

	st->irq = platform_get_irq(pdev, 0);
	if (st->irq <= 0) {
		if (!st->irq)
			st->irq = -ENXIO;

		return st->irq;
	}

	st->per_clk = devm_clk_get(&pdev->dev, "adc_clk");
	if (IS_ERR(st->per_clk))
		return PTR_ERR(st->per_clk);

	st->reg = devm_regulator_get(&pdev->dev, "vddana");
	if (IS_ERR(st->reg))
		return PTR_ERR(st->reg);

	st->vref = devm_regulator_get(&pdev->dev, "vref");
	if (IS_ERR(st->vref))
		return PTR_ERR(st->vref);

	ret = devm_request_irq(&pdev->dev, st->irq, at91_adc_interrupt, 0,
			       pdev->dev.driver->name, indio_dev);
	if (ret)
		return ret;

	ret = regulator_enable(st->reg);
	if (ret)
		return ret;

	ret = regulator_enable(st->vref);
	if (ret)
		goto reg_disable;

	st->vref_uv = regulator_get_voltage(st->vref);
	if (st->vref_uv <= 0) {
		ret = -EINVAL;
		goto vref_disable;
	}

	at91_adc_writel(st, AT91_ADC8XX_CR, AT91_ADC8XX_CR_SWRST);
	at91_adc_writel(st, AT91_ADC8XX_IDR, 0xffffffff);

	at91_adc_setup_samp_freq(st, st->soc_info.min_sample_rate);

	ret = clk_prepare_enable(st->per_clk);
	if (ret)
		goto vref_disable;

	ret = iio_device_register(indio_dev);
	if (ret < 0)
		goto per_clk_disable_unprepare;

	dev_info(&pdev->dev, "version: %x\n",
		 readl_relaxed(st->base + AT91_ADC8XX_VERSION));

	return 0;

per_clk_disable_unprepare:
	clk_disable_unprepare(st->per_clk);
vref_disable:
	regulator_disable(st->vref);
reg_disable:
	regulator_disable(st->reg);
	return ret;
}

static int at91_adc_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct at91_adc_state *st = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);

	clk_disable_unprepare(st->per_clk);

	regulator_disable(st->vref);
	regulator_disable(st->reg);

	return 0;
}

static const struct of_device_id at91_adc_dt_match[] = {
	{
		.compatible = "atmel,sama5d2-adc",
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, at91_adc_dt_match);

static struct platform_driver at91_adc_driver = {
	.probe = at91_adc_probe,
	.remove = at91_adc_remove,
	.driver = {
		.name = "at91_adc8xx",
		.of_match_table = at91_adc_dt_match,
	},
};
module_platform_driver(at91_adc_driver)

MODULE_AUTHOR("Ludovic Desroches <ludovic.desroches@atmel.com>");
MODULE_DESCRIPTION("Atmel AT91 ADC 8xx");
MODULE_LICENSE("GPL v2");
