/*
 * DAC driver for the Apex Embedded Systems STX104
 * Copyright (C) 2016 William Breathitt Gray
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>

#define STX104_NUM_CHAN 2

#define STX104_CHAN(chan) {				\
	.type = IIO_VOLTAGE,				\
	.channel = chan,				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
	.indexed = 1,					\
	.output = 1					\
}

static unsigned stx104_base;
module_param(stx104_base, uint, 0);
MODULE_PARM_DESC(stx104_base, "Apex Embedded Systems STX104 base address");

/**
 * struct stx104_iio - IIO device private data structure
 * @chan_out_states:	channels' output states
 * @base:		base port address of the IIO device
 */
struct stx104_iio {
	unsigned chan_out_states[STX104_NUM_CHAN];
	unsigned base;
};

static int stx104_read_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int *val, int *val2, long mask)
{
	struct stx104_iio *const priv = iio_priv(indio_dev);

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	*val = priv->chan_out_states[chan->channel];

	return IIO_VAL_INT;
}

static int stx104_write_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int val, int val2, long mask)
{
	struct stx104_iio *const priv = iio_priv(indio_dev);
	const unsigned chan_addr_offset = 2 * chan->channel;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	priv->chan_out_states[chan->channel] = val;
	outw(val, priv->base + 4 + chan_addr_offset);

	return 0;
}

static const struct iio_info stx104_info = {
	.driver_module = THIS_MODULE,
	.read_raw = stx104_read_raw,
	.write_raw = stx104_write_raw
};

static const struct iio_chan_spec stx104_channels[STX104_NUM_CHAN] = {
	STX104_CHAN(0),
	STX104_CHAN(1)
};

static int __init stx104_probe(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct device *const dev = &pdev->dev;
	struct stx104_iio *priv;
	const unsigned extent = 16;
	const char *const name = dev_name(dev);

	indio_dev = devm_iio_device_alloc(dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	if (!devm_request_region(dev, stx104_base, extent, name)) {
		dev_err(dev, "Unable to lock port addresses (0x%X-0x%X)\n",
			stx104_base, stx104_base + extent);
		return -EBUSY;
	}

	indio_dev->info = &stx104_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = stx104_channels;
	indio_dev->num_channels = STX104_NUM_CHAN;
	indio_dev->name = name;

	priv = iio_priv(indio_dev);
	priv->base = stx104_base;

	platform_set_drvdata(pdev, indio_dev);

	/* initialize DAC output to 0V */
	outw(0, stx104_base + 4);
	outw(0, stx104_base + 6);

	return iio_device_register(indio_dev);
}

static int stx104_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);

	iio_device_unregister(indio_dev);

	return 0;
}

static struct platform_device *stx104_device;

static struct platform_driver stx104_driver = {
	.driver = {
		.name = "stx104"
	},
	.remove = stx104_remove
};

static void __exit stx104_exit(void)
{
	platform_device_unregister(stx104_device);
	platform_driver_unregister(&stx104_driver);
}

static int __init stx104_init(void)
{
	int err;

	stx104_device = platform_device_alloc(stx104_driver.driver.name, -1);
	if (!stx104_device)
		return -ENOMEM;

	err = platform_device_add(stx104_device);
	if (err)
		goto err_platform_device;

	err = platform_driver_probe(&stx104_driver, stx104_probe);
	if (err)
		goto err_platform_driver;

	return 0;

err_platform_driver:
	platform_device_del(stx104_device);
err_platform_device:
	platform_device_put(stx104_device);
	return err;
}

module_init(stx104_init);
module_exit(stx104_exit);

MODULE_AUTHOR("William Breathitt Gray <vilhelm.gray@gmail.com>");
MODULE_DESCRIPTION("Apex Embedded Systems STX104 DAC driver");
MODULE_LICENSE("GPL v2");
