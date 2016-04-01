/*
 * GS1662 device registration
 *
 * Copyright (C) 2015-2016 Nexvision
 * Author: Charles-Antoine Couret <charles-antoine.couret@nexvision.fr>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/platform_device.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/device.h>

#define READ_FLAG		0x8000
#define WRITE_FLAG		0x0000
#define BURST_FLAG		0x1000

#define ADDRESS_MASK	0x0FFF

static int gs1662_read_register(struct spi_device *spi, u16 addr, u16 *value)
{
	int ret;
	u16 buf_addr =  (READ_FLAG | (ADDRESS_MASK & addr));
	u16 buf_value = 0;
	struct spi_message msg;
	struct spi_transfer tx[] = {
		{
			.tx_buf = &buf_addr,
			.len = 2,
			.delay_usecs = 1,
		}, {
			.rx_buf = &buf_value,
			.len = 2,
			.delay_usecs = 1,
		},
	};

	spi_message_init(&msg);
	spi_message_add_tail(&tx[0], &msg);
	spi_message_add_tail(&tx[1], &msg);
	ret = spi_sync(spi, &msg);

	*value = buf_value;

	return ret;
}

static int gs1662_write_register(struct spi_device *spi, u16 addr, u16 value)
{
	int ret;
	u16 buf_addr = (WRITE_FLAG | (ADDRESS_MASK & addr));
	u16 buf_value = value;
	struct spi_message msg;
	struct spi_transfer tx[] = {
		{
			.tx_buf = &buf_addr,
			.len = 2,
			.delay_usecs = 1,
		}, {
			.tx_buf = &buf_value,
			.len = 2,
			.delay_usecs = 1,
		},
	};

	spi_message_init(&msg);
	spi_message_add_tail(&tx[0], &msg);
	spi_message_add_tail(&tx[1], &msg);
	ret = spi_sync(spi, &msg);

	return ret;
}

static int gs1662_probe(struct spi_device *spi)
{
	int ret;

	spi->mode = SPI_MODE_0;
	spi->irq = -1;
	spi->max_speed_hz = 10000000;
	spi->bits_per_word = 16;
	ret = spi_setup(spi);

	/* Set H_CONFIG to SMPTE timings */
	gs1662_write_register(spi, 0x0, 0x100);

	return ret;
}

static int gs1662_remove(struct spi_device *spi)
{
	return 0;
}

static struct spi_driver gs1662_driver = {
	.driver = {
		.name		= "gs1662",
		.owner		= THIS_MODULE,
	},

	.probe		= gs1662_probe,
	.remove		= gs1662_remove,
};

static int __init gs1662_init(void)
{
	spi_register_driver(&gs1662_driver);
	return 0;
}

static void __exit gs1662_exit(void)
{
	spi_unregister_driver(&gs1662_driver);
}

module_init(gs1662_init);
module_exit(gs1662_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Charles-Antoine Couret <charles-antoine.couret@nexvision.fr>");
MODULE_DESCRIPTION("GS1662 SPI driver");
