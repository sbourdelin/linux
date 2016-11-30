/**
 * Copyright (c) 2015 United Western Technologies, Corporation
 *
 * Joshua Clayton <stillcompiling@gmail.com>
 *
 * Manage Altera fpga firmware that is loaded over spi.
 * Firmware must be in binary "rbf" format.
 * Works on Cyclone V. Should work on cyclone series.
 * May work on other Altera fpgas.
 *
 */

#include <linux/bitrev.h>
#include <linux/delay.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/spi/spi.h>
#include <linux/sizes.h>

#define FPGA_RESET_TIME		50   /* time in usecs to trigger FPGA config */
#define FPGA_MIN_DELAY		50   /* min usecs to wait for config status */
#define FPGA_MAX_DELAY		1000 /* max usecs to wait for config status */

struct cyclonespi_conf {
	struct gpio_desc *config;
	struct gpio_desc *status;
	struct spi_device *spi;
};

static const struct of_device_id of_ef_match[] = {
	{ .compatible = "altr,cyclone-ps-spi-fpga-mgr", },
	{}
};
MODULE_DEVICE_TABLE(of, of_ef_match);

static enum fpga_mgr_states cyclonespi_state(struct fpga_manager *mgr)
{
	return mgr->state;
}

static int cyclonespi_write_init(struct fpga_manager *mgr, u32 flags,
				 const char *buf, size_t count)
{
	struct cyclonespi_conf *conf = (struct cyclonespi_conf *)mgr->priv;
	int i;

	if (flags & FPGA_MGR_PARTIAL_RECONFIG) {
		dev_err(&mgr->dev, "Partial reconfiguration not supported.\n");
		return -EINVAL;
	}

	gpiod_set_value(conf->config, 0);
	usleep_range(FPGA_RESET_TIME, FPGA_RESET_TIME + 20);
	if (gpiod_get_value(conf->status) == 1) {
		dev_err(&mgr->dev, "Status pin should be low.\n");
		return -EIO;
	}

	gpiod_set_value(conf->config, 1);
	for (i = 0; i < (FPGA_MAX_DELAY / FPGA_MIN_DELAY); i++) {
		usleep_range(FPGA_MIN_DELAY, FPGA_MIN_DELAY + 20);
		if (gpiod_get_value(conf->status))
			return 0;
	}

	dev_err(&mgr->dev, "Status pin not ready.\n");
	return -EIO;
}

static void rev_buf(void *buf, size_t len)
{
	u32 *fw32 = (u32 *)buf;
	const u32 *fw_end = (u32 *)(buf + len);

	/* set buffer to lsb first */
	while (fw32 < fw_end) {
		*fw32 = bitrev8x4(*fw32);
		fw32++;
	}
}

static int cyclonespi_write(struct fpga_manager *mgr, const char *buf,
			    size_t count)
{
	struct cyclonespi_conf *conf = (struct cyclonespi_conf *)mgr->priv;
	const char *fw_data = buf;
	const char *fw_data_end = fw_data + count;

	while (fw_data < fw_data_end) {
		int ret;
		size_t stride = min(fw_data_end - fw_data, SZ_4K);

		rev_buf((void *)fw_data, stride);
		ret = spi_write(conf->spi, fw_data, stride);
		if (ret) {
			dev_err(&mgr->dev, "spi error in firmware write: %d\n",
				ret);
			return ret;
		}
		fw_data += stride;
	}

	return 0;
}

static int cyclonespi_write_complete(struct fpga_manager *mgr, u32 flags)
{
	struct cyclonespi_conf *conf = (struct cyclonespi_conf *)mgr->priv;

	if (gpiod_get_value(conf->status) == 0) {
		dev_err(&mgr->dev, "Error during configuration.\n");
		return -EIO;
	}

	return 0;
}

static const struct fpga_manager_ops cyclonespi_ops = {
	.state = cyclonespi_state,
	.write_init = cyclonespi_write_init,
	.write = cyclonespi_write,
	.write_complete = cyclonespi_write_complete,
};

static int cyclonespi_probe(struct spi_device *spi)
{
	struct cyclonespi_conf *conf = devm_kzalloc(&spi->dev, sizeof(*conf),
						GFP_KERNEL);

	if (!conf)
		return -ENOMEM;

	conf->spi = spi;
	conf->config = devm_gpiod_get(&spi->dev, "config", GPIOD_OUT_LOW);
	if (IS_ERR(conf->config)) {
		dev_err(&spi->dev, "Failed to get config gpio: %ld\n",
			PTR_ERR(conf->config));
		return PTR_ERR(conf->config);
	}

	conf->status = devm_gpiod_get(&spi->dev, "status", GPIOD_IN);
	if (IS_ERR(conf->status)) {
		dev_err(&spi->dev, "Failed to get status gpio: %ld\n",
			PTR_ERR(conf->status));
		return PTR_ERR(conf->status);
	}

	return fpga_mgr_register(&spi->dev,
				 "Altera Cyclone PS SPI FPGA Manager",
				 &cyclonespi_ops, conf);
}

static int cyclonespi_remove(struct spi_device *spi)
{
	fpga_mgr_unregister(&spi->dev);

	return 0;
}

static struct spi_driver cyclonespi_driver = {
	.driver = {
		.name   = "cyclone-ps-spi",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(of_ef_match),
	},
	.probe  = cyclonespi_probe,
	.remove = cyclonespi_remove,
};

module_spi_driver(cyclonespi_driver)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joshua Clayton <stillcompiling@gmail.com>");
MODULE_DESCRIPTION("Module to load Altera FPGA firmware over spi");
