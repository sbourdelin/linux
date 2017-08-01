/*
 * lmp92001-adc.c - Support for TI LMP92001 ADCs
 *
 * Copyright 2016-2017 Celestica Ltd.
 *
 * Author: Abhisit Sangjan <s.abhisit@gmail.com>
 *
 * Inspired by wm831x and ad5064 drivers.
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
#include <linux/interrupt.h>

#include <linux/mfd/lmp92001/core.h>

static int lmp92001_read_raw(struct iio_dev *indio_dev,
                                struct iio_chan_spec const *channel, int *val,
                                int *val2, long mask)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        unsigned int code, cgen, sgen, try;
        int ret;

        ret = regmap_read(lmp92001->regmap, LMP92001_CGEN, &cgen);
        if (ret < 0)
                return ret;

        /*
         * Is not continuous conversion?
         * Lock the registers (if needed).
         * Triggering single-short conversion.
         * Waiting for conversion successfully.
         */
        if (!(cgen & 1))
        {
                if (!(cgen & 2))
                {
                        ret = regmap_update_bits(lmp92001->regmap,
                                                        LMP92001_CGEN, 2, 2);
                        if (ret < 0)
                                return ret;
                }

                ret = regmap_write(lmp92001->regmap, LMP92001_CTRIG, 1);
                if (ret < 0)
                        return ret;

                try = 10;
                do {
                        ret = regmap_read(lmp92001->regmap,
                                                LMP92001_SGEN, &sgen);
                        if(ret < 0)
                                return ret;
                } while ((sgen & 1<<7) && (--try > 0));

                if (!try)
                        return -ETIME;
        }

        ret = regmap_read(lmp92001->regmap, 0x1F + channel->channel, &code);
        if (ret < 0)
                return ret;

        switch (mask)
        {
        case IIO_CHAN_INFO_RAW:
                switch (channel->type) {
                case IIO_VOLTAGE:
                case IIO_TEMP:
                        *val = code;
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

/*
 * TODO: do your attributes even handler for
 * Current limit low/high for CH 1-3, 9-11!
 * In case INT1 and INT2 were connected to i.MX6.
 */
static const struct iio_info lmp92001_info = {
        .read_raw = lmp92001_read_raw,
        .driver_module = THIS_MODULE,
};

static ssize_t lmp92001_avref_read(struct iio_dev *indio_dev, uintptr_t private,
                        struct iio_chan_spec const *channel, char *buf)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        unsigned int cref;
        int ret;

        ret = regmap_read(lmp92001->regmap, LMP92001_CREF, &cref);
        if (ret < 0)
                return ret;

        return sprintf(buf, "%s\n", cref & 2 ? "external" : "internal");
}

static ssize_t lmp92001_avref_write(struct iio_dev *indio_dev, uintptr_t private,
                         struct iio_chan_spec const *channel, const char *buf,
                         size_t len)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        unsigned int cref;
        int ret;

        if (strcmp("external\n", buf) == 0)
                cref = 2;
        else if (strcmp("internal\n", buf) == 0)
                cref = 0;
        else
                return -EINVAL;

        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CREF, 2, cref);
        if (ret < 0)
                return ret;

        return len;
}

static ssize_t lmp92001_enable_read(struct iio_dev *indio_dev, uintptr_t private,
                        struct iio_chan_spec const *channel, char *buf)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        unsigned int reg, cad;
        int ret;

        switch (channel->channel)
        {
        case 1 ... 8:
                reg = LMP92001_CAD1;
                break;
        case 9 ... 16:
                reg = LMP92001_CAD2;
                break;
        case 17:
                reg = LMP92001_CAD3;
                break;
        default:
                return -EINVAL;
        }

        ret = regmap_read(lmp92001->regmap, reg, &cad);
        if (ret < 0)
                return ret;

        if (channel->channel <= 8)
                cad >>= channel->channel - 1;
        else if(channel->channel > 8)
                cad >>= channel->channel - 9;
        else if(channel->channel > 16)
                cad >>= channel->channel - 17;
        else
                return -EINVAL;

        return sprintf(buf, "%s\n", cad & 1 ? "enable" : "disable");
}

static ssize_t lmp92001_enable_write(struct iio_dev *indio_dev, uintptr_t private,
                         struct iio_chan_spec const *channel, const char *buf,
                         size_t len)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        unsigned int reg, enable, shif, mask;
        int ret;

        switch (channel->channel)
        {
        case 1 ... 8:
                reg = LMP92001_CAD1;
                shif = (channel->channel - 1);
                break;
        case 9 ... 16:
                reg = LMP92001_CAD2;
                shif = (channel->channel - 9);
                break;
        case 17:
                reg = LMP92001_CAD3;
                shif = (channel->channel - 17);
                break;
        default:
                return -EINVAL;
        }

        if (strcmp("enable\n", buf) == 0)
                enable = 1;
        else if (strcmp("disable\n", buf) == 0)
                enable = 0;
        else
                return -EINVAL;

        enable <<= shif;
        mask = 1 << shif;

        ret = regmap_update_bits(lmp92001->regmap, reg, mask, enable);
        if (ret < 0)
                return ret;

        return len;
}

static ssize_t lmp92001_mode_read(struct iio_dev *indio_dev, uintptr_t private,
                        struct iio_chan_spec const *channel, char *buf)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        unsigned int cgen;
        int ret;

        ret = regmap_read(lmp92001->regmap, LMP92001_CGEN, &cgen);
        if (ret < 0)
                return ret;

        return sprintf(buf, "%s\n", cgen & 1 ? "continuous" : "single-shot");
}

static ssize_t lmp92001_mode_write(struct iio_dev *indio_dev, uintptr_t private,
                         struct iio_chan_spec const *channel, const char *buf,
                         size_t len)
{
        struct lmp92001 *lmp92001 = iio_device_get_drvdata(indio_dev);
        unsigned int cgen;
        int ret;

        if (strcmp("continuous\n", buf) == 0)
                cgen = 1;
        else if (strcmp("single-shot\n", buf) == 0)
                cgen = 0;
        else
                return -EINVAL;

        /*
         * Unlock the registers.
         * Set conversion mode.
         * Lock the registers.
         */
        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CGEN, 2, 0);
        if (ret < 0)
                return ret;

        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CGEN, 1, cgen);
        if (ret < 0)
                return ret;

        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CGEN, 2, 2);
        if (ret < 0)
                return ret;

        return len;
}

static const struct iio_chan_spec_ext_info lmp92001_ext_info[] = {
        {
                .name = "vref",
                .read = lmp92001_avref_read,
                .write = lmp92001_avref_write,
                .shared = IIO_SHARED_BY_ALL,
        },
        {
                .name = "en",
                .read = lmp92001_enable_read,
                .write = lmp92001_enable_write,
                .shared = IIO_SEPARATE,
        },
        {
                .name = "mode",
                .read = lmp92001_mode_read,
                .write = lmp92001_mode_write,
                .shared = IIO_SHARED_BY_ALL,
        },
        { },
};

static const struct iio_event_spec lmp92001_events[] = {
        {
                .type = IIO_EV_TYPE_THRESH,
                .dir = IIO_EV_DIR_RISING,
                .mask_separate = BIT(IIO_EV_INFO_ENABLE) |
                        BIT(IIO_EV_INFO_VALUE),
        },
        {
                .type = IIO_EV_TYPE_THRESH,
                .dir = IIO_EV_DIR_FALLING,
                .mask_separate = BIT(IIO_EV_INFO_ENABLE) |
                        BIT(IIO_EV_INFO_VALUE),
        },
        { },
};

#define LMP92001_CHAN_SPEC(_ch, _type, _event, _nevent) \
{ \
        .channel = _ch, \
        .scan_index = _ch, \
        .scan_type = { \
                        .sign = 'u', \
                        .realbits = 12, \
                        .storagebits = 16, \
                        .repeat = 1, \
                        .endianness = IIO_BE, \
        }, \
        .type = _type, \
        .indexed = 1, \
        .info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
        .event_spec = _event, \
        .num_event_specs = _nevent, \
        .ext_info = lmp92001_ext_info, \
}

/*
 * TODO: do your ext_info for current low/high limit.
 * Example driver/iio/dac/ad5064.c
 */
static const struct iio_chan_spec lmp92001_adc_channels[] = {
        LMP92001_CHAN_SPEC( 1, IIO_VOLTAGE, lmp92001_events, ARRAY_SIZE(lmp92001_events)),
        LMP92001_CHAN_SPEC( 2, IIO_VOLTAGE, lmp92001_events, ARRAY_SIZE(lmp92001_events)),
        LMP92001_CHAN_SPEC( 3, IIO_VOLTAGE, lmp92001_events, ARRAY_SIZE(lmp92001_events)),
        LMP92001_CHAN_SPEC( 4, IIO_VOLTAGE, NULL, 0),
        LMP92001_CHAN_SPEC( 5, IIO_VOLTAGE, NULL, 0),
        LMP92001_CHAN_SPEC( 6, IIO_VOLTAGE, NULL, 0),
        LMP92001_CHAN_SPEC( 7, IIO_VOLTAGE, NULL, 0),
        LMP92001_CHAN_SPEC( 8, IIO_VOLTAGE, NULL, 0),
        LMP92001_CHAN_SPEC( 9, IIO_VOLTAGE, lmp92001_events, ARRAY_SIZE(lmp92001_events)),
        LMP92001_CHAN_SPEC(10, IIO_VOLTAGE, lmp92001_events, ARRAY_SIZE(lmp92001_events)),
        LMP92001_CHAN_SPEC(11, IIO_VOLTAGE, lmp92001_events, ARRAY_SIZE(lmp92001_events)),
        LMP92001_CHAN_SPEC(12, IIO_VOLTAGE, NULL, 0),
        LMP92001_CHAN_SPEC(13, IIO_VOLTAGE, NULL, 0),
        LMP92001_CHAN_SPEC(14, IIO_VOLTAGE, NULL, 0),
        LMP92001_CHAN_SPEC(15, IIO_VOLTAGE, NULL, 0),
        LMP92001_CHAN_SPEC(16, IIO_VOLTAGE, NULL, 0),
        LMP92001_CHAN_SPEC(17,    IIO_TEMP, NULL, 0),
};

static int lmp92001_adc_probe(struct platform_device *pdev)
{
        struct lmp92001 *lmp92001 = dev_get_drvdata(pdev->dev.parent);
        struct iio_dev *indio_dev;
        struct device_node *np = pdev->dev.of_node;
        const char *conversion;
        unsigned int cgen = 0, cad1, cad2, cad3;
        u32 mask;
        int ret;

        indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*lmp92001));
        if (!indio_dev)
                return -ENOMEM;

        iio_device_set_drvdata(indio_dev, lmp92001);

        indio_dev->name = pdev->name;
        indio_dev->dev.parent = &pdev->dev;
        indio_dev->modes = INDIO_DIRECT_MODE;
        indio_dev->info = &lmp92001_info;
        indio_dev->channels = lmp92001_adc_channels;
        indio_dev->num_channels = ARRAY_SIZE(lmp92001_adc_channels);

        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CGEN, 0x80, 0x80);
        if (ret < 0)
        {
                dev_err(&pdev->dev,"failed to self reset all registers\n");
                return ret;
        }

        ret = of_property_read_u32(np, "ti,lmp92001-adc-mask", &mask);
        if (ret < 0)
        {
                cad1 = cad2 = cad3 = 0xFF;
                dev_info(&pdev->dev, "turn on all of channels by default\n");
        }
        else
        {
                cad1 = mask & 0xFF;
                cad2 = (mask >> 8) & 0xFF;
                cad3 = (mask >> 16) & 0xFF;
        }

        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CAD1, 0xFF, cad1);
        if (ret < 0)
        {
                dev_err(&pdev->dev,"failed to enable channels 1-8\n");
                return ret;
        }

        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CAD2, 0xFF, cad2);
        if (ret < 0)
        {
                dev_err(&pdev->dev, "failed to enable channels 9-16\n");
                return ret;
        }

        ret = regmap_update_bits(lmp92001->regmap, LMP92001_CAD3, 1, cad3);
        if (ret < 0)
        {
                dev_err(&pdev->dev, "failed to enable channel 17 (temperature)\n");
                return ret;
        }

        ret = of_property_read_string_index(np, "ti,lmp92001-adc-mode", 0,
                                                &conversion);
        if (!ret)
        {
                if (strcmp("continuous", conversion) == 0)
                        cgen |= 1;
                else if (strcmp("single-shot", conversion) == 0)
                        { /* Okay */ }
                else
                        dev_warn(&pdev->dev,
                        "wrong adc mode! set to single-short conversion\n");
        }
        else
                dev_info(&pdev->dev,
                "single-short conversion was chosen by default\n");

        /*
         * Lock the registers and set conversion mode.
         */
        ret = regmap_update_bits(lmp92001->regmap,
                                        LMP92001_CGEN, 3, cgen | 2);
        if (ret < 0)
                return ret;

        platform_set_drvdata(pdev, indio_dev);

        return iio_device_register(indio_dev);
}

static int lmp92001_adc_remove(struct platform_device *pdev)
{
        struct iio_dev *indio_dev = platform_get_drvdata(pdev);

        iio_device_unregister(indio_dev);

        return 0;
}

static struct platform_driver lmp92001_adc_driver = {
        .driver.name    = "lmp92001-adc",
        .driver.owner   = THIS_MODULE,
        .probe          = lmp92001_adc_probe,
        .remove         = lmp92001_adc_remove,
};

static int __init lmp92001_adc_init(void)
{
        return platform_driver_register(&lmp92001_adc_driver);
}
subsys_initcall(lmp92001_adc_init);

static void __exit lmp92001_adc_exit(void)
{
        platform_driver_unregister(&lmp92001_adc_driver);
}
module_exit(lmp92001_adc_exit);

MODULE_AUTHOR("Abhisit Sangjan <s.abhisit@gmail.com>");
MODULE_DESCRIPTION("IIO ADC interface for TI LMP92001");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:lmp92001-adc");
