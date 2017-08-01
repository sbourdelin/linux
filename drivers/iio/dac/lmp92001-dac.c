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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/iio/iio.h>
#include <linux/mfd/core.h>
#include <linux/platform_device.h>

#include <linux/mfd/lmp92001/core.h>

static int lmp92001_read_raw(struct iio_dev *indio_dev,
                                struct iio_chan_spec const *channel, int *val,
                                int *val2, long mask)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        int ret;

        switch (mask)
        {
        case IIO_CHAN_INFO_RAW:
                switch (channel->type) {
                case IIO_VOLTAGE:
                        ret = regmap_read(lmp92001->regmap,
                                        0x7F + channel->channel, val);
                        if (ret < 0)
                                return ret;

                        return IIO_VAL_INT;
                default:
                        break;
                }
                break;
        default:
                break;
        }

        return -EINVAL;
}

int lmp92001_write_raw(struct iio_dev *indio_dev,
                                struct iio_chan_spec const *channel,
                                int val, int val2, long mask)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        int ret;

        if (val > 4095)
                return -EINVAL;

        switch (mask)
        {
        case IIO_CHAN_INFO_RAW:
                switch (channel->type) {
                case IIO_VOLTAGE:
                        ret = regmap_write(lmp92001->regmap,
                                        0x7F + channel->channel, val);
                        if (ret < 0)
                                return ret;

                        return 0;
                default:
                        break;
                }
                break;
        default:
                break;
        }

        return -EINVAL;
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

        return sprintf(buf, "%s\n", cref & 1 ? "external" : "internal");
}

ssize_t lmp92001_dvref_write(struct iio_dev *indio_dev, uintptr_t private,
                         struct iio_chan_spec const *channel, const char *buf,
                         size_t len)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        unsigned int cref;
        int ret;

        if (strcmp("external\n", buf) == 0)
                cref = 1;
        else if (strcmp("internal\n", buf) == 0)
                cref = 0;
        else
                return -EINVAL;

        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CREF, 1, cref);
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

        if (cdac & 1)
                outx = "hiz";
        else
        {
                if (cdac & 2)
                        outx = "1 or dac";
                else
                        outx = "0 or dac";
        }

        return sprintf(buf, "%s\n", outx);
}

ssize_t lmp92001_outx_write(struct iio_dev *indio_dev, uintptr_t private,
                         struct iio_chan_spec const *channel, const char *buf,
                         size_t len)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        unsigned int cdac, mask;
        int ret;

        if (strcmp("hiz\n", buf) == 0)
        {
                cdac = 1;
                mask = 1;
        }
        else if (strcmp("dac\n", buf) == 0)
        {
                cdac = 0;
                mask = 1;
        }
        else if (strcmp("0\n", buf) == 0)
        {
                cdac = 0;
                mask = 3;
        }
        else if (strcmp("1\n", buf) == 0)
        {
                cdac = 2;
                mask = 3;
        }
        else
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

        return sprintf(buf, "%s\n", cdac & 4 ? "1" : "0");
}

ssize_t lmp92001_gang_write(struct iio_dev *indio_dev, uintptr_t private,
                         struct iio_chan_spec const *channel, const char *buf,
                         size_t len)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        unsigned int cdac = 0;
        int ret;

        if (strcmp("0\n", buf) == 0)
                cdac = 0;
        else if (strcmp("1\n", buf) == 0)
                cdac = 4;
        else
                return -EINVAL;

        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CDAC, 4, cdac);
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
        .scan_index = _ch, \
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

        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CDAC, 7, cdac);
        if (ret < 0)
                return ret;

        platform_set_drvdata(pdev, indio_dev);

        return iio_device_register(indio_dev);
}

static int lmp92001_dac_remove(struct platform_device *pdev)
{
        struct iio_dev *indio_dev = platform_get_drvdata(pdev);

        iio_device_unregister(indio_dev);

        return 0;
}

static struct platform_driver lmp92001_dac_driver = {
        .driver.name    = "lmp92001-dac",
        .driver.owner   = THIS_MODULE,
        .probe          = lmp92001_dac_probe,
        .remove         = lmp92001_dac_remove,
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
