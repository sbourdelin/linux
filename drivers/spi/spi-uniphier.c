// SPDX-License-Identifier: GPL-2.0
/*
 * spi-uniphier.c - Socionext UniPhier SPI controller driver
 *
 * Copyright 2012      Panasonic Corporation
 * Copyright 2016-2018 Socionext Inc.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define SSI_TIMEOUT		2000	/* ms */
#define SSI_MAX_CLK_DIVIDER	254
#define SSI_MIN_CLK_DIVIDER	4

struct uniphier_spi_priv {
	void __iomem *base;
	int irq;
	struct clk *clk;
	struct spi_master *master;
	struct completion xfer_done;

	int error;
	unsigned int tx_bytes;
	unsigned int rx_bytes;
	const u8 *tx_buf;
	u8 *rx_buf;

	u8 bits_per_word;
	u16 mode;
	u32 speed_hz;
};

#define SSI_CTL			0x0
#define   SSI_CTL_EN		BIT(0)

#define SSI_CKS			0x4
#define   SSI_CKS_CKRAT_MASK	GENMASK(7, 0)
#define   SSI_CKS_CKPHS		BIT(14)
#define   SSI_CKS_CKINIT	BIT(13)
#define   SSI_CKS_CKDLY		BIT(12)

#define SSI_TXWDS		0x8
#define   SSI_TXWDS_WDLEN_MASK	GENMASK(13, 8)
#define   SSI_TXWDS_TDTF_MASK	GENMASK(7, 6)
#define   SSI_TXWDS_DTLEN_MASK	GENMASK(5, 0)

#define SSI_RXWDS		0xc
#define   SSI_RXWDS_DTLEN_MASK	GENMASK(5, 0)

#define SSI_FPS			0x10
#define   SSI_FPS_FSPOL		BIT(15)
#define   SSI_FPS_FSTRT		BIT(14)

#define SSI_SR			0x14
#define   SSI_SR_RNE		BIT(0)

#define SSI_IE			0x18
#define   SSI_IE_RCIE		BIT(3)
#define   SSI_IE_RORIE		BIT(0)

#define SSI_IS			0x1c
#define   SSI_IS_RXRS		BIT(9)
#define   SSI_IS_RCID		BIT(3)
#define   SSI_IS_RORID		BIT(0)

#define SSI_IC			0x1c
#define   SSI_IC_TCIC		BIT(4)
#define   SSI_IC_RCIC		BIT(3)
#define   SSI_IC_RORIC		BIT(0)

#define SSI_FC			0x20
#define   SSI_FC_TXFFL		BIT(12)
#define   SSI_FC_TXFTH_MASK	GENMASK(11, 8)
#define   SSI_FC_RXFFL		BIT(4)
#define   SSI_FC_RXFTH_MASK	GENMASK(3, 0)

#define SSI_TXDR		0x24
#define SSI_RXDR		0x24

#define SSI_FIFO_DEPTH		8

#define BYTES_PER_WORD(x)			\
({						\
	int __x;				\
	__x = (x <= 8)  ? 1 :			\
	      (x <= 16) ? 2 : 4;		\
	__x;					\
})

static inline void uniphier_spi_irq_enable(struct spi_device *spi, u32 mask)
{
	struct uniphier_spi_priv *priv = spi_master_get_devdata(spi->master);
	u32 val;

	val = readl(priv->base + SSI_IE);
	val |= mask;
	writel(val, priv->base + SSI_IE);
}

static inline void uniphier_spi_irq_disable(struct spi_device *spi, u32 mask)
{
	struct uniphier_spi_priv *priv = spi_master_get_devdata(spi->master);
	u32 val;

	val = readl(priv->base + SSI_IE);
	val &= ~mask;
	writel(val, priv->base + SSI_IE);
}

static void uniphier_spi_set_transfer_size(struct spi_device *spi, int size)
{
	struct uniphier_spi_priv *priv = spi_master_get_devdata(spi->master);
	u32 val;

	val = readl(priv->base + SSI_TXWDS);
	val &= ~(SSI_TXWDS_WDLEN_MASK | SSI_TXWDS_DTLEN_MASK);
	val |= FIELD_PREP(SSI_TXWDS_WDLEN_MASK, size);
	val |= FIELD_PREP(SSI_TXWDS_DTLEN_MASK, size);
	writel(val, priv->base + SSI_TXWDS);

	val = readl(priv->base + SSI_RXWDS);
	val &= ~SSI_RXWDS_DTLEN_MASK;
	val |= FIELD_PREP(SSI_RXWDS_DTLEN_MASK, size);
	writel(val, priv->base + SSI_RXWDS);
}

static int uniphier_spi_set_baudrate(struct spi_device *spi, unsigned int speed)
{
	struct uniphier_spi_priv *priv = spi_master_get_devdata(spi->master);
	u32 val, ckrat;

	/*
	 * the supported rates are even numbers from 4 to 254. (4,6,8...254)
	 * round up as we look for equal or less speed
	 */
	ckrat = DIV_ROUND_UP(clk_get_rate(priv->clk), speed);
	ckrat = roundup(ckrat, 2);

	/* check if requested speed is too small */
	if (ckrat > SSI_MAX_CLK_DIVIDER)
		return -EINVAL;

	if (ckrat < SSI_MIN_CLK_DIVIDER)
		ckrat = SSI_MIN_CLK_DIVIDER;

	val = readl(priv->base + SSI_CKS);
	val &= ~SSI_CKS_CKRAT_MASK;
	val |= ckrat & SSI_CKS_CKRAT_MASK;
	writel(val, priv->base + SSI_CKS);

	return 0;
}

static int uniphier_spi_setup_transfer(struct spi_device *spi,
				       struct spi_transfer *t)
{
	struct uniphier_spi_priv *priv = spi_master_get_devdata(spi->master);
	u32 val;
	int ret;

	priv->error = 0;
	priv->tx_buf = t->tx_buf;
	priv->rx_buf = t->rx_buf;
	priv->tx_bytes = priv->rx_bytes = t->len;

	if (priv->bits_per_word != t->bits_per_word) {
		uniphier_spi_set_transfer_size(spi, t->bits_per_word);
		priv->bits_per_word = t->bits_per_word;
	}

	if (priv->speed_hz != t->speed_hz) {
		ret = uniphier_spi_set_baudrate(spi, t->speed_hz);
		if (ret)
			return ret;
		priv->speed_hz = t->speed_hz;
	}

	/* reset FIFOs */
	val = SSI_FC_TXFFL | SSI_FC_RXFFL;
	writel(val, priv->base + SSI_FC);

	return 0;
}

static void uniphier_spi_send(struct uniphier_spi_priv *priv)
{
	int i, loop;
	u32 val = 0;

	loop = BYTES_PER_WORD(priv->bits_per_word);
	if (priv->tx_bytes < loop)
		loop = priv->tx_bytes;

	priv->tx_bytes -= loop;

	if (priv->tx_buf)
		for (i = 0; i < loop; i++) {
			val |= (*(const u8 *)priv->tx_buf)
						<< (BITS_PER_BYTE * i);
			(const u8 *)priv->tx_buf++;
		}

	writel(val, priv->base + SSI_TXDR);
}

static void uniphier_spi_recv(struct uniphier_spi_priv *priv)
{
	int i, loop;
	u32 val;

	loop = BYTES_PER_WORD(priv->bits_per_word);
	if (priv->rx_bytes < loop)
		loop = priv->rx_bytes;

	priv->rx_bytes -= loop;

	val = readl(priv->base + SSI_RXDR);

	if (priv->rx_buf)
		for (i = 0; i < loop; i++) {
			val = val >> (BITS_PER_BYTE * i);
			*(u8 *)priv->rx_buf = val & GENMASK(7, 0);
			(u8 *)priv->rx_buf++;
		}
}

static void uniphier_spi_fill_tx_fifo(struct uniphier_spi_priv *priv)
{
	unsigned int tx_count;
	int bytes_per_word = BYTES_PER_WORD(priv->bits_per_word);
	u32 val;

	tx_count = priv->tx_bytes / bytes_per_word;
	if (tx_count > SSI_FIFO_DEPTH)
		tx_count = SSI_FIFO_DEPTH;

	/* set fifo threthold */
	val = readl(priv->base + SSI_FC);
	val &= ~(SSI_FC_TXFTH_MASK | SSI_FC_RXFTH_MASK);
	val |= FIELD_PREP(SSI_FC_TXFTH_MASK, tx_count);
	val |= FIELD_PREP(SSI_FC_RXFTH_MASK, tx_count);
	writel(val, priv->base + SSI_FC);

	while (tx_count--)
		uniphier_spi_send(priv);
}

static void uniphier_spi_set_cs(struct spi_device *spi, bool enable)
{
	struct uniphier_spi_priv *priv = spi_master_get_devdata(spi->master);
	u32 val;

	val = readl(priv->base + SSI_FPS);

	if (enable)
		val |= SSI_FPS_FSPOL;
	else
		val &= ~SSI_FPS_FSPOL;

	writel(val, priv->base + SSI_FPS);
}

static int uniphier_spi_transfer_one(struct spi_master *master,
				     struct spi_device *spi,
				     struct spi_transfer *t)
{
	struct uniphier_spi_priv *priv = spi_master_get_devdata(master);
	int status;

	status = uniphier_spi_setup_transfer(spi, t);
	if (status < 0)
		return status;

	reinit_completion(&priv->xfer_done);

	uniphier_spi_fill_tx_fifo(priv);

	uniphier_spi_irq_enable(spi, SSI_IE_RCIE | SSI_IE_RORIE);

	status = wait_for_completion_timeout(&priv->xfer_done,
					     msecs_to_jiffies(SSI_TIMEOUT));

	uniphier_spi_irq_disable(spi, SSI_IE_RCIE | SSI_IE_RORIE);

	if (status < 0)
		return status;

	return priv->error;
}

static int uniphier_spi_prepare_transfer_hardware(struct spi_master *master)
{
	struct uniphier_spi_priv *priv = spi_master_get_devdata(master);

	writel(SSI_CTL_EN, priv->base + SSI_CTL);

	return 0;
}

static int uniphier_spi_unprepare_transfer_hardware(struct spi_master *master)
{
	struct uniphier_spi_priv *priv = spi_master_get_devdata(master);

	writel(0, priv->base + SSI_CTL);

	return 0;
}

static int uniphier_spi_setup(struct spi_device *spi)
{
	struct uniphier_spi_priv *priv = spi_master_get_devdata(spi->master);
	u32 val1, val2;

	if (priv->mode == spi->mode)
		return 0;

	priv->mode = spi->mode;

	/* clock setting
	 * CKPHS    capture timing. 0:rising edge, 1:falling edge
	 * CKINIT   clock initial level. 0:low, 1:high
	 * CKDLY    clock delay. 0:no delay, 1:delay depending on FSTRT
	 *          (FSTRT=0: 1 clock, FSTRT=1: 0.5 clock)
	 *
	 * frame setting
	 * FSPOL    frame signal porarity. 0: low, 1: high
	 * FSTRT    start frame timing
	 *          0: rising edge of clock, 1: falling edge of clock
	 */
	switch (spi->mode & (SPI_CPOL | SPI_CPHA)) {
	case SPI_MODE_0:
		/* CKPHS=1, CKINIT=0, CKDLY=1, FSTRT=0 */
		val1 = SSI_CKS_CKPHS | SSI_CKS_CKDLY;
		val2 = 0;
		break;
	case SPI_MODE_1:
		/* CKPHS=0, CKINIT=0, CKDLY=0, FSTRT=1 */
		val1 = 0;
		val2 = SSI_FPS_FSTRT;
		break;
	case SPI_MODE_2:
		/* CKPHS=0, CKINIT=1, CKDLY=1, FSTRT=1 */
		val1 = SSI_CKS_CKINIT | SSI_CKS_CKDLY;
		val2 = SSI_FPS_FSTRT;
		break;
	case SPI_MODE_3:
		/* CKPHS=1, CKINIT=1, CKDLY=0, FSTRT=0 */
		val1 = SSI_CKS_CKPHS | SSI_CKS_CKINIT;
		val2 = 0;
		break;
	}

	if (!(spi->mode & SPI_CS_HIGH))
		val2 |= SSI_FPS_FSPOL;

	writel(val1, priv->base + SSI_CKS);
	writel(val2, priv->base + SSI_FPS);

	val1 = 0;
	if (spi->mode & SPI_LSB_FIRST)
		val1 |= FIELD_PREP(SSI_TXWDS_TDTF_MASK, 1);
	writel(val1, priv->base + SSI_TXWDS);
	writel(val1, priv->base + SSI_RXWDS);

	return 0;
}

static irqreturn_t uniphier_spi_handler(int irq, void *dev_id)
{
	struct uniphier_spi_priv *priv = dev_id;
	u32 val, stat;

	stat = readl(priv->base + SSI_IS);
	val = SSI_IC_TCIC | SSI_IC_RCIC | SSI_IC_RORIC;
	writel(val, priv->base + SSI_IC);

	/* rx fifo overrun */
	if (stat & SSI_IS_RORID) {
		priv->error = -EIO;
		goto done;
	}

	/* rx complete */
	if ((stat & SSI_IS_RCID) && (stat & SSI_IS_RXRS)) {
		while ((readl(priv->base + SSI_SR) & SSI_SR_RNE) &&
				(priv->rx_bytes - priv->tx_bytes) > 0)
			uniphier_spi_recv(priv);

		if ((readl(priv->base + SSI_SR) & SSI_SR_RNE) ||
				(priv->rx_bytes != priv->tx_bytes)) {
			priv->error = -EIO;
			goto done;
		} else if (priv->rx_bytes == 0)
			goto done;

		/* next tx transfer */
		uniphier_spi_fill_tx_fifo(priv);
	}

	return IRQ_HANDLED;

done:
	complete(&priv->xfer_done);
	return IRQ_HANDLED;
}

static int uniphier_spi_probe(struct platform_device *pdev)
{
	struct uniphier_spi_priv *priv;
	struct spi_master *master;
	struct resource *res;
	unsigned long clksrc;
	int ret;

	master = spi_alloc_master(&pdev->dev, sizeof(*priv));
	if (!master)
		return -ENOMEM;

	platform_set_drvdata(pdev, master);

	priv = spi_master_get_devdata(master);
	priv->master = master;
	priv->bits_per_word = 0;
	priv->mode = 0;
	priv->speed_hz = 0;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->base)) {
		ret = PTR_ERR(priv->base);
		goto out_master_put;
	}

	priv->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(&pdev->dev, "failed to get clock\n");
		ret = PTR_ERR(priv->clk);
		goto out_master_put;
	}

	ret = clk_prepare_enable(priv->clk);
	if (ret)
		goto out_master_put;

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0) {
		dev_err(&pdev->dev, "failed to get IRQ\n");
		ret = -ENXIO;
		goto out_disable_clk;
	}

	ret = devm_request_irq(&pdev->dev, priv->irq, uniphier_spi_handler,
			       0, "uniphier-spi", priv);
	if (ret) {
		dev_err(&pdev->dev, "failed to request IRQ\n");
		goto out_disable_clk;
	}

	init_completion(&priv->xfer_done);

	clksrc = clk_get_rate(priv->clk);

	master->max_speed_hz = DIV_ROUND_UP(clksrc, SSI_MIN_CLK_DIVIDER);
	master->min_speed_hz = DIV_ROUND_UP(clksrc, SSI_MAX_CLK_DIVIDER);
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH | SPI_LSB_FIRST;
	master->dev.of_node = pdev->dev.of_node;
	master->bus_num = pdev->id;
	master->bits_per_word_mask = SPI_BPW_RANGE_MASK(1, 32);

	master->setup = uniphier_spi_setup;
	master->set_cs = uniphier_spi_set_cs;
	master->transfer_one = uniphier_spi_transfer_one;
	master->prepare_transfer_hardware
				= uniphier_spi_prepare_transfer_hardware;
	master->unprepare_transfer_hardware
				= uniphier_spi_unprepare_transfer_hardware;
	master->num_chipselect = 1;

	ret = devm_spi_register_master(&pdev->dev, master);
	if (ret)
		goto out_disable_clk;

	return ret;

out_disable_clk:
	clk_disable_unprepare(priv->clk);

out_master_put:
	spi_master_put(master);
	return ret;
}

static int uniphier_spi_remove(struct platform_device *pdev)
{
	struct uniphier_spi_priv *priv = platform_get_drvdata(pdev);

	clk_disable_unprepare(priv->clk);

	return 0;
}

static const struct of_device_id uniphier_spi_match[] = {
	{ .compatible = "socionext,uniphier-scssi", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, uniphier_spi_match);

static struct platform_driver uniphier_spi_driver = {
	.probe = uniphier_spi_probe,
	.remove = uniphier_spi_remove,
	.driver = {
		.name = "uniphier-spi",
		.of_match_table = uniphier_spi_match,
	},
};
module_platform_driver(uniphier_spi_driver);

MODULE_AUTHOR("Kunihiko Hayashi <hayashi.kunihiko@socionext.com>");
MODULE_AUTHOR("Keiji Hayashibara <hayashibara.keiji@socionext.com>");
MODULE_DESCRIPTION("Socionext UniPhier SPI controller driver");
MODULE_LICENSE("GPL v2");
