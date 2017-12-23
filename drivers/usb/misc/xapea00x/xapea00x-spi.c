// SPDX-License-Identifier: GPL-2.0+
/*
 *  Driver for the XAP-EA-00x series of the Xaptum Edge Access Card, a
 *  TPM 2.0-based hardware module for authenticating IoT devices and
 *  gateways.
 *
 *  Copyright (c) 2017 Xaptum, Inc.
 */

#include "xapea00x.h"

/*******************************************************************************
 * SPI master functions
 */

/**
 * xapea00x_spi_setup - Setup the SPI channel for the TPM.
 *
 * Context: !in_interrupt()
 *
 * Return: If successful, 0. Otherwise a negative error number.
 */
static int xapea00x_spi_setup(struct spi_device *spi)
{
	struct xapea00x_device *dev;
	int retval;

	dev = spi_master_get_devdata(spi->master);

	/* Verify that this is the TPM device */
	if (spi->chip_select != 0) {
		retval = -EINVAL;
		goto err;
	}

	/* Set the SPI parameters for the TPM channel. */
	retval = xapea00x_br_set_spi_word(dev, 0, XAPEA00X_TPM_SPI_WORD);
	if (retval)
		goto err;

	/*
	 * Disable auto chip select for the TPM channel.
	 * Must be done after setting the SPI parameters.
	 */
	retval = xapea00x_br_set_gpio_cs(dev, 0, XAPEA00X_GPIO_CS_DISABLED);
	if (retval)
		goto err;

	/* De-assert chip select for the TPM channel. */
	retval = xapea00x_br_set_gpio_value(dev, 0, 1);
	if (retval)
		goto err;

	dev_dbg(&dev->interface->dev, "configured spi channel for tpm\n");
	return 0;

err:
	dev_err(&dev->interface->dev,
		"configuring SPI channel failed with %d\n", retval);
	return retval;
}

/**
 * xapea00x_spi_cleanup
 *
 * Context: !in_interrupt()
 */
static void xapea00x_spi_cleanup(struct spi_device *spi)
{
	dev_dbg(&spi->dev, "%s\n", __func__);
}

/**
 * xapea00x_spi_transfer - Execute a single SPI transfer.
 * @dev: pointer to the device to do the transfer on
 * @tx_buf: pointer to the data to send, if not NULL
 * @rx_buf: pointer to the buffer to store the received data, if not NULL
 * @len: length in bytes of the data to send/receive
 * @cs_hold: If non-zero, the chip select will remain asserted
 * @delay_usecs: If nonzero, how long to delay after last bit transfer
 *
 * Context: !in_interrupt()
 *
 * Return: If successful, 0. Otherwise a negative error number.
 */
int xapea00x_spi_transfer(struct xapea00x_device *dev,
				 const void *tx_buf, void *rx_buf, u32 len,
				 int cs_hold, u16 delay_usecs)
{
	int retval;

	/* Assert chip select */
	retval = xapea00x_br_set_gpio_value(dev, 0, 0);
	if (retval)
		goto out;

	/* empty transfer */
	if (!tx_buf && !rx_buf)
		retval = 0;
	/* read transfer */
	else if (!tx_buf)
		retval = xapea00x_br_spi_read(dev, rx_buf, len);
	/* write transfer */
	else if (!rx_buf)
		retval = xapea00x_br_spi_write(dev, tx_buf, len);
	/* write_read transfer */
	else
		retval = xapea00x_br_spi_write_read(dev, tx_buf, rx_buf, len);

	/* Deassert chip select, if requested */
	if (!cs_hold)
		retval = xapea00x_br_set_gpio_value(dev, 0, 1);

	/* Delay for the requested time */
	udelay(delay_usecs);

out:
	return retval;
}

/**
 * xapea00x_spi_transfer_one_message - Execute a full SPI message.
 * @master: The SPI master on which to execute the message.
 * @msg: The SPI message to execute.
 *
 * Context: !in_interrupt()
 *
 * Return: If successfull, 0. Otherwise a negative erorr number.
 */
static int xapea00x_spi_transfer_one_message(struct spi_master *master,
					     struct spi_message *msg)
{
	struct xapea00x_device *dev;
	struct spi_transfer *xfer;
	int is_last, retval;

	dev = spi_master_get_devdata(master);

	/* perform all transfers */
	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		is_last = list_is_last(&xfer->transfer_list, &msg->transfers);

		/* Transfer message */
		retval = xapea00x_spi_transfer(dev, xfer->tx_buf,
					       xfer->rx_buf, xfer->len,
					       is_last == xfer->cs_change,
					       xfer->delay_usecs);
		if (retval)
			goto out;

		msg->actual_length += xfer->len;
	}

	retval = 0;

out:
	msg->status = retval;
	spi_finalize_current_message(master);
	return retval;
}

/**
 * xapea00x_spi_probe - Register and configure the SPI master.
 * @dev: the device whose SPI master to register
 *
 * Return: If successful, 0. Otherwise a negative error number.
 */
static int xapea00x_spi_probe(struct xapea00x_device *dev)
{
	struct spi_master *spi_master;
	int retval;

	spi_master = spi_alloc_master(&dev->udev->dev, sizeof(void *));
	if (!spi_master) {
		retval = -ENOMEM;
		goto err_out;
	}

	spi_master_set_devdata(spi_master, dev);

	spi_master->min_speed_hz = 93 * 1000 + 800; /* 93.9kHz */
	spi_master->max_speed_hz = 12 * 1000 * 1000; /* 12 MHz */

	spi_master->bus_num = -1; /* dynamically assigned */
	spi_master->num_chipselect = XAPEA00X_NUM_CS;
	spi_master->mode_bits = SPI_MODE_0;

	spi_master->flags = 0;
	spi_master->setup = xapea00x_spi_setup;
	spi_master->cleanup = xapea00x_spi_cleanup;
	spi_master->transfer_one_message = xapea00x_spi_transfer_one_message;

	retval = spi_register_master(spi_master);

	if (retval)
		goto free_spi;

	dev->spi_master = spi_master;
	dev_dbg(&dev->interface->dev, "registered SPI master\n");

	return 0;

free_spi:
	spi_master_put(spi_master);
	dev->spi_master = NULL;

err_out:
	return retval;
}
