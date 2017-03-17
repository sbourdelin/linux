/*
 * Driver for STMicroelectronics STM32F7 I2C controller
 *
 * This I2C controller is described in the STM32F75xxx and STM32F74xxx Soc
 * reference manual.
 * Please see below a link to the documentation:
 * http://www.st.com/resource/en/reference_manual/dm00124865.pdf
 *
 * Copyright (C) M'boumba Cedric Madianga 2017
 * Author: M'boumba Cedric Madianga <cedric.madianga@gmail.com>
 *
 * This driver is based on i2c-stm32f4.c
 *
 * License terms:  GNU General Public License (GPL), version 2
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "i2c-stm32.h"

/* STM32F7 I2C registers */
#define STM32F7_I2C_CR1				0x00
#define STM32F7_I2C_CR2				0x04
#define STM32F7_I2C_TIMINGR			0x10
#define STM32F7_I2C_ISR				0x18
#define STM32F7_I2C_ICR				0x1C
#define STM32F7_I2C_RXDR			0x24
#define STM32F7_I2C_TXDR			0x28

/* STM32F7 I2C control 1 */
#define STM32F7_I2C_CR1_ERRIE			BIT(7)
#define STM32F7_I2C_CR1_TCIE			BIT(6)
#define STM32F7_I2C_CR1_STOPIE			BIT(5)
#define STM32F7_I2C_CR1_NACKIE			BIT(4)
#define STM32F7_I2C_CR1_ADDRIE			BIT(3)
#define STM32F7_I2C_CR1_RXIE			BIT(2)
#define STM32F7_I2C_CR1_TXIE			BIT(1)
#define STM32F7_I2C_CR1_PE			BIT(0)
#define STM32F7_I2C_ALL_IRQ_MASK		(STM32F7_I2C_CR1_ERRIE \
						| STM32F7_I2C_CR1_TCIE \
						| STM32F7_I2C_CR1_STOPIE \
						| STM32F7_I2C_CR1_NACKIE \
						| STM32F7_I2C_CR1_RXIE \
						| STM32F7_I2C_CR1_TXIE)

/* STM32F7 I2C control 2 */
#define STM32F7_I2C_CR2_RELOAD			BIT(24)
#define STM32F7_I2C_CR2_NBYTES_MASK		GENMASK(23, 16)
#define STM32F7_I2C_CR2_NBYTES(n)		(((n) & 0xff) << 16)
#define STM32F7_I2C_CR2_NACK			BIT(15)
#define STM32F7_I2C_CR2_STOP			BIT(14)
#define STM32F7_I2C_CR2_START			BIT(13)
#define STM32F7_I2C_CR2_RD_WRN			BIT(10)
#define STM32F7_I2C_CR2_SADD7_MASK		GENMASK(7, 1)
#define STM32F7_I2C_CR2_SADD7(n)		(((n) & 0x7f) << 1)

/* STM32F7 I2C Interrupt Status */
#define STM32F7_I2C_ISR_BUSY			BIT(15)
#define STM32F7_I2C_ISR_ARLO			BIT(9)
#define STM32F7_I2C_ISR_BERR			BIT(8)
#define STM32F7_I2C_ISR_TCR			BIT(7)
#define STM32F7_I2C_ISR_TC			BIT(6)
#define STM32F7_I2C_ISR_STOPF			BIT(5)
#define STM32F7_I2C_ISR_NACKF			BIT(4)
#define STM32F7_I2C_ISR_RXNE			BIT(2)
#define STM32F7_I2C_ISR_TXIS			BIT(1)

/* STM32F7 I2C Interrupt Clear */
#define STM32F7_I2C_ICR_ARLOCF			BIT(9)
#define STM32F7_I2C_ICR_BERRCF			BIT(8)
#define STM32F7_I2C_ICR_STOPCF			BIT(5)
#define STM32F7_I2C_ICR_NACKCF			BIT(4)

#define STM32F7_I2C_MAX_LEN			0xff

/**
 * struct stm32f7_i2c_msg - client specific data
 * @addr: 8-bit slave addr, including r/w bit
 * @count: number of bytes to be transferred
 * @buf: data buffer
 * @result: result of the transfer
 * @stop: last I2C msg to be sent, i.e. STOP to be generated
 */
struct stm32f7_i2c_msg {
	u8 addr;
	u32 count;
	u8 *buf;
	int result;
	bool stop;
};

/**
 * struct stm32f7_i2c_dev - private data of the controller
 * @adap: I2C adapter for this controller
 * @dev: device for this controller
 * @base: virtual memory area
 * @complete: completion of I2C message
 * @clk: hw i2c clock
 * @speed: I2C clock frequency of the controller. Standard, Fast or Fast+
 * @msg: Pointer to data to be written
 * @msg_num: number of I2C messages to be executed
 * @msg_id: message identifiant
 * @f7_msg: customized i2c msg for driver usage
 */
struct stm32f7_i2c_dev {
	struct i2c_adapter adap;
	struct device *dev;
	void __iomem *base;
	struct completion complete;
	struct clk *clk;
	int speed;
	struct i2c_msg *msg;
	unsigned int msg_num;
	unsigned int msg_id;
	struct stm32f7_i2c_msg f7_msg;
};

static inline void stm32f7_i2c_set_bits(void __iomem *reg, u32 mask)
{
	writel_relaxed(readl_relaxed(reg) | mask, reg);
}

static inline void stm32f7_i2c_clr_bits(void __iomem *reg, u32 mask)
{
	writel_relaxed(readl_relaxed(reg) & ~mask, reg);
}

static int stm32f7_i2c_hw_config(struct stm32f7_i2c_dev *i2c_dev)
{
	struct device_node *of_node = i2c_dev->dev->of_node;
	u32 timing;
	int ret;

	ret = of_property_read_u32(of_node, "st,i2c-timing", &timing);
	if (ret) {
		dev_err(i2c_dev->dev, "Error: missing i2c timing property\n");
		return ret;
	}

	/* Timing settings */
	writel_relaxed(timing, i2c_dev->base + STM32F7_I2C_TIMINGR);

	/* Enable I2C */
	writel_relaxed(STM32F7_I2C_CR1_PE, i2c_dev->base + STM32F7_I2C_CR1);

	return 0;
}

static void stm32f7_i2c_write_tx_data(struct stm32f7_i2c_dev *i2c_dev)
{
	struct stm32f7_i2c_msg *f7_msg = &i2c_dev->f7_msg;
	void __iomem *base = i2c_dev->base;

	if (f7_msg->count) {
		writeb_relaxed(*f7_msg->buf++, base + STM32F7_I2C_TXDR);
		f7_msg->count--;
	}
}

static void stm32f7_i2c_read_rx_data(struct stm32f7_i2c_dev *i2c_dev)
{
	struct stm32f7_i2c_msg *f7_msg = &i2c_dev->f7_msg;
	void __iomem *base = i2c_dev->base;

	if (f7_msg->count) {
		*f7_msg->buf++ = readb_relaxed(base + STM32F7_I2C_RXDR);
		f7_msg->count--;
	}
}

static void stm32f7_i2c_reload(struct stm32f7_i2c_dev *i2c_dev)
{
	struct stm32f7_i2c_msg *f7_msg = &i2c_dev->f7_msg;
	u32 cr2;

	cr2 = readl_relaxed(i2c_dev->base + STM32F7_I2C_CR2);

	cr2 &= ~STM32F7_I2C_CR2_NBYTES_MASK;
	if (f7_msg->count > STM32F7_I2C_MAX_LEN) {
		cr2 |= STM32F7_I2C_CR2_NBYTES(STM32F7_I2C_MAX_LEN);
	} else {
		cr2 &= ~STM32F7_I2C_CR2_RELOAD;
		cr2 |= STM32F7_I2C_CR2_NBYTES(f7_msg->count);
	}

	writel_relaxed(cr2, i2c_dev->base + STM32F7_I2C_CR2);
}

static int stm32f7_i2c_wait_free_bus(struct stm32f7_i2c_dev *i2c_dev)
{
	u32 status;
	int ret;

	ret = readl_relaxed_poll_timeout(i2c_dev->base + STM32F7_I2C_ISR,
					 status,
					 !(status & STM32F7_I2C_ISR_BUSY),
					 10, 1000);
	if (ret) {
		dev_dbg(i2c_dev->dev, "bus busy\n");
		ret = -EBUSY;
	}

	return ret;
}

static void stm32f7_i2c_xfer_msg(struct stm32f7_i2c_dev *i2c_dev,
				 struct i2c_msg *msg)
{
	struct stm32f7_i2c_msg *f7_msg = &i2c_dev->f7_msg;
	void __iomem *base = i2c_dev->base;
	u32 cr1, cr2;

	f7_msg->addr = msg->addr;
	f7_msg->buf = msg->buf;
	f7_msg->count = msg->len;
	f7_msg->result = 0;
	f7_msg->stop = (i2c_dev->msg_id >= i2c_dev->msg_num - 1);

	reinit_completion(&i2c_dev->complete);

	cr1 = readl_relaxed(base + STM32F7_I2C_CR1);
	cr2 = readl_relaxed(base + STM32F7_I2C_CR2);

	/* Set transfer direction */
	cr2 &= ~STM32F7_I2C_CR2_RD_WRN;
	if (msg->flags & I2C_M_RD)
		cr2 |= STM32F7_I2C_CR2_RD_WRN;

	/* Set slave address */
	cr2 &= ~STM32F7_I2C_CR2_SADD7_MASK;
	cr2 |= STM32F7_I2C_CR2_SADD7(f7_msg->addr);

	/* Set nb bytes to transfer and reload if needed */
	cr2 &= ~(STM32F7_I2C_CR2_NBYTES_MASK | STM32F7_I2C_CR2_RELOAD);
	if (f7_msg->count > STM32F7_I2C_MAX_LEN) {
		cr2 |= STM32F7_I2C_CR2_NBYTES(STM32F7_I2C_MAX_LEN);
		cr2 |= STM32F7_I2C_CR2_RELOAD;
	} else {
		cr2 |= STM32F7_I2C_CR2_NBYTES(f7_msg->count);
	}

	/* Enable NACK, STOP, error and transfer complete interrupts */
	cr1 |= STM32F7_I2C_CR1_ERRIE | STM32F7_I2C_CR1_TCIE |
		STM32F7_I2C_CR1_STOPIE | STM32F7_I2C_CR1_NACKIE;

	/* Clear TX/RX interrupt */
	cr1 &= ~(STM32F7_I2C_CR1_RXIE | STM32F7_I2C_CR1_TXIE);

	/* Enable RX/TX interrupt according to msg direction */
	if (msg->flags & I2C_M_RD)
		cr1 |= STM32F7_I2C_CR1_RXIE;
	else
		cr1 |= STM32F7_I2C_CR1_TXIE;

	/* Configure Start/Repeated Start */
	cr2 |= STM32F7_I2C_CR2_START;

	/* Write configurations registers */
	writel_relaxed(cr1, base + STM32F7_I2C_CR1);
	writel_relaxed(cr2, base + STM32F7_I2C_CR2);
}

static void stm32f7_i2c_disable_irq(struct stm32f7_i2c_dev *i2c_dev, u32 mask)
{
	stm32f7_i2c_clr_bits(i2c_dev->base + STM32F7_I2C_CR1, mask);
}

static irqreturn_t stm32f7_i2c_isr_event(int irq, void *data)
{
	struct stm32f7_i2c_dev *i2c_dev = data;
	struct stm32f7_i2c_msg *f7_msg = &i2c_dev->f7_msg;
	void __iomem *base = i2c_dev->base;
	u32 status, mask;

	status = readl_relaxed(i2c_dev->base + STM32F7_I2C_ISR);

	/* Tx empty */
	if (status & STM32F7_I2C_ISR_TXIS)
		stm32f7_i2c_write_tx_data(i2c_dev);

	/* RX not empty */
	if (status & STM32F7_I2C_ISR_RXNE)
		stm32f7_i2c_read_rx_data(i2c_dev);

	/* NACK received */
	if (status & STM32F7_I2C_ISR_NACKF) {
		dev_dbg(i2c_dev->dev, "<%s>: Receive NACK\n", __func__);
		writel_relaxed(STM32F7_I2C_ICR_NACKCF, base + STM32F7_I2C_ICR);
		f7_msg->result = -EBADE;
	}

	/* STOP detection flag */
	if (status & STM32F7_I2C_ISR_STOPF) {
		/* Disable interrupts */
		stm32f7_i2c_disable_irq(i2c_dev, STM32F7_I2C_ALL_IRQ_MASK);

		/* Clear STOP flag */
		writel_relaxed(STM32F7_I2C_ICR_STOPCF, base + STM32F7_I2C_ICR);

		complete(&i2c_dev->complete);
	}

	/* Transfer complete */
	if (status & STM32F7_I2C_ISR_TC) {
		if (f7_msg->stop) {
			mask = STM32F7_I2C_CR2_STOP;
			stm32f7_i2c_set_bits(base + STM32F7_I2C_CR2, mask);
		} else {
			i2c_dev->msg_id++;
			i2c_dev->msg++;
			stm32f7_i2c_xfer_msg(i2c_dev, i2c_dev->msg);
		}
	}

	/*
	 * Transfer Complete Reload: 255 data bytes have been transferred
	 * We have to prepare the I2C controller to transfer the remaining
	 * data
	 */
	if (status & STM32F7_I2C_ISR_TCR)
		stm32f7_i2c_reload(i2c_dev);

	return IRQ_HANDLED;
}

static irqreturn_t stm32f7_i2c_isr_error(int irq, void *data)
{
	struct stm32f7_i2c_dev *i2c_dev = data;
	struct stm32f7_i2c_msg *f7_msg = &i2c_dev->f7_msg;
	void __iomem *base = i2c_dev->base;
	struct device *dev = i2c_dev->dev;
	u32 status;

	status = readl_relaxed(i2c_dev->base + STM32F7_I2C_ISR);

	/* Bus error */
	if (status & STM32F7_I2C_ISR_BERR) {
		dev_err(dev, "<%s>: Bus error\n", __func__);
		writel_relaxed(STM32F7_I2C_ICR_BERRCF, base + STM32F7_I2C_ICR);
		f7_msg->result = -EIO;
	}

	/* Arbitration loss */
	if (status & STM32F7_I2C_ISR_ARLO) {
		dev_err(dev, "<%s>: Arbitration loss\n", __func__);
		writel_relaxed(STM32F7_I2C_ICR_ARLOCF, base + STM32F7_I2C_ICR);
		f7_msg->result = -EAGAIN;
	}

	stm32f7_i2c_disable_irq(i2c_dev, STM32F7_I2C_ALL_IRQ_MASK);

	complete(&i2c_dev->complete);

	return IRQ_HANDLED;
}

static int stm32f7_i2c_xfer(struct i2c_adapter *i2c_adap,
			    struct i2c_msg msgs[], int num)
{
	struct stm32f7_i2c_dev *i2c_dev = i2c_get_adapdata(i2c_adap);
	struct stm32f7_i2c_msg *f7_msg = &i2c_dev->f7_msg;
	unsigned long timeout;
	int ret;

	i2c_dev->msg = msgs;
	i2c_dev->msg_num = num;
	i2c_dev->msg_id = 0;

	ret = clk_enable(i2c_dev->clk);
	if (ret) {
		dev_err(i2c_dev->dev, "Failed to enable clock\n");
		return ret;
	}

	ret = stm32f7_i2c_wait_free_bus(i2c_dev);
	if (ret)
		goto clk_free;

	stm32f7_i2c_xfer_msg(i2c_dev, msgs);

	timeout = wait_for_completion_timeout(&i2c_dev->complete,
					      i2c_dev->adap.timeout);
	ret = f7_msg->result;

	if (!timeout) {
		dev_dbg(i2c_dev->dev, "Access to slave 0x%x timed out\n",
			i2c_dev->msg->addr);
		ret = -ETIMEDOUT;
	}

clk_free:
	clk_disable(i2c_dev->clk);

	return (ret < 0) ? ret : num;
}

static u32 stm32f7_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static struct i2c_algorithm stm32f7_i2c_algo = {
	.master_xfer = stm32f7_i2c_xfer,
	.functionality = stm32f7_i2c_func,
};

static int stm32f7_i2c_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct stm32f7_i2c_dev *i2c_dev;
	struct resource *res;
	u32 irq_event, irq_error, clk_rate;
	struct i2c_adapter *adap;
	struct reset_control *rst;
	int ret;

	i2c_dev = devm_kzalloc(&pdev->dev, sizeof(*i2c_dev), GFP_KERNEL);
	if (!i2c_dev)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	i2c_dev->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2c_dev->base))
		return PTR_ERR(i2c_dev->base);

	irq_event = irq_of_parse_and_map(np, 0);
	if (!irq_event) {
		dev_err(&pdev->dev, "IRQ event missing or invalid\n");
		return -EINVAL;
	}

	irq_error = irq_of_parse_and_map(np, 1);
	if (!irq_error) {
		dev_err(&pdev->dev, "IRQ error missing or invalid\n");
		return -EINVAL;
	}

	i2c_dev->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(i2c_dev->clk)) {
		dev_err(&pdev->dev, "Error: Missing controller clock\n");
		return PTR_ERR(i2c_dev->clk);
	}
	ret = clk_prepare_enable(i2c_dev->clk);
	if (ret) {
		dev_err(&pdev->dev, "Failed to prepare_enable clock\n");
		return ret;
	}

	rst = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(rst)) {
		dev_err(&pdev->dev, "Error: Missing controller reset\n");
		ret = PTR_ERR(rst);
		goto clk_free;
	}
	reset_control_assert(rst);
	udelay(2);
	reset_control_deassert(rst);

	i2c_dev->speed = STM32_I2C_SPEED_STANDARD;
	ret = of_property_read_u32(np, "clock-frequency", &clk_rate);
	if ((!ret) && (clk_rate == 400000))
		i2c_dev->speed = STM32_I2C_SPEED_FAST;
	else if ((!ret) && (clk_rate == 1000000))
		i2c_dev->speed = STM32_I2C_SPEED_FAST_PLUS;

	i2c_dev->dev = &pdev->dev;

	ret = devm_request_irq(&pdev->dev, irq_event, stm32f7_i2c_isr_event, 0,
			       pdev->name, i2c_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request irq event %i\n",
			irq_event);
		goto clk_free;
	}

	ret = devm_request_irq(&pdev->dev, irq_error, stm32f7_i2c_isr_error, 0,
			       pdev->name, i2c_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request irq error %i\n",
			irq_error);
		goto clk_free;
	}

	ret = stm32f7_i2c_hw_config(i2c_dev);
	if (ret)
		goto clk_free;

	adap = &i2c_dev->adap;
	i2c_set_adapdata(adap, i2c_dev);
	snprintf(adap->name, sizeof(adap->name), "STM32F7 I2C(%pa)",
		 &res->start);
	adap->owner = THIS_MODULE;
	adap->timeout = 2 * HZ;
	adap->retries = 0;
	adap->algo = &stm32f7_i2c_algo;
	adap->dev.parent = &pdev->dev;
	adap->dev.of_node = pdev->dev.of_node;

	init_completion(&i2c_dev->complete);

	ret = i2c_add_adapter(adap);
	if (ret) {
		dev_err(&pdev->dev, "Failed to add adapter\n");
		goto clk_free;
	}

	platform_set_drvdata(pdev, i2c_dev);

	clk_disable(i2c_dev->clk);

	dev_info(i2c_dev->dev, "STM32F7 I2C-%d driver registered\n", adap->nr);

	return 0;

clk_free:
	clk_disable_unprepare(i2c_dev->clk);

	return ret;
}

static int stm32f7_i2c_remove(struct platform_device *pdev)
{
	struct stm32f7_i2c_dev *i2c_dev = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c_dev->adap);

	clk_unprepare(i2c_dev->clk);

	return 0;
}

static const struct of_device_id stm32f7_i2c_match[] = {
	{ .compatible = "st,stm32f7-i2c", },
	{},
};
MODULE_DEVICE_TABLE(of, stm32f7_i2c_match);

static struct platform_driver stm32f7_i2c_driver = {
	.driver = {
		.name = "stm32f7-i2c",
		.of_match_table = stm32f7_i2c_match,
	},
	.probe = stm32f7_i2c_probe,
	.remove = stm32f7_i2c_remove,
};

module_platform_driver(stm32f7_i2c_driver);

MODULE_AUTHOR("M'boumba Cedric Madianga <cedric.madianga@gmail.com>");
MODULE_DESCRIPTION("STMicroelectronics STM32F7 I2C driver");
MODULE_LICENSE("GPL v2");
