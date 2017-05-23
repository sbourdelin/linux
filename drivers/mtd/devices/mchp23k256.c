/*
 * mchp23k256.c
 *
 * Driver for Microchip 23k256 SPI RAM chips
 *
 * Copyright © 2016 Andrew Lunn <andrew@lunn.ch>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/spi/flash.h>
#include <linux/spi/spi.h>
#include <linux/of_device.h>

#define MAX_CMD_SIZE		4
enum chips { mchp23k256, mchp23lcv1024 };

struct mchp23k256_flash {
	struct spi_device	*spi;
	struct mutex		lock;
	struct mtd_info		mtd;
	u8			addr_width;
};

#define MCHP23K256_CMD_WRITE_STATUS	0x01
#define MCHP23K256_CMD_WRITE		0x02
#define MCHP23K256_CMD_READ		0x03
#define MCHP23K256_MODE_SEQ		BIT(6)

#define to_mchp23k256_flash(x) container_of(x, struct mchp23k256_flash, mtd)

static void mchp23k256_addr2cmd(struct mchp23k256_flash *flash,
				unsigned int addr, u8 *cmd)
{
	/* cmd[0] has opcode */
	cmd[1] = addr >> (flash->addr_width * 8 -  8);
	cmd[2] = addr >> (flash->addr_width * 8 - 16);
	cmd[3] = addr >> (flash->addr_width * 8 - 24);
}

static int mchp23k256_cmdsz(struct mchp23k256_flash *flash)
{
	return 1 + flash->addr_width;
}

static int mchp23k256_write(struct mtd_info *mtd, loff_t to, size_t len,
			    size_t *retlen, const unsigned char *buf)
{
	struct mchp23k256_flash *flash = to_mchp23k256_flash(mtd);
	struct spi_transfer transfer[2] = {};
	struct spi_message message;
	unsigned char command[MAX_CMD_SIZE];

	spi_message_init(&message);

	command[0] = MCHP23K256_CMD_WRITE;
	mchp23k256_addr2cmd(flash, to, command);

	transfer[0].tx_buf = command;
	transfer[0].len = mchp23k256_cmdsz(flash);
	spi_message_add_tail(&transfer[0], &message);

	transfer[1].tx_buf = buf;
	transfer[1].len = len;
	spi_message_add_tail(&transfer[1], &message);

	mutex_lock(&flash->lock);

	spi_sync(flash->spi, &message);

	if (retlen && message.actual_length > sizeof(command))
		*retlen += message.actual_length - sizeof(command);

	mutex_unlock(&flash->lock);
	return 0;
}

static int mchp23k256_read(struct mtd_info *mtd, loff_t from, size_t len,
			   size_t *retlen, unsigned char *buf)
{
	struct mchp23k256_flash *flash = to_mchp23k256_flash(mtd);
	struct spi_transfer transfer[2] = {};
	struct spi_message message;
	unsigned char command[MAX_CMD_SIZE];

	spi_message_init(&message);

	memset(&transfer, 0, sizeof(transfer));
	command[0] = MCHP23K256_CMD_READ;
	mchp23k256_addr2cmd(flash, from, command);

	transfer[0].tx_buf = command;
	transfer[0].len = mchp23k256_cmdsz(flash);
	spi_message_add_tail(&transfer[0], &message);

	transfer[1].rx_buf = buf;
	transfer[1].len = len;
	spi_message_add_tail(&transfer[1], &message);

	mutex_lock(&flash->lock);

	spi_sync(flash->spi, &message);

	if (retlen && message.actual_length > sizeof(command))
		*retlen += message.actual_length - sizeof(command);

	mutex_unlock(&flash->lock);
	return 0;
}

/*
 * Set the device into sequential mode. This allows read/writes to the
 * entire SRAM in a single operation
 */
static int mchp23k256_set_mode(struct spi_device *spi)
{
	struct spi_transfer transfer = {};
	struct spi_message message;
	unsigned char command[2];

	spi_message_init(&message);

	command[0] = MCHP23K256_CMD_WRITE_STATUS;
	command[1] = MCHP23K256_MODE_SEQ;

	transfer.tx_buf = command;
	transfer.len = sizeof(command);
	spi_message_add_tail(&transfer, &message);

	return spi_sync(spi, &message);
}

static int mchp23k256_probe(struct spi_device *spi)
{
	struct mchp23k256_flash *flash;
	struct flash_platform_data *data;
	int err;
	enum chips chip;

	flash = devm_kzalloc(&spi->dev, sizeof(*flash), GFP_KERNEL);
	if (!flash)
		return -ENOMEM;

	flash->spi = spi;
	mutex_init(&flash->lock);
	spi_set_drvdata(spi, flash);

	err = mchp23k256_set_mode(spi);
	if (err)
		return err;

	data = dev_get_platdata(&spi->dev);

	if (spi->dev.of_node)
		chip = (enum chips)of_device_get_match_data(&spi->dev);
	else
		chip = mchp23k256;

	mtd_set_of_node(&flash->mtd, spi->dev.of_node);
	flash->mtd.dev.parent	= &spi->dev;
	flash->mtd.type		= MTD_RAM;
	flash->mtd.flags	= MTD_CAP_RAM;
	flash->mtd.writesize	= 1;
	flash->mtd._read	= mchp23k256_read;
	flash->mtd._write	= mchp23k256_write;

	switch (chip) {
	case mchp23lcv1024:
		flash->mtd.size		= SZ_128K;
		flash->addr_width	= 3;
		break;
	default:
		flash->mtd.size		= SZ_32K;
		flash->addr_width	= 2;
		break;
	}

	err = mtd_device_register(&flash->mtd, data ? data->parts : NULL,
				  data ? data->nr_parts : 0);
	if (err)
		return err;

	return 0;
}

static int mchp23k256_remove(struct spi_device *spi)
{
	struct mchp23k256_flash *flash = spi_get_drvdata(spi);

	return mtd_device_unregister(&flash->mtd);
}

static const struct of_device_id mchp23k256_of_table[] = {
	{ .compatible = "microchip,mchp23k256", .data = (void *)mchp23k256 },
	{ .compatible = "microchip,mchp23lcv1024", .data = (void *)mchp23lcv1024 },
	{}
};
MODULE_DEVICE_TABLE(of, mchp23k256_of_table);

static struct spi_driver mchp23k256_driver = {
	.driver = {
		.name	= "mchp23k256",
		.of_match_table = of_match_ptr(mchp23k256_of_table),
	},
	.probe		= mchp23k256_probe,
	.remove		= mchp23k256_remove,
};

module_spi_driver(mchp23k256_driver);

MODULE_DESCRIPTION("MTD SPI driver for MCHP23K256 RAM chips");
MODULE_AUTHOR("Andrew Lunn <andre@lunn.ch>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("spi:mchp23k256");
