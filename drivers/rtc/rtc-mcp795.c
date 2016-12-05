/*
 * SPI Driver for Microchip MCP795 RTC
 *
 * Copyright (C) Josef Gajdusek <atx@atx.name>
 *
 * based on other Linux RTC drivers
 *
 * Device datasheet:
 * http://ww1.microchip.com/downloads/en/DeviceDoc/22280A.pdf
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/spi/spi.h>
#include <linux/rtc.h>
#include <linux/of.h>
#include <linux/delay.h>

/* MCP795 Instructions, see datasheet table 3-1 */
#define MCP795_EEREAD		0x03
#define MCP795_EEWRITE		0x02
#define MCP795_EEWRDI		0x04
#define MCP795_EEWREN		0x06
#define MCP795_SRREAD		0x05
#define MCP795_SRWRITE		0x01
#define MCP795_READ		0x13
#define MCP795_WRITE		0x12
#define MCP795_UNLOCK		0x14
#define MCP795_IDWRITE		0x32
#define MCP795_IDREAD		0x33
#define MCP795_CLRWDT		0x44
#define MCP795_CLRRAM		0x54

/* MCP795 RTCC registers, see datasheet table 4-1 */
#define MCP795_REG_SECONDS	0x01
#define MCP795_REG_DAY		0x04
#define MCP795_REG_MONTH	0x06
#define MCP795_REG_CONTROL	0x08

#define MCP795_ST_BIT		0x80
#define MCP795_24_BIT		0x40
#define MCP795_LP_BIT		0x20
#define MCP795_EXTOSC_BIT	0x08
#define MCP795_OSCON_BIT	0x20

static int mcp795_rtcc_read(struct device *dev, u8 addr, u8 *buf, u8 count)
{
	struct spi_device *spi = to_spi_device(dev);
	int ret;
	u8 tx[2];

	tx[0] = MCP795_READ;
	tx[1] = addr;
	ret = spi_write_then_read(spi, tx, sizeof(tx), buf, count);

	if (ret)
		dev_err(dev, "Failed reading %d bytes from address %x.\n",
					count, addr);

	return ret;
}

static int mcp795_rtcc_write(struct device *dev, u8 addr, u8 *data, u8 count)
{
	struct spi_device *spi = to_spi_device(dev);
	int ret;
	u8 tx[2 + count];

	tx[0] = MCP795_WRITE;
	tx[1] = addr;
	memcpy(&tx[2], data, count);

	ret = spi_write(spi, tx, 2 + count);

	if (ret)
		dev_err(dev, "Failed to write %d bytes to address %x.\n",
					count, addr);

	return ret;
}

static int mcp795_rtcc_set_bits(struct device *dev, u8 addr, u8 mask, u8 state)
{
	int ret;
	u8 tmp;

	ret = mcp795_rtcc_read(dev, addr, &tmp, 1);
	if (ret)
		return ret;

	if ((tmp & mask) != state) {
		tmp = (tmp & ~mask) | state;
		ret = mcp795_rtcc_write(dev, addr, &tmp, 1);
	}

	return ret;
}

static int mcp795_stop_oscillator(struct device *dev)
{
	int retries = 5;
	int ret;
	u8 data;

	ret = mcp795_rtcc_set_bits(dev, MCP795_REG_SECONDS, MCP795_ST_BIT, 0);
	if (ret)
		return ret;
	ret = mcp795_rtcc_set_bits(dev, MCP795_REG_CONTROL, MCP795_EXTOSC_BIT, 0);
	if (ret)
		return ret;
	do {
		usleep_range(700, 800);
		ret = mcp795_rtcc_read(dev, MCP795_REG_DAY,
					&data, sizeof(data));
		if (ret)
			break;
		if (!(data & MCP795_OSCON_BIT))
			break;

	} while (--retries);

	return !retries ? -EIO : ret;
}

static int mcp795_start_oscillator(struct device *dev)
{
	return mcp795_rtcc_set_bits(dev, MCP795_REG_SECONDS,
					MCP795_ST_BIT, MCP795_ST_BIT);
}

static int mcp795_set_time(struct device *dev, struct rtc_time *tim)
{
	int month;
	int ret;
	u8 data[7];

	/* Stop RTC while updating the registers */
	ret = mcp795_stop_oscillator(dev);
	if (ret)
		return ret;

	/* Read first, so we can leave config bits untouched */
	ret = mcp795_rtcc_read(dev, MCP795_REG_SECONDS, data, sizeof(data));

	if (ret)
		return ret;

	data[0] = (data[0] & 0x80) | ((tim->tm_sec / 10) << 4) | (tim->tm_sec % 10);
	data[1] = (data[1] & 0x80) | ((tim->tm_min / 10) << 4) | (tim->tm_min % 10);
	data[2] = ((tim->tm_hour / 10) << 4) | (tim->tm_hour % 10);
	data[4] = ((tim->tm_mday / 10) << 4) | ((tim->tm_mday) % 10);

	/* Always write the date and month using a separate Write command.
	 * This is a workaround for a know silicon issue that some combinations
	 * of date and month values may result in the date being reset to 1.
	 */
	ret = mcp795_rtcc_write(dev, MCP795_REG_SECONDS, data, 5);

	if (ret)
		return ret;

	month = tim->tm_mon + 1;
	data[5] = (data[5] & MCP795_LP_BIT) |
			((month / 10) << 4) | (month % 10);

	if (tim->tm_year > 100)
		tim->tm_year -= 100;

	data[6] = ((tim->tm_year / 10) << 4) | (tim->tm_year % 10);

	ret = mcp795_rtcc_write(dev, MCP795_REG_MONTH, &data[5], 2);

	if (ret)
		return ret;

	/* Start back RTC */
	ret = mcp795_start_oscillator(dev);
	if (ret)
		return ret;

	dev_dbg(dev, "Set mcp795: %04d-%02d-%02d %02d:%02d:%02d\n",
			tim->tm_year + 1900, tim->tm_mon, tim->tm_mday,
			tim->tm_hour, tim->tm_min, tim->tm_sec);

	return 0;
}

static int mcp795_read_time(struct device *dev, struct rtc_time *tim)
{
	int ret;
	u8 data[7];

	ret = mcp795_rtcc_read(dev, MCP795_REG_SECONDS, data, sizeof(data));

	if (ret)
		return ret;

	tim->tm_sec	= ((data[0] & 0x70) >> 4) * 10 + (data[0] & 0x0f);
	tim->tm_min	= ((data[1] & 0x70) >> 4) * 10 + (data[1] & 0x0f);
	tim->tm_hour	= ((data[2] & 0x30) >> 4) * 10 + (data[2] & 0x0f);
	tim->tm_mday	= ((data[4] & 0x30) >> 4) * 10 + (data[4] & 0x0f);
	tim->tm_mon	= ((data[5] & 0x10) >> 4) * 10 + (data[5] & 0x0f) - 1;
	tim->tm_year	= ((data[6] & 0xf0) >> 4) * 10 + (data[6] & 0x0f) + 100; /* Assume we are in 20xx */

	dev_dbg(dev, "Read from mcp795: %04d-%02d-%02d %02d:%02d:%02d\n",
				tim->tm_year + 1900, tim->tm_mon, tim->tm_mday,
				tim->tm_hour, tim->tm_min, tim->tm_sec);

	return rtc_valid_tm(tim);
}

static const struct rtc_class_ops mcp795_rtc_ops = {
		.read_time = mcp795_read_time,
		.set_time = mcp795_set_time
};

static int mcp795_probe(struct spi_device *spi)
{
	struct rtc_device *rtc;
	int ret;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret) {
		dev_err(&spi->dev, "Unable to setup SPI\n");
		return ret;
	}

	/* Start the oscillator */
	mcp795_start_oscillator(&spi->dev);
	/* Clear the 12 hour mode flag*/
	mcp795_rtcc_set_bits(&spi->dev, 0x03, MCP795_24_BIT, 0);

	rtc = devm_rtc_device_register(&spi->dev, "rtc-mcp795",
								&mcp795_rtc_ops, THIS_MODULE);
	if (IS_ERR(rtc))
		return PTR_ERR(rtc);

	spi_set_drvdata(spi, rtc);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id mcp795_of_match[] = {
	{ .compatible = "maxim,mcp795" },
	{ }
};
MODULE_DEVICE_TABLE(of, mcp795_of_match);
#endif

static struct spi_driver mcp795_driver = {
		.driver = {
				.name = "rtc-mcp795",
				.of_match_table = of_match_ptr(mcp795_of_match),
		},
		.probe = mcp795_probe,
};

module_spi_driver(mcp795_driver);

MODULE_DESCRIPTION("MCP795 RTC SPI Driver");
MODULE_AUTHOR("Josef Gajdusek <atx@atx.name>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:mcp795");
