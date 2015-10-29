/*
 * TSC2005 touchscreen driver
 *
 * Copyright (C) 2006-2010 Nokia Corporation
 *
 * Author: Lauri Leukkunen <lauri.leukkunen@nokia.com>
 * based on TSC2301 driver by Klaus K. Pedersen <klaus.k.pedersen@nokia.com>
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

#include <linux/module.h>
#include <linux/input.h>
#include <linux/pm.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include "tsc200x-core.h"

#define TSC2005_SPI_MAX_SPEED_HZ	10000000

static int tsc2005_probe(struct spi_device *spi)
{
	int error;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	if (!spi->max_speed_hz)
		spi->max_speed_hz = TSC2005_SPI_MAX_SPEED_HZ;

	error = spi_setup(spi);
	if (error)
		return error;

	return tsc200x_probe(&spi->dev, spi->irq, BUS_SPI,
			     devm_regmap_init_spi(spi, &tsc2005_regmap_config));
}

static int tsc2005_remove(struct spi_device *spi)
{
	return tsc200x_remove(&spi->dev);
}

static SIMPLE_DEV_PM_OPS(tsc2005_pm_ops, tsc200x_suspend, tsc200x_resume);

static struct spi_driver tsc2005_driver = {
	.driver	= {
		.name	= "tsc2005",
		.owner	= THIS_MODULE,
		.pm	= &tsc2005_pm_ops,
	},
	.probe	= tsc2005_probe,
	.remove	= tsc2005_remove,
};

module_spi_driver(tsc2005_driver);

MODULE_AUTHOR("Lauri Leukkunen <lauri.leukkunen@nokia.com>");
MODULE_DESCRIPTION("TSC2005 Touchscreen Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:tsc2005");
