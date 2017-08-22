/*
 * lmp92001-dac.c - Support for TI LMP92001 DACs
 *
 * Copyright 2016-2017 Celestica Ltd.
 *
 * Author: Abhisit Sangjan <s.abhisit@gmail.com>
 *
 * Inspired by wm831x driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/iio/iio.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <linux/mfd/lmp92001/core.h>

#define CREF_DEXT	(1 << 0) /* 1 - DAC external reference.
				  * 0 - DAC internal reference.
				  */
#define CDAC_OFF	(1 << 0) /* 1 - Forces all outputs to high impedance. */
#define CDAC_OLVL	(1 << 1) /* 1 - Cy=0 will force associated OUTx outputs
				  *     to VDD.
				  * 0 - Cy=0 will force associated OUTx outputs
				  *     to GND.
				  */
#define CDAC_GANG	(1 << 2) /* Controls the association of analog output
				  * channels OUTx with asynchronous control
				  * inputs Cy.
				  *
				  *         Cy to OUTx Assignment
				  * --------------------------------------
				  * | Cy | CDAC:GANG = 0 | CDAC:GANG = 1 |
				  * --------------------------------------
				  * | C1 | OUT[1:4]      | OUT[1:3]      |
				  * --------------------------------------
				  * | C2 | OUT[5:6]      | OUT[4:6]      |
				  * --------------------------------------
				  * | C3 | OUT[7:8]      | OUT[7:9]      |
				  * --------------------------------------
				  * | C4 | OUT[9:12]     | OUT[10:12]    |
				  * --------------------------------------
				  */

static int lmp92001_read_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *channel,
	int *val, int *val2,
	long mask)
{
	struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
	int ret;

	mutex_lock(&lmp92001->dac_lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (channel->type) {
		case IIO_VOLTAGE:
			ret = regmap_read(lmp92001->regmap,
					0x7F + channel->channel, val);
			if (ret < 0)
				goto exit;

			ret = IIO_VAL_INT;
			goto exit;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	/* In case of no match channel info/type is return here. */
	ret = -EINVAL;

exit:
	mutex_unlock(&lmp92001->dac_lock);

	return ret;
}

int lmp92001_write_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *channel,
	int val, int val2,
	long mask)
{
	struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
	int ret;

	mutex_lock(&lmp92001->dac_lock);

	if (val < 0 || val > 4095) {
		ret = -EINVAL;
		goto exit;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (channel->type) {
		case IIO_VOLTAGE:
			ret = regmap_write(lmp92001->regmap,
					0x7F + channel->channel, val);
			if (ret < 0)
				goto exit;

			ret = 0;
			goto exit;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	/* In case of no match channel info/type is return here. */
	ret = -EINVAL;

exit:
	mutex_unlock(&lmp92001->dac_lock);

	return ret;
}

static const struct iio_info lmp92001_info = {
	.read_raw = lmp92001_read_raw,
	.write_raw = lmp92001_write_raw,
	.driver_module = THIS_MODULE,
};

ssize_t lmp92001_dvref_read(struct iio_dev *indio_dev, uintptr_t private,
	struct iio_chan_spec const *channel, char *buf)
{
	struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
	unsigned int cref;
	int ret;

	ret = regmap_read(lmp92001->regmap, LMP92001_CREF, &cref);
	if (ret < 0)
		return ret;

	return sprintf(buf, "%s\n", cref & CREF_DEXT ? "external" : "internal");
}

ssize_t lmp92001_dvref_write(struct iio_dev *indio_dev, uintptr_t private,
	struct iio_chan_spec const *channel, const char *buf, size_t len)
{
	struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
	unsigned int cref;
	int ret;

	if (strncmp("external", buf, 8) == 0)
		cref = 1;
	else if (strncmp("internal", buf, 8) == 0)
		cref = 0;
	else
		return -EINVAL;

	ret = regmap_update_bits(lmp92001->regmap, LMP92001_CREF, CREF_DEXT,
					cref);
	if (ret < 0)
		return ret;

	return len;
}

ssize_t lmp92001_outx_read(struct iio_dev *indio_dev, uintptr_t private,
	struct iio_chan_spec const *channel, char *buf)
{
	struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
	unsigned int cdac;
	const char *outx;
	int ret;

	ret = regmap_read(lmp92001->regmap, LMP92001_CDAC, &cdac);
	if (ret < 0)
		return ret;

	if (cdac & CDAC_OFF)
		outx = "hiz";
	else {
		if (cdac & CDAC_OLVL)
			outx = "1 or dac";
		else
			outx = "0 or dac";
	}

	return sprintf(buf, "%s\n", outx);
}

ssize_t lmp92001_outx_write(struct iio_dev *indio_dev, uintptr_t private,
	struct iio_chan_spec const *channel, const char *buf, size_t len)
{
	struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
	unsigned int cdac, mask;
	int ret;

	if (strncmp("hiz", buf, 3) == 0) {
		cdac = CDAC_OFF;
		mask = CDAC_OFF;
	} else if (strncmp("dac", buf, 3) == 0) {
		cdac = ~CDAC_OFF;
		mask = CDAC_OFF;
	} else if (strncmp("0", buf, 1) == 0) {
		cdac = ~(CDAC_OLVL | CDAC_OFF);
		mask = CDAC_OLVL | CDAC_OFF;
	} else if (strncmp("1", buf, 1) == 0) {
		cdac = CDAC_OLVL;
		mask = CDAC_OLVL | CDAC_OFF;
	} else
		return -EINVAL;

	ret = regmap_update_bits(lmp92001->regmap, LMP92001_CDAC, mask, cdac);
	if (ret < 0)
		return ret;

	return len;
}

ssize_t lmp92001_gang_read(struct iio_dev *indio_dev, uintptr_t private,
	struct iio_chan_spec const *channel, char *buf)
{
	struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
	unsigned int cdac;
	int ret;

	ret = regmap_read(lmp92001->regmap, LMP92001_CDAC, &cdac);
	if (ret < 0)
		return ret;

	return sprintf(buf, "%s\n", cdac & CDAC_GANG ? "1" : "0");
}

ssize_t lmp92001_gang_write(struct iio_dev *indio_dev, uintptr_t private,
	struct iio_chan_spec const *channel, const char *buf, size_t len)
{
	struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
	unsigned int cdac = 0;
	int ret;

	if (strncmp("0", buf, 1) == 0)
		cdac = ~CDAC_GANG;
	else if (strncmp("1", buf, 1) == 0)
		cdac = CDAC_GANG;
	else
		return -EINVAL;

	ret = regmap_update_bits(lmp92001->regmap, LMP92001_CDAC, CDAC_GANG,
					cdac);
	if (ret < 0)
		return ret;

	return len;
}

static const struct iio_chan_spec_ext_info lmp92001_ext_info[] = {
	{
		.name = "vref",
		.read = lmp92001_dvref_read,
		.write = lmp92001_dvref_write,
		.shared = IIO_SHARED_BY_ALL,
	},
	{
		.name = "outx",
		.read = lmp92001_outx_read,
		.write = lmp92001_outx_write,
		.shared = IIO_SHARED_BY_ALL,
	},
	{
		.name = "gang",
		.read = lmp92001_gang_read,
		.write = lmp92001_gang_write,
		.shared = IIO_SHARED_BY_ALL,
	},
	{ },
};

#define LMP92001_CHAN_SPEC(_ch) \
{ \
	.channel = _ch, \
	.type = IIO_VOLTAGE, \
	.indexed = 1, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
	.ext_info = lmp92001_ext_info, \
	.output = 1, \
}

static const struct iio_chan_spec lmp92001_dac_channels[] = {
	LMP92001_CHAN_SPEC(1),
	LMP92001_CHAN_SPEC(2),
	LMP92001_CHAN_SPEC(3),
	LMP92001_CHAN_SPEC(4),
	LMP92001_CHAN_SPEC(5),
	LMP92001_CHAN_SPEC(6),
	LMP92001_CHAN_SPEC(7),
	LMP92001_CHAN_SPEC(8),
	LMP92001_CHAN_SPEC(9),
	LMP92001_CHAN_SPEC(10),
	LMP92001_CHAN_SPEC(11),
	LMP92001_CHAN_SPEC(12),
};

static int lmp92001_dac_probe(struct platform_device *pdev)
{
	struct lmp92001 *lmp92001 = dev_get_drvdata(pdev->dev.parent);
	struct iio_dev *indio_dev;
	struct device_node *np = pdev->dev.of_node;
	u8 gang = 0, outx = 0, hiz = 0;
	unsigned int cdac = 0;
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*lmp92001));
	if (!indio_dev)
		return -ENOMEM;

	mutex_init(&lmp92001->dac_lock);

	iio_device_set_drvdata(indio_dev, lmp92001);

	indio_dev->name = pdev->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &lmp92001_info;
	indio_dev->channels = lmp92001_dac_channels;
	indio_dev->num_channels = ARRAY_SIZE(lmp92001_dac_channels);

	of_property_read_u8(np, "ti,lmp92001-dac-hiz", &hiz);
	cdac |= hiz;

	of_property_read_u8(np, "ti,lmp92001-dac-outx", &outx);
	cdac |= outx << 1;

	of_property_read_u8(np, "ti,lmp92001-dac-gang", &gang);
	cdac |= gang << 2;

	ret = regmap_update_bits(lmp92001->regmap, LMP92001_CDAC,
					CDAC_GANG | CDAC_OLVL | CDAC_OFF, cdac);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, indio_dev);

	return devm_iio_device_register(&pdev->dev, indio_dev);
}

static int lmp92001_dac_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);

	devm_iio_device_unregister(&pdev->dev, indio_dev);

	return 0;
}

static struct platform_driver lmp92001_dac_driver = {
	.driver.name	= "lmp92001-dac",
	.probe		= lmp92001_dac_probe,
	.remove		= lmp92001_dac_remove,
};

static int __init lmp92001_dac_init(void)
{
	return platform_driver_register(&lmp92001_dac_driver);
}
subsys_initcall(lmp92001_dac_init);

static void __exit lmp92001_dac_exit(void)
{
	platform_driver_unregister(&lmp92001_dac_driver);
}
module_exit(lmp92001_dac_exit);

MODULE_AUTHOR("Abhisit Sangjan <s.abhisit@gmail.com>");
MODULE_DESCRIPTION("IIO DAC interface for TI LMP92001");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:lmp92001-dac");
