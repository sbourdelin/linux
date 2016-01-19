/*
 * PCM179X ASoC SPI driver
 *
 * Copyright (c) Amarula Solutions B.V. 2013
 *
 *     Michael Trimarchi <michael@amarulasolutions.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>

#include "pcm179x.h"

static int pcm179x_spi_probe(struct spi_device *spi)
{
	struct pcm179x_private *pcm179x;
	int ret;

	pcm179x = devm_kzalloc(&spi->dev, sizeof(struct pcm179x_private),
				GFP_KERNEL);
	if (!pcm179x)
		return -ENOMEM;

	spi_set_drvdata(spi, pcm179x);

	pcm179x->dev = &spi->dev;

	pcm179x->regmap = devm_regmap_init_spi(spi, &pcm179x_regmap_config);
	if (IS_ERR(pcm179x->regmap)) {
		ret = PTR_ERR(pcm179x->regmap);
		dev_err(&spi->dev, "Failed to register regmap: %d\n", ret);
		return ret;
	}

	return pcm179x_common_init(pcm179x);
}

static int pcm179x_spi_remove(struct spi_device *spi)
{
	return pcm179x_common_exit(spi_get_drvdata(spi));
}

static const struct of_device_id pcm179x_of_match[] = {
	{ .compatible = "ti,pcm1792a", },
	{ }
};
MODULE_DEVICE_TABLE(of, pcm179x_of_match);

static const struct spi_device_id pcm179x_spi_ids[] = {
	{ "pcm179x", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, pcm179x_spi_ids);

static struct spi_driver pcm179x_spi_driver = {
	.driver = {
		.name = "pcm179x",
		.of_match_table = of_match_ptr(pcm179x_of_match),
	},
	.id_table = pcm179x_spi_ids,
	.probe = pcm179x_spi_probe,
	.remove = pcm179x_spi_remove,
};

module_spi_driver(pcm179x_spi_driver);

MODULE_DESCRIPTION("ASoC PCM179X SPI driver");
MODULE_AUTHOR("Michael Trimarchi <michael@amarulasolutions.com>");
MODULE_LICENSE("GPL");
