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

#define ISC_DISABLE		0x00000026
#define ISC_ENABLE		0x000008c6
#define ISC_ERASE		0x0000040e
#define ISC_NOOP		0xffffffff
#define ISC_PROGRAMDONE		0x0000005e
#define LSC_CHECKBUSY		0x000000f0
#define LSC_INITADDRESS		0x00000046
#define LSC_PROGINCRNV		0x01000070
#define LSC_REFRESH		0x00000079

#define BUSYFLAG		BIT(7)

/*
 * Max CCLK in Slave SPI mode according to 'MachXO2 Family Data
 * Sheet' sysCONFIG Port Timing Specifications (3-36)
 */
#define MACHXO2_MAX_SPEED	66000000

#define MACHXO2_LOW_DELAY	5	/* us */
#define MACHXO2_HIGH_DELAY	200	/* us */
#define MACHXO2_REFRESH		4800	/* us */

#define MACHXO2_OP_SIZE		sizeof(u32)
#define MACHXO2_PAGE_SIZE	16

static int wait_until_not_busy(struct spi_device *spi)
{
	struct spi_message msg;
	struct spi_transfer rx, tx;
	u32 checkbusy = LSC_CHECKBUSY;
	u8 busy;
	int ret;

	do {
		memset(&rx, 0, sizeof(rx));
		memset(&tx, 0, sizeof(tx));
		tx.tx_buf = &checkbusy;
		tx.len = MACHXO2_OP_SIZE;
		rx.rx_buf = &busy;
		rx.len = sizeof(busy);
		spi_message_init(&msg);
		spi_message_add_tail(&tx, &msg);
		spi_message_add_tail(&rx, &msg);
		ret = spi_sync(spi, &msg);
		if (ret)
			return ret;
	} while (busy & BUSYFLAG);

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
	struct spi_message msg;
	struct spi_transfer tx[5];
	u32 disable = ISC_DISABLE;
	u32 bypass = ISC_NOOP;
	u32 enable = ISC_ENABLE;
	u32 erase = ISC_ERASE;
	u32 initaddr = LSC_INITADDRESS;
	int ret;

	if ((info->flags & FPGA_MGR_PARTIAL_RECONFIG)) {
		dev_err(&mgr->dev,
			"Partial reconfiguration is not supported\n");
		return -ENOTSUPP;
	}

	memset(tx, 0, sizeof(tx));
	spi_message_init(&msg);
	tx[0].tx_buf = &disable;
	tx[0].len = MACHXO2_OP_SIZE - 1;
	spi_message_add_tail(&tx[0], &msg);

	tx[1].tx_buf = &bypass;
	tx[1].len = MACHXO2_OP_SIZE;
	spi_message_add_tail(&tx[1], &msg);
	ret = spi_sync(spi, &msg);
	if (ret)
		goto fail;

	ret = wait_until_not_busy(spi);
	if (ret)
		goto fail;

	spi_message_init(&msg);
	tx[2].tx_buf = &enable;
	tx[2].len = MACHXO2_OP_SIZE;
	tx[2].delay_usecs = MACHXO2_LOW_DELAY;
	spi_message_add_tail(&tx[2], &msg);

	tx[3].tx_buf = &erase;
	tx[3].len = MACHXO2_OP_SIZE;
	spi_message_add_tail(&tx[3], &msg);
	ret = spi_sync(spi, &msg);
	if (ret)
		goto fail;

	ret = wait_until_not_busy(spi);
	if (ret)
		goto fail;

	spi_message_init(&msg);
	tx[4].tx_buf = &initaddr;
	tx[4].len = MACHXO2_OP_SIZE;
	spi_message_add_tail(&tx[4], &msg);
	ret = spi_sync(spi, &msg);
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
	struct spi_message msg;
	struct spi_transfer *tx;
	u32 progincr = LSC_PROGINCRNV;
	u8 *payload, *ptr;
	int i, num, ret;

	if (count % MACHXO2_PAGE_SIZE != 0) {
		dev_err(&mgr->dev, "Malformed payload.\n");
		ret = -EINVAL;
		goto out;
	}

	num = count / MACHXO2_PAGE_SIZE;
	payload = kzalloc(count + (num * MACHXO2_OP_SIZE), GFP_KERNEL);
	if (!payload) {
		dev_err(&mgr->dev, "Failed to allocate payload buffer\n");
		ret = -ENOMEM;
		goto out;
	}
	tx = kcalloc(num, sizeof(*tx), GFP_KERNEL);
	if (!tx) {
		dev_err(&mgr->dev, "Failed to allocate spi_transfer list\n");
		ret = -ENOMEM;
		goto fail;
	}
	memset(tx, 0, num * sizeof(*tx));

	spi_message_init(&msg);
	ptr = payload;
	for (i = 0; i < num; i++) {
		tx[i].tx_buf = ptr;
		tx[i].len = MACHXO2_OP_SIZE + MACHXO2_PAGE_SIZE;
		tx[i].delay_usecs = MACHXO2_HIGH_DELAY;
		memcpy(ptr, &progincr, MACHXO2_OP_SIZE);
		ptr += MACHXO2_OP_SIZE;
		memcpy(ptr, buf, MACHXO2_PAGE_SIZE);
		buf += MACHXO2_PAGE_SIZE;
		ptr += MACHXO2_PAGE_SIZE;
		spi_message_add_tail(&tx[i], &msg);
	}

	ret = spi_sync(spi, &msg);
	if (ret)
		dev_err(&mgr->dev, "Error loading the bitstream.\n");

	kfree(tx);
fail:
	kfree(payload);
out:
	return ret;
}

static int machxo2_write_complete(struct fpga_manager *mgr,
				  struct fpga_image_info *info)
{
	struct spi_device *spi = mgr->priv;
	struct spi_message msg;
	struct spi_transfer tx[2];
	u32 progdone = ISC_PROGRAMDONE;
	u32 refresh = LSC_REFRESH;
	int ret;

	memset(tx, 0, sizeof(tx));
	spi_message_init(&msg);
	tx[0].tx_buf = &progdone;
	tx[0].len = MACHXO2_OP_SIZE;
	spi_message_add_tail(&tx[0], &msg);
	ret = spi_sync(spi, &msg);
	if (ret)
		goto fail;

	ret = wait_until_not_busy(spi);
	if (ret)
		goto fail;

	spi_message_init(&msg);
	tx[1].tx_buf = &refresh;
	tx[1].len = MACHXO2_OP_SIZE - 1;
	tx[1].delay_usecs = MACHXO2_REFRESH;
	spi_message_add_tail(&tx[1], &msg);
	ret = spi_sync(spi, &msg);
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
