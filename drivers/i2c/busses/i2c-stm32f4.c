/*
 * Driver for STMicroelectronics STM32 I2C controller
 *
 * This I2C controller is described in the STM32F429/439 Soc reference manual.
 * Please see below a link to the documentation:
 * http://www.st.com/resource/en/reference_manual/DM00031020.pdf
 *
 * Copyright (C) M'boumba Cedric Madianga 2016
 * Author: M'boumba Cedric Madianga <cedric.madianga@gmail.com>
 *
 * This driver is based on i2c-st.c
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
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

/* STM32F4 I2C offset registers */
#define STM32F4_I2C_CR1			0x00
#define STM32F4_I2C_CR2			0x04
#define STM32F4_I2C_DR			0x10
#define STM32F4_I2C_SR1			0x14
#define STM32F4_I2C_SR2			0x18
#define STM32F4_I2C_CCR			0x1C
#define STM32F4_I2C_TRISE		0x20
#define STM32F4_I2C_FLTR		0x24

/* STM32F4 I2C control 1*/
#define STM32F4_I2C_CR1_POS		BIT(11)
#define STM32F4_I2C_CR1_ACK		BIT(10)
#define STM32F4_I2C_CR1_STOP		BIT(9)
#define STM32F4_I2C_CR1_START		BIT(8)
#define STM32F4_I2C_CR1_PE		BIT(0)

/* STM32F4 I2C control 2 */
#define STM32F4_I2C_CR2_FREQ_MASK	GENMASK(5, 0)
#define STM32F4_I2C_CR2_FREQ(n)		((n) & STM32F4_I2C_CR2_FREQ_MASK)
#define STM32F4_I2C_CR2_ITBUFEN		BIT(10)
#define STM32F4_I2C_CR2_ITEVTEN		BIT(9)
#define STM32F4_I2C_CR2_ITERREN		BIT(8)
#define STM32F4_I2C_CR2_IRQ_MASK	(STM32F4_I2C_CR2_ITBUFEN | \
					 STM32F4_I2C_CR2_ITEVTEN | \
					 STM32F4_I2C_CR2_ITERREN)

/* STM32F4 I2C Status 1 */
#define STM32F4_I2C_SR1_AF		BIT(10)
#define STM32F4_I2C_SR1_ARLO		BIT(9)
#define STM32F4_I2C_SR1_BERR		BIT(8)
#define STM32F4_I2C_SR1_TXE		BIT(7)
#define STM32F4_I2C_SR1_RXNE		BIT(6)
#define STM32F4_I2C_SR1_BTF		BIT(2)
#define STM32F4_I2C_SR1_ADDR		BIT(1)
#define STM32F4_I2C_SR1_SB		BIT(0)
#define STM32F4_I2C_SR1_ITEVTEN_MASK	(STM32F4_I2C_SR1_BTF | \
					 STM32F4_I2C_SR1_ADDR | \
					 STM32F4_I2C_SR1_SB)
#define STM32F4_I2C_SR1_ITBUFEN_MASK	(STM32F4_I2C_SR1_TXE | \
					 STM32F4_I2C_SR1_RXNE)
#define STM32F4_I2C_SR1_ITERREN_MASK	(STM32F4_I2C_SR1_AF | \
					 STM32F4_I2C_SR1_ARLO | \
					 STM32F4_I2C_SR1_BERR)

/* STM32F4 I2C Status 2 */
#define STM32F4_I2C_SR2_BUSY		BIT(1)

/* STM32F4 I2C Control Clock */
#define STM32F4_I2C_CCR_CCR_MASK	GENMASK(11, 0)
#define STM32F4_I2C_CCR_CCR(n)		((n) & STM32F4_I2C_CCR_CCR_MASK)
#define STM32F4_I2C_CCR_FS		BIT(15)
#define STM32F4_I2C_CCR_DUTY		BIT(14)

/* STM32F4 I2C Trise */
#define STM32F4_I2C_TRISE_VALUE_MASK	GENMASK(5, 0)
#define STM32F4_I2C_TRISE_VALUE(n)	((n) & STM32F4_I2C_TRISE_VALUE_MASK)

/* STM32F4 I2C Filter */
#define STM32F4_I2C_FLTR_DNF_MASK	GENMASK(3, 0)
#define STM32F4_I2C_FLTR_DNF(n)		((n) & STM32F4_I2C_FLTR_DNF_MASK)
#define STM32F4_I2C_FLTR_ANOFF		BIT(4)

#define STM32F4_I2C_MIN_FREQ		2U
#define STM32F4_I2C_MAX_FREQ		46U
#define HZ_TO_MHZ			1000000

enum stm32f4_i2c_speed {
	STM32F4_I2C_SPEED_STANDARD, /* 100 kHz */
	STM32F4_I2C_SPEED_FAST, /* 400 kHz */
	STM32F4_I2C_SPEED_END,
};

/**
 * struct stm32f4_i2c_timings - per-Mode tuning parameters
 * @duty: Fast mode duty cycle
 * @scl_period: SCL period in microsecond to reach 100kHz/400kHz SCL frequency
 */
struct stm32f4_i2c_timings {
	u32 duty;
	u32 scl_period;
};

/**
 * struct stm32f4_i2c_msg - client specific data
 * @addr: 8-bit slave addr, including r/w bit
 * @count: number of bytes to be transferred
 * @buf: data buffer
 * @result: result of the transfer
 * @stop: last I2C msg to be sent, i.e. STOP to be generated
 */
struct stm32f4_i2c_msg {
	u8 addr;
	u32 count;
	u8 *buf;
	int result;
	bool stop;
};

/**
 * struct stm32f4_i2c_dev - private data of the controller
 * @adap: I2C adapter for this controller
 * @dev: device for this controller
 * @base: virtual memory area
 * @complete: completion of I2C message
 * @clk: hw i2c clock
 * speed: I2C clock frequency of the controller. Standard or Fast only supported
 * @msg: I2C transfer information
 */
struct stm32f4_i2c_dev {
	struct i2c_adapter adap;
	struct device *dev;
	void __iomem *base;
	struct completion complete;
	struct clk *clk;
	int speed;
	struct stm32f4_i2c_msg msg;
};

/*
 * In standard mode:
 * SCL period = SCL high period = SCL low period = CCR * I2C parent clk period
 *
 * In fast mode:
 * If Duty = 0; SCL high period = 1  * CCR * I2C parent clk period
 *		SCL low period  = 2  * CCR * I2C parent clk period
 * If Duty = 1; SCL high period = 9  * CCR * I2C parent clk period
 *		SCL low period  = 16 * CCR * I2C parent clk period
 * In order to reach 400 kHz with lower I2C parent clk frequencies we always set
 * Duty = 1
 *
 * For both modes, we have CCR = SCL period * I2C parent clk frequency
 * with scl_period = 5 microseconds in Standard mode and scl_period = 1
 * microsecond in Fast Mode in order to satisfy scl_high and scl_low periods
 * constraints defined by i2c bus specification
 */
static struct stm32f4_i2c_timings i2c_timings[] = {
	[STM32F4_I2C_SPEED_STANDARD] = {
		.duty			= 0,
		.scl_period		= 5,
	},
	[STM32F4_I2C_SPEED_FAST] = {
		.duty			= 1,
		.scl_period		= 1,
	},
};

static inline void stm32f4_i2c_set_bits(void __iomem *reg, u32 mask)
{
	writel_relaxed(readl_relaxed(reg) | mask, reg);
}

static inline void stm32f4_i2c_clr_bits(void __iomem *reg, u32 mask)
{
	writel_relaxed(readl_relaxed(reg) & ~mask, reg);
}

static void stm32f4_i2c_disable_irq(struct stm32f4_i2c_dev *i2c_dev)
{
	void __iomem *reg = i2c_dev->base + STM32F4_I2C_CR2;

	stm32f4_i2c_clr_bits(reg, STM32F4_I2C_CR2_IRQ_MASK);
}

static int stm32f4_i2c_set_periph_clk_freq(struct stm32f4_i2c_dev *i2c_dev)
{
	u32 clk_rate, cr2, freq;

	/*
	 * The minimum allowed frequency is 2MHz, the maximum APB frequency is
	 * limited by an intrinsic value of 46 MHz
	 */
	clk_rate = clk_get_rate(i2c_dev->clk);
	freq = DIV_ROUND_UP(clk_rate, HZ_TO_MHZ);
	if (freq < STM32F4_I2C_MIN_FREQ || freq > STM32F4_I2C_MAX_FREQ) {
		dev_err(i2c_dev->dev, "out of scope parent clk freq at %dMHz\n",
			freq);
		return -EINVAL;
	}

	cr2 = readl_relaxed(i2c_dev->base + STM32F4_I2C_CR2);
	cr2 &= ~STM32F4_I2C_CR2_FREQ_MASK;
	cr2 |= STM32F4_I2C_CR2_FREQ(freq);
	writel_relaxed(cr2, i2c_dev->base + STM32F4_I2C_CR2);

	return 0;
}

static void stm32f4_i2c_set_rise_time(struct stm32f4_i2c_dev *i2c_dev)
{
	u32 trise, freq, cr2;

	/*
	 * These bits must be programmed with the maximum SCL rise time given in
	 * the I2C bus specification, incremented by 1.
	 *
	 * In standard mode, the maximum allowed SCL rise time is 1000 ns.
	 * If, in the I2C_CR2 register, the value of FREQ[5:0] bits is equal to
	 * 0x08 so period = 125 ns therefore the TRISE[5:0] bits must be
	 * programmed with 09h.(1000 ns / 125 ns = 8 + 1)
	 * So, for I2C standard mode TRISE = FREQ[5:0] + 1
	 *
	 * In fast mode, the maximum allowed SCL rise time is 300 ns.
	 * If, in the I2C_CR2 register, the value of FREQ[5:0] bits is equal to
	 * 0x08 so period = 125 ns therefore the TRISE[5:0] bits must be
	 * programmed with 03h.(300 ns / 125 ns = 2 + 1)
	 * So, for I2C fast mode TRISE = FREQ[5:0] * 300 / 1000 + 1
	 */

	cr2 = readl_relaxed(i2c_dev->base + STM32F4_I2C_CR2);
	freq = cr2 & STM32F4_I2C_CR2_FREQ_MASK;

	if (i2c_dev->speed == STM32F4_I2C_SPEED_STANDARD)
		trise = freq + 1;
	else
		trise = freq * 300 / 1000 + 1;

	writel_relaxed(STM32F4_I2C_TRISE_VALUE(trise),
		       i2c_dev->base + STM32F4_I2C_TRISE);
}

static void stm32f4_i2c_set_speed_mode(struct stm32f4_i2c_dev *i2c_dev)
{
	struct stm32f4_i2c_timings *t = &i2c_timings[i2c_dev->speed];
	u32 cr2, ccr, freq, val;

	ccr = readl_relaxed(i2c_dev->base + STM32F4_I2C_CCR);
	ccr &= ~(STM32F4_I2C_CCR_FS | STM32F4_I2C_CCR_DUTY |
		 STM32F4_I2C_CCR_CCR_MASK);

	/*
	 * Please see the comments above regarding i2c_timings[] declaration
	 * to understand the below calculation
	 */
	cr2 = readl_relaxed(i2c_dev->base + STM32F4_I2C_CR2);
	freq = cr2 & STM32F4_I2C_CR2_FREQ_MASK;
	val = freq * t->scl_period;
	ccr |= STM32F4_I2C_CCR_CCR(val);

	if (t->duty)
		ccr |= STM32F4_I2C_CCR_FS | STM32F4_I2C_CCR_DUTY;

	writel_relaxed(ccr, i2c_dev->base + STM32F4_I2C_CCR);
}

static void stm32f4_i2c_set_filter(struct stm32f4_i2c_dev *i2c_dev)
{
	u32 filter;

	/* Enable analog noise filter and disable digital noise filter */
	filter = readl_relaxed(i2c_dev->base + STM32F4_I2C_FLTR);
	filter &= ~(STM32F4_I2C_FLTR_ANOFF | STM32F4_I2C_FLTR_DNF_MASK);
	writel_relaxed(filter, i2c_dev->base + STM32F4_I2C_FLTR);
}

/**
 * stm32f4_i2c_hw_config() - Prepare I2C block
 * @i2c_dev: Controller's private data
 */
static int stm32f4_i2c_hw_config(struct stm32f4_i2c_dev *i2c_dev)
{
	void __iomem *reg = i2c_dev->base + STM32F4_I2C_CR1;
	int ret = 0;

	/* Disable I2C */
	stm32f4_i2c_clr_bits(reg, STM32F4_I2C_CR1_PE);

	ret = stm32f4_i2c_set_periph_clk_freq(i2c_dev);
	if (ret)
		return ret;

	stm32f4_i2c_set_rise_time(i2c_dev);

	stm32f4_i2c_set_speed_mode(i2c_dev);

	stm32f4_i2c_set_filter(i2c_dev);

	/* Enable I2C */
	stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_PE);

	return ret;
}

static int stm32f4_i2c_wait_free_bus(struct stm32f4_i2c_dev *i2c_dev)
{
	u32 status;
	int ret;

	ret = readl_relaxed_poll_timeout(i2c_dev->base + STM32F4_I2C_SR2,
					 status,
					 !(status & STM32F4_I2C_SR2_BUSY),
					 10, 1000);
	if (ret) {
		dev_dbg(i2c_dev->dev, "bus not free\n");
		ret = -EBUSY;
	}

	return ret;
}

/**
 * stm32f4_i2c_write_ byte() - Write a byte in the data register
 * @i2c_dev: Controller's private data
 * @byte: Data to write in the register
 */
static void stm32f4_i2c_write_byte(struct stm32f4_i2c_dev *i2c_dev, u8 byte)
{
	writel_relaxed(byte, i2c_dev->base + STM32F4_I2C_DR);
}

/**
 * stm32f4_i2c_write_msg() - Fill the data register in write mode
 * @i2c_dev: Controller's private data
 *
 * This function fills the data register with I2C transfer buffer
 */
static void stm32f4_i2c_write_msg(struct stm32f4_i2c_dev *i2c_dev)
{
	struct stm32f4_i2c_msg *msg = &i2c_dev->msg;

	stm32f4_i2c_write_byte(i2c_dev, *msg->buf++);
	msg->count--;
}

static void stm32f4_i2c_read_msg(struct stm32f4_i2c_dev *i2c_dev)
{
	struct stm32f4_i2c_msg *msg = &i2c_dev->msg;
	u32 rbuf;

	rbuf = readl_relaxed(i2c_dev->base + STM32F4_I2C_DR);
	*msg->buf++ = rbuf & 0xff;
	msg->count--;
}

static void stm32f4_i2c_terminate_xfer(struct stm32f4_i2c_dev *i2c_dev)
{
	struct stm32f4_i2c_msg *msg = &i2c_dev->msg;
	void __iomem *reg = i2c_dev->base + STM32F4_I2C_CR2;

	stm32f4_i2c_disable_irq(i2c_dev);

	reg = i2c_dev->base + STM32F4_I2C_CR1;
	if (msg->stop)
		stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_STOP);
	else
		stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_START);

	complete(&i2c_dev->complete);
}

/**
 * stm32f4_i2c_handle_write() - Handle FIFO empty interrupt in case of write
 * @i2c_dev: Controller's private data
 */
static void stm32f4_i2c_handle_write(struct stm32f4_i2c_dev *i2c_dev)
{
	struct stm32f4_i2c_msg *msg = &i2c_dev->msg;
	void __iomem *reg = i2c_dev->base + STM32F4_I2C_CR2;

	if (msg->count) {
		stm32f4_i2c_write_msg(i2c_dev);
		if (!msg->count) {
			/* Disable buffer interrupts for RXNE/TXE events */
			stm32f4_i2c_clr_bits(reg, STM32F4_I2C_CR2_ITBUFEN);
		}
	} else {
		stm32f4_i2c_terminate_xfer(i2c_dev);
	}
}

/**
 * stm32f4_i2c_handle_read() - Handle FIFO empty interrupt in case of read
 * @i2c_dev: Controller's private data
 */
static void stm32f4_i2c_handle_read(struct stm32f4_i2c_dev *i2c_dev)
{
	struct stm32f4_i2c_msg *msg = &i2c_dev->msg;
	void __iomem *reg = i2c_dev->base + STM32F4_I2C_CR2;

	switch (msg->count) {
	case 1:
		stm32f4_i2c_disable_irq(i2c_dev);
		stm32f4_i2c_read_msg(i2c_dev);
		complete(&i2c_dev->complete);
		break;
	/*
	 * For 2 or 3-byte reception, we do not have to read the data register
	 * when RXNE occurs as we have to wait for byte transferred finished
	 * event before reading data. So, here we just disable buffer
	 * interrupt in order to avoid another system preemption due to RXNE
	 * event
	 */
	case 2:
	case 3:
		stm32f4_i2c_clr_bits(reg, STM32F4_I2C_CR2_ITBUFEN);
		break;
	/* For N byte reception with N > 3 we directly read data register */
	default:
		stm32f4_i2c_read_msg(i2c_dev);
	}
}

/**
 * stm32f4_i2c_handle_rx_btf() - Handle byte transfer finished interrupt
 * in case of read
 * @i2c_dev: Controller's private data
 */
static void stm32f4_i2c_handle_rx_btf(struct stm32f4_i2c_dev *i2c_dev)
{
	struct stm32f4_i2c_msg *msg = &i2c_dev->msg;
	void __iomem *reg;
	u32 mask;
	int i;

	switch (msg->count) {
	case 2:
		/*
		 * In order to correctly send the Stop or Repeated Start
		 * condition on the I2C bus, the STOP/START bit has to be set
		 * before reading the last two bytes.
		 * After that, we could read the last two bytes, disable
		 * remaining interrupts and notify the end of xfer to the
		 * client
		 */
		reg = i2c_dev->base + STM32F4_I2C_CR1;
		if (msg->stop)
			stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_STOP);
		else
			stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_START);

		for (i = 2; i > 0; i--)
			stm32f4_i2c_read_msg(i2c_dev);

		reg = i2c_dev->base + STM32F4_I2C_CR2;
		mask = STM32F4_I2C_CR2_ITEVTEN | STM32F4_I2C_CR2_ITERREN;
		stm32f4_i2c_clr_bits(reg, mask);

		complete(&i2c_dev->complete);
		break;
	case 3:
		/*
		 * In order to correctly send the ACK on the I2C bus for the
		 * last two bytes, we have to set ACK bit before reading the
		 * third last data byte
		 */
		reg = i2c_dev->base + STM32F4_I2C_CR1;
		stm32f4_i2c_clr_bits(reg, STM32F4_I2C_CR1_ACK);
		stm32f4_i2c_read_msg(i2c_dev);
		break;
	default:
		stm32f4_i2c_read_msg(i2c_dev);
	}
}

/**
 * stm32f4_i2c_handle_rx_addr() - Handle address matched interrupt in case of
 * master receiver
 * @i2c_dev: Controller's private data
 */
static void stm32f4_i2c_handle_rx_addr(struct stm32f4_i2c_dev *i2c_dev)
{
	struct stm32f4_i2c_msg *msg = &i2c_dev->msg;
	void __iomem *reg;

	switch (msg->count) {
	case 0:
		stm32f4_i2c_terminate_xfer(i2c_dev);
		/* Clear ADDR flag */
		readl_relaxed(i2c_dev->base + STM32F4_I2C_SR2);
		break;
	case 1:
		/*
		 * Single byte reception:
		 * Enable NACK, clear ADDR flag and generate STOP or RepSTART
		 */
		reg = i2c_dev->base + STM32F4_I2C_CR1;
		stm32f4_i2c_clr_bits(reg, STM32F4_I2C_CR1_ACK);
		readl_relaxed(i2c_dev->base + STM32F4_I2C_SR2);
		if (msg->stop)
			stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_STOP);
		else
			stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_START);
		break;
	case 2:
		/*
		 * 2-byte reception:
		 * Enable NACK and set POS
		 */
		reg = i2c_dev->base + STM32F4_I2C_CR1;
		stm32f4_i2c_clr_bits(reg, STM32F4_I2C_CR1_ACK);
		stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_POS);
		readl_relaxed(i2c_dev->base + STM32F4_I2C_SR2);
		break;

	default:
		/* N-byte reception: Enable ACK */
		reg = i2c_dev->base + STM32F4_I2C_CR1;
		stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_ACK);
		readl_relaxed(i2c_dev->base + STM32F4_I2C_SR2);
		break;
	}
}

/**
 * stm32f4_i2c_isr_event() - Interrupt routine for I2C bus event
 * @irq: interrupt number
 * @data: Controller's private data
 */
static irqreturn_t stm32f4_i2c_isr_event(int irq, void *data)
{
	struct stm32f4_i2c_dev *i2c_dev = data;
	struct stm32f4_i2c_msg *msg = &i2c_dev->msg;
	u32 possible_status = STM32F4_I2C_SR1_ITEVTEN_MASK;
	void __iomem *reg;
	u32 status, ien, event;

	ien = readl_relaxed(i2c_dev->base + STM32F4_I2C_CR2);
	ien &= STM32F4_I2C_CR2_IRQ_MASK;

	/* Update possible_status if buffer interrupt is enabled */
	if (ien & STM32F4_I2C_CR2_ITBUFEN)
		possible_status |= STM32F4_I2C_SR1_ITBUFEN_MASK;

	status = readl_relaxed(i2c_dev->base + STM32F4_I2C_SR1);
	event = status & possible_status;
	if (!event) {
		dev_dbg(i2c_dev->dev,
			"spurious evt irq (status=0x%08x, ien=0x%08x)\n",
			status, ien);
		return IRQ_NONE;
	}

	if (event & STM32F4_I2C_SR1_SB)
		stm32f4_i2c_write_byte(i2c_dev, msg->addr);

	if (event & STM32F4_I2C_SR1_ADDR) {
		if (msg->addr & I2C_M_RD)
			stm32f4_i2c_handle_rx_addr(i2c_dev);
		else
			readl_relaxed(i2c_dev->base + STM32F4_I2C_SR2);

		/* Enable buffer interrupts for RXNE/TXE events */
		reg = i2c_dev->base + STM32F4_I2C_CR2;
		stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR2_ITBUFEN);
	}

	if ((event & STM32F4_I2C_SR1_TXE) && !(msg->addr & I2C_M_RD))
		stm32f4_i2c_handle_write(i2c_dev);

	if ((event & STM32F4_I2C_SR1_RXNE) && (msg->addr & I2C_M_RD))
		stm32f4_i2c_handle_read(i2c_dev);

	if (event & STM32F4_I2C_SR1_BTF) {
		if (msg->addr & I2C_M_RD)
			stm32f4_i2c_handle_rx_btf(i2c_dev);
		else
			stm32f4_i2c_handle_write(i2c_dev);
	}

	return IRQ_HANDLED;
}

/**
 * stm32f4_i2c_isr_error() - Interrupt routine for I2C bus error
 * @irq: interrupt number
 * @data: Controller's private data
 */
static irqreturn_t stm32f4_i2c_isr_error(int irq, void *data)
{
	struct stm32f4_i2c_dev *i2c_dev = data;
	struct stm32f4_i2c_msg *msg = &i2c_dev->msg;
	void __iomem *reg;
	u32 status;

	status = readl_relaxed(i2c_dev->base + STM32F4_I2C_SR1);

	/* Arbitration lost */
	if (status & STM32F4_I2C_SR1_ARLO) {
		reg = i2c_dev->base + STM32F4_I2C_SR1;
		stm32f4_i2c_clr_bits(reg, STM32F4_I2C_SR1_ARLO);
		msg->result = -EAGAIN;
	}

	/*
	 * Acknowledge failure:
	 * In master transmitter mode a Stop must be generated by software
	 */
	if (status & STM32F4_I2C_SR1_AF) {
		if (!(msg->addr & I2C_M_RD)) {
			reg = i2c_dev->base + STM32F4_I2C_CR1;
			stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_STOP);
		}
		reg = i2c_dev->base + STM32F4_I2C_SR1;
		stm32f4_i2c_clr_bits(reg, STM32F4_I2C_SR1_AF);
		msg->result = -EIO;
	}

	/* Bus error */
	if (status & STM32F4_I2C_SR1_BERR) {
		reg = i2c_dev->base + STM32F4_I2C_SR1;
		stm32f4_i2c_clr_bits(reg, STM32F4_I2C_SR1_BERR);
		msg->result = -EIO;
	}

	stm32f4_i2c_disable_irq(i2c_dev);
	complete(&i2c_dev->complete);

	return IRQ_HANDLED;
}

/**
 * stm32f4_i2c_xfer_msg() - Transfer a single I2C message
 * @i2c_dev: Controller's private data
 * @msg: I2C message to transfer
 * @is_first: first message of the sequence
 * @is_last: last message of the sequence
 */
static int stm32f4_i2c_xfer_msg(struct stm32f4_i2c_dev *i2c_dev,
				struct i2c_msg *msg, bool is_first,
				bool is_last)
{
	struct stm32f4_i2c_msg *f4_msg = &i2c_dev->msg;
	void __iomem *reg = i2c_dev->base + STM32F4_I2C_CR1;
	unsigned long timeout;
	u32 mask;
	int ret;

	f4_msg->addr = i2c_8bit_addr_from_msg(msg);
	f4_msg->buf = msg->buf;
	f4_msg->count = msg->len;
	f4_msg->result = 0;
	f4_msg->stop = is_last;

	reinit_completion(&i2c_dev->complete);

	/* Enable events and errors interrupts */
	mask = STM32F4_I2C_CR2_ITEVTEN | STM32F4_I2C_CR2_ITERREN;
	stm32f4_i2c_set_bits(i2c_dev->base + STM32F4_I2C_CR2, mask);

	if (is_first) {
		ret = stm32f4_i2c_wait_free_bus(i2c_dev);
		if (ret)
			return ret;

		/* START generation */
		stm32f4_i2c_set_bits(reg, STM32F4_I2C_CR1_START);
	}

	timeout = wait_for_completion_timeout(&i2c_dev->complete,
					      i2c_dev->adap.timeout);
	ret = f4_msg->result;

	/* Disable POS position Ack */
	stm32f4_i2c_clr_bits(reg, STM32F4_I2C_CR1_POS);

	if (!timeout)
		ret = -ETIMEDOUT;

	return ret;
}

/**
 * stm32f4_i2c_xfer() - Transfer combined I2C message
 * @i2c_adap: Adapter pointer to the controller
 * @msgs: Pointer to data to be written.
 * @num: Number of messages to be executed
 */
static int stm32f4_i2c_xfer(struct i2c_adapter *i2c_adap, struct i2c_msg msgs[],
			    int num)
{
	struct stm32f4_i2c_dev *i2c_dev = i2c_get_adapdata(i2c_adap);
	int ret, i;

	ret = clk_enable(i2c_dev->clk);
	if (ret) {
		dev_err(i2c_dev->dev, "Failed to enable clock\n");
		return ret;
	}

	for (i = 0; i < num && !ret; i++)
		ret = stm32f4_i2c_xfer_msg(i2c_dev, &msgs[i], i == 0,
					   i == num - 1);

	clk_disable(i2c_dev->clk);

	return (ret < 0) ? ret : num;
}

static u32 stm32f4_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static struct i2c_algorithm stm32f4_i2c_algo = {
	.master_xfer = stm32f4_i2c_xfer,
	.functionality = stm32f4_i2c_func,
};

static int stm32f4_i2c_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct stm32f4_i2c_dev *i2c_dev;
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
		dev_err(i2c_dev->dev, "Failed to prepare_enable clock\n");
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

	i2c_dev->speed = STM32F4_I2C_SPEED_STANDARD;
	ret = of_property_read_u32(np, "clock-frequency", &clk_rate);
	if (!ret && clk_rate >= 40000)
		i2c_dev->speed = STM32F4_I2C_SPEED_FAST;

	i2c_dev->dev = &pdev->dev;

	ret = devm_request_irq(&pdev->dev, irq_event, stm32f4_i2c_isr_event, 0,
			       pdev->name, i2c_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request irq event %i\n",
			irq_event);
		goto clk_free;
	}

	ret = devm_request_irq(&pdev->dev, irq_error, stm32f4_i2c_isr_error, 0,
			       pdev->name, i2c_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request irq error %i\n",
			irq_error);
		goto clk_free;
	}

	ret = stm32f4_i2c_hw_config(i2c_dev);
	if (ret)
		goto clk_free;

	adap = &i2c_dev->adap;
	i2c_set_adapdata(adap, i2c_dev);
	snprintf(adap->name, sizeof(adap->name), "STM32 I2C(%pa)", &res->start);
	adap->owner = THIS_MODULE;
	adap->timeout = 2 * HZ;
	adap->retries = 0;
	adap->algo = &stm32f4_i2c_algo;
	adap->dev.parent = &pdev->dev;
	adap->dev.of_node = pdev->dev.of_node;

	init_completion(&i2c_dev->complete);

	ret = i2c_add_adapter(adap);
	if (ret)
		goto clk_free;

	platform_set_drvdata(pdev, i2c_dev);

	clk_disable(i2c_dev->clk);

	dev_info(i2c_dev->dev, "STM32F4 I2C driver registered\n");

	return 0;

clk_free:
	clk_disable_unprepare(i2c_dev->clk);
	return ret;
}

static int stm32f4_i2c_remove(struct platform_device *pdev)
{
	struct stm32f4_i2c_dev *i2c_dev = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c_dev->adap);

	clk_unprepare(i2c_dev->clk);

	return 0;
}

static const struct of_device_id stm32f4_i2c_match[] = {
	{ .compatible = "st,stm32f4-i2c", },
	{},
};
MODULE_DEVICE_TABLE(of, stm32f4_i2c_match);

static struct platform_driver stm32f4_i2c_driver = {
	.driver = {
		.name = "stm32f4-i2c",
		.of_match_table = stm32f4_i2c_match,
	},
	.probe = stm32f4_i2c_probe,
	.remove = stm32f4_i2c_remove,
};

module_platform_driver(stm32f4_i2c_driver);

MODULE_AUTHOR("M'boumba Cedric Madianga <cedric.madianga@gmail.com>");
MODULE_DESCRIPTION("STMicroelectronics STM32F4 I2C driver");
MODULE_LICENSE("GPL v2");
