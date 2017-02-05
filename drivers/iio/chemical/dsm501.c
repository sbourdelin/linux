/*
 * Samyoung DSM501 particle sensor driver
 *
 * Copyright (C) 2017 Tomasz Duszynski <tduszyns@gmail.com>
 *
 * Datasheets:
 *  http://www.samyoungsnc.com/products/3-1%20Specification%20DSM501.pdf
 *  http://wiki.timelab.org/images/f/f9/PPD42NS.pdf
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/timekeeping.h>

#define DSM501_DRV_NAME "dsm501"
#define	DSM501_IRQ_NAME "dsm501_irq"

#define DSM501_DEFAULT_MEASUREMENT_TIME 30 /* seconds */

struct dsm501_data {
	ktime_t ts;
	ktime_t low_time;
	ktime_t meas_time;

	int irq;
	struct gpio_desc *gpio;

	struct mutex lock;

	int (*number_concentration)(struct dsm501_data *data);
};

/*
 * Series of data points in Fig. 8-3 (Low Ratio vs Particle)
 * can be approximated by following polynomials:
 *
 * p(r) = 0 (undefined) for r < 4
 * p(r) = 2353564.2r - 4373814.7 for 4 <= r < 20
 * p(r) = 4788112.4r - 53581390 for r >= 20
 *
 * Note: Result is in pcs/m3. To convert to pcs/0.01cf multiply
 *	 by 0.0002831685.
 */
static int dsm501_number_concentartion(struct dsm501_data *data)
{
	s64 retval = 0, r = div64_s64(ktime_to_ns(data->low_time) * 100,
				      ktime_to_ns(data->meas_time));

	if (r >= 4 && r < 20)
		retval = 23535642 * r - 43738147;
	else if (r >= 20)
		retval = 47881124 * r - 535813900;

	return div_s64(retval, 10);
}

/*
 * Series of data points in Fig. 2 (Lo Pulse Occupancy Time vs Concentration)
 * can be approximated by following polynomial:
 *
 * p(r) = 3844.2r^3 - 16201.3r^2 + 1848746.1r + 52497.2
 *
 * Note: Result is in pcs/m3. To convert to pcs/0.01cf multiply
 *	 by 0.0002831685.
 */
static int ppd42ns_number_concentration(struct dsm501_data *data)
{
	s64 retval, r3, r2, r = div64_s64(ktime_to_ns(data->low_time) * 100,
					  ktime_to_ns(data->meas_time));

	r2 = r * r;
	r3 = r2 * r;

	retval = 38442 * r3;
	retval -= 162013 * r2;
	retval += 18487461 * r;
	retval += 524972;

	return div_s64(retval, 10);
}

static irqreturn_t dsm501_irq(int irq, void *dev_id)
{
	struct dsm501_data *data = iio_priv(dev_id);
	int val = gpiod_get_value(data->gpio);
	ktime_t dt, ts = ktime_get();

	if (ktime_to_ns(data->ts) == 0) {
		data->ts = ts;
		data->low_time = ktime_set(0, 0);
	}

	if (val) {
		dt = ktime_sub(ts, data->ts);
		data->low_time = ktime_add(data->low_time, dt);
	} else {
		data->ts = ts;
	}

	return IRQ_HANDLED;
}

static int dsm501_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan, int *val,
			   int *val2, long mask)
{
	struct dsm501_data *data = iio_priv(indio_dev);
	struct device *dev = indio_dev->dev.parent;
	unsigned long irqflags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;
	int ret;


	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
		mutex_lock(&data->lock);
		data->ts = ktime_set(0, 0);

		ret = devm_request_irq(dev, data->irq, dsm501_irq, irqflags,
				       DSM501_IRQ_NAME, indio_dev);
		if (ret) {
			dev_err(dev, "Failed to request interrupt %d\n", data->irq);
			mutex_unlock(&data->lock);
			return ret;
		}

		msleep_interruptible(ktime_to_ms(data->meas_time));
		devm_free_irq(dev, data->irq, indio_dev);

		*val = data->number_concentration(data);
		mutex_unlock(&data->lock);

		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info dsm501_info = {
	.driver_module	= THIS_MODULE,
	.read_raw = dsm501_read_raw,
};

static const struct iio_chan_spec dsm501_channels[] = {
	{
		.type = IIO_NUMBERCONCENTRATION,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

static int dsm501_probe(struct platform_device *pdev)
{
	struct dsm501_data *data;
	struct iio_dev *indio_dev;
	struct device *dev = &pdev->dev;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	platform_set_drvdata(pdev, indio_dev);

	data->gpio = devm_gpiod_get_index(dev, NULL, 0, GPIOD_IN);
	if (IS_ERR(data->gpio)) {
		dev_err(dev, "Failed to get GPIO\n");
		return PTR_ERR(data->gpio);
	}

	data->irq = gpiod_to_irq(data->gpio);
	if (data->irq < 0) {
		dev_err(dev, "GPIO has no interrupt\n");
		return data->irq;
	}

	data->meas_time = ktime_set(DSM501_DEFAULT_MEASUREMENT_TIME, 0);
	data->number_concentration = of_device_get_match_data(dev);
	mutex_init(&data->lock);

	indio_dev->name = DSM501_DRV_NAME;
	indio_dev->dev.parent = dev;
	indio_dev->info = &dsm501_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = dsm501_channels;
	indio_dev->num_channels = ARRAY_SIZE(dsm501_channels);

	return devm_iio_device_register(&pdev->dev, indio_dev);
}

static const struct of_device_id dsm501_id[] = {
	{
		.compatible = "samyoung,dsm501",
		.data = dsm501_number_concentartion,
	},
	{
		.compatible = "shinyei,ppd42ns",
		.data = ppd42ns_number_concentration,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, dsm501_id);

static struct platform_driver dsm501_driver = {
	.driver	= {
		.name = DSM501_DRV_NAME,
		.of_match_table = of_match_ptr(dsm501_id)
	},
	.probe = dsm501_probe
};
module_platform_driver(dsm501_driver);

MODULE_AUTHOR("Tomasz Duszynski <tduszyns@gmail.com>");
MODULE_DESCRIPTION("Samyoung DSM501 particle sensor driver");
MODULE_LICENSE("GPL v2");
