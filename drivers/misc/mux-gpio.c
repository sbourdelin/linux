/*
 * GPIO-controlled multiplexer driver
 *
 * Copyright (C) 2016 Axentia Technologies AB
 *
 * Author: Peter Rosin <peda@axentia.se>
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
	int *val;
};

static int mux_gpio_set(struct mux_control *mux, int state)
{
	struct mux_gpio *mux_gpio = mux_chip_priv(mux->chip);
	int i;

	for (i = 0; i < mux_gpio->gpios->ndescs; i++)
		mux_gpio->val[i] = (state >> i) & 1;

	gpiod_set_array_value_cansleep(mux_gpio->gpios->ndescs,
				       mux_gpio->gpios->desc,
				       mux_gpio->val);

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
	struct mux_chip *mux_chip;
	struct mux_gpio *mux_gpio;
	int pins;
	u32 idle_state;
	int ret;

	if (!np)
		return -ENODEV;

	pins = gpiod_count(dev, "mux");
	if (pins < 0)
		return pins;

	mux_chip = mux_chip_alloc(dev, 1, sizeof(*mux_gpio) +
				  pins * sizeof(*mux_gpio->val));
	if (!mux_chip)
		return -ENOMEM;

	mux_gpio = mux_chip_priv(mux_chip);
	mux_gpio->val = (int *)(mux_gpio + 1);
	mux_chip->ops = &mux_gpio_ops;

	platform_set_drvdata(pdev, mux_chip);

	mux_gpio->gpios = devm_gpiod_get_array(dev, "mux", GPIOD_OUT_LOW);
	if (IS_ERR(mux_gpio->gpios)) {
		ret = PTR_ERR(mux_gpio->gpios);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get gpios\n");
		goto free_mux_chip;
	}
	WARN_ON(pins != mux_gpio->gpios->ndescs);
	mux_chip->mux->states = 1 << pins;

	ret = of_property_read_u32(np, "idle-state", &idle_state);
	if (ret >= 0) {
		if (idle_state >= mux_chip->mux->states) {
			dev_err(dev, "invalid idle-state %u\n", idle_state);
			ret = -EINVAL;
			goto free_mux_chip;
		}

		mux_chip->mux->idle_state = idle_state;
	}

	ret = mux_chip_register(mux_chip);
	if (ret < 0) {
		dev_err(dev, "failed to register mux-chip\n");
		goto free_mux_chip;
	}

	dev_info(dev, "%u-way mux-controller registered\n",
		 mux_chip->mux->states);

	return 0;

free_mux_chip:
	mux_chip_free(mux_chip);
	return ret;
}

static int mux_gpio_remove(struct platform_device *pdev)
{
	struct mux_chip *mux_chip = platform_get_drvdata(pdev);

	mux_chip_unregister(mux_chip);
	mux_chip_free(mux_chip);

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

MODULE_DESCRIPTION("GPIO-controlled multiplexer driver");
MODULE_AUTHOR("Peter Rosin <peda@axentia.se");
MODULE_LICENSE("GPL v2");
