/*
 * Cavium ThunderX SPI driver.
 *
 * Copyright (C) 2016 Cavium Inc.
 * Authors: Jan Glauber <jglauber@cavium.com>
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/spi/spi.h>

#include "spi-cavium.h"

#define DRV_NAME "spi-thunderx"

#define SYS_FREQ_DEFAULT		700000000

static void thunderx_spi_clock_enable(struct device *dev, struct octeon_spi *p)
{
	int ret;

	p->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(p->clk)) {
		p->clk = NULL;
		goto skip;
	}

	ret = clk_prepare_enable(p->clk);
	if (ret)
		goto skip;
	p->sys_freq = clk_get_rate(p->clk);

skip:
	if (!p->sys_freq)
		p->sys_freq = SYS_FREQ_DEFAULT;

	dev_info(dev, "Set system clock to %u\n", p->sys_freq);
}

static void thunderx_spi_clock_disable(struct device *dev, struct clk *clk)
{
	if (!clk)
		return;
	clk_disable_unprepare(clk);
	devm_clk_put(dev, clk);
}

static int thunderx_spi_probe(struct pci_dev *pdev,
			      const struct pci_device_id *ent)
{
	struct device *dev = &pdev->dev;
	struct spi_master *master;
	struct octeon_spi *p;
	int ret = -ENOENT;

	master = spi_alloc_master(dev, sizeof(struct octeon_spi));
	if (!master)
		return -ENOMEM;
	p = spi_master_get_devdata(master);

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(dev, "Failed to enable PCI device\n");
		goto out_free;
	}

	ret = pci_request_regions(pdev, DRV_NAME);
	if (ret) {
		dev_err(dev, "PCI request regions failed 0x%x\n", ret);
		goto out_disable;
	}

	p->register_base = pci_ioremap_bar(pdev, 0);
	if (!p->register_base) {
		dev_err(dev, "Cannot map reg base\n");
		ret = -EINVAL;
		goto out_region;
	}

	p->regs.config = 0x1000;
	p->regs.status = 0x1008;
	p->regs.tx = 0x1010;
	p->regs.data = 0x1080;

	thunderx_spi_clock_enable(dev, p);

	master->num_chipselect = 4;
	master->mode_bits = SPI_CPHA | SPI_CPOL | SPI_CS_HIGH |
			    SPI_LSB_FIRST | SPI_3WIRE;
	master->transfer_one_message = octeon_spi_transfer_one_message;
	master->bits_per_word_mask = SPI_BPW_MASK(8);
	master->max_speed_hz = OCTEON_SPI_MAX_CLOCK_HZ;
	master->dev.of_node = pdev->dev.of_node;

	pci_set_drvdata(pdev, master);
	ret = devm_spi_register_master(dev, master);
	if (ret) {
		dev_err(&pdev->dev, "Register master failed: %d\n", ret);
		goto out_unmap;
	}

	dev_info(&pdev->dev, "Cavium SPI bus driver probed\n");
	return 0;

out_unmap:
	thunderx_spi_clock_disable(dev, p->clk);
	iounmap(p->register_base);
out_region:
	pci_release_regions(pdev);
out_disable:
	pci_disable_device(pdev);
out_free:
	spi_master_put(master);
	return ret;
}

static void thunderx_spi_remove(struct pci_dev *pdev)
{
	struct spi_master *master = pci_get_drvdata(pdev);
	struct octeon_spi *p;

	p = spi_master_get_devdata(master);
	if (!p)
		return;

	/* Put everything in a known state. */
	writeq(0, p->register_base + OCTEON_SPI_CFG(p));

	thunderx_spi_clock_disable(&pdev->dev, p->clk);
	iounmap(p->register_base);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
}

#define PCI_DEVICE_ID_THUNDERX_SPI      0xa00b

static const struct pci_device_id thunderx_spi_pci_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVICE_ID_THUNDERX_SPI) },
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, thunderx_spi_pci_id_table);

static struct pci_driver thunderx_spi_driver = {
	.name		= DRV_NAME,
	.id_table	= thunderx_spi_pci_id_table,
	.probe		= thunderx_spi_probe,
	.remove		= thunderx_spi_remove,
};

module_pci_driver(thunderx_spi_driver);

MODULE_DESCRIPTION("Cavium, Inc. ThunderX SPI bus driver");
MODULE_AUTHOR("Jan Glauber");
MODULE_LICENSE("GPL");
