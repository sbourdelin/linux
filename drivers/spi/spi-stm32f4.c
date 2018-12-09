// SPDX-License-Identifier: GPL-2.0
//
// STMicroelectronics STM32F4 SPI Controller driver (master mode only)
//
// Author(s): Cezary Gapinski <cezary.gapinski@gmail.com>
//
// This driver is based on spi-stm32h7.c

#include <linux/debugfs.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>

#define DRIVER_NAME "spi_stm32f4"

/* STM32F4 SPI registers */
#define STM32F4_SPI_CR1			0x00
#define STM32F4_SPI_CR2			0x04
#define STM32F4_SPI_SR			0x08
#define STM32F4_SPI_DR			0x0C
#define STM32F4_SPI_I2SCFGR		0x1C

/* STM32F4_SPI_CR1 bit fields */
#define STM32F4_SPI_CR1_CPHA		BIT(0)
#define STM32F4_SPI_CR1_CPOL		BIT(1)
#define STM32F4_SPI_CR1_MSTR		BIT(2)
#define STM32F4_SPI_CR1_BR_SHIFT	3
#define STM32F4_SPI_CR1_BR		GENMASK(5, 3)
#define STM32F4_SPI_CR1_SPE		BIT(6)
#define STM32F4_SPI_CR1_LSBFRST		BIT(7)
#define STM32F4_SPI_CR1_SSI		BIT(8)
#define STM32F4_SPI_CR1_SSM		BIT(9)
#define STM32F4_SPI_CR1_RXONLY		BIT(10)
#define STM32F4_SPI_CR1_DFF		BIT(11)
#define STM32F4_SPI_CR1_CRCNEXT		BIT(12)
#define STM32F4_SPI_CR1_CRCEN		BIT(13)
#define STM32F4_SPI_CR1_BIDIOE		BIT(14)
#define STM32F4_SPI_CR1_BIDIMODE	BIT(15)
#define STM32F4_SPI_CR1_BR_MIN		0
#define STM32F4_SPI_CR1_BR_MAX		(GENMASK(5, 3) >> 3)

/* STM32F4_SPI_CR2 bit fields */
#define STM32F4_SPI_CR2_RXDMAEN		BIT(0)
#define STM32F4_SPI_CR2_TXDMAEN		BIT(1)
#define STM32F4_SPI_CR2_SSOE		BIT(2)
#define STM32F4_SPI_CR2_FRF		BIT(4)
#define STM32F4_SPI_CR2_ERRIE		BIT(5)
#define STM32F4_SPI_CR2_RXNEIE		BIT(6)
#define STM32F4_SPI_CR2_TXEIE		BIT(7)

/* STM32F4_SPI_SR bit fields */
#define STM32F4_SPI_SR_RXNE		BIT(0)
#define STM32F4_SPI_SR_TXE		BIT(1)
#define STM32F4_SPI_SR_CHSIDE		BIT(2)
#define STM32F4_SPI_SR_UDR		BIT(3)
#define STM32F4_SPI_SR_CRCERR		BIT(4)
#define STM32F4_SPI_SR_MODF		BIT(5)
#define STM32F4_SPI_SR_OVR		BIT(6)
#define STM32F4_SPI_SR_BSY		BIT(7)
#define STM32F4_SPI_SR_FRE		BIT(8)

/* STM32F4_SPI_I2SCFGR bit fields */
#define STM32F4_SPI_I2SCFGR_I2SMOD	BIT(11)

/* STM32F4 SPI Baud Rate min/max divisor */
#define STM32F4_SPI_BR_DIV_MIN		(2 << STM32F4_SPI_CR1_BR_MIN)
#define STM32F4_SPI_BR_DIV_MAX		(2 << STM32F4_SPI_CR1_BR_MAX)

/* use PIO for small transfers, avoiding DMA setup/teardown overhead */
#define STM32F4_DMA_MIN_BYTES		16

/**
 * struct stm32f4_spi - private data of the SPI controller
 * @dev: driver model representation of the controller
 * @master: controller master interface
 * @base: virtual memory area
 * @clk: hw kernel clock feeding the SPI clock generator
 * @clk_rate: rate of the hw kernel clock feeding the SPI clock generator
 * @rst: SPI controller reset line
 * @lock: prevent I/O concurrent access
 * @irq: SPI controller interrupt line
 * @cur_speed: speed configured in Hz
 * @cur_bpw: number of bits in a single SPI data frame
 * @cur_xferlen: current transfer length in bytes
 * @cur_usedma: boolean to know if dma is used in current transfer
 * @tx_buf: data to be written, or NULL
 * @rx_buf: data to be read, or NULL
 * @tx_len: number of data to be written in bytes
 * @rx_len: number of data to be read in bytes
 * @phys_addr: SPI registers physical base address
 */
struct stm32f4_spi {
	struct device *dev;
	struct spi_master *master;
	void __iomem *base;
	struct clk *clk;
	u32 clk_rate;
	struct reset_control *rst;
	spinlock_t lock;
	int irq;

	unsigned int cur_speed;
	unsigned int cur_bpw;
	unsigned int cur_xferlen;
	bool cur_usedma;

	const void *tx_buf;
	void *rx_buf;
	int tx_len;
	int rx_len;
	dma_addr_t phys_addr;
};

static inline void stm32f4_spi_set_bits(struct stm32f4_spi *spi,
					u32 offset, u32 bits)
{
	writel_relaxed(readl_relaxed(spi->base + offset) | bits,
		       spi->base + offset);
}

static inline void stm32f4_spi_clr_bits(struct stm32f4_spi *spi,
					u32 offset, u32 bits)
{
	writel_relaxed(readl_relaxed(spi->base + offset) & ~bits,
		       spi->base + offset);
}

/**
 * stm32f4_spi_prepare_mbr - Determine STM32F4_SPI_CR1.BR value
 * @spi: pointer to the spi controller data structure
 * @speed_hz: requested speed
 *
 * Return STM32F4_SPI_CR1.BR value in case of success or -EINVAL
 */
static int stm32f4_spi_prepare_mbr(struct stm32f4_spi *spi, u32 speed_hz)
{
	u32 div, mbrdiv;

	div = DIV_ROUND_UP(spi->clk_rate, speed_hz);

	/*
	 * SPI framework set xfer->speed_hz to master->max_speed_hz if
	 * xfer->speed_hz is greater than master->max_speed_hz, and it returns
	 * an error when xfer->speed_hz is lower than master->min_speed_hz, so
	 * no need to check it there.
	 * However, we need to ensure the following calculations.
	 */
	if (div < STM32F4_SPI_BR_DIV_MIN ||
	    div > STM32F4_SPI_BR_DIV_MAX)
		return -EINVAL;

	/* Determine the first power of 2 greater than or equal to div */
	if (div & (div - 1))
		mbrdiv = fls(div);
	else
		mbrdiv = fls(div) - 1;

	spi->cur_speed = spi->clk_rate / (1 << mbrdiv);

	return mbrdiv - 1;
}

/**
 * stm32f4_spi_write_tx - Write bytes to Data Register
 * @spi: pointer to the spi controller data structure
 *
 * Read from tx_buf depends on remaining bytes to avoid to read beyond
 * tx_buf end.
 */
static void stm32f4_spi_write_tx(struct stm32f4_spi *spi)
{
	if (spi->tx_len > 0) {
		u32 offs = spi->cur_xferlen - spi->tx_len;

		if (spi->cur_bpw == 16) {
			const u16 *tx_buf16 = (const u16 *)(spi->tx_buf + offs);

			writew_relaxed(*tx_buf16, spi->base + STM32F4_SPI_DR);
			spi->tx_len -= sizeof(u16);
		} else {
			const u8 *tx_buf8 = (const u8 *)(spi->tx_buf + offs);

			writeb_relaxed(*tx_buf8, spi->base + STM32F4_SPI_DR);
			spi->tx_len -= sizeof(u8);
		}
	}

	dev_dbg(spi->dev, "%s: %d bytes left\n", __func__, spi->tx_len);
}

/**
 * stm32f4_spi_read_rx - Read bytes from Data Register
 * @spi: pointer to the spi controller data structure
 *
 * Write in rx_buf depends on remaining bytes to avoid to write beyond
 * rx_buf end.
 */
static void stm32f4_spi_read_rx(struct stm32f4_spi *spi)
{
	if (spi->rx_len > 0) {
		u32 offs = spi->cur_xferlen - spi->rx_len;

		if (spi->cur_bpw == 16) {
			u16 *rx_buf16 = (u16 *)(spi->rx_buf + offs);

			*rx_buf16 = readw_relaxed(spi->base + STM32F4_SPI_DR);
			spi->rx_len -= sizeof(u16);
		} else {
			u8 *rx_buf8 = (u8 *)(spi->rx_buf + offs);

			*rx_buf8 = readb_relaxed(spi->base + STM32F4_SPI_DR);
			spi->rx_len -= sizeof(u8);
		}
	}

	dev_dbg(spi->dev, "%s: %d bytes left\n", __func__, spi->rx_len);
}

/**
 * stm32f4_spi_enable - Enable SPI controller
 * @spi: pointer to the spi controller data structure
 */
static void stm32f4_spi_enable(struct stm32f4_spi *spi)
{
	dev_dbg(spi->dev, "enable controller\n");

	stm32f4_spi_set_bits(spi, STM32F4_SPI_CR1, STM32F4_SPI_CR1_SPE);
}

/**
 * stm32f4_spi_disable - Disable SPI controller
 * @spi: pointer to the spi controller data structure
 */
static void stm32f4_spi_disable(struct stm32f4_spi *spi)
{
	unsigned long flags;
	struct spi_master *master = spi->master;

	dev_dbg(spi->dev, "disable controller\n");

	spin_lock_irqsave(&spi->lock, flags);

	stm32f4_spi_clr_bits(spi, STM32F4_SPI_CR1, STM32F4_SPI_CR1_SPE);

	if (spi->cur_usedma) {
		dmaengine_terminate_all(master->dma_tx);
		dmaengine_terminate_all(master->dma_rx);
	}

	stm32f4_spi_clr_bits(spi, STM32F4_SPI_CR2, STM32F4_SPI_CR2_TXDMAEN |
						   STM32F4_SPI_CR2_RXDMAEN);

	/* Disable interrupts */
	stm32f4_spi_clr_bits(spi, STM32F4_SPI_CR2, STM32F4_SPI_CR2_RXNEIE |
						   STM32F4_SPI_CR2_ERRIE);
	/* Sequence to clear OVR flag */
	readl_relaxed(spi->base + STM32F4_SPI_DR);
	readl_relaxed(spi->base + STM32F4_SPI_SR);

	spin_unlock_irqrestore(&spi->lock, flags);
}

/**
 * stm32f4_spi_can_dma - Determine if the transfer is eligible for DMA use
 *
 * If the current transfer size is greater than defined size, use DMA.
 */
static bool stm32f4_spi_can_dma(struct spi_master *master,
				struct spi_device *spi_dev,
				struct spi_transfer *transfer)
{
	struct stm32f4_spi *spi = spi_master_get_devdata(master);

	dev_dbg(spi->dev, "%s: %s\n", __func__,
		(transfer->len > STM32F4_DMA_MIN_BYTES) ? "true" : "false");

	return (transfer->len > STM32F4_DMA_MIN_BYTES);
}

/**
 * stm32f4_spi_irq_event - Interrupt handler for SPI controller events
 * @irq: interrupt line
 * @dev_id: SPI controller master interface
 */
static irqreturn_t stm32f4_spi_irq_event(int irq, void *dev_id)
{
	struct spi_master *master = dev_id;
	struct stm32f4_spi *spi = spi_master_get_devdata(master);
	u32 sr, mask;
	unsigned long flags;
	bool end = false;

	spin_lock_irqsave(&spi->lock, flags);

	sr = readl_relaxed(spi->base + STM32F4_SPI_SR);
	/*
	 * BSY flag is not handled in interrupt
	 * TXE flag is set and is handled when RXNE flag occurs
	 */
	sr &= ~(STM32F4_SPI_SR_BSY | STM32F4_SPI_SR_TXE);

	mask = STM32F4_SPI_SR_RXNE;
	if (spi->cur_usedma)
		mask |= STM32F4_SPI_SR_OVR;

	if (!(sr & mask)) {
		dev_dbg(spi->dev, "spurious IT (sr=0x%08x)\n", sr);
		spin_unlock_irqrestore(&spi->lock, flags);
		return IRQ_NONE;
	}

	if (sr & STM32F4_SPI_SR_OVR) {
		dev_warn(spi->dev, "Overrun: received value discarded\n");

		/* Sequence to clear OVR flag */
		readl_relaxed(spi->base + STM32F4_SPI_DR);
		readl_relaxed(spi->base + STM32F4_SPI_SR);

		/*
		 * If overrun is detected, it means that something went wrong,
		 * so stop the current transfer. For interrupt transfer for
		 * current configuration it should newer occurs. If it is
		 * detected for DMA stop transfer.
		 */
		end = true;
		goto end_irq;
	}

	stm32f4_spi_read_rx(spi);
	if (spi->rx_len == 0)
		end = true;
	else
		stm32f4_spi_write_tx(spi);

end_irq:
	spin_unlock_irqrestore(&spi->lock, flags);

	if (end)
		return IRQ_WAKE_THREAD;

	return IRQ_HANDLED;
}

/**
 * stm32f4_spi_irq_thread - Thread of interrupt handler for SPI controller
 * @irq: interrupt line
 * @dev_id: SPI controller master interface
 */
static irqreturn_t stm32f4_spi_irq_thread(int irq, void *dev_id)
{
	struct spi_master *master = dev_id;
	struct stm32f4_spi *spi = spi_master_get_devdata(master);

	spi_finalize_current_transfer(master);
	stm32f4_spi_disable(spi);

	return IRQ_HANDLED;
}

/**
 * stm32f4_spi_setup - setup device chip select
 */
static int stm32f4_spi_setup(struct spi_device *spi_dev)
{
	int ret = 0;

	if (!gpio_is_valid(spi_dev->cs_gpio)) {
		dev_err(&spi_dev->dev, "%d is not a valid gpio\n",
			spi_dev->cs_gpio);
		return -EINVAL;
	}

	dev_dbg(&spi_dev->dev, "%s: set gpio%d output %s\n", __func__,
		spi_dev->cs_gpio,
		(spi_dev->mode & SPI_CS_HIGH) ? "low" : "high");

	ret = gpio_direction_output(spi_dev->cs_gpio,
				    !(spi_dev->mode & SPI_CS_HIGH));

	return ret;
}

/**
 * stm32f4_spi_prepare_msg - set up the controller to transfer a single message
 */
static int stm32f4_spi_prepare_msg(struct spi_master *master,
				   struct spi_message *msg)
{
	struct stm32f4_spi *spi = spi_master_get_devdata(master);
	struct spi_device *spi_dev = msg->spi;
	unsigned long flags;
	u32 cr1_clrb = 0, cr1_setb = 0;

	if (spi_dev->mode & SPI_CPOL)
		cr1_setb |= STM32F4_SPI_CR1_CPOL;
	else
		cr1_clrb |= STM32F4_SPI_CR1_CPOL;

	if (spi_dev->mode & SPI_CPHA)
		cr1_setb |= STM32F4_SPI_CR1_CPHA;
	else
		cr1_clrb |= STM32F4_SPI_CR1_CPHA;

	if (spi_dev->mode & SPI_LSB_FIRST)
		cr1_setb |= STM32F4_SPI_CR1_LSBFRST;
	else
		cr1_clrb |= STM32F4_SPI_CR1_LSBFRST;

	dev_dbg(spi->dev, "cpol=%d cpha=%d lsb_first=%d cs_high=%d\n",
		spi_dev->mode & SPI_CPOL,
		spi_dev->mode & SPI_CPHA,
		spi_dev->mode & SPI_LSB_FIRST,
		spi_dev->mode & SPI_CS_HIGH);

	spin_lock_irqsave(&spi->lock, flags);

	if (cr1_clrb || cr1_setb)
		writel_relaxed((readl_relaxed(spi->base + STM32F4_SPI_CR1) &
				~cr1_clrb) | cr1_setb,
			       spi->base + STM32F4_SPI_CR1);

	spin_unlock_irqrestore(&spi->lock, flags);

	return 0;
}

/**
 * stm32f4_spi_dma_rx_cb - DMA callback
 *
 * DMA callback is called when the transfer is complete for receiving mode.
 */
static void stm32f4_spi_dma_rx_cb(void *data)
{
	struct spi_master *master = data;
	struct stm32f4_spi *spi = spi_master_get_devdata(master);

	spi_finalize_current_transfer(master);
	stm32f4_spi_disable(spi);
}

/**
 * stm32f4_spi_dma_config - configure DMA slave channel depending on current
 *			    transfer bits_per_word.
 */
static void stm32f4_spi_dma_config(struct stm32f4_spi *spi,
				   struct dma_slave_config *dma_conf,
				   enum dma_transfer_direction dir)
{
	enum dma_slave_buswidth buswidth;

	if (spi->cur_bpw == 16)
		buswidth = DMA_SLAVE_BUSWIDTH_2_BYTES;
	else
		buswidth = DMA_SLAVE_BUSWIDTH_1_BYTE;

	memset(dma_conf, 0, sizeof(struct dma_slave_config));
	dma_conf->direction = dir;
	if (dma_conf->direction == DMA_DEV_TO_MEM) { /* RX */
		dma_conf->src_addr = spi->phys_addr + STM32F4_SPI_DR;
		dma_conf->src_addr_width = buswidth;

		dev_dbg(spi->dev, "Rx DMA config buswidth=%d\n", buswidth);
	} else if (dma_conf->direction == DMA_MEM_TO_DEV) { /* TX */
		dma_conf->dst_addr = spi->phys_addr + STM32F4_SPI_DR;
		dma_conf->dst_addr_width = buswidth;

		dev_dbg(spi->dev, "Tx DMA config buswidth=%d\n", buswidth);
	}
}

/**
 * stm32f4_spi_transfer_one_irq - transfer a single spi_transfer using
 *				  interrupts
 *
 * It must returns 0 if the transfer is finished or 1 if the transfer is still
 * in progress.
 */
static int stm32f4_spi_transfer_one_irq(struct stm32f4_spi *spi)
{
	unsigned long flags;

	spin_lock_irqsave(&spi->lock, flags);

	stm32f4_spi_set_bits(spi, STM32F4_SPI_CR2, STM32F4_SPI_CR2_RXNEIE |
						   STM32F4_SPI_CR2_ERRIE);
	stm32f4_spi_enable(spi);
	stm32f4_spi_write_tx(spi);

	spin_unlock_irqrestore(&spi->lock, flags);

	return 1;
}

/**
 * stm32f4_spi_transfer_one_dma - transfer a single spi_transfer using DMA
 *
 * It must returns 0 if the transfer is finished or 1 if the transfer is still
 * in progress.
 */
static int stm32f4_spi_transfer_one_dma(struct stm32f4_spi *spi,
					struct spi_transfer *xfer)
{
	struct dma_slave_config tx_dma_conf, rx_dma_conf;
	struct dma_async_tx_descriptor *tx_dma_desc, *rx_dma_desc;
	unsigned long flags;
	struct spi_master *master = spi->master;

	spin_lock_irqsave(&spi->lock, flags);

	rx_dma_desc = NULL;
	stm32f4_spi_dma_config(spi, &rx_dma_conf, DMA_DEV_TO_MEM);
	dmaengine_slave_config(master->dma_rx, &rx_dma_conf);

	/* Enable Rx DMA request */
	stm32f4_spi_set_bits(spi, STM32F4_SPI_CR2, STM32F4_SPI_CR2_RXDMAEN);

	rx_dma_desc = dmaengine_prep_slave_sg(
				master->dma_rx, xfer->rx_sg.sgl,
				xfer->rx_sg.nents,
				rx_dma_conf.direction,
				DMA_PREP_INTERRUPT);

	tx_dma_desc = NULL;
	stm32f4_spi_dma_config(spi, &tx_dma_conf, DMA_MEM_TO_DEV);
	dmaengine_slave_config(master->dma_tx, &tx_dma_conf);

	tx_dma_desc = dmaengine_prep_slave_sg(
				master->dma_tx, xfer->tx_sg.sgl,
				xfer->tx_sg.nents,
				tx_dma_conf.direction,
				DMA_PREP_INTERRUPT);

	if (!tx_dma_desc || !rx_dma_desc)
		goto dma_desc_error;

	rx_dma_desc->callback = stm32f4_spi_dma_rx_cb;
	rx_dma_desc->callback_param = spi->master;

	if (dma_submit_error(dmaengine_submit(rx_dma_desc))) {
		dev_err(spi->dev, "Rx DMA submit failed\n");
		goto dma_desc_error;
	}
	/* Enable Rx DMA channel */
	dma_async_issue_pending(master->dma_rx);

	if (dma_submit_error(dmaengine_submit(tx_dma_desc))) {
		dev_err(spi->dev, "Tx DMA submit failed\n");
		goto dma_submit_error;
	}
	/* Enable Tx DMA channel */
	dma_async_issue_pending(master->dma_tx);

	/* Enable Tx DMA request */
	stm32f4_spi_set_bits(spi, STM32F4_SPI_CR2, STM32F4_SPI_CR2_TXDMAEN);

	/*
	 * End of transfer will be handled in DMA RX Callback.
	 * Enable the interrupts to detect OVR flag
	 */
	stm32f4_spi_set_bits(spi, STM32F4_SPI_CR2, STM32F4_SPI_CR2_ERRIE);

	stm32f4_spi_enable(spi);

	spin_unlock_irqrestore(&spi->lock, flags);

	return 1;

dma_submit_error:
	dmaengine_terminate_all(master->dma_rx);

dma_desc_error:
	stm32f4_spi_clr_bits(spi, STM32F4_SPI_CR2, STM32F4_SPI_CR2_RXDMAEN);

	spin_unlock_irqrestore(&spi->lock, flags);

	dev_info(spi->dev, "DMA issue: fall back to irq transfer\n");

	return stm32f4_spi_transfer_one_irq(spi);
}

/**
 * stm32f4_spi_transfer_one_setup - common setup to transfer a single
 *				    spi_transfer either using DMA or
 *				    interrupts.
 */
static int stm32f4_spi_transfer_one_setup(struct stm32f4_spi *spi,
					  struct spi_device *spi_dev,
					  struct spi_transfer *transfer)
{
	unsigned long flags;
	u32 cr1_clrb = 0, cr1_setb = 0;
	u32 nb_words;
	int ret = 0;

	spin_lock_irqsave(&spi->lock, flags);

	if (spi->cur_bpw != transfer->bits_per_word) {
		spi->cur_bpw = transfer->bits_per_word;
		nb_words = spi->cur_bpw;
		cr1_clrb |= STM32F4_SPI_CR1_DFF;

		if (spi->cur_bpw == 16)
			cr1_setb |= STM32F4_SPI_CR1_DFF;
	}

	if (spi->cur_speed != transfer->speed_hz) {
		int mbr;

		/* Update spi->cur_speed with real clock speed */
		mbr = stm32f4_spi_prepare_mbr(spi, transfer->speed_hz);
		if (mbr < 0) {
			ret = mbr;
			goto out;
		}

		transfer->speed_hz = spi->cur_speed;

		cr1_clrb |= STM32F4_SPI_CR1_BR;
		cr1_setb |= ((u32)mbr << STM32F4_SPI_CR1_BR_SHIFT) &
			     STM32F4_SPI_CR1_BR;
	}

	if (cr1_clrb || cr1_setb)
		writel_relaxed((readl_relaxed(spi->base + STM32F4_SPI_CR1) &
				~cr1_clrb) | cr1_setb,
			       spi->base + STM32F4_SPI_CR1);

	spi->cur_xferlen = transfer->len;

	dev_dbg(spi->dev, "full-duplex communication mode\n");
	dev_dbg(spi->dev, "data frame of %d-bit\n", spi->cur_bpw);
	dev_dbg(spi->dev, "speed set to %dHz\n", spi->cur_speed);
	dev_dbg(spi->dev, "transfer of %d bytes (%d data frames)\n",
		spi->cur_xferlen, nb_words);
	dev_dbg(spi->dev, "dma %s\n",
		(spi->cur_usedma) ? "enabled" : "disabled");

out:
	spin_unlock_irqrestore(&spi->lock, flags);

	return ret;
}

/**
 * stm32f4_spi_transfer_one - transfer a single spi_transfer
 *
 * It must return 0 if the transfer is finished or 1 if the transfer is still
 * in progress.
 */
static int stm32f4_spi_transfer_one(struct spi_master *master,
				    struct spi_device *spi_dev,
				    struct spi_transfer *transfer)
{
	struct stm32f4_spi *spi = spi_master_get_devdata(master);
	int ret;

	spi->tx_buf = transfer->tx_buf;
	spi->rx_buf = transfer->rx_buf;
	spi->tx_len = spi->tx_buf ? transfer->len : 0;
	spi->rx_len = spi->rx_buf ? transfer->len : 0;

	spi->cur_usedma = (master->can_dma &&
			   stm32f4_spi_can_dma(master, spi_dev, transfer));

	ret = stm32f4_spi_transfer_one_setup(spi, spi_dev, transfer);
	if (ret) {
		dev_err(spi->dev, "SPI transfer setup failed\n");
		return ret;
	}

	if (spi->cur_usedma)
		return stm32f4_spi_transfer_one_dma(spi, transfer);
	else
		return stm32f4_spi_transfer_one_irq(spi);
}

/**
 * stm32f4_spi_unprepare_msg - relax the hardware
 */
static int stm32f4_spi_unprepare_msg(struct spi_master *master,
				     struct spi_message *msg)
{
	struct stm32f4_spi *spi = spi_master_get_devdata(master);

	stm32f4_spi_disable(spi);

	return 0;
}

/**
 * stm32f4_spi_config - configure SPI controller as SPI master
 */
static int stm32f4_spi_config(struct stm32f4_spi *spi)
{
	unsigned long flags;

	spin_lock_irqsave(&spi->lock, flags);

	/* Ensure I2SMOD bit is kept cleared */
	stm32f4_spi_clr_bits(spi, STM32F4_SPI_I2SCFGR,
			     STM32F4_SPI_I2SCFGR_I2SMOD);

	/*
	 * - SS input value high
	 * - Set the master mode (default Motorola mode)
	 * - Consider 1 master/n slaves configuration and
	 *   SS input value is determined by the SSI bit
	 */
	stm32f4_spi_set_bits(spi, STM32F4_SPI_CR1, STM32F4_SPI_CR1_SSI |
						   STM32F4_SPI_CR1_MSTR |
						   STM32F4_SPI_CR1_SSM);

	spin_unlock_irqrestore(&spi->lock, flags);

	return 0;
}

/**
 * stm32f4_release_dma - release DMA tx and rx channels
 */
static void stm32f4_release_dma(struct spi_master *master)
{
	if (master->dma_tx)
		dma_release_channel(master->dma_tx);
	if (master->dma_rx)
		dma_release_channel(master->dma_rx);
}

/**
 * stm32f4_spi_dma_prep - prepare controller to use DMA tx and rx channels
 */
static int stm32f4_spi_dma_prep(struct stm32f4_spi *spi,  struct device *dev)
{
	struct spi_master *master = spi->master;

	master->dma_tx = dma_request_slave_channel(spi->dev, "tx");
	if (!master->dma_tx) {
		dev_warn(dev, "failed to request tx dma channel\n");
		return -ENODEV;
	}

	master->dma_rx = dma_request_slave_channel(spi->dev, "rx");
	if (!master->dma_rx) {
		dev_warn(dev, "failed to request rx dma channel\n");
		stm32f4_release_dma(master);
		return -ENODEV;
	}

	master->can_dma = stm32f4_spi_can_dma;
	dev_info(dev, "DMA available");
	return 0;
}

static const struct of_device_id stm32f4_spi_of_match[] = {
	{ .compatible = "st,stm32f4-spi", },
	{},
};
MODULE_DEVICE_TABLE(of, stm32f4_spi_of_match);

static int stm32f4_spi_probe(struct platform_device *pdev)
{
	struct spi_master *master;
	struct stm32f4_spi *spi;
	struct resource *res;
	int i, ret;

	master = spi_alloc_master(&pdev->dev, sizeof(struct stm32f4_spi));
	if (!master) {
		dev_err(&pdev->dev, "spi master allocation failed\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, master);

	spi = spi_master_get_devdata(master);
	spi->dev = &pdev->dev;
	spi->master = master;
	spin_lock_init(&spi->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	spi->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(spi->base)) {
		ret = PTR_ERR(spi->base);
		goto err_master_put;
	}
	spi->phys_addr = (dma_addr_t)res->start;

	spi->irq = platform_get_irq(pdev, 0);
	if (spi->irq <= 0) {
		dev_err(&pdev->dev, "no irq: %d\n", spi->irq);
		ret = -ENOENT;
		goto err_master_put;
	}
	ret = devm_request_threaded_irq(&pdev->dev, spi->irq,
					stm32f4_spi_irq_event,
					stm32f4_spi_irq_thread,
					IRQF_ONESHOT, pdev->name, master);
	if (ret) {
		dev_err(&pdev->dev, "irq%d request failed: %d\n", spi->irq,
			ret);
		goto err_master_put;
	}

	spi->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(spi->clk)) {
		ret = PTR_ERR(spi->clk);
		dev_err(&pdev->dev, "clk get failed: %d\n", ret);
		goto err_master_put;
	}

	ret = clk_prepare_enable(spi->clk);
	if (ret) {
		dev_err(&pdev->dev, "clk enable failed: %d\n", ret);
		goto err_master_put;
	}
	spi->clk_rate = clk_get_rate(spi->clk);
	if (!spi->clk_rate) {
		dev_err(&pdev->dev, "clk rate = 0\n");
		ret = -EINVAL;
		goto err_clk_disable;
	}

	spi->rst = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (!IS_ERR(spi->rst)) {
		reset_control_assert(spi->rst);
		udelay(2);
		reset_control_deassert(spi->rst);
	}

	ret = stm32f4_spi_config(spi);
	if (ret) {
		dev_err(&pdev->dev, "controller configuration failed: %d\n",
			ret);
		goto err_clk_disable;
	}

	master->dev.of_node = pdev->dev.of_node;
	master->auto_runtime_pm = true;
	master->bus_num = pdev->id;
	master->mode_bits = SPI_CPHA | SPI_CPOL | SPI_CS_HIGH | SPI_LSB_FIRST;
	master->flags = SPI_MASTER_MUST_RX | SPI_MASTER_MUST_TX;
	master->bits_per_word_mask = SPI_BPW_MASK(8) | SPI_BPW_MASK(16);
	master->max_speed_hz = spi->clk_rate / STM32F4_SPI_BR_DIV_MIN;
	master->min_speed_hz = spi->clk_rate / STM32F4_SPI_BR_DIV_MAX;
	master->setup = stm32f4_spi_setup;
	master->prepare_message = stm32f4_spi_prepare_msg;
	master->transfer_one = stm32f4_spi_transfer_one;
	master->unprepare_message = stm32f4_spi_unprepare_msg;

	/* optional DMA support */
	ret = stm32f4_spi_dma_prep(spi, &pdev->dev);
	if (ret < 0)
		dev_warn(&pdev->dev, "DMA not available, using PIO mode\n");

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = devm_spi_register_master(&pdev->dev, master);
	if (ret) {
		dev_err(&pdev->dev, "spi master registration failed: %d\n",
			ret);
		goto err_dma_release;
	}

	if (!master->cs_gpios) {
		dev_err(&pdev->dev, "no CS gpios available\n");
		ret = -EINVAL;
		goto err_dma_release;
	}

	for (i = 0; i < master->num_chipselect; i++) {
		if (!gpio_is_valid(master->cs_gpios[i])) {
			dev_err(&pdev->dev, "%i is not a valid gpio\n",
				master->cs_gpios[i]);
			ret = -EINVAL;
			goto err_dma_release;
		}

		ret = devm_gpio_request(&pdev->dev, master->cs_gpios[i],
					DRIVER_NAME);
		if (ret) {
			dev_err(&pdev->dev, "can't get CS gpio %i\n",
				master->cs_gpios[i]);
			goto err_dma_release;
		}
	}

	dev_info(&pdev->dev, "driver initialized\n");

	return 0;

err_dma_release:
	stm32f4_release_dma(master);

	pm_runtime_disable(&pdev->dev);
err_clk_disable:
	clk_disable_unprepare(spi->clk);
err_master_put:
	spi_master_put(master);

	return ret;
}

static int stm32f4_spi_remove(struct platform_device *pdev)
{
	struct spi_master *master = platform_get_drvdata(pdev);
	struct stm32f4_spi *spi = spi_master_get_devdata(master);

	stm32f4_spi_disable(spi);

	if (master->dma_tx)
		dma_release_channel(master->dma_tx);
	if (master->dma_rx)
		dma_release_channel(master->dma_rx);

	clk_disable_unprepare(spi->clk);

	pm_runtime_disable(&pdev->dev);

	return 0;
}

#ifdef CONFIG_PM
static int stm32f4_spi_runtime_suspend(struct device *dev)
{
	struct spi_master *master = dev_get_drvdata(dev);
	struct stm32f4_spi *spi = spi_master_get_devdata(master);

	clk_disable_unprepare(spi->clk);

	return 0;
}

static int stm32f4_spi_runtime_resume(struct device *dev)
{
	struct spi_master *master = dev_get_drvdata(dev);
	struct stm32f4_spi *spi = spi_master_get_devdata(master);

	return clk_prepare_enable(spi->clk);
}
#endif

#ifdef CONFIG_PM_SLEEP
static int stm32f4_spi_suspend(struct device *dev)
{
	struct spi_master *master = dev_get_drvdata(dev);
	int ret;

	ret = spi_master_suspend(master);
	if (ret)
		return ret;

	return pm_runtime_force_suspend(dev);
}

static int stm32f4_spi_resume(struct device *dev)
{
	struct spi_master *master = dev_get_drvdata(dev);
	struct stm32f4_spi *spi = spi_master_get_devdata(master);
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;

	ret = spi_master_resume(master);
	if (ret)
		clk_disable_unprepare(spi->clk);

	return ret;
}
#endif

static const struct dev_pm_ops stm32f4_spi_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(stm32f4_spi_suspend, stm32f4_spi_resume)
	SET_RUNTIME_PM_OPS(stm32f4_spi_runtime_suspend,
			   stm32f4_spi_runtime_resume, NULL)
};

static struct platform_driver stm32f4_spi_driver = {
	.probe = stm32f4_spi_probe,
	.remove = stm32f4_spi_remove,
	.driver = {
		.name = DRIVER_NAME,
		.pm = &stm32f4_spi_pm_ops,
		.of_match_table = stm32f4_spi_of_match,
	},
};

module_platform_driver(stm32f4_spi_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("STMicroelectronics STM32F4 SPI Controller driver");
MODULE_AUTHOR("Cezary Gapinski <cezary.gapinski@gmail.com>");
MODULE_LICENSE("GPL v2");
