/*
 * J-Core SPI controller driver
 *
 * Copyright (C) 2012-2016 SEI Inc.
 *
 * Current version by Rich Felker
 * Based loosely on initial version by Oleksandr G Zhadan
 *
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/delay.h>

#define USE_MESSAGE_MODE 1

#define DRV_NAME "jcore_spi"

#define MAX_SPI_SPEED			12500000	/* 12.5 MHz */

#define CTRL_REG		0x0
#define DATA_REG		0x4

#define SPI_NOCHIP_CS	0
#define SPI_FLASH_CS	1
#define SPI_CONF_CS	2
#define SPI_SD_CS	2
#define SPI_CODEC_CS	3

#define JCORE_SPI_CTRL_ACS		0x01
#define JCORE_SPI_CTRL_XMIT		0x02
#define JCORE_SPI_STAT_BUSY		0x02
#define JCORE_SPI_CTRL_CCS		0x04
#define JCORE_SPI_CTRL_LOOP		0x08
#define JCORE_SPI_CTRL_DCS		0x10

#define JCORE_SPI_WAIT_RDY_MAX_LOOP	2000000	/* in usec */

struct jcore_spi {
	struct spi_master *master;
	void __iomem *base;
	volatile unsigned int ctrlReg;
	unsigned int csReg;
	unsigned int speedReg;
	unsigned int speed_hz;
};

static void jcore_spi_wait_till_ready(struct jcore_spi *hw, int timeout)
{
	while (timeout--) {
		hw->ctrlReg = readl(hw->base + CTRL_REG);
		if (!(hw->ctrlReg & JCORE_SPI_STAT_BUSY))
			return;
		cpu_relax();
	}
	pr_err("%s: Timeout..\n", __func__);
}

static void jcore_spi_program(struct jcore_spi *hw)
{
	jcore_spi_wait_till_ready(hw, JCORE_SPI_WAIT_RDY_MAX_LOOP);
	writel(hw->csReg | hw->speedReg, hw->base + CTRL_REG);	
}

static void jcore_spi_chipsel(struct spi_device *spi, bool value)
{
	struct jcore_spi *hw = spi_master_get_devdata(spi->master);

	pr_debug("%s: CS=%d\n", __func__, value);

	hw->csReg = ( JCORE_SPI_CTRL_ACS | JCORE_SPI_CTRL_CCS | JCORE_SPI_CTRL_DCS )
		^ (!value << 2*spi->chip_select);

	jcore_spi_program(hw);
}

static void jcore_spi_baudrate(struct jcore_spi *hw, int speed)
{
	if (speed == hw->speed_hz) return;
	hw->speed_hz = speed;
	hw->speedReg = ((MAX_SPI_SPEED / speed) - 1) << 27;
	jcore_spi_program(hw);
	pr_debug("%s: speed=%d pre=0x%x\n", __func__, speed, hw->speedReg);
}

static int jcore_spi_txrx(struct spi_master *master, struct spi_device *spi, struct spi_transfer *t)
{
	struct jcore_spi *hw = spi_master_get_devdata(master);

	void *ctrl_reg = hw->base + CTRL_REG;
	void *data_reg = hw->base + DATA_REG;
	int timeout;
	int xmit;
	int status;

	/* data buffers */
	const unsigned char *tx;
	unsigned char *rx;
	int len;
	int count;

	jcore_spi_baudrate(hw, t->speed_hz);

	xmit = hw->csReg | hw->speedReg | JCORE_SPI_CTRL_XMIT;
	tx = t->tx_buf;
	rx = t->rx_buf;
	len = t->len;

	for (count = 0; count < len; count++) {
		timeout = JCORE_SPI_WAIT_RDY_MAX_LOOP;
		do status = readl(ctrl_reg);
		while ((status & JCORE_SPI_STAT_BUSY) && --timeout);
		if (!timeout) break;

		writel(tx ? *tx++ : 0, data_reg);
		writel(xmit, ctrl_reg);

		timeout = JCORE_SPI_WAIT_RDY_MAX_LOOP;
		do status = readl(ctrl_reg);
		while ((status & JCORE_SPI_STAT_BUSY) && --timeout);
		if (!timeout) break;

		if (rx) *rx++ = readl(data_reg);
	}

#if !USE_MESSAGE_MODE
	spi_finalize_current_transfer(master);
#endif

	return count<len ? -EREMOTEIO : 0;
}

#if USE_MESSAGE_MODE
static int jcore_spi_transfer_one_message(struct spi_master *master,
					struct spi_message *msg)
{
	struct spi_device *spi = msg->spi;
	struct spi_transfer *xfer;
	bool keep_cs = false;
	int ret = 0;

	jcore_spi_chipsel(spi, false);

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		ret = jcore_spi_txrx(master, spi, xfer);
		if (ret) break;
		if (xfer->delay_usecs)
			udelay(xfer->delay_usecs);
		if (xfer->cs_change) {
			if (list_is_last(&xfer->transfer_list,
					 &msg->transfers)) {
				keep_cs = true;
			} else {
				jcore_spi_chipsel(spi, true);
				udelay(10);
				jcore_spi_chipsel(spi, false);
			}
		}
		msg->actual_length += xfer->len;
	}

	if (!keep_cs)
		jcore_spi_chipsel(spi, true);

	msg->status = ret;

	spi_finalize_current_message(master);

	return ret;
}
#endif

static int jcore_spi_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct jcore_spi *hw;
	struct spi_master *master;
	struct resource *res;
	int err = -ENODEV;

	master = spi_alloc_master(&pdev->dev, sizeof(struct jcore_spi));
	if (!master)
		return err;

	/* setup the master state. */
	master->num_chipselect = 3;
	master->mode_bits = SPI_MODE_3;
#if USE_MESSAGE_MODE
	master->transfer_one_message = jcore_spi_transfer_one_message;
#else
	master->transfer_one = jcore_spi_txrx;
#endif
	master->set_cs = jcore_spi_chipsel;
	master->dev.of_node = node;

	hw = spi_master_get_devdata(master);
	hw->master = master;
	platform_set_drvdata(pdev, hw);

	/* find and map our resources */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		goto exit_busy;
	if (!devm_request_mem_region
	    (&pdev->dev, res->start, resource_size(res), pdev->name))
		goto exit_busy;
	hw->base =
	    devm_ioremap_nocache(&pdev->dev, res->start, resource_size(res));
	if (!hw->base)
		goto exit_busy;

	jcore_spi_baudrate(hw, 400000);

	pdev->dev.dma_mask = 0;
	/* register our spi controller */
	err = spi_register_master(master);
	if (err)
		goto exit;
	dev_info(&pdev->dev, "base %p, noirq\n", hw->base);

	return 0;

exit_busy:
	err = -EBUSY;
exit:
	platform_set_drvdata(pdev, NULL);
	spi_master_put(master);
	return err;
}

static int jcore_spi_remove(struct platform_device *dev)
{
	struct jcore_spi *hw = platform_get_drvdata(dev);
	struct spi_master *master = hw->master;

	platform_set_drvdata(dev, NULL);
	spi_master_put(master);
	return 0;
}

static const struct of_device_id jcore_spi_of_match[] = {
	{ .compatible = "jcore,spi2" },
	{},
};

static struct platform_driver jcore_spi_driver = {
	.probe = jcore_spi_probe,
	.remove = jcore_spi_remove,
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = NULL,
		.of_match_table = jcore_spi_of_match,
	},
};

module_platform_driver(jcore_spi_driver);

MODULE_DESCRIPTION("J-Core SPI driver");
MODULE_AUTHOR("Rich Felker <dalias@libc.org>");
MODULE_ALIAS("platform:" DRV_NAME);
