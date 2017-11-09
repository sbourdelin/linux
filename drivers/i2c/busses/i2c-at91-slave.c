/*
 *  i2c slave support for Atmel's AT91 Two-Wire Interface (TWI)
 *
 *  Copyright (C) 2017 Juergen Fitschen <me@jue.yt>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>

#include "i2c-at91.h"

static irqreturn_t atmel_twi_interrupt_slave(int irq, void *dev_id)
{
	struct at91_twi_dev *dev = dev_id;
	const unsigned status = at91_twi_read(dev, AT91_TWI_SR);
	const unsigned irqstatus = status & at91_twi_read(dev, AT91_TWI_IMR);
	u8 value;

	if (!irqstatus)
		return IRQ_NONE;

	/*
	 * The order of processing AT91_TWI_TXRDY [a], AT91_TWI_SVACC [b] and
	 * AT91_TWI_RXRDY [c] status bit is important:
	 *  + Remote master wants to read data:
	 *    - AT91_TWI_SVACC IRQ with AT91_TWI_SVREAD unset is raised
	 *    - I2C_SLAVE_READ_REQUESTED slave event is fired and first byte
	 *      received from the I2C slave backend
	 *    - Byte is written to AT91_TWI_THR
	 *    - AT91_TWI_TXRDY is still set since AT91_TWI_SR isn't reread but
	 *      AT91_TWI_THR must not be written a second time!
	 *    --> Check AT91_TWI_TXRDY before AT91_TWI_SVACC
	 *  + Remote master wants to write data:
	 *    - AT91_TWI_SVACC IRQ with AT91_TWI_SVREAD set is raised
	 *    - If the first byte already has been received due to interrupt
	 *      latency, AT91_TWI_RXRDY is set and AT91_TWI_RHR has to be read
	 *      in the same IRQ handler run!
	 *    --> Check AT91_TWI_RXRDY after AT91_TWI_SVACC
	 */

	/*
	 * [a] Next byte can be stored into transmit holding register
	 */
	if ((dev->state == AT91_TWI_STATE_TX) && (status & AT91_TWI_TXRDY)) {
		i2c_slave_event(dev->slave, I2C_SLAVE_READ_PROCESSED, &value);
		writeb_relaxed(value, dev->base + AT91_TWI_THR);

		dev_dbg(dev->dev, "DATA %02x", value);
	}

	/*
	 * [b] An I2C transmission has been started and the interface detected
	 * its slave address.
	 */
	if ((dev->state == AT91_TWI_STATE_STOP) && (status & AT91_TWI_SVACC)) {
		/*
		 * AT91_TWI_SVREAD indicates whether data should be read from or
		 * written to the slave. This works flawlessly until the
		 * transmission has been stopped (i.e. AT91_TWI_EOSACC is set).
		 * If the interrupt latency is high, a master can start a write
		 * transmission, write one byte and stop the transmission before
		 * the IRQ handler is called. In that case AT91_TWI_SVACC,
		 * AT91_TWI_RXRDY and AT91_TWI_EOSACC are set, but we cannot
		 * rely on AT91_TWI_SVREAD. That's the reason why the following
		 * condition looks like it does.
		 */
		if (!(status & AT91_TWI_SVREAD) ||
		    ((status & AT91_TWI_EOSACC) && (status & AT91_TWI_RXRDY))) {
			i2c_slave_event(dev->slave,
					I2C_SLAVE_WRITE_REQUESTED, &value);

			at91_twi_write(dev, AT91_TWI_IER,
				       AT91_TWI_RXRDY | AT91_TWI_EOSACC);

			dev->state = AT91_TWI_STATE_RX;

			dev_dbg(dev->dev, "START LOCAL <- REMOTE");
		} else {
			i2c_slave_event(dev->slave,
					I2C_SLAVE_READ_REQUESTED, &value);
			writeb_relaxed(value, dev->base + AT91_TWI_THR);

			at91_twi_write(dev, AT91_TWI_IER,
				       AT91_TWI_TXRDY | AT91_TWI_EOSACC);

			dev->state = AT91_TWI_STATE_TX;

			dev_dbg(dev->dev, "START LOCAL -> REMOTE");
			dev_dbg(dev->dev, "DATA %02x", value);
		}
		at91_twi_write(dev, AT91_TWI_IDR, AT91_TWI_SVACC);
	}

	/*
	 * [c] Byte can be read from receive holding register
	 */
	if ((dev->state == AT91_TWI_STATE_RX) && (status & AT91_TWI_RXRDY)) {
		int rc;

		value = readb_relaxed(dev->base + AT91_TWI_RHR);
		rc = i2c_slave_event(dev->slave, I2C_SLAVE_WRITE_RECEIVED, &value);
		if ((rc < 0) && (dev->pdata->slave_mode_features & AT91_TWI_SM_CAN_NACK))
			at91_twi_write(dev, AT91_TWI_SMR, dev->smr | AT91_TWI_SMR_NACKEN);
		else
			at91_twi_write(dev, AT91_TWI_SMR, dev->smr);

		dev_dbg(dev->dev, "DATA %02x", value);
	}

	/*
	 * Master sent stop
	 */
	if ((dev->state != AT91_TWI_STATE_STOP) && (status & AT91_TWI_EOSACC)) {
		at91_twi_write(dev, AT91_TWI_IDR,
			       AT91_TWI_TXRDY | AT91_TWI_RXRDY | AT91_TWI_EOSACC);
		at91_twi_write(dev, AT91_TWI_CR,
			       AT91_TWI_THRCLR | AT91_TWI_RHRCLR);
		at91_twi_write(dev, AT91_TWI_IER,
			       AT91_TWI_SVACC);

		i2c_slave_event(dev->slave, I2C_SLAVE_STOP, &value);

		dev->state = AT91_TWI_STATE_STOP;
		dev_dbg(dev->dev, "STOP");
	}

	return IRQ_HANDLED;
}

static int at91_reg_slave(struct i2c_client *slave)
{
	struct at91_twi_dev *dev = i2c_get_adapdata(slave->adapter);

	if (dev->slave)
		return -EBUSY;

	if (slave->flags & I2C_CLIENT_TEN)
		return -EAFNOSUPPORT;

	/* Make sure twi_clk doesn't get turned off! */
	pm_runtime_get_sync(dev->dev);

	dev->slave = slave;
	dev->smr = AT91_TWI_SMR_SADR(slave->addr);

	at91_init_twi_bus(dev);

	dev_info(dev->dev, "entered slave mode (ADR=%d)\n", slave->addr);

	return 0;
}

static int at91_unreg_slave(struct i2c_client *slave)
{
	struct at91_twi_dev *dev = i2c_get_adapdata(slave->adapter);

	WARN_ON(!dev->slave);

	dev_info(dev->dev, "leaving slave mode\n");

	dev->slave = NULL;
	dev->smr = 0;

	at91_init_twi_bus(dev);

	pm_runtime_put(dev->dev);

	return 0;
}

static u32 at91_twi_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SLAVE | I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL
		| I2C_FUNC_SMBUS_READ_BLOCK_DATA;
}

static const struct i2c_algorithm at91_twi_algorithm_slave = {
	.reg_slave	= at91_reg_slave,
	.unreg_slave	= at91_unreg_slave,
	.functionality	= at91_twi_func,
};

int at91_twi_probe_slave(struct platform_device *pdev,
			 u32 phy_addr, struct at91_twi_dev *dev)
{
	int rc;

	rc = devm_request_irq(&pdev->dev, dev->irq, atmel_twi_interrupt_slave,
			      0, dev_name(dev->dev), dev);
	if (rc) {
		dev_err(dev->dev, "Cannot get irq %d: %d\n", dev->irq, rc);
		return rc;
	}

	dev->adapter.algo = &at91_twi_algorithm_slave;

	return 0;
}

void at91_init_twi_bus_slave(struct at91_twi_dev *dev)
{
	at91_twi_write(dev, AT91_TWI_CR, AT91_TWI_MSDIS);
	if (dev->smr) {
		dev->state = AT91_TWI_STATE_STOP;
		at91_twi_write(dev, AT91_TWI_SMR, dev->smr);
		at91_twi_write(dev, AT91_TWI_CR, AT91_TWI_SVEN);
		at91_twi_write(dev, AT91_TWI_IER, AT91_TWI_SVACC);
	}
}
