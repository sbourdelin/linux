/*
 * Generic regulator driver for industrial IO.
 *
 * Copyright (C) 2016 BayLibre SAS
 *
 * Author:
 *   Bartosz Golaszewski <bgolaszewski@baylibre.com.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

struct iio_regulator_context {
	struct regulator *regulator;
};

static ssize_t iio_regulator_enable_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct iio_regulator_context *ctx = iio_priv(dev_to_iio_dev(dev));

	return sprintf(buf, "%d\n", regulator_is_enabled(ctx->regulator));
}

static ssize_t iio_regulator_enable_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t len)
{
	struct iio_regulator_context *ctx = iio_priv(dev_to_iio_dev(dev));
	int ret, enabled;
	bool val;

	ret = strtobool(buf, &val);
	if (ret)
		return ret;

	enabled = regulator_is_enabled(ctx->regulator);
	if ((val && enabled) || (!val && !enabled))
		return -EPERM;

	ret = val ? regulator_enable(ctx->regulator) :
		    regulator_disable(ctx->regulator);
	if (ret)
		return ret;

	return len;
}

static IIO_DEVICE_ATTR(in_enable, 0644,
		       iio_regulator_enable_show,
		       iio_regulator_enable_store, 0);

static struct attribute *iio_regulator_attributes[] = {
	&iio_dev_attr_in_enable.dev_attr.attr,
	NULL,
};

static const struct attribute_group iio_regulator_attribute_group = {
	.attrs = iio_regulator_attributes,
};

static const struct iio_info iio_regulator_info = {
	.driver_module = THIS_MODULE,
	.attrs = &iio_regulator_attribute_group,
};

static int iio_regulator_probe(struct platform_device *pdev)
{
	struct iio_regulator_context *ctx;
	struct iio_dev *iio_dev;
	struct device *dev;

	dev = &pdev->dev;

	iio_dev = devm_iio_device_alloc(dev, sizeof(*ctx));
	if (!iio_dev)
		return -ENOMEM;

	ctx = iio_priv(iio_dev);

	ctx->regulator = devm_regulator_get(dev, "vcc");
	if (IS_ERR(ctx->regulator)) {
		dev_err(dev, "unable to get vcc regulator: %ld\n",
			PTR_ERR(ctx->regulator));
		return PTR_ERR(ctx->regulator);
	}

	iio_dev->dev.parent = dev;
	iio_dev->dev.of_node = dev->of_node;
	iio_dev->name = dev->driver->name;
	iio_dev->info = &iio_regulator_info;

	return devm_iio_device_register(dev, iio_dev);
}

static const struct of_device_id iio_regulator_of_match[] = {
	{ .compatible = "iio-regulator", },
	{ },
};

static struct platform_driver iio_regulator_platform_driver = {
	.probe = iio_regulator_probe,
	.driver = {
		.name = "iio-regulator",
		.of_match_table = iio_regulator_of_match,
	},
};
module_platform_driver(iio_regulator_platform_driver);

MODULE_AUTHOR("Bartosz Golaszewski <bgolaszewski@baylibre.com>");
MODULE_DESCRIPTION("Regulator driver for iio");
MODULE_LICENSE("GPL v2");
