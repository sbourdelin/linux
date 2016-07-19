/*
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 * Author: Andi Shyti <andi.shyti@samsung.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * SPI driven IR LED device driver
 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <media/rc-core.h>

#define IR_SPI_DRIVER_NAME		"ir-spi"

#define IR_SPI_DEFAULT_FREQUENCY	38000
#define IR_SPI_BIT_PER_WORD		    8

struct ir_spi_data {
	struct rc_dev *rc;
	struct spi_device *spi;
	struct spi_transfer xfer;
	struct mutex mutex;
	struct regulator *regulator;
};

static int ir_spi_tx(struct rc_dev *dev, unsigned *buffer, unsigned n)
{
	int ret;
	struct ir_spi_data *idata = (struct ir_spi_data *) dev->priv;

	ret = regulator_enable(idata->regulator);
	if (ret)
		return ret;

	mutex_lock(&idata->mutex);
	idata->xfer.len = n;
	idata->xfer.tx_buf = buffer;
	mutex_unlock(&idata->mutex);

	ret = spi_sync_transfer(idata->spi, &idata->xfer, 1);
	if (ret)
		dev_err(&idata->spi->dev, "unable to deliver the signal\n");

	regulator_disable(idata->regulator);

	return ret;
}

static int ir_spi_set_tx_carrier(struct rc_dev *dev, u32 carrier)
{
	struct ir_spi_data *idata = (struct ir_spi_data *) dev->priv;

	if (!carrier)
		return -EINVAL;

	mutex_lock(&idata->mutex);
	idata->xfer.speed_hz = carrier;
	mutex_unlock(&idata->mutex);

	return 0;
}

static int ir_spi_probe(struct spi_device *spi)
{
	int ret;
	struct ir_spi_data *idata;

	idata = devm_kzalloc(&spi->dev, sizeof(*idata), GFP_KERNEL);
	if (!idata)
		return -ENOMEM;

	idata->regulator = devm_regulator_get(&spi->dev, "irda_regulator");
	if (IS_ERR(idata->regulator))
		return PTR_ERR(idata->regulator);

	idata->rc = rc_allocate_device(RC_DRIVER_IR_RAW_TX);
	if (!idata->rc)
		return -ENOMEM;

	idata->rc->s_tx_carrier = ir_spi_set_tx_carrier;
	idata->rc->tx_ir = ir_spi_tx;
	idata->rc->driver_name = IR_SPI_DRIVER_NAME;
	idata->rc->priv = idata;

        ret = rc_register_device(idata->rc);
	if (ret)
		return ret;

	mutex_init(&idata->mutex);

	idata->spi = spi;

	idata->xfer.bits_per_word = IR_SPI_BIT_PER_WORD;
	idata->xfer.speed_hz = IR_SPI_DEFAULT_FREQUENCY;

	return 0;
}

static int ir_spi_remove(struct spi_device *spi)
{
	struct ir_spi_data *idata = spi_get_drvdata(spi);

	rc_unregister_device(idata->rc);

	return 0;
}

static const struct of_device_id ir_spi_of_match[] = {
	{ .compatible = "ir-spi" },
	{},
};

static struct spi_driver ir_spi_driver = {
	.probe = ir_spi_probe,
	.remove = ir_spi_remove,
	.driver = {
		.name = IR_SPI_DRIVER_NAME,
		.of_match_table = ir_spi_of_match,
	},
};

module_spi_driver(ir_spi_driver);

MODULE_AUTHOR("Andi Shyti <andi.shyti@samsung.com>");
MODULE_DESCRIPTION("SPI IR LED");
MODULE_LICENSE("GPL v2");
