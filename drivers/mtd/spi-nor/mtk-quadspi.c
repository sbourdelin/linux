/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: Bayi Cheng <bayi.cheng@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/spi-nor.h>

#define MTK_NOR_CMD_REG			0x00
#define MTK_NOR_CNT_REG			0x04
#define MTK_NOR_RDSR_REG		0x08
#define MTK_NOR_RDATA_REG		0x0c
#define MTK_NOR_RADR0_REG		0x10
#define MTK_NOR_RADR1_REG		0x14
#define MTK_NOR_RADR2_REG		0x18
#define MTK_NOR_WDATA_REG		0x1c
#define MTK_NOR_PRGDATA0_REG		0x20
#define MTK_NOR_PRGDATA1_REG		0x24
#define MTK_NOR_PRGDATA2_REG		0x28
#define MTK_NOR_PRGDATA3_REG		0x2c
#define MTK_NOR_PRGDATA4_REG		0x30
#define MTK_NOR_PRGDATA5_REG		0x34
#define MTK_NOR_SHREG0_REG		0x38
#define MTK_NOR_SHREG1_REG		0x3c
#define MTK_NOR_SHREG2_REG		0x40
#define MTK_NOR_SHREG3_REG		0x44
#define MTK_NOR_SHREG4_REG		0x48
#define MTK_NOR_SHREG5_REG		0x4c
#define MTK_NOR_SHREG6_REG		0x50
#define MTK_NOR_SHREG7_REG		0x54
#define MTK_NOR_SHREG8_REG		0x58
#define MTK_NOR_SHREG9_REG		0x5c
#define MTK_NOR_CFG1_REG		0x60
#define MTK_NOR_CFG2_REG		0x64
#define MTK_NOR_CFG3_REG		0x68
#define MTK_NOR_STATUS0_REG		0x70
#define MTK_NOR_STATUS1_REG		0x74
#define MTK_NOR_STATUS2_REG		0x78
#define MTK_NOR_STATUS3_REG		0x7c
#define MTK_NOR_FLHCFG_REG		0x84
#define MTK_NOR_TIME_REG		0x94
#define MTK_NOR_PP_DATA_REG		0x98
#define MTK_NOR_PREBUF_STUS_REG		0x9c
#define MTK_NOR_DELSEL0_REG		0xa0
#define MTK_NOR_DELSEL1_REG		0xa4
#define MTK_NOR_INTRSTUS_REG		0xa8
#define MTK_NOR_INTREN_REG		0xac
#define MTK_NOR_CHKSUM_CTL_REG		0xb8
#define MTK_NOR_CHKSUM_REG		0xbc
#define MTK_NOR_CMD2_REG		0xc0
#define MTK_NOR_WRPROT_REG		0xc4
#define MTK_NOR_RADR3_REG		0xc8
#define MTK_NOR_DUAL_REG		0xcc
#define MTK_NOR_DELSEL2_REG		0xd0
#define MTK_NOR_DELSEL3_REG		0xd4
#define MTK_NOR_DELSEL4_REG		0xd8

/* commands for mtk nor controller */
#define MTK_NOR_READ_CMD		0x0
#define MTK_NOR_RDSR_CMD		0x2
#define MTK_NOR_PRG_CMD			0x4
#define MTK_NOR_WR_CMD			0x10
#define MTK_NOR_PIO_WR_CMD		0x90
#define MTK_NOR_WRSR_CMD		0x20
#define MTK_NOR_PIO_READ_CMD		0x81
#define MTK_NOR_WR_BUF_ENABLE		0x1
#define MTK_NOR_WR_BUF_DISABLE		0x0
#define MTK_NOR_ENABLE_SF_CMD		0x30
#define MTK_NOR_DUAD_ADDR_EN		0x8
#define MTK_NOR_QUAD_READ_EN		0x4
#define MTK_NOR_DUAL_ADDR_EN		0x2
#define MTK_NOR_DUAL_READ_EN		0x1
#define MTK_NOR_DUAL_DISABLE		0x0
#define MTK_NOR_FAST_READ		0x1

#define SFLASH_WRBUF_SIZE		128
#define get_nth_byte(d, n)		((d >> (8 * n)) & 0xff)

struct mt8173_nor {
	struct spi_nor nor;
	struct device *dev;
	void __iomem *base;	/* nor flash base address */
	struct clk *spi_clk;
	struct clk *nor_clk;
};

static void mt8173_nor_set_read_mode(struct mt8173_nor *mt8173_nor)
{
	struct spi_nor *nor = &mt8173_nor->nor;

	writeb(nor->read_opcode, mt8173_nor->base + MTK_NOR_PRGDATA3_REG);

	switch (nor->flash_read) {
	case SPI_NOR_FAST:
		writeb(MTK_NOR_FAST_READ, mt8173_nor->base +
		       MTK_NOR_CFG1_REG);
		break;
	case SPI_NOR_DUAL:
		writeb(MTK_NOR_DUAL_READ_EN, mt8173_nor->base +
		       MTK_NOR_DUAL_REG);
		break;
	case SPI_NOR_QUAD:
		writeb(MTK_NOR_QUAD_READ_EN, mt8173_nor->base +
		       MTK_NOR_DUAL_REG);
		break;
	default:
		writeb(MTK_NOR_DUAL_DISABLE, mt8173_nor->base +
		       MTK_NOR_DUAL_REG);
		break;
	}
}

static int mt8173_nor_execute_cmd(struct mt8173_nor *mt8173_nor, u8 cmdval)
{
	int reg;
	u8 val = cmdval & 0x1f;

	writeb(cmdval, mt8173_nor->base + MTK_NOR_CMD_REG);
	return readl_poll_timeout(mt8173_nor->base + MTK_NOR_CMD_REG, reg,
				  !(reg & val), 100, 10000);
}

static int mt8173_nor_do_tx(struct mt8173_nor *mt8173_nor, u8 op, u8 *buf,
			    int len)
{
	int i;

	if (len > 5)
		return -EINVAL;

	writeb(op, mt8173_nor->base + MTK_NOR_PRGDATA5_REG);

	for (i = 0; i < len; i++)
		writeb(buf[len - 1 - i], mt8173_nor->base +
			       MTK_NOR_PRGDATA0_REG + 4 * (4 - i));
	writeb((len + 1) * 8, mt8173_nor->base + MTK_NOR_CNT_REG);
	return mt8173_nor_execute_cmd(mt8173_nor, MTK_NOR_PRG_CMD);
}

/*
 * this function is used to execute special read commands.
 * such as SPINOR_OP_RDFSR, SPINOR_OP_RDCR, SPINOR_OP_RD_EVCR and so on.
 * len is no more than 1.
 */
static int mt8173_nor_do_rx(struct mt8173_nor *mt8173_nor, u8 op, u8 *buf,
			    int len)
{
	if (len > 1)
		return -EINVAL;

	writeb(op, mt8173_nor->base + MTK_NOR_PRGDATA5_REG);

	writeb(8, mt8173_nor->base + MTK_NOR_CNT_REG);
	return mt8173_nor_execute_cmd(mt8173_nor, MTK_NOR_PRG_CMD);
}

/* cmd1 sent to nor flash, cmd2 write to nor controller */
static int mt8173_nor_set_para(struct mt8173_nor *mt8173_nor, int cmd1,
			       int cmd2)
{
	writeb(cmd1, mt8173_nor->base + MTK_NOR_PRGDATA5_REG);
	writeb(8, mt8173_nor->base + MTK_NOR_CNT_REG);
	return mt8173_nor_execute_cmd(mt8173_nor, cmd2);
}

static int mt8173_nor_write_buffer_enable(struct mt8173_nor *mt8173_nor)
{
	u8 reg;

	/* the bit0 of MTK_NOR_CFG2_REG is pre-fetch buffer
	 * 0: pre-fetch buffer use for read
	 * 1: pre-fetch buffer use for page program
	 */
	writel(MTK_NOR_WR_BUF_ENABLE, mt8173_nor->base + MTK_NOR_CFG2_REG);
	return readb_poll_timeout(mt8173_nor->base + MTK_NOR_CFG2_REG, reg,
				  0x01 == (reg & 0x01), 100, 10000);
}

static int mt8173_nor_write_buffer_disable(struct mt8173_nor *mt8173_nor)
{
	u8 reg;

	writel(MTK_NOR_WR_BUF_DISABLE, mt8173_nor->base + MTK_NOR_CFG2_REG);
	return readb_poll_timeout(mt8173_nor->base + MTK_NOR_CFG2_REG, reg,
				  MTK_NOR_WR_BUF_DISABLE == (reg & 0x1), 100,
				  10000);
}

static int mt8173_nor_erase_sector(struct spi_nor *nor, loff_t offset)
{
	u8 buf[4], i = 0;
	struct mt8173_nor *mt8173_nor = nor->priv;

	while (i < 4) {
		buf[i] = get_nth_byte(offset, i);
		i++;
	}
	if (nor->mtd.size <= 0x1000000)
		return mt8173_nor_do_tx(mt8173_nor, SPINOR_OP_BE_4K, buf, 3);
	else
		return mt8173_nor_do_tx(mt8173_nor, SPINOR_OP_BE_4K, buf, 4);
}

static int mt8173_nor_read(struct spi_nor *nor, loff_t from, size_t length,
			   size_t *retlen, u_char *buffer)
{
	int i, ret;
	int addr = (int)from;
	u8 *buf = (u8 *)buffer;
	struct mt8173_nor *mt8173_nor = nor->priv;

	/* set mode for fast read mode ,dual mode or quad mode */
	mt8173_nor_set_read_mode(mt8173_nor);
	writeb((addr >> 24), mt8173_nor->base + MTK_NOR_RADR3_REG);
	writeb((addr >> 16), mt8173_nor->base + MTK_NOR_RADR2_REG);
	writeb((addr >> 8), mt8173_nor->base + MTK_NOR_RADR1_REG);
	writeb(addr, mt8173_nor->base + MTK_NOR_RADR0_REG);

	for (i = 0; i < length; i++, (*retlen)++) {
		ret = mt8173_nor_execute_cmd(mt8173_nor, MTK_NOR_PIO_READ_CMD);
		if (ret < 0)
			return ret;
		buf[i] = readb(mt8173_nor->base + MTK_NOR_RDATA_REG);
	}
	return 0;
}

static int mt8173_nor_write_single_byte(struct mt8173_nor *mt8173_nor,
					int addr, int length, u8 *data)
{
	int i, ret;

	writeb((addr >> 24), mt8173_nor->base + MTK_NOR_RADR3_REG);
	writeb((addr >> 16), mt8173_nor->base + MTK_NOR_RADR2_REG);
	writeb((addr >> 8), mt8173_nor->base + MTK_NOR_RADR1_REG);
	writeb(addr, mt8173_nor->base + MTK_NOR_RADR0_REG);

	for (i = 0; i < length; i++) {
		ret = mt8173_nor_execute_cmd(mt8173_nor, MTK_NOR_PIO_WR_CMD);
		if (ret < 0)
			return ret;
		writeb(*data++, mt8173_nor->base + MTK_NOR_WDATA_REG);
	}
	return 0;
}

static int mt8173_nor_write_buffer(struct mt8173_nor *mt8173_nor, int addr,
				   const u8 *buf)
{
	int i, bufidx, data;

	writeb((addr >> 24), mt8173_nor->base + MTK_NOR_RADR3_REG);
	writeb((addr >> 16), mt8173_nor->base + MTK_NOR_RADR2_REG);
	writeb((addr >> 8), mt8173_nor->base + MTK_NOR_RADR1_REG);
	writeb(addr, mt8173_nor->base + MTK_NOR_RADR0_REG);

	bufidx = 0;
	for (i = 0; i < SFLASH_WRBUF_SIZE; i += 4) {
		data = buf[bufidx + 3]<<24 | buf[bufidx + 2]<<16 |
		       buf[bufidx + 1]<<8 | buf[bufidx];
		bufidx += 4;
		writel(data, mt8173_nor->base + MTK_NOR_PP_DATA_REG);
	}
	return mt8173_nor_execute_cmd(mt8173_nor, MTK_NOR_WR_CMD);
}

static void mt8173_nor_write(struct spi_nor *nor, loff_t to, size_t len,
			     size_t *retlen, const u_char *buf)
{
	int ret;
	struct mt8173_nor *mt8173_nor = nor->priv;

	ret = mt8173_nor_write_buffer_enable(mt8173_nor);
	if (ret < 0)
		dev_warn(mt8173_nor->dev, "write buffer enable failed!\n");

	while (len >= SFLASH_WRBUF_SIZE) {
		ret = mt8173_nor_write_buffer(mt8173_nor, to, buf);
		if (ret < 0)
			dev_err(mt8173_nor->dev, "write buffer failed!\n");
		len -= SFLASH_WRBUF_SIZE;
		to += SFLASH_WRBUF_SIZE;
		buf += SFLASH_WRBUF_SIZE;
		(*retlen) += SFLASH_WRBUF_SIZE;
	}
	ret = mt8173_nor_write_buffer_disable(mt8173_nor);
	if (ret < 0)
		dev_warn(mt8173_nor->dev, "write buffer disable failed!\n");

	if (len) {
		ret = mt8173_nor_write_single_byte(mt8173_nor, to, (int)len,
						   (u8 *)buf);
		if (ret < 0)
			dev_err(mt8173_nor->dev, "write single byte failed!\n");
		(*retlen) += len;
	}
}

static int mt8173_nor_read_reg(struct spi_nor *nor, u8 opcode, u8 *buf, int len)
{
	int ret;
	struct mt8173_nor *mt8173_nor = nor->priv;

	/* mtk nor controller doesn't supoort SPINOR_OP_RDCR */
	switch (opcode) {
	case SPINOR_OP_RDID:
		/* read JEDEC ID need 4 bytes commands */
		memset(buf, 0x0, 4);
		ret = mt8173_nor_do_tx(mt8173_nor, opcode, buf, 3);
		if (ret < 0)
			return ret;

		/* mtk nor flash controller only support 3 bytes IDs */
		buf[2] = readb(mt8173_nor->base + MTK_NOR_SHREG0_REG);
		buf[1] = readb(mt8173_nor->base + MTK_NOR_SHREG1_REG);
		buf[0] = readb(mt8173_nor->base + MTK_NOR_SHREG2_REG);
		break;
	case SPINOR_OP_RDSR:
		ret = mt8173_nor_execute_cmd(mt8173_nor, MTK_NOR_RDSR_CMD);
		if (ret < 0)
			return ret;
		*buf = readb(mt8173_nor->base + MTK_NOR_RDSR_REG);
		break;
	default:
		/* read other register of nor flash */
		ret = mt8173_nor_do_rx(mt8173_nor, opcode, buf, len);
		*buf = readb(mt8173_nor->base + MTK_NOR_SHREG2_REG);
		break;
	}
	return ret;
}

static int mt8173_nor_write_reg(struct spi_nor *nor, u8 opcode, u8 *buf,
				int len)
{
	int ret;
	struct mt8173_nor *mt8173_nor = nor->priv;

	switch (opcode) {
	case SPINOR_OP_WRSR:
		ret = mt8173_nor_set_para(mt8173_nor, *buf,
					  MTK_NOR_WRSR_CMD);
		break;
	case SPINOR_OP_CHIP_ERASE:
		ret = mt8173_nor_set_para(mt8173_nor, opcode,
					  MTK_NOR_PRG_CMD);
		break;
	default:
		ret = mt8173_nor_do_tx(mt8173_nor, opcode, NULL, 0);
		if (ret)
			dev_warn(mt8173_nor->dev, "set write enable fail!\n");
		break;
	}
	return ret;
}

static int __init mtk_nor_init(struct mt8173_nor *mt8173_nor,
			       struct mtd_part_parser_data *ppdata)
{
	int ret;
	struct spi_nor *nor;

	writel(MTK_NOR_ENABLE_SF_CMD, mt8173_nor->base + MTK_NOR_WRPROT_REG);

	nor = &mt8173_nor->nor;
	nor->dev = mt8173_nor->dev;
	nor->priv = mt8173_nor;
	nor->flash_node = ppdata->of_node;

	/* fill the hooks to spi nor */
	nor->read = mt8173_nor_read;
	nor->read_reg = mt8173_nor_read_reg;
	nor->write = mt8173_nor_write;
	nor->write_reg = mt8173_nor_write_reg;
	nor->erase = mt8173_nor_erase_sector;
	nor->mtd.owner = THIS_MODULE;
	nor->mtd.name = "mtk_nor";
	/* initialized with NULL */
	ret = spi_nor_scan(nor, NULL, SPI_NOR_DUAL);
	if (ret)
		return ret;

	return mtd_device_parse_register(&nor->mtd, NULL, ppdata, NULL, 0);
}

static int mtk_nor_drv_probe(struct platform_device *pdev)
{
	struct device_node *flash_np;
	struct mtd_part_parser_data ppdata;
	struct resource *res;
	int ret;
	struct mt8173_nor *mt8173_nor;

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev, "No DT found\n");
		return -EINVAL;
	}

	mt8173_nor = devm_kzalloc(&pdev->dev, sizeof(*mt8173_nor), GFP_KERNEL);
	if (!mt8173_nor)
		return -ENOMEM;
	platform_set_drvdata(pdev, mt8173_nor);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mt8173_nor->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mt8173_nor->base))
		return PTR_ERR(mt8173_nor->base);

	mt8173_nor->spi_clk = devm_clk_get(&pdev->dev, "spi");
	if (IS_ERR(mt8173_nor->spi_clk)) {
		ret = PTR_ERR(mt8173_nor->spi_clk);
		goto nor_free;
	}

	mt8173_nor->nor_clk = devm_clk_get(&pdev->dev, "sf");
	if (IS_ERR(mt8173_nor->nor_clk)) {
		ret = PTR_ERR(mt8173_nor->nor_clk);
		goto nor_free;
	}

	mt8173_nor->dev = &pdev->dev;
	clk_prepare_enable(mt8173_nor->spi_clk);
	clk_prepare_enable(mt8173_nor->nor_clk);

	flash_np = of_get_next_available_child(pdev->dev.of_node, NULL);
	if (!flash_np) {
		dev_err(&pdev->dev, "no SPI flash device to configure\n");
		ret = -ENODEV;
		goto nor_free;
	}
	ppdata.of_node = flash_np;
	ret = mtk_nor_init(mt8173_nor, &ppdata);

nor_free:
	return ret;
}

static int mtk_nor_drv_remove(struct platform_device *pdev)
{
	struct mt8173_nor *mt8173_nor = platform_get_drvdata(pdev);

	clk_disable_unprepare(mt8173_nor->spi_clk);
	clk_disable_unprepare(mt8173_nor->nor_clk);
	return 0;
}

static const struct of_device_id mtk_nor_of_ids[] = {
	{ .compatible = "mediatek,mt8173-nor"},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_nor_of_ids);

static struct platform_driver mtk_nor_driver = {
	.probe = mtk_nor_drv_probe,
	.remove = mtk_nor_drv_remove,
	.driver = {
		.name = "mtk-nor",
		.of_match_table = mtk_nor_of_ids,
	},
};

module_platform_driver(mtk_nor_driver);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek SPI NOR Flash Driver");
