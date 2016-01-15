/* HiSilicon SPI Nor Flash Controller Driver
 *
 * Copyright (c) 2015-2016 HiSilicon Technologies Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/spi-nor.h>
#include <linux/of_platform.h>
#include <linux/slab.h>

/* Hardware register offsets and field definitions */
#define FMC_CFG				0x00
#define SPI_NOR_ADDR_MODE		BIT(10)
#define FMC_GLOBAL_CFG			0x04
#define FMC_GLOBAL_CFG_WP_ENABLE	BIT(6)
#define FMC_SPI_TIMING_CFG		0x08
#define TIMING_CFG_TCSH(nr)		(((nr) & 0xf) << 8)
#define TIMING_CFG_TCSS(nr)		(((nr) & 0xf) << 4)
#define TIMING_CFG_TSHSL(nr)		((nr) & 0xf)
#define CS_HOLD_TIME			0x6
#define CS_SETUP_TIME			0x6
#define CS_DESELECT_TIME		0xf
#define FMC_INT				0x18
#define FMC_INT_OP_DONE			BIT(0)
#define FMC_INT_CLR			0x20
#define FMC_CMD				0x24
#define FMC_CMD_CMD1(_cmd)		((_cmd) & 0xff)
#define FMC_ADDRL			0x2c
#define FMC_OP_CFG			0x30
#define OP_CFG_FM_CS(_cs)		((_cs) << 11)
#define OP_CFG_MEM_IF_TYPE(_type)	(((_type) & 0x7) << 7)
#define OP_CFG_ADDR_NUM(_addr)		(((_addr) & 0x7) << 4)
#define OP_CFG_DUMMY_NUM(_dummy)	((_dummy) & 0xf)
#define FMC_DATA_NUM			0x38
#define FMC_DATA_NUM_CNT(_n)		((_n) & 0x3fff)
#define FMC_OP				0x3c
#define FMC_OP_DUMMY_EN			BIT(8)
#define FMC_OP_CMD1_EN			BIT(7)
#define FMC_OP_ADDR_EN			BIT(6)
#define FMC_OP_WRITE_DATA_EN		BIT(5)
#define FMC_OP_READ_DATA_EN		BIT(2)
#define FMC_OP_READ_STATUS_EN		BIT(1)
#define FMC_OP_REG_OP_START		BIT(0)
#define FMC_DMA_LEN			0x40
#define FMC_DMA_LEN_SET(_len)		((_len) & 0x0fffffff)
#define FMC_DMA_SADDR_D0		0x4c
#define HIFMC_DMA_MAX_LEN		(4096)
#define HIFMC_DMA_MASK			(HIFMC_DMA_MAX_LEN - 1)
#define FMC_OP_DMA			0x68
#define OP_CTRL_RD_OPCODE(_code)	(((_code) & 0xff) << 16)
#define OP_CTRL_WR_OPCODE(_code)	(((_code) & 0xff) << 8)
#define OP_CTRL_RW_OP(_op)		((_op) << 1)
#define OP_CTRL_DMA_OP_READY		BIT(0)
#define FMC_OP_READ			0x0
#define FMC_OP_WRITE			0x1
#define FMC_WAIT_TIMEOUT		10000000

enum hifmc_iftype {
	IF_TYPE_STD,
	IF_TYPE_DUAL,
	IF_TYPE_DIO,
	IF_TYPE_QUAD,
	IF_TYPE_QIO,
};

struct hifmc_priv {
	int chipselect;
	u32 clkrate;
	struct hifmc_host *host;
};

#define HIFMC_MAX_CHIP_NUM		2
struct hifmc_host {
	struct device *dev;
	struct mutex lock;

	void __iomem *regbase;
	void __iomem *iobase;
	struct clk *clk;
	void *buffer;
	dma_addr_t dma_buffer;

	struct spi_nor	nor[HIFMC_MAX_CHIP_NUM];
	struct hifmc_priv priv[HIFMC_MAX_CHIP_NUM];
	int num_chip;
};

static inline int wait_op_finish(struct hifmc_host *host)
{
	unsigned int reg, timeout = FMC_WAIT_TIMEOUT;

	do {
		reg = readl(host->regbase + FMC_INT);
	} while (!(reg & FMC_INT_OP_DONE) && --timeout);

	if (!timeout) {
		dev_dbg(host->dev, "wait for operation finish timeout\n");
		return -EAGAIN;
	}

	return 0;
}

static int get_if_type(enum read_mode flash_read)
{
	enum hifmc_iftype if_type;

	switch (flash_read) {
	case SPI_NOR_DUAL:
		if_type = IF_TYPE_DUAL;
		break;
	case SPI_NOR_QUAD:
		if_type = IF_TYPE_QUAD;
		break;
	case SPI_NOR_NORMAL:
	case SPI_NOR_FAST:
	default:
		if_type = IF_TYPE_STD;
		break;
	}

	return if_type;
}

void hisi_spi_nor_init(struct hifmc_host *host)
{
	unsigned int reg;

	reg = TIMING_CFG_TCSH(CS_HOLD_TIME)
		| TIMING_CFG_TCSS(CS_SETUP_TIME)
		| TIMING_CFG_TSHSL(CS_DESELECT_TIME);
	writel(reg, host->regbase + FMC_SPI_TIMING_CFG);
}

static int hisi_spi_nor_prep(struct spi_nor *nor, enum spi_nor_ops ops)
{
	struct hifmc_priv *priv = nor->priv;
	struct hifmc_host *host = priv->host;
	int ret;

	mutex_lock(&host->lock);

	ret = clk_set_rate(host->clk, priv->clkrate);
	if (ret)
		goto out;

	ret = clk_prepare_enable(host->clk);
	if (ret)
		goto out;

	return 0;

out:
	mutex_unlock(&host->lock);
	return ret;
}

static void hisi_spi_nor_unprep(struct spi_nor *nor, enum spi_nor_ops ops)
{
	struct hifmc_priv *priv = nor->priv;
	struct hifmc_host *host = priv->host;

	clk_disable_unprepare(host->clk);
	mutex_unlock(&host->lock);
}

static void hisi_spi_nor_cmd_prepare(struct hifmc_host *host, u8 cmd,
		u32 *opcfg)
{
	u32 reg;

	*opcfg |= FMC_OP_CMD1_EN;
	switch (cmd) {
	case SPINOR_OP_RDID:
	case SPINOR_OP_RDSR:
	case SPINOR_OP_RDCR:
		*opcfg |= FMC_OP_READ_DATA_EN;
		break;
	case SPINOR_OP_WREN:
		reg = readl(host->regbase + FMC_GLOBAL_CFG);
		if (reg & FMC_GLOBAL_CFG_WP_ENABLE) {
			reg &= ~FMC_GLOBAL_CFG_WP_ENABLE;
			writel(reg, host->regbase + FMC_GLOBAL_CFG);
		}
		break;
	case SPINOR_OP_WRSR:
		*opcfg |= FMC_OP_WRITE_DATA_EN;
		break;
	case SPINOR_OP_BE_4K:
	case SPINOR_OP_BE_4K_PMC:
	case SPINOR_OP_SE_4B:
	case SPINOR_OP_SE:
		*opcfg |= FMC_OP_ADDR_EN;
		break;
	case SPINOR_OP_EN4B:
		reg = readl(host->regbase + FMC_CFG);
		reg |= SPI_NOR_ADDR_MODE;
		writel(reg, host->regbase + FMC_CFG);
		break;
	case SPINOR_OP_EX4B:
		reg = readl(host->regbase + FMC_CFG);
		reg &= ~SPI_NOR_ADDR_MODE;
		writel(reg, host->regbase + FMC_CFG);
		break;
	case SPINOR_OP_CHIP_ERASE:
	default:
		break;
	}
}

static int hisi_spi_nor_send_cmd(struct spi_nor *nor, u8 cmd, int len)
{
	struct hifmc_priv *priv = nor->priv;
	struct hifmc_host *host = priv->host;
	u32 reg, op_cfg = 0;

	hisi_spi_nor_cmd_prepare(host, cmd, &op_cfg);

	reg = FMC_CMD_CMD1(cmd);
	writel(reg, host->regbase + FMC_CMD);

	reg = OP_CFG_FM_CS(priv->chipselect);
	if (op_cfg & FMC_OP_ADDR_EN)
		reg |= OP_CFG_ADDR_NUM(nor->addr_width);
	writel(reg, host->regbase + FMC_OP_CFG);

	reg = FMC_DATA_NUM_CNT(len);
	writel(reg, host->regbase + FMC_DATA_NUM);

	writel(0xff, host->regbase + FMC_INT_CLR);
	reg = op_cfg | FMC_OP_REG_OP_START;
	writel(reg, host->regbase + FMC_OP);

	return wait_op_finish(host);
}

static int hisi_spi_nor_read_reg(struct spi_nor *nor, u8 opcode, u8 *buf,
		int len)
{
	struct hifmc_priv *priv = nor->priv;
	struct hifmc_host *host = priv->host;
	int ret;

	ret = hisi_spi_nor_send_cmd(nor, opcode, len);
	if (ret)
		return ret;

	memcpy(buf, host->iobase, len);

	return ret;
}

static int hisi_spi_nor_write_reg(struct spi_nor *nor, u8 opcode,
				u8 *buf, int len)
{
	struct hifmc_priv *priv = nor->priv;
	struct hifmc_host *host = priv->host;

	if (len)
		memcpy(host->iobase, buf, len);

	return hisi_spi_nor_send_cmd(nor, opcode, len);
}

static void hisi_spi_nor_dma_transfer(struct spi_nor *nor, u32 start_off,
		u32 dma_buf, u32 len, u8 op_type)
{
	struct hifmc_priv *priv = nor->priv;
	struct hifmc_host *host = priv->host;
	u8 if_type = 0, dummy = 0;
	u8 w_cmd = 0, r_cmd = 0;
	u32 reg;

	writel(start_off, host->regbase + FMC_ADDRL);

	if (op_type == FMC_OP_READ) {
		if_type = get_if_type(nor->flash_read);
		dummy = nor->read_dummy >> 3;
		r_cmd = nor->read_opcode;
	} else
		w_cmd = nor->program_opcode;

	reg = OP_CFG_FM_CS(priv->chipselect)
		| OP_CFG_MEM_IF_TYPE(if_type)
		| OP_CFG_ADDR_NUM(nor->addr_width)
		| OP_CFG_DUMMY_NUM(dummy);
	writel(reg, host->regbase + FMC_OP_CFG);

	reg = FMC_DMA_LEN_SET(len);
	writel(reg, host->regbase + FMC_DMA_LEN);
	writel(dma_buf, host->regbase + FMC_DMA_SADDR_D0);

	reg = OP_CTRL_RD_OPCODE(r_cmd)
		| OP_CTRL_WR_OPCODE(w_cmd)
		| OP_CTRL_RW_OP(op_type)
		| OP_CTRL_DMA_OP_READY;
	writel(0xff, host->regbase + FMC_INT_CLR);
	writel(reg, host->regbase + FMC_OP_DMA);
	wait_op_finish(host);
}

static int hisi_spi_nor_read(struct spi_nor *nor, loff_t from, size_t len,
		size_t *retlen, u_char *read_buf)
{
	struct hifmc_priv *priv = nor->priv;
	struct hifmc_host *host = priv->host;
	unsigned char *ptr = read_buf;
	int num;

	while (len > 0) {
		num = (len >= HIFMC_DMA_MAX_LEN)
			? HIFMC_DMA_MAX_LEN : len;
		hisi_spi_nor_dma_transfer(nor, from, host->dma_buffer,
				num, FMC_OP_READ);
		memcpy(ptr, host->buffer, num);
		ptr += num;
		from += num;
		len -= num;
	}
	*retlen += (size_t)(ptr - read_buf);

	return 0;
}

static void hisi_spi_nor_write(struct spi_nor *nor, loff_t to,
			size_t len, size_t *retlen, const u_char *write_buf)
{
	struct hifmc_priv *priv = nor->priv;
	struct hifmc_host *host = priv->host;
	const unsigned char *ptr = write_buf;
	int num;

	while (len > 0) {
		if (to & HIFMC_DMA_MASK)
			num = (HIFMC_DMA_MAX_LEN - (to & HIFMC_DMA_MASK))
				>= len	? len
				: (HIFMC_DMA_MAX_LEN - (to & HIFMC_DMA_MASK));
		else
			num = (len >= HIFMC_DMA_MAX_LEN)
				? HIFMC_DMA_MAX_LEN : len;
		memcpy(host->buffer, ptr, num);
		hisi_spi_nor_dma_transfer(nor, to, host->dma_buffer, num,
				FMC_OP_WRITE);
		to += num;
		ptr += num;
		len -= num;
	}
	*retlen += (size_t)(ptr - write_buf);
}

static int hisi_spi_nor_erase(struct spi_nor *nor, loff_t offs)
{
	struct hifmc_priv *priv = nor->priv;
	struct hifmc_host *host = priv->host;

	writel(offs, host->regbase + FMC_ADDRL);

	return hisi_spi_nor_send_cmd(nor, nor->erase_opcode, 0);
}

static int hisi_spi_nor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct hifmc_host *host;
	struct device_node *np;
	int ret, i = 0;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	platform_set_drvdata(pdev, host);
	host->dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "control");
	host->regbase = devm_ioremap_resource(dev, res);
	if (IS_ERR(host->regbase))
		return PTR_ERR(host->regbase);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "memory");
	host->iobase = devm_ioremap_resource(dev, res);
	if (IS_ERR(host->iobase))
		return PTR_ERR(host->iobase);

	host->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(host->clk))
		return PTR_ERR(host->clk);

	host->buffer = dmam_alloc_coherent(dev, HIFMC_DMA_MAX_LEN,
			&host->dma_buffer, GFP_KERNEL);
	if (!host->buffer)
		return -ENOMEM;

	mutex_init(&host->lock);
	clk_prepare_enable(host->clk);
	hisi_spi_nor_init(host);

	for_each_available_child_of_node(dev->of_node, np) {
		struct spi_nor *nor = &host->nor[i];
		struct hifmc_priv *priv = &host->priv[i];
		struct mtd_info *mtd = &nor->mtd;
		struct mtd_partition *parts = NULL;
		int nr_parts = 0;

		mtd->name = np->name;
		nor->dev = dev;
		nor->flash_node = np;
		ret = of_property_read_u32(np, "reg", &priv->chipselect);
		if (ret)
			goto fail;
		ret = of_property_read_u32(np, "spi-max-frequency",
				&priv->clkrate);
		if (ret)
			goto fail;
		priv->host = host;
		nor->priv = priv;

		nor->prepare = hisi_spi_nor_prep;
		nor->unprepare = hisi_spi_nor_unprep;
		nor->read_reg = hisi_spi_nor_read_reg;
		nor->write_reg = hisi_spi_nor_write_reg;
		nor->read = hisi_spi_nor_read;
		nor->write = hisi_spi_nor_write;
		nor->erase = hisi_spi_nor_erase;
		ret = spi_nor_scan(nor, NULL, SPI_NOR_QUAD);
		if (ret)
			goto fail;

		ret = mtd_device_register(mtd, parts, nr_parts);
		if (ret)
			goto fail;

		i++;
		host->num_chip++;
		if (i == HIFMC_MAX_CHIP_NUM)
			break;
	}

	return 0;

fail:
	for (i = 0; i < host->num_chip; i++)
		mtd_device_unregister(&host->nor[i].mtd);

	clk_disable_unprepare(host->clk);
	mutex_destroy(&host->lock);

	return ret;
}

static int hisi_spi_nor_remove(struct platform_device *pdev)
{
	struct hifmc_host *host = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < host->num_chip; i++)
		mtd_device_unregister(&host->nor[i].mtd);

	clk_disable_unprepare(host->clk);
	mutex_destroy(&host->lock);

	return 0;
}

static const struct of_device_id hisi_spi_nor_dt_ids[] = {
	{ .compatible = "hisilicon,hisi-sfc"},
	{ .compatible = "hisilicon,hi3519-sfc"},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hisi_spi_nor_dt_ids);

static struct platform_driver hisi_spi_nor_driver = {
	.driver = {
		.name	= "hisi-sfc",
		.of_match_table = hisi_spi_nor_dt_ids,
	},
	.probe	= hisi_spi_nor_probe,
	.remove	= hisi_spi_nor_remove,
};
module_platform_driver(hisi_spi_nor_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HiSilicon SPI Nor Flash Controller Driver");
