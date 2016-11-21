/*
 * GPIO-controlled multiplexer driver
 *
 * Copyright (C) 2016 Axentia Technologies AB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mux.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

struct mux_gpio {
	struct gpio_descs *gpios;
};

static int mux_gpio_set(struct mux_control *mux, int val)
{
	struct mux_gpio *mux_gpio = mux_control_priv(mux);
	int i;

	for (i = 0; i < mux_gpio->gpios->ndescs; i++)
		gpiod_set_value_cansleep(mux_gpio->gpios->desc[i],
					 val & (1 << i));

	return 0;
}

static const struct mux_control_ops mux_gpio_ops = {
	.set = mux_gpio_set,
};

static const struct of_device_id mux_gpio_dt_ids[] = {
	{ .compatible = "mux-gpio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mux_gpio_dt_ids);

static int mux_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct mux_control *mux;
	struct mux_gpio *mux_gpio;
	u32 idle_state;
	int ret;

	if (!np)
		return -ENODEV;

	mux = mux_control_alloc(sizeof(*mux_gpio));
	if (!mux)
		return -ENOMEM;
	mux_gpio = mux_control_priv(mux);
	mux->dev.parent = dev;
	mux->ops = &mux_gpio_ops;

	platform_set_drvdata(pdev, mux);

	mux_gpio->gpios = devm_gpiod_get_array(dev, "mux", GPIOD_OUT_LOW);
	if (IS_ERR(mux_gpio->gpios)) {
		if (PTR_ERR(mux_gpio->gpios) != -EPROBE_DEFER)
			dev_err(dev, "failed to get gpios\n");
		mux_control_put(mux);
		return PTR_ERR(mux_gpio->gpios);
	}
	mux->states = 1 << mux_gpio->gpios->ndescs;

	ret = of_property_read_u32(np, "idle-state", &idle_state);
	if (ret >= 0) {
		if (idle_state >= mux->states) {
			dev_err(dev, "invalid idle-state %u\n", idle_state);
			return -EINVAL;
		}
		mux->idle_state = idle_state;
	}

	ret = mux_control_register(mux);
	if (ret < 0) {
		dev_err(dev, "failed to register mux_control\n");
		mux_control_put(mux);
		return ret;
	}

	return ret;
}

static int mux_gpio_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mux_control *mux = to_mux_control(dev);

	mux_control_unregister(mux);
	mux_control_put(mux);
	return 0;
}

static struct platform_driver mux_gpio_driver = {
	.driver = {
		.name = "mux-gpio",
		.of_match_table	= of_match_ptr(mux_gpio_dt_ids),
	},
	.probe = mux_gpio_probe,
	.remove = mux_gpio_remove,
};
module_platform_driver(mux_gpio_driver);

MODULE_AUTHOR("Peter Rosin <peda@axentia.se");
MODULE_DESCRIPTION("GPIO-controlled multiplexer driver");
MODULE_LICENSE("GPL v2");
