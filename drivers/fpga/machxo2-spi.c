/**
 * Lattice MachXO2 Slave SPI Driver
 *
 * Copyright (C) 2017 Paolo Pisati <p.pisati@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * Manage Lattice FPGA firmware that is loaded over SPI using
 * the slave serial configuration interface.
 */

#include <linux/delay.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

/* MachXO2 Programming Guide - sysCONFIG Programming Commands */

#define ISC_ENABLE		0x000008c6
#define ISC_ERASE		0x0000040e
#define ISC_PROGRAMDONE		0x0000005e
#define LSC_CHECKBUSY		0x000000f0
#define LSC_INITADDRESS		0x00000046
#define LSC_PROGINCRNV		0x01000070
#define LSC_REFRESH		0x00000079

#define BUSYFLAG		0x80

/*
 * Max CCLK in Slave SPI mode according to 'MachXO2 Family Data
 * Sheet' sysCONFIG Port Timing Specifications (3-36)
 */
#define MACHXO2_MAX_SPEED	66000000

#define MACHXO2_LOW_DELAY	5	/* us */
#define MACHXO2_HIGH_DELAY	200	/* us */

#define MACHXO2_OP_SIZE		sizeof(u32)
#define MACHXO2_PAGE_SIZE	16
#define MACHXO2_BUF_SIZE	(MACHXO2_OP_SIZE + MACHXO2_PAGE_SIZE)

static int wait_until_not_busy(struct spi_device *spi)
{
	u8 rx;
	u32 checkbusy = LSC_CHECKBUSY;
	int ret;

	do {
		ret = spi_write_then_read(spi, &checkbusy, MACHXO2_OP_SIZE,
					  &rx, sizeof(rx));
		if (ret)
			return ret;
	} while (rx & BUSYFLAG);

	return 0;
}

static enum fpga_mgr_states machxo2_spi_state(struct fpga_manager *mgr)
{
	return FPGA_MGR_STATE_UNKNOWN;
}

static int machxo2_write_init(struct fpga_manager *mgr,
			      struct fpga_image_info *info,
			      const char *buf, size_t count)
{
	struct spi_device *spi = mgr->priv;
	u32 enable = ISC_ENABLE;
	u32 erase = ISC_ERASE;
	u32 initaddr = LSC_INITADDRESS;
	int ret;

	if ((info->flags & FPGA_MGR_PARTIAL_RECONFIG)) {
		dev_err(&mgr->dev,
			"Partial reconfiguration is not supported\n");
		return -ENOTSUPP;
	}

	ret = spi_write(spi, &enable, MACHXO2_OP_SIZE);
	if (ret)
		goto fail;

	udelay(MACHXO2_LOW_DELAY);
	ret = spi_write(spi, &erase, MACHXO2_OP_SIZE);
	if (ret)
		goto fail;

	ret = wait_until_not_busy(spi);
	if (ret)
		goto fail;

	ret = spi_write(spi, &initaddr, MACHXO2_OP_SIZE);
	if (ret)
		goto fail;

	return 0;

fail:
	dev_err(&mgr->dev, "Error during FPGA init.\n");
	return ret;
}

static int machxo2_write(struct fpga_manager *mgr, const char *buf,
			 size_t count)
{
	struct spi_device *spi = mgr->priv;
	u32 progincr = LSC_PROGINCRNV;
	u8 payload[MACHXO2_BUF_SIZE];
	int i, ret;

	if (count % MACHXO2_PAGE_SIZE != 0) {
		dev_err(&mgr->dev, "Malformed payload.\n");
		return -EINVAL;
	}

	memcpy(payload, &progincr, MACHXO2_OP_SIZE);
	for (i = 0; i < count; i += MACHXO2_PAGE_SIZE) {
		memcpy(&payload[MACHXO2_OP_SIZE], &buf[i], MACHXO2_PAGE_SIZE);
		ret = spi_write(spi, payload, MACHXO2_BUF_SIZE);
		if (ret) {
			dev_err(&mgr->dev, "Error loading the bitstream.\n");
			return ret;
		}
		udelay(MACHXO2_HIGH_DELAY);
	}

	return 0;
}

static int machxo2_write_complete(struct fpga_manager *mgr,
				  struct fpga_image_info *info)
{
	struct spi_device *spi = mgr->priv;
	u32 progdone = ISC_PROGRAMDONE;
	u32 refresh = LSC_REFRESH;
	int ret;

	ret = spi_write(spi, &progdone, MACHXO2_OP_SIZE);
	if (ret)
		goto fail;

	udelay(MACHXO2_HIGH_DELAY);
	/* yep, LSC_REFRESH is 3 bytes long actually */
	ret = spi_write(spi, &refresh, MACHXO2_OP_SIZE - 1);
	if (ret)
		goto fail;

	return 0;

fail:
	dev_err(&mgr->dev, "Refresh failed.\n");
	return ret;
}

static const struct fpga_manager_ops machxo2_ops = {
	.state = machxo2_spi_state,
	.write_init = machxo2_write_init,
	.write = machxo2_write,
	.write_complete = machxo2_write_complete,
};

static int machxo2_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;

	if (spi->max_speed_hz > MACHXO2_MAX_SPEED) {
		dev_err(dev, "Speed is too high\n");
		return -EINVAL;
	}

	return fpga_mgr_register(dev, "Lattice MachXO2 SPI FPGA Manager",
				 &machxo2_ops, spi);
}

static int machxo2_spi_remove(struct spi_device *spi)
{
	struct device *dev = &spi->dev;

	fpga_mgr_unregister(dev);

	return 0;
}

static const struct of_device_id of_match[] = {
	{ .compatible = "lattice,machxo2-slave-spi", },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static const struct spi_device_id lattice_ids[] = {
	{ "machxo2-slave-spi", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, lattice_ids);

static struct spi_driver machxo2_spi_driver = {
	.driver = {
		.name = "machxo2-slave-spi",
		.of_match_table = of_match_ptr(of_match),
	},
	.probe = machxo2_spi_probe,
	.remove = machxo2_spi_remove,
	.id_table = lattice_ids,
};

module_spi_driver(machxo2_spi_driver)

MODULE_AUTHOR("Paolo Pisati <p.pisati@gmail.com>");
MODULE_DESCRIPTION("Load Lattice FPGA firmware over SPI");
MODULE_LICENSE("GPL v2");
