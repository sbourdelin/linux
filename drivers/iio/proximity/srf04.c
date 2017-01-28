/*
 * SRF04: ultrasonic sensor for distance measuring by using GPIOs
 *
 * Copyright (c) 2017 Andreas Klinger <ak@it-klinger.de>
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
 * For details about the device see:
 * http://www.robot-electronics.co.uk/htm/srf04tech.htm
 */
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

struct srf04_data {
	struct device		*dev;
	struct gpio_desc	*gpiod_trig;
	struct gpio_desc	*gpiod_echo;
	struct mutex		lock;
	int			irqnr;
	ktime_t			ts_rising;
	ktime_t			ts_falling;
	struct completion	rising;
	struct completion	falling;
};

static irqreturn_t srf04_handle_irq(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct srf04_data *data = iio_priv(indio_dev);

	if (gpiod_get_value(data->gpiod_echo)) {
		data->ts_rising = ktime_get();
		complete(&data->rising);
	} else {
		data->ts_falling = ktime_get();
		complete(&data->falling);
	}

	return IRQ_HANDLED;
}

static int srf04_read(struct srf04_data *data)
{
	int ret;
	ktime_t ktime_dt;
	u64 dt_ns;
	u32 time_ns;
	u32 distance_mm;

	mutex_lock(&data->lock);

	reinit_completion(&data->rising);
	reinit_completion(&data->falling);

	gpiod_set_value(data->gpiod_trig, 1);
	udelay(10);
	gpiod_set_value(data->gpiod_trig, 0);

	mutex_unlock(&data->lock);

	/* it cannot take more than 20 ms */
	ret = wait_for_completion_killable_timeout(&data->rising, HZ/50);
	if (ret < 0)
		return -ETIMEDOUT;

	ret = wait_for_completion_killable_timeout(&data->falling, HZ/50);
	if (ret < 0)
		return -ETIMEDOUT;

	ktime_dt = ktime_sub(data->ts_falling, data->ts_rising);

	dt_ns = ktime_to_ns(ktime_dt);
	/*
	 * measuring more than 3 meters is beyond the posibilities of
	 * the sensor
	 */
	if (dt_ns > 8750000) {
		return -EFAULT;
	}
	time_ns = dt_ns;

	/*
	 * the speed as function of the temperature is approximately:
	 * speed = 331,5 + 0,6 * Temp
	 *   with Temp in °C
	 *   and speed in m/s
	 *
	 * use 343 m/s as ultrasonic speed at 20 °C here in absence of the
	 * temperature
	 *
	 * therefore:
	 * distance = time / 10^6 * 343 / 2
	 *   with time in ns
	 *   and distance in mm (one way)
	 *
	 * because we limit to 3 meters the multiplication with 343 just
	 * fits into 32 bit
	 */
	distance_mm = time_ns * 343 / 2000000;

	dev_info (data->dev, "ns: %llu, dist: %d\n", dt_ns, distance_mm);

	return distance_mm;
}

static int srf04_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *channel, int *val,
			    int *val2, long mask)
{
	struct srf04_data *data = iio_priv(indio_dev);
	int ret;

	if (channel->type != IIO_DISTANCE)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = srf04_read(data);
		if (ret < 0)
			return ret;
		*val = ret;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		/*
		 * theoretical maximum resolution is 3 mm
		 * 1 LSB is 1 mm
		 */
		*val = 0;
		*val2 = 1000;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static const struct iio_info srf04_iio_info = {
	.driver_module		= THIS_MODULE,
	.read_raw		= srf04_read_raw,
};

static const struct iio_chan_spec srf04_chan_spec[] = {
	{
		.type = IIO_DISTANCE,
		.info_mask_separate =
				BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int srf04_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct srf04_data *data = NULL;
	struct iio_dev *indio_dev;
	int ret = 0;

	indio_dev = devm_iio_device_alloc(dev, sizeof(struct srf04_data));
	if (!indio_dev) {
		dev_err(dev, "failed to allocate IIO device\n");
		return -ENOMEM;
	}

	data = iio_priv(indio_dev);
	data->dev = dev;

	mutex_init(&data->lock);
	init_completion(&data->rising);
	init_completion(&data->falling);

	data->gpiod_trig = devm_gpiod_get(dev, "trig", GPIOD_OUT_LOW);
	if (IS_ERR(data->gpiod_trig)) {
		dev_err(dev, "failed to get trig-gpiod: err=%ld\n",
					PTR_ERR(data->gpiod_trig));
		return PTR_ERR(data->gpiod_trig);
	}

	data->gpiod_echo = devm_gpiod_get(dev, "echo", GPIOD_IN);
	if (IS_ERR(data->gpiod_echo)) {
		dev_err(dev, "failed to get echo-gpiod: err=%ld\n",
					PTR_ERR(data->gpiod_echo));
		return PTR_ERR(data->gpiod_echo);
	}

	if (gpiod_cansleep(data->gpiod_echo)) {
		dev_err(data->dev, "cansleep-GPIOs not supported\n");
		return -ENODEV;
	}

	data->irqnr = gpiod_to_irq(data->gpiod_echo);
	if (data->irqnr < 0) {
		dev_err(data->dev, "gpiod_to_irq: %d\n", ret);
		return ret;
	}

	ret = request_irq(data->irqnr, srf04_handle_irq,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			pdev->name, indio_dev);
	if (ret < 0) {
		dev_err(data->dev, "request_irq: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, indio_dev);

	indio_dev->name = "srf04";
	indio_dev->dev.parent = &pdev->dev;
	indio_dev->info = &srf04_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = srf04_chan_spec;
	indio_dev->num_channels = ARRAY_SIZE(srf04_chan_spec);

	return devm_iio_device_register(dev, indio_dev);
}

static int srf04_remove(struct platform_device *pdev)
{
	struct srf04_data *data = NULL;
	struct iio_dev *indio_dev;

	indio_dev = platform_get_drvdata(pdev);
	data = iio_priv(indio_dev);

	free_irq(data->irqnr, indio_dev);

	return 0;
}

static const struct of_device_id of_srf04_match[] = {
	{ .compatible = "devantech,srf04", },
	{},
};

MODULE_DEVICE_TABLE(of, of_srf04_match);

static struct platform_driver srf04_driver = {
	.probe		= srf04_probe,
	.remove		= srf04_remove,
	.driver		= {
		.name		= "srf04-gpio",
		.of_match_table	= of_srf04_match,
	},
};

module_platform_driver(srf04_driver);

MODULE_AUTHOR("Andreas Klinger <ak@it-klinger.de>");
MODULE_DESCRIPTION("SRF04 ultrasonic sensor for distance measuring using GPIOs");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:srf04");
