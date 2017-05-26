/*
 * Driver for the Analog Devices digital potentiometers (SPI bus)
 *
 * Copyright (C) 2010-2011 Michael Hennerich, Analog Devices Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/spi/spi.h>
#include <linux/module.h>
#include <linux/of_device.h>

#include "ad525x_dpot.h"

/* SPI bus functions */
static int write8(void *client, u8 val)
{
	u8 data = val;

	return spi_write(client, &data, 1);
}

static int write16(void *client, u8 reg, u8 val)
{
	u8 data[2] = {reg, val};

	return spi_write(client, data, 2);
}

static int write24(void *client, u8 reg, u16 val)
{
	u8 data[3] = {reg, val >> 8, val};

	return spi_write(client, data, 3);
}

static int read8(void *client)
{
	int ret;
	u8 data;

	ret = spi_read(client, &data, 1);
	if (ret < 0)
		return ret;

	return data;
}

static int read16(void *client, u8 reg)
{
	int ret;
	u8 buf_rx[2];

	write16(client, reg, 0);
	ret = spi_read(client, buf_rx, 2);
	if (ret < 0)
		return ret;

	return (buf_rx[0] << 8) |  buf_rx[1];
}

static int read24(void *client, u8 reg)
{
	int ret;
	u8 buf_rx[3];

	write24(client, reg, 0);
	ret = spi_read(client, buf_rx, 3);
	if (ret < 0)
		return ret;

	return (buf_rx[1] << 8) |  buf_rx[2];
}

static const struct ad_dpot_bus_ops bops = {
	.read_d8	= read8,
	.read_r8d8	= read16,
	.read_r8d16	= read24,
	.write_d8	= write8,
	.write_r8d8	= write16,
	.write_r8d16	= write24,
};
static int ad_dpot_spi_probe(struct spi_device *spi)
{
	int ret;
	const struct of_device_id *of_id = of_match_device(ad_dpot_spi_of_match,
							   &spi->dev);

	struct ad_dpot_bus_data bdata = {
		.client = spi,
		.bops = &bops,
	};

	if (of_id) {
		ret = ad_dpot_probe(&spi->dev, &bdata,
			     of_id->data,
			     of_id->name);
	} else {
		ret = ad_dpot_probe(&spi->dev, &bdata,
			     spi_get_device_id(spi)->driver_data,
			     spi_get_device_id(spi)->name);
	}

	return ret;
}

static int ad_dpot_spi_remove(struct spi_device *spi)
{
	return ad_dpot_remove(&spi->dev);
}

static const struct spi_device_id ad_dpot_spi_id[] = {
	{"ad5160", AD5160_ID},
	{"ad5161", AD5161_ID},
	{"ad5162", AD5162_ID},
	{"ad5165", AD5165_ID},
	{"ad5200", AD5200_ID},
	{"ad5201", AD5201_ID},
	{"ad5203", AD5203_ID},
	{"ad5204", AD5204_ID},
	{"ad5206", AD5206_ID},
	{"ad5207", AD5207_ID},
	{"ad5231", AD5231_ID},
	{"ad5232", AD5232_ID},
	{"ad5233", AD5233_ID},
	{"ad5235", AD5235_ID},
	{"ad5260", AD5260_ID},
	{"ad5262", AD5262_ID},
	{"ad5263", AD5263_ID},
	{"ad5290", AD5290_ID},
	{"ad5291", AD5291_ID},
	{"ad5292", AD5292_ID},
	{"ad5293", AD5293_ID},
	{"ad7376", AD7376_ID},
	{"ad8400", AD8400_ID},
	{"ad8402", AD8402_ID},
	{"ad8403", AD8403_ID},
	{"adn2850", ADN2850_ID},
	{"ad5270", AD5270_ID},
	{"ad5271", AD5271_ID},
	{}
};
MODULE_DEVICE_TABLE(spi, ad_dpot_spi_id);

static const struct of_device_id ad_dpot_spi_of_match[] = {
	{
		.compatible	= "ad,ad5160",
		.name		= "ad5160",
		.data		= (void *)AD5160_ID,
	},
	{
		.compatible	= "ad,ad5161",
		.name		= "ad5161",
		.data		= (void *)AD5161_ID,
	},
	{
		.compatible	= "ad,ad5162",
		.name		= "ad5162",
		.data		= (void *)AD5162_ID,
	},
	{
		.compatible	= "ad,ad5165",
		.name		= "ad5165",
		.data		= (void *)AD5165_ID,
	},
	{
		.compatible	= "ad,ad5200",
		.name		= "ad5200",
		.data		= (void *)AD5200_ID,
	},
	{
		.compatible	= "ad,ad5201",
		.name		= "ad5201",
		.data		= (void *)AD5201_ID,
	},
	{
		.compatible	= "ad,ad5203",
		.name		= "ad5203",
		.data		= (void *)AD5203_ID,
	},
	{
		.compatible	= "ad,ad5204",
		.name		= "ad5204",
		.data		= (void *)AD5204_ID,
	},
	{
		.compatible	= "ad,ad5206",
		.name		= "ad5206",
		.data		= (void *)AD5206_ID,
	},
	{
		.compatible	= "ad,ad5207",
		.name		= "ad5207",
		.data		= (void *)AD5207_ID,
	},
	{
		.compatible	= "ad,ad5231",
		.name		= "ad5231",
		.data		= (void *)AD5231_ID,
	},
	{
		.compatible	= "ad,ad5232",
		.name		= "ad5232",
		.data		= (void *)AD5232_ID,
	},
	{
		.compatible	= "ad,ad5233",
		.name		= "ad5233",
		.data		= (void *)AD5233_ID,
	},
	{
		.compatible	= "ad,ad5235",
		.name		= "ad5235",
		.data		= (void *)AD5235_ID,
	},
	{
		.compatible	= "ad,ad5260",
		.name		= "ad5260",
		.data		= (void *)AD5260_ID,
	},
	{
		.compatible	= "ad,ad5262",
		.name		= "ad5262",
		.data		= (void *)AD5262_ID,
	},
	{
		.compatible	= "ad,ad5263",
		.name		= "ad5263",
		.data		= (void *)AD5263_ID,
	},
	{
		.compatible	= "ad,ad5290",
		.name		= "ad5290",
		.data		= (void *)AD5290_ID,
	},
	{
		.compatible	= "ad,ad5291",
		.name		= "ad5291",
		.data		= (void *)AD5291_ID,
	},
	{
		.compatible	= "ad,ad5292",
		.name		= "ad5292",
		.data		= (void *)AD5292_ID,
	},
	{
		.compatible	= "ad,ad5293",
		.name		= "ad5293",
		.data		= (void *)AD5293_ID,
	},
	{
		.compatible	= "ad,ad7376",
		.name		= "ad7376",
		.data		= (void *)AD7376_ID,
	},
	{
		.compatible	= "ad,ad8400",
		.name		= "ad8400",
		.data		= (void *)AD8400_ID,
	},
	{
		.compatible	= "ad,ad8402",
		.name		= "ad8402",
		.data		= (void *)AD8402_ID,
	},
	{
		.compatible	= "ad,ad8403",
		.name		= "ad8403",
		.data		= (void *)AD8403_ID,
	},
	{
		.compatible	= "ad,adn2850",
		.name		= "adn2850",
		.data		= (void *)ADN2850_ID,
	},
	{
		.compatible	= "ad,ad5270",
		.name		= "ad5270",
		.data		= (void *)AD5270_ID,
	},
	{
		.compatible	= "ad,ad5271",
		.name		= "ad5271",
		.data		= (void *)AD5271_ID,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, ad_dpot_spi_of_match);

static struct spi_driver ad_dpot_spi_driver = {
	.driver = {
		.name	= "ad_dpot",
		.of_match_table = ad_dpot_spi_of_match,
	},
	.probe		= ad_dpot_spi_probe,
	.remove		= ad_dpot_spi_remove,
	.id_table	= ad_dpot_spi_id,
};

module_spi_driver(ad_dpot_spi_driver);

MODULE_AUTHOR("Michael Hennerich <hennerich@blackfin.uclinux.org>");
MODULE_DESCRIPTION("digital potentiometer SPI bus driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:ad_dpot");
