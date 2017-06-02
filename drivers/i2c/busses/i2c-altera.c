/*
 *  Copyright Intel Corporation (C) 2017.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Based on the i2c-axxia.c driver.
 */
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

#define ALTR_I2C_TFR_CMD	0x00	/* Transfer Command register */
#define     ALTR_I2C_TFR_CMD_STA	BIT(9)	/* send START before byte */
#define     ALTR_I2C_TFR_CMD_STO	BIT(8)	/* send STOP after byte */
#define     ALTR_I2C_TFR_CMD_RW_D	BIT(0)	/* Direction of transfer */
#define ALTR_I2C_RX_DATA	0x04	/* RX data FIFO register */
#define ALTR_I2C_CTRL		0x08	/* Control register */
#define	    ALTR_I2C_CTRL_RXT_SHFT	4	/* RX FIFO Threshold */
#define     ALTR_I2C_CTRL_TCT_SHFT	2	/* TFER CMD FIFO Threshold */
#define     ALTR_I2C_CTRL_BSPEED	BIT(1)	/* Bus Speed (1=Fast) */
#define     ALTR_I2C_CTRL_EN	BIT(0)	/* Enable Core (1=Enable) */
#define ALTR_I2C_ISER		0x0C	/* Interrupt Status Enable register */
#define     ALTR_I2C_ISER_RXOF_EN	BIT(4)	/* Enable RX OVERFLOW IRQ */
#define     ALTR_I2C_ISER_ARB_EN	BIT(3)	/* Enable ARB LOST IRQ */
#define     ALTR_I2C_ISER_NACK_EN	BIT(2)	/* Enable NACK DET IRQ */
#define     ALTR_I2C_ISER_RXRDY_EN	BIT(1)	/* Enable RX Ready IRQ */
#define     ALTR_I2C_ISER_TXRDY_EN	BIT(0)	/* Enable TX Ready IRQ */
#define ALTR_I2C_ISR		0x10	/* Interrupt Status register */
#define     ALTR_I2C_ISR_RXOF		BIT(4)	/* RX OVERFLOW IRQ */
#define     ALTR_I2C_ISR_ARB		BIT(3)	/* ARB LOST IRQ */
#define     ALTR_I2C_ISR_NACK		BIT(2)	/* NACK DET IRQ */
#define     ALTR_I2C_ISR_RXRDY		BIT(1)	/* RX Ready IRQ */
#define     ALTR_I2C_ISR_TXRDY		BIT(0)	/* TX Ready IRQ */
#define ALTR_I2C_STATUS		0x14	/* Status register */
#define	    ALTR_I2C_STAT_CORE		BIT(0)	/* Core Status (0=idle) */
#define ALTR_I2C_TC_FIFO_LVL	0x18	/* Transfer FIFO LVL register */
#define ALTR_I2C_RX_FIFO_LVL	0x1C	/* Receive FIFO LVL register */
#define ALTR_I2C_SCL_LOW	0x20	/* SCL low count register */
#define ALTR_I2C_SCL_HIGH	0x24	/* SCL high count register */
#define ALTR_I2C_SDA_HOLD	0x28	/* SDA hold count register */

#define ALTR_I2C_ALL_IRQ	(ALTR_I2C_ISR_RXOF | ALTR_I2C_ISR_ARB | \
				 ALTR_I2C_ISR_NACK | ALTR_I2C_ISR_RXRDY | \
				 ALTR_I2C_ISR_TXRDY)

#define ALTR_I2C_THRESHOLD	0	/*IRQ Threshold at 1 element */
#define ALTR_I2C_DFLT_FIFO_SZ	4
#define ALTR_I2C_TIMEOUT	100
#define ALTR_I2C_XFER_TIMEOUT	(msecs_to_jiffies(250))

/**
 * altr_i2c_dev - I2C device context
 * @base: pointer to register struct
 * @msg: pointer to current message
 * @msg_len: number of bytes transferred in msg
 * @msg_err: error code for completed message
 * @msg_complete: xfer completion object
 * @dev: device reference
 * @adapter: core i2c abstraction
 * @i2c_clk: clock reference for i2c input clock
 * @bus_clk_rate: current i2c bus clock rate
 * @buf: ptr to msg buffer for easier use.
 * @fifo_size: size of the FIFO passed in.
 */
struct altr_i2c_dev {
	void __iomem *base;
	struct i2c_msg *msg;
	size_t msg_len;
	int msg_err;
	struct completion msg_complete;
	struct device *dev;
	struct i2c_adapter adapter;
	struct clk *i2c_clk;
	u32 bus_clk_rate;
	u8 *buf;
	unsigned int fifo_size;
};

static void i2c_int_disable(struct altr_i2c_dev *idev, u32 mask)
{
	u32 int_en = readl(idev->base + ALTR_I2C_ISER);

	writel(int_en & ~mask, idev->base + ALTR_I2C_ISER);
}

static void i2c_int_enable(struct altr_i2c_dev *idev, u32 mask)
{
	u32 int_en = readl(idev->base + ALTR_I2C_ISER);

	writel(int_en | mask, idev->base + ALTR_I2C_ISER);
}

static void i2c_int_clear(struct altr_i2c_dev *idev, u32 mask)
{
	u32 int_en = readl(idev->base + ALTR_I2C_ISR);

	writel(int_en | mask, idev->base + ALTR_I2C_ISR);
}

static void altr_i2c_core_disable(struct altr_i2c_dev *idev)
{
	u32 tmp = readl(idev->base + ALTR_I2C_CTRL);

	writel(tmp & ~ALTR_I2C_CTRL_EN, idev->base + ALTR_I2C_CTRL);
}

static void altr_i2c_core_enable(struct altr_i2c_dev *idev)
{
	u32 tmp = readl(idev->base + ALTR_I2C_CTRL);

	writel(tmp | ALTR_I2C_CTRL_EN, idev->base + ALTR_I2C_CTRL);
}

static void altr_i2c_reset(struct altr_i2c_dev *idev)
{
	altr_i2c_core_disable(idev);
	altr_i2c_core_enable(idev);
}

static void altr_i2c_recover(struct altr_i2c_dev *idev)
{
	altr_i2c_reset(idev);
	/* Clock start bit + 8 bits + stop bit out */
	writel(ALTR_I2C_TFR_CMD_STA | ALTR_I2C_TFR_CMD_STO,
	       idev->base + ALTR_I2C_TFR_CMD);
	altr_i2c_reset(idev);

	i2c_recover_bus(&idev->adapter);
}

static inline void altr_i2c_stop(struct altr_i2c_dev *idev)
{
	writel(ALTR_I2C_TFR_CMD_STO, idev->base + ALTR_I2C_TFR_CMD);
}

static void altr_i2c_init(struct altr_i2c_dev *idev)
{
	u32 divisor = clk_get_rate(idev->i2c_clk) / idev->bus_clk_rate;
	u32 clk_mhz = clk_get_rate(idev->i2c_clk) / 1000000;
	u32 tmp = (ALTR_I2C_THRESHOLD << ALTR_I2C_CTRL_RXT_SHFT) |
		  (ALTR_I2C_THRESHOLD << ALTR_I2C_CTRL_TCT_SHFT);
	u32 t_high, t_low;

	if (idev->bus_clk_rate <= 100000) {
		tmp &= ~ALTR_I2C_CTRL_BSPEED;
		/* Standard mode SCL 50/50 */
		t_high = divisor * 1 / 2;
		t_low = divisor * 1 / 2;
	} else {
		tmp |= ALTR_I2C_CTRL_BSPEED;
		/* Fast mode SCL 33/66 */
		t_high = divisor * 1 / 3;
		t_low = divisor * 2 / 3;
	}
	writel(tmp, idev->base + ALTR_I2C_CTRL);

	dev_dbg(idev->dev, "rate=%uHz per_clk=%uMHz -> ratio=1:%u\n",
		idev->bus_clk_rate, clk_mhz, divisor);

	/* Reset controller */
	altr_i2c_reset(idev);

	/* SCL High Time */
	writel(t_high, idev->base + ALTR_I2C_SCL_HIGH);
	/* SCL Low Time */
	writel(t_low, idev->base + ALTR_I2C_SCL_LOW);
	/* SDA Hold Time, 300ns */
	writel(div_u64(300 * clk_mhz, 1000), idev->base + ALTR_I2C_SDA_HOLD);

	/* Mask all master interrupt bits */
	i2c_int_disable(idev, ~0);
}

static int i2c_m_rd(const struct i2c_msg *msg)
{
	return (msg->flags & I2C_M_RD) != 0;
}

/**
 * altr_i2c_empty_rx_fifo - Fetch data from RX FIFO until end of
 * transfer. Send a Stop bit on the last byte.
 */
static void altr_i2c_empty_rx_fifo(struct altr_i2c_dev *idev)
{
	size_t rx_fifo_avail = readl(idev->base + ALTR_I2C_RX_FIFO_LVL);
	int bytes_to_transfer = min(rx_fifo_avail, idev->msg_len);

	while (bytes_to_transfer-- > 0) {
		*idev->buf++ = readl(idev->base + ALTR_I2C_RX_DATA);
		if (idev->msg_len == 1)
			altr_i2c_stop(idev);
		else
			writel(0, idev->base + ALTR_I2C_TFR_CMD);

		idev->msg_len--;
	}
}

/**
 * altr_i2c_fill_tx_fifo - Fill TX FIFO from current message buffer.
 * @return: Number of bytes left to transfer.
 */
static int altr_i2c_fill_tx_fifo(struct altr_i2c_dev *idev)
{
	size_t tx_fifo_avail = idev->fifo_size - readl(idev->base +
						       ALTR_I2C_TC_FIFO_LVL);
	int bytes_to_transfer = min(tx_fifo_avail, idev->msg_len);
	int ret = idev->msg_len - bytes_to_transfer;

	while (bytes_to_transfer-- > 0) {
		if (idev->msg_len == 1)
			writel(ALTR_I2C_TFR_CMD_STO | *idev->buf++,
			       idev->base + ALTR_I2C_TFR_CMD);
		else
			writel(*idev->buf++, idev->base + ALTR_I2C_TFR_CMD);
		idev->msg_len--;
	}

	return ret;
}

/**
 * altr_i2c_wait_for_core_idle - After TX, check core idle for all bytes TX.
 * @return: 0 on success or -ETIMEDOUT on timeout.
 */
static int altr_i2c_wait_for_core_idle(struct altr_i2c_dev *idev)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(ALTR_I2C_TIMEOUT);

	do {
		if (time_after(jiffies, timeout)) {
			dev_err(idev->dev, "Core Idle timeout\n");
			return -ETIMEDOUT;
		}
	} while (readl(idev->base + ALTR_I2C_STATUS) & ALTR_I2C_STAT_CORE);

	return 0;
}

static irqreturn_t altr_i2c_isr(int irq, void *_dev)
{
	struct altr_i2c_dev *idev = _dev;
	/* Read IRQ status but only interested in Enabled IRQs. */
	u32 status = readl(idev->base + ALTR_I2C_ISR) &
		     readl(idev->base + ALTR_I2C_ISER);

	if (!idev->msg) {
		dev_warn(idev->dev, "unexpected interrupt\n");
		goto out;
	}

	/* handle Lost Arbitration */
	if (unlikely(status & ALTR_I2C_ISR_ARB)) {
		dev_err(idev->dev, "%s: arbitration lost\n", __func__);
		i2c_int_clear(idev, ALTR_I2C_ISR_ARB);
		idev->msg_err = -EAGAIN;
		goto complete;
	}

	if (unlikely(status & ALTR_I2C_ISR_NACK)) {
		dev_dbg(idev->dev, "%s: could not get ACK\n", __func__);
		idev->msg_err = -ENXIO;
		i2c_int_clear(idev, ALTR_I2C_ISR_NACK);
		altr_i2c_stop(idev);
		goto complete;
	}

	/* handle RX FIFO Overflow */
	if (i2c_m_rd(idev->msg) && unlikely(status & ALTR_I2C_ISR_RXOF)) {
		altr_i2c_empty_rx_fifo(idev);
		i2c_int_clear(idev, ALTR_I2C_ISR_RXRDY);
		altr_i2c_stop(idev);
		dev_err(idev->dev, "%s: RX FIFO Overflow\n", __func__);
		goto complete;
	}

	/* RX FIFO needs service? */
	if (i2c_m_rd(idev->msg) && (status & ALTR_I2C_ISR_RXRDY)) {
		altr_i2c_empty_rx_fifo(idev);
		i2c_int_clear(idev, ALTR_I2C_ISR_RXRDY);
		if (!idev->msg_len)
			goto complete;

		goto out;
	}

	/* TX FIFO needs service? */
	if (!i2c_m_rd(idev->msg) && (status & ALTR_I2C_ISR_TXRDY)) {
		i2c_int_clear(idev, ALTR_I2C_ISR_TXRDY);
		if (!altr_i2c_fill_tx_fifo(idev))
			goto complete;

		goto out;
	}

complete:
	dev_dbg(idev->dev, "%s: Message Complete\n", __func__);
	if (altr_i2c_wait_for_core_idle(idev))
		dev_err(idev->dev, "%s: message timeout\n", __func__);
	i2c_int_disable(idev, ALTR_I2C_ALL_IRQ);
	i2c_int_clear(idev, ALTR_I2C_ALL_IRQ);
	complete(&idev->msg_complete);

out:
	return IRQ_HANDLED;
}

static int altr_i2c_xfer_msg(struct altr_i2c_dev *idev, struct i2c_msg *msg)
{
	u32 int_mask = ALTR_I2C_ISR_RXOF | ALTR_I2C_ISR_ARB | ALTR_I2C_ISR_NACK;
	u32 addr = (msg->addr << 1) & 0xFF;
	unsigned long time_left;

	idev->msg = msg;
	idev->msg_len = msg->len;
	idev->buf = msg->buf;
	idev->msg_err = 0;
	reinit_completion(&idev->msg_complete);
	altr_i2c_core_enable(idev);

	if (i2c_m_rd(msg)) {
		/* Dummy read to ensure RX FIFO is empty */
		readl(idev->base + ALTR_I2C_RX_DATA);
		addr |= ALTR_I2C_TFR_CMD_RW_D;
	}

	writel(ALTR_I2C_TFR_CMD_STA | addr, idev->base + ALTR_I2C_TFR_CMD);

	if (i2c_m_rd(msg)) {
		/* write the first byte to start the RX */
		writel(0, idev->base + ALTR_I2C_TFR_CMD);
		int_mask |= ALTR_I2C_ISER_RXOF_EN | ALTR_I2C_ISER_RXRDY_EN;
	} else {
		altr_i2c_fill_tx_fifo(idev);
		int_mask |= ALTR_I2C_ISR_TXRDY;
	}
	i2c_int_enable(idev, int_mask);

	time_left = wait_for_completion_timeout(&idev->msg_complete,
						ALTR_I2C_XFER_TIMEOUT);

	i2c_int_disable(idev, int_mask);

	if (readl(idev->base + ALTR_I2C_STATUS) & ALTR_I2C_STAT_CORE)
		dev_err(idev->dev, "%s: Core Status not IDLE...\n", __func__);

	if (time_left == 0) {
		idev->msg_err = -ETIMEDOUT;
		dev_err(idev->dev, "%s: Transaction timed out.\n", __func__);
		altr_i2c_recover(idev);
	}

	if (unlikely(idev->msg_err) && idev->msg_err != -ENXIO)
		altr_i2c_init(idev);

	altr_i2c_core_disable(idev);

	return idev->msg_err;
}

static int
altr_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	struct altr_i2c_dev *idev = i2c_get_adapdata(adap);
	int i;
	int ret = 0;

	for (i = 0; ret == 0 && i < num; ++i)
		ret = altr_i2c_xfer_msg(idev, &msgs[i]);

	return ret ? : i;
}

static u32 altr_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm altr_i2c_algo = {
	.master_xfer = altr_i2c_xfer,
	.functionality = altr_i2c_func,
};

static int altr_i2c_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct altr_i2c_dev *idev = NULL;
	struct resource *res;
	int irq;
	int ret = 0;

	idev = devm_kzalloc(&pdev->dev, sizeof(*idev), GFP_KERNEL);
	if (!idev)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	idev->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(idev->base))
		return PTR_ERR(idev->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "missing interrupt resource\n");
		return irq;
	}

	idev->i2c_clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(idev->i2c_clk)) {
		dev_err(&pdev->dev, "missing clock\n");
		return PTR_ERR(idev->i2c_clk);
	}

	idev->dev = &pdev->dev;
	init_completion(&idev->msg_complete);

	if (of_property_read_u32(np, "fifo-size", &idev->fifo_size))
		idev->fifo_size = ALTR_I2C_DFLT_FIFO_SZ;

	if (of_property_read_u32(np, "clock-frequency", &idev->bus_clk_rate))
		idev->bus_clk_rate = 100000;	/* default clock rate */

	if (idev->bus_clk_rate > 400000) {
		dev_err(&pdev->dev, "invalid clock-frequency %d\n",
			idev->bus_clk_rate);
		return -EINVAL;
	}

	ret = devm_request_irq(&pdev->dev, irq, altr_i2c_isr, 0,
			       pdev->name, idev);
	if (ret) {
		dev_err(&pdev->dev, "failed to claim IRQ%d\n", irq);
		return ret;
	}

	ret = clk_prepare_enable(idev->i2c_clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable clock\n");
		return ret;
	}

	altr_i2c_init(idev);

	i2c_set_adapdata(&idev->adapter, idev);
	strlcpy(idev->adapter.name, pdev->name, sizeof(idev->adapter.name));
	idev->adapter.owner = THIS_MODULE;
	idev->adapter.algo = &altr_i2c_algo;
	idev->adapter.dev.parent = &pdev->dev;
	idev->adapter.dev.of_node = pdev->dev.of_node;

	platform_set_drvdata(pdev, idev);

	ret = i2c_add_adapter(&idev->adapter);
	if (ret) {
		clk_disable_unprepare(idev->i2c_clk);
		return ret;
	}
	dev_info(&pdev->dev, "Altera SoftIP I2C Probe Complete\n");

	return 0;
}

static int altr_i2c_remove(struct platform_device *pdev)
{
	struct altr_i2c_dev *idev = platform_get_drvdata(pdev);

	clk_disable_unprepare(idev->i2c_clk);
	i2c_del_adapter(&idev->adapter);

	return 0;
}

/* Match table for of_platform binding */
static const struct of_device_id altr_i2c_of_match[] = {
	{ .compatible = "altr,softip-i2c-v1.0" },
	{},
};
MODULE_DEVICE_TABLE(of, altr_i2c_of_match);

static struct platform_driver altr_i2c_driver = {
	.probe = altr_i2c_probe,
	.remove = altr_i2c_remove,
	.driver = {
		.name = "altera-i2c",
		.of_match_table = altr_i2c_of_match,
	},
};

module_platform_driver(altr_i2c_driver);

MODULE_DESCRIPTION("Altera Soft IP I2C bus driver");
MODULE_AUTHOR("Thor Thayer <thor.thayer@linux.intel.com>");
MODULE_LICENSE("GPL v2");
