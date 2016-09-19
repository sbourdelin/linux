/*
 * Copyright © 2016 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/* This driver supports using the Raspberry Pi's firmware interface to
 * access its GPIO lines.  This lets us interact with the GPIO lines
 * on the Raspberry Pi 3's FXL6408 expander, which we otherwise have
 * no way to access (since the firmware is polling the chip
 * continuously).
 */

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

struct rpi_gpio {
	struct device *dev;
	struct gpio_chip gc;
	struct rpi_firmware *firmware;

	/* Offset of our pins in the GET_GPIO_STATE/SET_GPIO_STATE calls. */
	unsigned offset;
};

static int rpi_gpio_dir_in(struct gpio_chip *gc, unsigned off)
{
	/* We don't have direction control. */
	return -EINVAL;
}

static int rpi_gpio_dir_out(struct gpio_chip *gc, unsigned off, int val)
{
	/* We don't have direction control. */
	return -EINVAL;
}

static void rpi_gpio_set(struct gpio_chip *gc, unsigned off, int val)
{
	struct rpi_gpio *rpi = container_of(gc, struct rpi_gpio, gc);
	u32 packet[2] = { rpi->offset + off, val };
	int ret;

	ret = rpi_firmware_property(rpi->firmware,
				    RPI_FIRMWARE_SET_GPIO_STATE,
				    &packet, sizeof(packet));
	if (ret)
		dev_err(rpi->dev, "Error setting GPIO %d state: %d\n", off, ret);
}

static int rpi_gpio_get(struct gpio_chip *gc, unsigned off)
{
	struct rpi_gpio *rpi = container_of(gc, struct rpi_gpio, gc);
	u32 packet[2] = { rpi->offset + off, 0 };
	int ret;

	ret = rpi_firmware_property(rpi->firmware,
				    RPI_FIRMWARE_GET_GPIO_STATE,
				    &packet, sizeof(packet));
	if (ret) {
		dev_err(rpi->dev, "Error getting GPIO state: %d\n", ret);
		return ret;
	} else if (packet[0]) {
		dev_err(rpi->dev, "Firmware error getting GPIO state: %d\n",
			packet[0]);
		return -EINVAL;
	} else {
		return packet[1];
	}
}

static int rpi_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *fw_node;
	struct rpi_gpio *rpi;
	u32 ngpio;
	int ret;

	rpi = devm_kzalloc(dev, sizeof *rpi, GFP_KERNEL);
	if (!rpi)
		return -ENOMEM;
	rpi->dev = dev;

	fw_node = of_parse_phandle(np, "firmware", 0);
	if (!fw_node) {
		dev_err(dev, "Missing firmware node\n");
		return -ENOENT;
	}

	rpi->firmware = rpi_firmware_get(fw_node);
	if (!rpi->firmware)
		return -EPROBE_DEFER;

	if (of_property_read_u32(pdev->dev.of_node, "ngpios", &ngpio)) {
		dev_err(dev, "Missing ngpios");
		return -ENOENT;
	}
	if (of_property_read_u32(pdev->dev.of_node,
				 "raspberrypi,firmware-gpio-offset",
				 &rpi->offset)) {
		dev_err(dev, "Missing raspberrypi,firmware-gpio-offset");
		return -ENOENT;
	}

	rpi->gc.label = np->full_name;
	rpi->gc.owner = THIS_MODULE;
	rpi->gc.of_node = np;
	rpi->gc.ngpio = ngpio;
	rpi->gc.direction_input = rpi_gpio_dir_in;
	rpi->gc.direction_output = rpi_gpio_dir_out;
	rpi->gc.get = rpi_gpio_get;
	rpi->gc.set = rpi_gpio_set;
	rpi->gc.can_sleep = true;

	ret = gpiochip_add(&rpi->gc);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, rpi);

	return 0;
}

static int rpi_gpio_remove(struct platform_device *pdev)
{
	struct rpi_gpio *rpi = platform_get_drvdata(pdev);

	gpiochip_remove(&rpi->gc);

	return 0;
}

static const struct of_device_id __maybe_unused rpi_gpio_ids[] = {
	{ .compatible = "raspberrypi,firmware-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, rpi_gpio_ids);

static struct platform_driver rpi_gpio_driver = {
	.driver	= {
		.name = "gpio-raspberrypi-firmware",
		.of_match_table	= of_match_ptr(rpi_gpio_ids),
	},
	.probe	= rpi_gpio_probe,
	.remove	= rpi_gpio_remove,
};
module_platform_driver(rpi_gpio_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("Raspberry Pi firmware GPIO driver");
