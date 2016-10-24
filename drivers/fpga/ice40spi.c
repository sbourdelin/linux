/*
 * FPGA Manager Driver for Lattice iCE40.
 *
 *  Copyright (c) 2016 Joel Holdsworth
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This driver adds support to the FPGA manager for configuring the SRAM of
 * Lattice iCE40 FPGAs through slave SPI.
 */

#include <linux/delay.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/gpio/consumer.h>
#include <linux/of_gpio.h>
#include <linux/spi/spi.h>

struct ice40_fpga_priv {
	struct spi_device *dev;
	struct gpio_desc *creset_b;
	struct gpio_desc *cdone;
	enum fpga_mgr_states state;
};

static enum fpga_mgr_states ice40_fpga_ops_state(struct fpga_manager *mgr)
{
	return mgr->state;
}

static void set_cs(struct spi_device *spi, bool enable)
{
	if (gpio_is_valid(spi->cs_gpio))
		gpio_set_value(spi->cs_gpio, !enable);
	else if (spi->master->set_cs)
		spi->master->set_cs(spi, !enable);
}

static int ice40_fpga_ops_write_init(struct fpga_manager *mgr, u32 flags,
	const char *buf, size_t count)
{
	struct ice40_fpga_priv *priv = mgr->priv;
	struct spi_device *dev = priv->dev;
	int ret;

	if ((flags & FPGA_MGR_PARTIAL_RECONFIG)) {
		dev_err(&dev->dev,
			"Partial reconfiguration is not supported\n");
		return -EINVAL;
	}

	/* Lock the bus, assert SS_B and CRESET_B */
	ret = spi_bus_lock(dev->master);
	if (ret) {
		dev_err(&dev->dev, "Failed to lock SPI bus, ret: %d\n", ret);
		return ret;
	}

	set_cs(dev, 1);
	gpiod_set_value(priv->creset_b, 1);

	/* Delay for >200ns */
	udelay(1);

	/* Come out of reset */
	gpiod_set_value(priv->creset_b, 0);

	/* Check CDONE is de-asserted i.e. the FPGA is reset */
	if (gpiod_get_value(priv->cdone)) {
		dev_err(&dev->dev, "Device reset failed, CDONE is asserted\n");
		ret = -EIO;
	}

	/* Wait for the housekeeping to complete */
	if (!ret)
		udelay(1200);

	/* Release the SS_B */
	set_cs(dev, 0);
	spi_bus_unlock(dev->master);

	return ret;
}

static int ice40_fpga_ops_write(struct fpga_manager *mgr,
	const char *buf, size_t count)
{
	struct ice40_fpga_priv *priv = mgr->priv;
	struct spi_device *dev = priv->dev;
	int ret;

	ret = spi_write(dev, buf, count);
	if (ret)
		dev_err(&dev->dev, "Error sending SPI data, ret: %d\n", ret);

	return ret;
}

static int ice40_fpga_ops_write_complete(struct fpga_manager *mgr, u32 flags)
{
	struct ice40_fpga_priv *priv = mgr->priv;
	struct spi_device *dev = priv->dev;
	int ret = 0;

	/* Check CDONE is asserted */
	if (!gpiod_get_value(priv->cdone)) {
		dev_err(&dev->dev,
			"CDONE was not asserted after firmware transfer\n");
		return -EIO;
	}

	/* Send >49-bits of zero-padding to activate the firmware */
	ret = spi_write(dev, NULL, 7);
	if (ret) {
		dev_err(&dev->dev, "Error sending zero padding, ret: %d\n",
			ret);
		return ret;
	}

	/* Success */
	return 0;
}

static const struct fpga_manager_ops ice40_fpga_ops = {
	.state = ice40_fpga_ops_state,
	.write_init = ice40_fpga_ops_write_init,
	.write = ice40_fpga_ops_write,
	.write_complete = ice40_fpga_ops_write_complete,
};

static int ice40_fpga_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct device_node *np = spi->dev.of_node;
	struct ice40_fpga_priv *priv;
	int ret;

	if (!np) {
		dev_err(dev, "No Device Tree entry\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(&spi->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = spi;

	/* Check board setup data. */
	if (spi->max_speed_hz > 25000000) {
		dev_err(dev, "speed is too high\n");
		return -EINVAL;
	} else if (spi->mode & SPI_CPHA) {
		dev_err(dev, "bad mode\n");
		return -EINVAL;
	}

	/* Set up the GPIOs */
	priv->cdone = devm_gpiod_get(dev, "cdone", GPIOD_IN);
	if (IS_ERR(priv->cdone)) {
		dev_err(dev, "Failed to get CDONE GPIO: %ld\n",
			PTR_ERR(priv->cdone));
		return ret;
	}

	priv->creset_b = devm_gpiod_get(dev, "creset_b", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->creset_b)) {
		dev_err(dev, "Failed to get CRESET_B GPIO: %ld\n",
			PTR_ERR(priv->creset_b));
		return ret;
	}

	/* Register with the FPGA manager */
	ret = fpga_mgr_register(dev, "Lattice iCE40 FPGA Manager",
				&ice40_fpga_ops, priv);
	if (ret) {
		dev_err(dev, "unable to register FPGA manager");
		return ret;
	}

	return 0;
}

static int ice40_fpga_remove(struct spi_device *spi)
{
	fpga_mgr_unregister(&spi->dev);
	return 0;
}

static const struct of_device_id ice40_fpga_of_match[] = {
	{ .compatible = "lattice,ice40-fpga-mgr", },
	{},
};

MODULE_DEVICE_TABLE(of, ice40_fpga_of_match);

static struct spi_driver ice40_fpga_driver = {
	.probe = ice40_fpga_probe,
	.remove = ice40_fpga_remove,
	.driver = {
		.name = "ice40spi",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ice40_fpga_of_match),
	},
};

module_spi_driver(ice40_fpga_driver);

MODULE_AUTHOR("Joel Holdsworth <joel@airwebreathe.org.uk>");
MODULE_DESCRIPTION("Lattice iCE40 FPGA Manager");
MODULE_LICENSE("GPL v2");
