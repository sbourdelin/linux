/*
 * Adaptrum Anarion Quad SPI controller driver
 *
 * Copyright (C) 2017, Adaptrum, Inc.
 * (Written by Alexandru Gagniuc <alex.g at adaptrum.com> for Adaptrum, Inc.)
 * Licensed under the GPLv2 or (at your option) any later version.
 */

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/spi-nor.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define ASPI_REG_CLOCK			0x00
#define ASPI_REG_GO			0x04
#define ASPI_REG_CHAIN			0x08
#define ASPI_REG_CMD1			0x0c
#define ASPI_REG_CMD2			0x10
#define ASPI_REG_ADDR1			0x14
#define ASPI_REG_ADDR2			0x18
#define ASPI_REG_PERF1			0x1c
#define ASPI_REG_PERF2			0x20
#define ASPI_REG_HI_Z			0x24
#define ASPI_REG_BYTE_COUNT		0x28
#define ASPI_REG_DATA1			0x2c
#define ASPI_REG_DATA2			0x30
#define ASPI_REG_FINISH			0x34
#define ASPI_REG_XIP			0x38
#define ASPI_REG_FIFO_STATUS		0x3c
#define ASPI_REG_LAT			0x40
#define ASPI_REG_OUT_DELAY_0		0x44
#define ASPI_REG_OUT_DELAY_1		0x48
#define ASPI_REG_IN_DELAY_0		0x4c
#define ASPI_REG_IN_DELAY_1		0x50
#define ASPI_REG_DQS_DELAY		0x54
#define ASPI_REG_STATUS			0x58
#define ASPI_REG_IRQ_ENABLE		0x5c
#define ASPI_REG_IRQ_STATUS		0x60
#define ASPI_REG_AXI_BAR		0x64
#define ASPI_REG_READ_CFG		0x6c

#define ASPI_CLK_SW_RESET		(1 << 0)
#define ASPI_CLK_RESET_BUF		(1 << 1)
#define ASPI_CLK_RESET_ALL		(ASPI_CLK_SW_RESET | ASPI_CLK_RESET_BUF)
#define ASPI_CLK_SPI_MODE3		(1 << 2)
#define ASPI_CLOCK_DIV_MASK		(0xff << 8)
#define ASPI_CLOCK_DIV(d)		(((d) << 8) & ASPI_CLOCK_DIV_MASK)

#define ASPI_TIMEOUT_US		100000

#define ASPI_DATA_LEN_MASK		0x3fff
#define ASPI_MAX_XFER_LEN		(size_t)(ASPI_DATA_LEN_MASK + 1)

#define MODE_IO_X1			(0 << 16)
#define MODE_IO_X2			(1 << 16)
#define MODE_IO_X4			(2 << 16)
#define MODE_IO_SDR_POS_SKEW		(0 << 20)
#define MODE_IO_SDR_NEG_SKEW		(1 << 20)
#define MODE_IO_DDR_34_SKEW		(2 << 20)
#define MODE_IO_DDR_PN_SKEW		(3 << 20)
#define MODE_IO_DDR_DQS			(5 << 20)

#define ASPI_STATUS_BUSY			(1 << 2)

/*
 * This mask does not match reality. Get over it:
 * DATA2:	0x3fff
 * CMD2:	0x0003
 * ADDR2:	0x0007
 * PERF2:	0x0000
 * HI_Z:	0x003f
 * BCNT:	0x0007
 */
#define CHAIN_LEN(x)		((x - 1) & ASPI_DATA_LEN_MASK)

struct anarion_qspi {
	struct		spi_nor nor;
	struct		device *dev;
	uintptr_t	regbase;
	uintptr_t	xipbase;
	uint32_t	xfer_mode_cmd;
	uint32_t	xfer_mode_addr;
	uint32_t	xfer_mode_data;
	uint8_t		num_hi_z_clocks;
};

struct qspi_io_chain {
	uint8_t action;
	uint32_t data;
	uint16_t data_len;
	uint32_t mode;
};

enum chain_code {
	CHAIN_NOP = 0,
	CHAIN_CMD = 1,
	CHAIN_ADDR = 2,
	CHAIN_WTFIUM = 3,
	CHAIN_HI_Z = 4,
	CHAIN_DATA_OUT = 5,
	CHAIN_DATA_IN = 6,
	CHAIN_FINISH = 7,
};

static const struct chain_to_reg {
	uint8_t data_reg;
	uint8_t ctl_reg;
} chain_to_reg_map[] = {
	[CHAIN_NOP] =		{0, 0},
	[CHAIN_CMD] =		{ASPI_REG_CMD1, ASPI_REG_CMD2},
	[CHAIN_ADDR] =		{ASPI_REG_ADDR1, ASPI_REG_ADDR2},
	[CHAIN_WTFIUM] =	{0, 0},
	[CHAIN_HI_Z] =		{0, ASPI_REG_HI_Z},
	[CHAIN_DATA_OUT] =	{0, ASPI_REG_DATA2},
	[CHAIN_DATA_IN] =	{0, ASPI_REG_DATA2},
	[CHAIN_FINISH] =	{0, ASPI_REG_FINISH},
};

static uint32_t aspi_read_reg(struct anarion_qspi *spi, uint8_t reg)
{
	return readl((void *)(spi->regbase + reg));
};

static void aspi_write_reg(struct anarion_qspi *spi, uint8_t reg, uint32_t val)
{
	writel(val, (void *)(spi->regbase + reg));
};

static size_t aspi_get_fifo_level(struct anarion_qspi *spi)
{
	return aspi_read_reg(spi, ASPI_REG_FIFO_STATUS) & 0xff;
}

static void aspi_drain_fifo(struct anarion_qspi *aspi, uint8_t *buf, size_t len)
{
	uint32_t data;

	aspi_write_reg(aspi, ASPI_REG_BYTE_COUNT, sizeof(uint32_t));
	while (len >= 4) {
		data = aspi_read_reg(aspi, ASPI_REG_DATA1);
		memcpy(buf, &data, sizeof(data));
		buf += 4;
		len -= 4;
	}

	if (len) {
		aspi_write_reg(aspi, ASPI_REG_BYTE_COUNT, len);
		data = aspi_read_reg(aspi, ASPI_REG_DATA1);
		memcpy(buf, &data, len);
	}
}

static void aspi_seed_fifo(struct anarion_qspi *spi,
			   const uint8_t *buf, size_t len)
{
	uint32_t data;

	aspi_write_reg(spi, ASPI_REG_BYTE_COUNT, sizeof(uint32_t));
	while (len >= 4) {
		memcpy(&data, buf, sizeof(data));
		aspi_write_reg(spi, ASPI_REG_DATA1, data);
		buf += 4;
		len -= 4;
	}

	if (len) {
		aspi_write_reg(spi, ASPI_REG_BYTE_COUNT, len);
		memcpy(&data, buf, len);
		aspi_write_reg(spi, ASPI_REG_DATA1, data);
	}
}

static int aspi_wait_idle(struct anarion_qspi *aspi)
{
	uint32_t status;
	void *status_reg = (void *)(aspi->regbase + ASPI_REG_STATUS);

	return readl_poll_timeout(status_reg, status,
				  !(status & ASPI_STATUS_BUSY),
				  1, ASPI_TIMEOUT_US);
}

static int aspi_poll_and_seed_fifo(struct anarion_qspi *spi,
				   const void *src_addr, size_t len)
{
	size_t wait_us, fifo_space = 0, xfer_len;
	const uint8_t *src = src_addr;

	while (len > 0) {
		wait_us = 0;
		while (wait_us++ < ASPI_TIMEOUT_US) {
			fifo_space = 64 - aspi_get_fifo_level(spi);
			if (fifo_space)
				break;
			udelay(1);
		}

		xfer_len = min(len, fifo_space);
		aspi_seed_fifo(spi, src, xfer_len);
		src += xfer_len;
		len -= xfer_len;
	}

	return 0;
}

static void aspi_setup_chain(struct anarion_qspi *aspi,
			     const struct qspi_io_chain *chain,
			     size_t chain_len)
{
	size_t i;
	uint32_t chain_reg = 0;
	const struct qspi_io_chain *link;
	const struct chain_to_reg *regs;

	for (link = chain, i = 0; i < chain_len; i++, link++) {
		regs = &chain_to_reg_map[link->action];

		if (link->data_len && regs->data_reg)
			aspi_write_reg(aspi, regs->data_reg, link->data);

		if (regs->ctl_reg)
			aspi_write_reg(aspi, regs->ctl_reg,
				       CHAIN_LEN(link->data_len) | link->mode);

		chain_reg |= link->action << (i * 4);
	}

	chain_reg |= CHAIN_FINISH << (i * 4);

	aspi_write_reg(aspi, ASPI_REG_CHAIN, chain_reg);
}

static int aspi_execute_chain(struct anarion_qspi *aspi)
{
	/* Go, johnny go */
	aspi_write_reg(aspi, ASPI_REG_GO, 1);
	return aspi_wait_idle(aspi);
}

static int anarion_spi_read_nor_reg(struct spi_nor *nor, uint8_t opcode,
				    uint8_t *buf, int len)
{
	struct anarion_qspi *aspi = nor->priv;
	struct qspi_io_chain chain[] =  {
		{CHAIN_CMD, opcode, 1, MODE_IO_X1},
		{CHAIN_DATA_IN, 0, (uint16_t)len, MODE_IO_X1},
	};

	if (len >= 8)
		return -EMSGSIZE;

	aspi_setup_chain(aspi, chain, ARRAY_SIZE(chain));
	aspi_execute_chain(aspi);

	aspi_drain_fifo(aspi, buf, len);

	return 0;
}

static int anarion_qspi_cmd_addr(struct anarion_qspi *aspi, uint16_t cmd,
				 uint32_t addr, int addr_len)
{
	size_t chain_size;
	const struct qspi_io_chain chain[] = {
		{CHAIN_CMD, cmd, 1, MODE_IO_X1},
		{CHAIN_ADDR, addr, addr_len, MODE_IO_X1},
	};

	chain_size = addr_len ? ARRAY_SIZE(chain) : (ARRAY_SIZE(chain) - 1);
	aspi_setup_chain(aspi, chain, chain_size);
	return aspi_execute_chain(aspi);
}

static int anarion_spi_write_nor_reg(struct spi_nor *nor, uint8_t opcode,
				     uint8_t *buf, int len)
{
	uint32_t addr, i;
	struct anarion_qspi *aspi = nor->priv;

	if (len > sizeof(uint32_t))
		return -ENOTSUPP;

	for (i = 0, addr = 0; i < len; i++)
		addr |= buf[len - 1 - i] << (i * 8);

	return anarion_qspi_cmd_addr(aspi, opcode, addr, len);
}

/* After every operation, we need to restore the IO chain for XIP to work. */
static void aspi_setup_xip_read_chain(struct anarion_qspi *spi,
				      struct spi_nor *nor)
{
	struct qspi_io_chain chain[] =  {
		{CHAIN_CMD, nor->read_opcode, 1, spi->xfer_mode_cmd},
		{CHAIN_ADDR, 0, nor->addr_width, spi->xfer_mode_addr},
		{CHAIN_HI_Z, 0, spi->num_hi_z_clocks, spi->xfer_mode_addr},
		{CHAIN_DATA_IN, 0, ASPI_DATA_LEN_MASK, spi->xfer_mode_data},
	};

	aspi_setup_chain(spi, chain, ARRAY_SIZE(chain));
}

static int aspi_do_write_xfer(struct anarion_qspi *spi,
			      struct spi_nor *nor, uint32_t addr,
			      const void *buf, size_t len)
{
	struct qspi_io_chain chain[] =  {
		{CHAIN_CMD, nor->program_opcode, 1, MODE_IO_X1},
		{CHAIN_ADDR, addr, nor->addr_width, MODE_IO_X1},
		{CHAIN_DATA_OUT, 0, len, MODE_IO_X1},
	};

	aspi_setup_chain(spi, chain, ARRAY_SIZE(chain));

	/* Go, johnny go */
	aspi_write_reg(spi, ASPI_REG_GO, 1);

	aspi_poll_and_seed_fifo(spi, buf, len);
	return aspi_wait_idle(spi);
}

/* While we could send read commands manually to the flash chip, we'd have to
 * get data back through the DATA2 register. That is on the AHB bus, whereas
 * XIP reads go over AXI. Hence, we use the memory-mapped flash space for read.
 * TODO: Look at using DMA instead of memcpy().
 */
static ssize_t anarion_spi_nor_read(struct spi_nor *nor, loff_t from,
				      size_t len, uint8_t *read_buf)
{
	struct anarion_qspi *aspi = nor->priv;
	void *from_xip = (void *)(aspi->xipbase + from);

	aspi_setup_xip_read_chain(aspi, nor);
	memcpy(read_buf, from_xip, len);

	return len;
}

static ssize_t anarion_spi_nor_write(struct spi_nor *nor, loff_t to,
				     size_t len, const uint8_t *src)
{
	int ret;
	struct anarion_qspi *aspi = nor->priv;

	dev_err(aspi->dev, "%s, @0x%llx + %zu\n", __func__, to, len);

	if (len > nor->page_size)
		return -EINVAL;

	ret = aspi_do_write_xfer(aspi, nor, to, src, len);
	return (ret < 0) ? ret : len;
}

/* TODO: Revisit this when we get actual HW. Right now max speed is 6 MHz. */
static void aspi_configure_clocks(struct anarion_qspi *aspi)
{
	uint8_t div = 0;
	uint32_t ck_ctl = aspi_read_reg(aspi, ASPI_REG_CLOCK);

	ck_ctl &= ~ASPI_CLOCK_DIV_MASK;
	ck_ctl |= ASPI_CLOCK_DIV(div);
	aspi_write_reg(aspi, ASPI_REG_CLOCK, ck_ctl);
}

static int anarion_qspi_drv_probe(struct platform_device *pdev)
{
	int ret;
	void __iomem *mmiobase;
	struct resource *res;
	struct anarion_qspi *aspi;
	struct device_node *flash_node;
	struct spi_nor *nor;

	aspi = devm_kzalloc(&pdev->dev, sizeof(*aspi), GFP_KERNEL);
	if (!aspi)
		return -ENOMEM;
	platform_set_drvdata(pdev, aspi);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mmiobase  = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mmiobase)) {
		dev_err(&pdev->dev, "Cannot get base addresses (%ld)!\n",
			PTR_ERR(mmiobase));
		return PTR_ERR(mmiobase);
	}
	aspi->regbase = (uintptr_t)mmiobase;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	mmiobase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mmiobase)) {
		dev_err(&pdev->dev, "Cannot get XIP addresses (%ld)!\n",
			PTR_ERR(mmiobase));
		return PTR_ERR(mmiobase);
	}
	aspi->xipbase = (uintptr_t)mmiobase;

	aspi->dev = &pdev->dev;

	/* only support one attached flash */
	flash_node = of_get_next_available_child(pdev->dev.of_node, NULL);
	if (!flash_node) {
		dev_err(&pdev->dev, "no SPI flash device to configure\n");
		return -ENODEV;
	}

	/* Reset the controller */
	aspi_write_reg(aspi, ASPI_REG_CLOCK, ASPI_CLK_RESET_ALL);
	aspi_write_reg(aspi, ASPI_REG_LAT, 0x010);
	aspi_configure_clocks(aspi);

	nor = &aspi->nor;
	nor->priv = aspi;
	nor->dev = aspi->dev;
	nor->read = anarion_spi_nor_read;
	nor->write = anarion_spi_nor_write;
	nor->read_reg = anarion_spi_read_nor_reg;
	nor->write_reg = anarion_spi_write_nor_reg;

	spi_nor_set_flash_node(nor, flash_node);

	ret = spi_nor_scan(&aspi->nor, NULL, SPI_NOR_DUAL);
	if (ret)
		return ret;

	switch (nor->flash_read) {
	default:		/* Fall through */
	case SPI_NOR_NORMAL:
		aspi->num_hi_z_clocks = nor->read_dummy;
		aspi->xfer_mode_cmd = MODE_IO_X1;
		aspi->xfer_mode_addr = MODE_IO_X1;
		aspi->xfer_mode_data = MODE_IO_X1;
		break;
	case SPI_NOR_FAST:
		aspi->num_hi_z_clocks = nor->read_dummy;
		aspi->xfer_mode_cmd = MODE_IO_X1;
		aspi->xfer_mode_addr = MODE_IO_X1;
		aspi->xfer_mode_data = MODE_IO_X1;
		break;
	case SPI_NOR_DUAL:
		aspi->num_hi_z_clocks = nor->read_dummy;
		aspi->xfer_mode_cmd = MODE_IO_X1;
		aspi->xfer_mode_addr = MODE_IO_X1;
		aspi->xfer_mode_data = MODE_IO_X2;
		break;
	case SPI_NOR_QUAD:
		aspi->num_hi_z_clocks = nor->read_dummy;
		aspi->xfer_mode_cmd = MODE_IO_X1;
		aspi->xfer_mode_addr = MODE_IO_X1;
		aspi->xfer_mode_data = MODE_IO_X4;
		break;
	}

	aspi_setup_xip_read_chain(aspi, nor);

	mtd_device_register(&aspi->nor.mtd, NULL, 0);

	return 0;
}

static int anarion_qspi_drv_remove(struct platform_device *pdev)
{
	struct anarion_qspi *aspi = platform_get_drvdata(pdev);

	mtd_device_unregister(&aspi->nor.mtd);
	return 0;
}

static const struct of_device_id anarion_qspi_of_match[] = {
	{ .compatible = "adaptrum,anarion-qspi" },
	{ }
};
MODULE_DEVICE_TABLE(of, anarion_qspi_of_match);

static struct platform_driver anarion_qspi_driver = {
	.driver = {
		.name	= "anarion-qspi",
		.of_match_table = anarion_qspi_of_match,
	},
	.probe          = anarion_qspi_drv_probe,
	.remove		= anarion_qspi_drv_remove,
};
module_platform_driver(anarion_qspi_driver);

MODULE_DESCRIPTION("Adaptrum Anarion Quad SPI Controller Driver");
MODULE_AUTHOR("Alexandru Gagniuc <mr.nuke.me@gmail.com>");
MODULE_LICENSE("GPL v2");
