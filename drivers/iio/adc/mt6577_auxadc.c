/*
 * Copyright (c) 2016 MediaTek Inc.
 * Author: Zhiyong Tao <zhiyong.tao@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/iio/iio.h>

/* Registers definitions */
#define MT65XX_AUXADC_CON0                    0x00
#define MT65XX_AUXADC_CON1                    0x04
#define MT65XX_AUXADC_CON2                    0x10
#define MT65XX_AUXADC_STA                     BIT(0)

#define MT65XX_AUXADC_DAT0                    0x14
#define MT65XX_AUXADC_RDY0                    BIT(12)

#define MT65XX_AUXADC_MISC                    0x94
#define MT65XX_AUXADC_PDN_EN                  BIT(14)

#define MT65XX_AUXADC_DAT_MASK                0xfff
#define MT65XX_AUXADC_SLEEP_US                1000
#define MT65XX_AUXADC_TIMEOUT_US              10000
#define MT65XX_AUXADC_POWER_READY_MS          1
#define MT65XX_AUXADC_SAMPLE_READY_US         25

struct mt65xx_auxadc_device {
	void __iomem *reg_base;
	struct clk *adc_clk;
	struct mutex lock;
	unsigned int power_ready_ms;
	unsigned int sample_ready_us;
};

#define MT65XX_AUXADC_CHANNEL(idx) {				    \
		.type = IIO_VOLTAGE,				    \
		.indexed = 1,					    \
		.channel = (idx),				    \
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED), \
}

static const struct iio_chan_spec mt65xx_auxadc_iio_channels[] = {
	MT65XX_AUXADC_CHANNEL(0),
	MT65XX_AUXADC_CHANNEL(1),
	MT65XX_AUXADC_CHANNEL(2),
	MT65XX_AUXADC_CHANNEL(3),
	MT65XX_AUXADC_CHANNEL(4),
	MT65XX_AUXADC_CHANNEL(5),
	MT65XX_AUXADC_CHANNEL(6),
	MT65XX_AUXADC_CHANNEL(7),
	MT65XX_AUXADC_CHANNEL(8),
	MT65XX_AUXADC_CHANNEL(9),
	MT65XX_AUXADC_CHANNEL(10),
	MT65XX_AUXADC_CHANNEL(11),
	MT65XX_AUXADC_CHANNEL(12),
	MT65XX_AUXADC_CHANNEL(13),
	MT65XX_AUXADC_CHANNEL(14),
	MT65XX_AUXADC_CHANNEL(15),
};

static int mt65xx_auxadc_read(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan)
{
	u32 rawdata, val;
	void __iomem *reg_channel;
	int ret;
	struct mt65xx_auxadc_device *adc_dev = iio_priv(indio_dev);

	reg_channel = adc_dev->reg_base + MT65XX_AUXADC_DAT0 +
		      chan->channel * 0x04;

	mutex_lock(&adc_dev->lock);

	val = readl(adc_dev->reg_base + MT65XX_AUXADC_CON1);
	val &= ~(1 << chan->channel);
	writel(val, adc_dev->reg_base + MT65XX_AUXADC_CON1);

	/* read channel and make sure old ready bit == 0 */
	ret = readl_poll_timeout(reg_channel, val,
				 ((val & MT65XX_AUXADC_RDY0) == 0),
				 MT65XX_AUXADC_SLEEP_US,
				 MT65XX_AUXADC_TIMEOUT_US);
	if (ret < 0) {
		dev_err(indio_dev->dev.parent,
			"wait for channel[%d] ready bit clear time out\n",
			chan->channel);

		mutex_unlock(&adc_dev->lock);

		return -EINVAL;
	}

	/* set bit to trigger sample */
	val = readl(adc_dev->reg_base + MT65XX_AUXADC_CON1);
	val |= 1 << chan->channel;
	writel(val, adc_dev->reg_base + MT65XX_AUXADC_CON1);

	/* we must delay here for hardware sample channel data */
	udelay(adc_dev->sample_ready_us);

	/* check MTK_AUXADC_CON2 if auxadc is idle */
	ret = readl_poll_timeout(adc_dev->reg_base + MT65XX_AUXADC_CON2, val,
				 ((val & MT65XX_AUXADC_STA) == 0),
				 MT65XX_AUXADC_SLEEP_US,
				 MT65XX_AUXADC_TIMEOUT_US);
	if (ret < 0) {
		dev_err(indio_dev->dev.parent,
			"wait for auxadc idle time out\n");

		mutex_unlock(&adc_dev->lock);

		return -EINVAL;
	}

	/* read channel and make sure ready bit == 1 */
	ret = readl_poll_timeout(reg_channel, val,
				 ((val & MT65XX_AUXADC_RDY0) != 0),
				 MT65XX_AUXADC_SLEEP_US,
				 MT65XX_AUXADC_TIMEOUT_US);
	if (ret < 0) {
		dev_err(indio_dev->dev.parent,
			"wait for channel[%d] data ready time out\n",
			chan->channel);

		mutex_unlock(&adc_dev->lock);

		return -EINVAL;
	}

	/* read data */
	rawdata = readl(reg_channel) & MT65XX_AUXADC_DAT_MASK;

	mutex_unlock(&adc_dev->lock);

	return rawdata;
}

static int mt65xx_auxadc_read_raw(struct iio_dev *indio_dev,
				  struct iio_chan_spec const *chan,
				  int *val,
				  int *val2,
				  long info)
{
	int ret;

	switch (info) {
	case IIO_CHAN_INFO_PROCESSED:
		*val = mt65xx_auxadc_read(indio_dev, chan);
		if (*val < 0) {
			dev_err(indio_dev->dev.parent,
				"failed to sample data on channel[%d]\n",
				chan->channel);
			return -EIO;
		}
		ret = IIO_VAL_INT;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct iio_info mt65xx_auxadc_info = {
	.driver_module = THIS_MODULE,
	.read_raw = &mt65xx_auxadc_read_raw,
};

static int mt65xx_auxadc_probe(struct platform_device *pdev)
{
	struct mt65xx_auxadc_device *adc_dev;
	unsigned long adc_clk_rate;
	struct resource *res;
	struct iio_dev *indio_dev;
	int ret;
	u32 val;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*adc_dev));
	if (!indio_dev)
		return -ENOMEM;

	adc_dev = iio_priv(indio_dev);
	indio_dev->dev.parent = &pdev->dev;
	indio_dev->name = dev_name(&pdev->dev);
	indio_dev->info = &mt65xx_auxadc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = mt65xx_auxadc_iio_channels;
	indio_dev->num_channels = ARRAY_SIZE(mt65xx_auxadc_iio_channels);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	adc_dev->reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(adc_dev->reg_base)) {
		dev_err(&pdev->dev, "failed to get auxadc base address.\n");
		return PTR_ERR(adc_dev->reg_base);
	}

	adc_dev->adc_clk = devm_clk_get(&pdev->dev, "main");
	if (IS_ERR(adc_dev->adc_clk)) {
		dev_err(&pdev->dev, "failed to get auxadc clock\n");
		return PTR_ERR(adc_dev->adc_clk);
	}

	ret = clk_prepare_enable(adc_dev->adc_clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable auxadc clock\n");
		return ret;
	}

	adc_clk_rate = clk_get_rate(adc_dev->adc_clk);
	if (!adc_clk_rate) {
		dev_err(&pdev->dev, "null clock rate!\n");
		goto err_disable_clk;
	}

	adc_dev->power_ready_ms = MT65XX_AUXADC_POWER_READY_MS;
	adc_dev->sample_ready_us = MT65XX_AUXADC_SAMPLE_READY_US;

	mutex_init(&adc_dev->lock);

	val = readl(adc_dev->reg_base + MT65XX_AUXADC_MISC);
	val |= MT65XX_AUXADC_PDN_EN;
	writel(val, adc_dev->reg_base + MT65XX_AUXADC_MISC);
	mdelay(adc_dev->power_ready_ms);

	val = readl(adc_dev->reg_base + MT65XX_AUXADC_MISC);
	if ((val & MT65XX_AUXADC_PDN_EN) == 0) {
		dev_err(&pdev->dev, "failed to enable auxadc power!\n");
		goto err_disable_clk;
	}

	platform_set_drvdata(pdev, indio_dev);

	ret = iio_device_register(indio_dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register iio device!\n");
		return ret;
	}

	return 0;

err_disable_clk:
	clk_disable_unprepare(adc_dev->adc_clk);
	return -EINVAL;
}

static int mt65xx_auxadc_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct mt65xx_auxadc_device *adc_dev = iio_priv(indio_dev);
	u32 val;

	iio_device_unregister(indio_dev);

	val = readl(adc_dev->reg_base + MT65XX_AUXADC_MISC);
	val &= ~MT65XX_AUXADC_PDN_EN;
	writel(val, adc_dev->reg_base + MT65XX_AUXADC_MISC);

	clk_disable_unprepare(adc_dev->adc_clk);

	return 0;
}

static const struct of_device_id mt65xx_auxadc_of_match[] = {
	{ .compatible = "mediatek,mt2701-auxadc", },
	{ .compatible = "mediatek,mt8173-auxadc", },
	{ }
};
MODULE_DEVICE_TABLE(of, mt65xx_auxadc_of_match);

static struct platform_driver mt65xx_auxadc_driver = {
	.driver = {
		.name   = "mt65xx-auxadc",
		.of_match_table = mt65xx_auxadc_of_match,
	},
	.probe	= mt65xx_auxadc_probe,
	.remove	= mt65xx_auxadc_remove,
};
module_platform_driver(mt65xx_auxadc_driver);

MODULE_AUTHOR("Zhiyong Tao <zhiyong.tao@mediatek.com>");
MODULE_DESCRIPTION("MTK AUXADC Device Driver");
MODULE_LICENSE("GPL v2");
