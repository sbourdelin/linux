#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/iio/iio.h>
#include <linux/iio/machine.h>
#include <linux/iio/driver.h>
#include <linux/interrupt.h>
#include <linux/mfd/sunxi-gpadc-mfd.h>
#include <linux/regmap.h>

#define TP_CTRL0			0x00
#define TP_CTRL1			0x04
#define TP_CTRL2			0x08
#define TP_CTRL3			0x0c
#define TP_TPR				0x18
#define TP_CDAT				0x1c
#define TEMP_DATA			0x20
#define TP_DATA				0x24

/* TP_CTRL0 bits */
#define ADC_FIRST_DLY(x)		((x) << 24) /* 8 bits */
#define ADC_FIRST_DLY_MODE		BIT(23)
#define ADC_CLK_SELECT			BIT(22)
#define ADC_CLK_DIVIDER(x)		((x) << 20) /* 2 bits */
#define FS_DIV(x)			((x) << 16) /* 4 bits */
#define T_ACQ(x)			((x) << 0)  /* 16 bits*/

/* TP_CTRL1 bits */
#define STYLUS_UP_DEBOUNCE(x)		((x) << 12) /* 8 bits */
#define STYLUS_UP_DEBOUNCE_EN		BIT(9)
#define TOUCH_PAN_CALI_EN		BIT(6)
#define TP_DUAL_EN			BIT(5)
#define TP_MODE_EN			BIT(4)
#define TP_ADC_SELECT			BIT(3)
#define ADC_CHAN_SELECT(x)		((x) << 0)  /* 3 bits */

/* TP_CTRL1 bits for sun6i SOCs */
#define SUN6I_TOUCH_PAN_CALI_EN		BIT(7)
#define SUN6I_TP_DUAL_EN		BIT(6)
#define SUN6I_TP_MODE_EN		BIT(5)
#define SUN6I_TP_ADC_SELECT		BIT(4)
#define SUN6I_ADC_CHAN_SELECT(x)	BIT(x)  /* 4 bits */

/* TP_CTRL2 bits */
#define TP_SENSITIVE_ADJUST(x)		((x) << 28) /* 4 bits */
#define TP_MODE_SELECT(x)		((x) << 26) /* 2 bits */
#define PRE_MEA_EN			BIT(24)
#define PRE_MEA_THRE_CNT(x)		((x) << 0)  /* 24 bits*/

/* TP_CTRL3 bits */
#define FILTER_EN			BIT(2)
#define FILTER_TYPE(x)			((x) << 0)  /* 2 bits */

/* TP_INT_FIFOC irq and fifo mask / control bits */
#define TEMP_IRQ_EN			BIT(18)
#define TP_OVERRUN_IRQ_EN		BIT(17)
#define TP_DATA_IRQ_EN			BIT(16)
#define TP_DATA_XY_CHANGE		BIT(13)
#define TP_FIFO_TRIG_LEVEL(x)		((x) << 8)  /* 5 bits */
#define TP_DATA_DRQ_EN			BIT(7)
#define TP_FIFO_FLUSH			BIT(4)
#define TP_UP_IRQ_EN			BIT(1)
#define TP_DOWN_IRQ_EN			BIT(0)

/* TP_INT_FIFOS irq and fifo status bits */
#define TEMP_DATA_PENDING		BIT(18)
#define FIFO_OVERRUN_PENDING		BIT(17)
#define FIFO_DATA_PENDING		BIT(16)
#define TP_IDLE_FLG			BIT(2)
#define TP_UP_PENDING			BIT(1)
#define TP_DOWN_PENDING			BIT(0)

/* TP_TPR bits */
#define TEMP_ENABLE(x)			((x) << 16)
#define TEMP_PERIOD(x)			((x) << 0)  /*t = x * 256 * 16 / clkin*/

#define ARCH_SUN4I			BIT(0)
#define ARCH_SUN5I			BIT(1)
#define ARCH_SUN6I			BIT(2)

struct sunxi_gpadc_dev {
	void __iomem			*regs;
	struct completion		completion;
	int				temp_data;
	u32				adc_data;
	struct regmap			*regmap;
	unsigned int			fifo_data_irq;
	unsigned int			temp_data_irq;
	unsigned int			flags;
};

#define ADC_CHANNEL(_channel, _name) {				\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = _channel,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.datasheet_name = _name,				\
}

static struct iio_map sunxi_gpadc_hwmon_maps[] = {
	{
		.adc_channel_label = "temp_adc",
		.consumer_dev_name = "iio_hwmon.0",
	},
	{},
};

static const struct iio_chan_spec sunxi_gpadc_channels[] = {
	ADC_CHANNEL(0, "adc_chan0"),
	ADC_CHANNEL(1, "adc_chan1"),
	ADC_CHANNEL(2, "adc_chan2"),
	ADC_CHANNEL(3, "adc_chan3"),
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
		.datasheet_name = "temp_adc",
	},
};

static int sunxi_gpadc_adc_read(struct iio_dev *indio_dev, int channel)
{
	struct sunxi_gpadc_dev *info = iio_priv(indio_dev);
	int val = 0;

	mutex_lock(&indio_dev->mlock);

	reinit_completion(&info->completion);
	regmap_write(info->regmap, TP_CTRL1, SUN6I_TP_MODE_EN |
					     SUN6I_TP_ADC_SELECT |
					     SUN6I_ADC_CHAN_SELECT(channel));
	regmap_write(info->regmap, TP_INT_FIFOC, TP_FIFO_TRIG_LEVEL(1) |
						 TP_FIFO_FLUSH);
	enable_irq(info->fifo_data_irq);

	if (!wait_for_completion_timeout(&info->completion,
					 msecs_to_jiffies(100))) {
		disable_irq(info->fifo_data_irq);
		mutex_unlock(&indio_dev->mlock);
		return -ETIMEDOUT;
	}

	val = info->adc_data;
	disable_irq(info->fifo_data_irq);
	mutex_unlock(&indio_dev->mlock);

	return val;
}

static int sunxi_gpadc_temp_read(struct iio_dev *indio_dev)
{
	struct sunxi_gpadc_dev *info = iio_priv(indio_dev);
	int val = 0;

	mutex_lock(&indio_dev->mlock);

	reinit_completion(&info->completion);

	regmap_write(info->regmap, TP_INT_FIFOC, TP_FIFO_TRIG_LEVEL(1) |
						 TP_FIFO_FLUSH);
	regmap_write(info->regmap, TP_CTRL1, SUN6I_TP_MODE_EN);
	enable_irq(info->temp_data_irq);

	if (!wait_for_completion_timeout(&info->completion,
					 msecs_to_jiffies(100))) {
		disable_irq(info->temp_data_irq);
		mutex_unlock(&indio_dev->mlock);
		return -ETIMEDOUT;
	}

	if (info->flags & ARCH_SUN4I)
		val = info->temp_data * 133 - 257000;
	else if (info->flags & ARCH_SUN5I)
		val = info->temp_data * 100 - 144700;
	else if (info->flags & ARCH_SUN6I)
		val = info->temp_data * 167 - 271000;

	disable_irq(info->temp_data_irq);
	mutex_unlock(&indio_dev->mlock);
	return val;
}

static int sunxi_gpadc_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
		*val = sunxi_gpadc_temp_read(indio_dev);
		if (*val < 0)
			return *val;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_RAW:
		*val = sunxi_gpadc_adc_read(indio_dev, chan->channel);
		if (*val < 0)
			return *val;

		return IIO_VAL_INT;
	default:
		break;
	}

	return -EINVAL;
}

static const struct iio_info sunxi_gpadc_iio_info = {
	.read_raw = sunxi_gpadc_read_raw,
	.driver_module = THIS_MODULE,
};

static irqreturn_t sunxi_gpadc_temp_data_irq_handler(int irq, void *dev_id)
{
	struct sunxi_gpadc_dev *info = dev_id;
	int ret;

	ret = regmap_read(info->regmap, TEMP_DATA, &info->temp_data);
	if (ret == 0)
		complete(&info->completion);

	return IRQ_HANDLED;
}

static irqreturn_t sunxi_gpadc_fifo_data_irq_handler(int irq, void *dev_id)
{
	struct sunxi_gpadc_dev *info = dev_id;
	int ret;

	ret = regmap_read(info->regmap, TP_DATA, &info->adc_data);
	if (ret == 0)
		complete(&info->completion);

	return IRQ_HANDLED;
}

static int sunxi_gpadc_probe(struct platform_device *pdev)
{
	struct sunxi_gpadc_dev *info = NULL;
	struct iio_dev *indio_dev = NULL;
	int ret = 0;
	unsigned int irq;
	struct sunxi_gpadc_mfd_dev *sunxi_gpadc_mfd_dev;

	sunxi_gpadc_mfd_dev = dev_get_drvdata(pdev->dev.parent);

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*info));
	if (!indio_dev) {
		dev_err(&pdev->dev, "failed to allocate iio device.\n");
		return -ENOMEM;
	}
	info = iio_priv(indio_dev);

	info->regmap = sunxi_gpadc_mfd_dev->regmap;
	init_completion(&info->completion);
	indio_dev->name = dev_name(&pdev->dev);
	indio_dev->dev.parent = &pdev->dev;
	indio_dev->dev.of_node = pdev->dev.of_node;
	indio_dev->info = &sunxi_gpadc_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->num_channels = ARRAY_SIZE(sunxi_gpadc_channels);
	indio_dev->channels = sunxi_gpadc_channels;

	info->flags = platform_get_device_id(pdev)->driver_data;

	regmap_write(info->regmap, TP_CTRL0, ADC_CLK_DIVIDER(2) |
					     FS_DIV(7) |
					     T_ACQ(63));
	regmap_write(info->regmap, TP_CTRL1, SUN6I_TP_MODE_EN);
	regmap_write(info->regmap, TP_CTRL3, FILTER_EN | FILTER_TYPE(1));
	regmap_write(info->regmap, TP_TPR, TEMP_ENABLE(1) | TEMP_PERIOD(1953));

	irq = platform_get_irq_byname(pdev, "TEMP_DATA_PENDING");
	if (irq < 0) {
		dev_err(&pdev->dev,
			"no TEMP_DATA_PENDING interrupt registered\n");
		return irq;
	}

	irq = regmap_irq_get_virq(sunxi_gpadc_mfd_dev->regmap_irqc, irq);
	ret = devm_request_any_context_irq(&pdev->dev, irq,
					   sunxi_gpadc_temp_data_irq_handler,
					   0, "temp_data", info);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"could not request TEMP_DATA_PENDING interrupt: %d\n",
			ret);
		return ret;
	}

	info->temp_data_irq = irq;
	disable_irq(irq);

	irq = platform_get_irq_byname(pdev, "FIFO_DATA_PENDING");
	if (irq < 0) {
		dev_err(&pdev->dev,
			"no FIFO_DATA_PENDING interrupt registered\n");
		return irq;
	}

	irq = regmap_irq_get_virq(sunxi_gpadc_mfd_dev->regmap_irqc, irq);
	ret = devm_request_any_context_irq(&pdev->dev, irq,
					   sunxi_gpadc_fifo_data_irq_handler,
					   0, "fifo_data", info);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"could not request FIFO_DATA_PENDING interrupt: %d\n",
			ret);
		return ret;
	}

	info->fifo_data_irq = irq;
	disable_irq(irq);

	ret = iio_map_array_register(indio_dev, sunxi_gpadc_hwmon_maps);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register iio map array\n");
		return ret;
	}

	platform_set_drvdata(pdev, indio_dev);

	ret = iio_device_register(indio_dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not register the device\n");
		iio_map_array_unregister(indio_dev);
		return ret;
	}

	dev_info(&pdev->dev, "successfully loaded\n");

	return ret;
}

static int sunxi_gpadc_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = NULL;
	struct sunxi_gpadc_dev *info = NULL;

	iio_device_unregister(indio_dev);
	iio_map_array_unregister(indio_dev);
	info = iio_priv(indio_dev);
	regmap_write(info->regmap, TP_INT_FIFOC, 0);

	return 0;
}

static const struct platform_device_id sunxi_gpadc_id[] = {
	{ "sun4i-a10-gpadc-iio", ARCH_SUN4I },
	{ "sun5i-a13-gpadc-iio", ARCH_SUN5I },
	{ "sun6i-a31-gpadc-iio", ARCH_SUN6I },
	{ /*sentinel*/ },
};

static struct platform_driver sunxi_gpadc_driver = {
	.driver = {
		.name = "sunxi-gpadc-iio",
	},
	.id_table = sunxi_gpadc_id,
	.probe = sunxi_gpadc_probe,
	.remove = sunxi_gpadc_remove,
};

module_platform_driver(sunxi_gpadc_driver);

MODULE_DESCRIPTION("ADC driver for sunxi platforms");
MODULE_AUTHOR("Quentin Schulz <quentin.schulz@free-electrons.com>");
MODULE_LICENSE("GPL v2");
