/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2012 FUJITSU SEMICONDUCTOR LIMITED
 */

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define WAIT_PCLK(n, rate)	ndelay((((1000000000 +  (rate) - 1) / \
					 (rate) + n - 1) / n) + 10)
#define SYNQUACER_I2C_TIMEOUT(x) (msecs_to_jiffies(x))

/* I2C register adress definitions */
#define SYNQUACER_I2C_REG_BSR		(0x00 << 2) // Bus Status
#define SYNQUACER_I2C_REG_BCR		(0x01 << 2) // Bus Control
#define SYNQUACER_I2C_REG_CCR		(0x02 << 2) // Clock Control
#define SYNQUACER_I2C_REG_ADR		(0x03 << 2) // Address
#define SYNQUACER_I2C_REG_DAR		(0x04 << 2) // Data
#define SYNQUACER_I2C_REG_CSR		(0x05 << 2) // Expansion CS
#define SYNQUACER_I2C_REG_FSR		(0x06 << 2) // Bus Clock Freq
#define SYNQUACER_I2C_REG_BC2R		(0x07 << 2) // Bus Control 2

/* I2C register bit definitions */
#define SYNQUACER_I2C_BSR_FBT		BIT(0)	// First Byte Transfer
#define SYNQUACER_I2C_BSR_GCA		BIT(1)	// General Call Address
#define SYNQUACER_I2C_BSR_AAS		BIT(2)	// Address as Slave
#define SYNQUACER_I2C_BSR_TRX		BIT(3)	// Transfer/Receive
#define SYNQUACER_I2C_BSR_LRB		BIT(4)	// Last Received Bit
#define SYNQUACER_I2C_BSR_AL		BIT(5)	// Arbitration Lost
#define SYNQUACER_I2C_BSR_RSC		BIT(6)	// Repeated Start Cond.
#define SYNQUACER_I2C_BSR_BB		BIT(7)	// Bus Busy

#define SYNQUACER_I2C_BCR_INT		BIT(0)	// Interrupt
#define SYNQUACER_I2C_BCR_INTE		BIT(1)	// Interrupt Enable
#define SYNQUACER_I2C_BCR_GCAA		BIT(2)	// Gen. Call Access Ack.
#define SYNQUACER_I2C_BCR_ACK		BIT(3)	// Acknowledge
#define SYNQUACER_I2C_BCR_MSS		BIT(4)	// Master Slave Select
#define SYNQUACER_I2C_BCR_SCC		BIT(5)	// Start Condition Cont.
#define SYNQUACER_I2C_BCR_BEIE		BIT(6)	// Bus Error Int Enable
#define SYNQUACER_I2C_BCR_BER		BIT(7)	// Bus Error

#define SYNQUACER_I2C_CCR_CS_MASK	(0x1f)	// CCR Clock Period Sel.
#define SYNQUACER_I2C_CCR_EN		BIT(5)	// Enable
#define SYNQUACER_I2C_CCR_FM		BIT(6)	// Speed Mode Select

#define SYNQUACER_I2C_CSR_CS_MASK	(0x3f)	// CSR Clock Period Sel.

#define SYNQUACER_I2C_BC2R_SCLL		BIT(0)	// SCL Low Drive
#define SYNQUACER_I2C_BC2R_SDAL		BIT(1)	// SDA Low Drive
#define SYNQUACER_I2C_BC2R_SCLS		BIT(4)	// SCL Status
#define SYNQUACER_I2C_BC2R_SDAS		BIT(5)	// SDA Status

/* PCLK frequency */
#define SYNQUACER_I2C_BUS_CLK_FR(rate)	(((rate) / 20000000) + 1)

/* STANDARD MODE frequency */
#define SYNQUACER_I2C_CLK_MASTER_STD(rate)			\
	DIV_ROUND_UP(DIV_ROUND_UP((rate), 100000) - 2, 2)
/* FAST MODE frequency */
#define SYNQUACER_I2C_CLK_MASTER_FAST(rate)			\
	DIV_ROUND_UP((DIV_ROUND_UP((rate), 400000) - 2) * 2, 3)

/* (clkrate <= 18000000) */
/* calculate the value of CS bits in CCR register on standard mode */
#define SYNQUACER_I2C_CCR_CS_STD_MAX_18M(rate)			\
	   ((SYNQUACER_I2C_CLK_MASTER_STD(rate) - 65)		\
					& SYNQUACER_I2C_CCR_CS_MASK)

/* calculate the value of CS bits in CSR register on standard mode */
#define SYNQUACER_I2C_CSR_CS_STD_MAX_18M(rate)		0x00

/* calculate the value of CS bits in CCR register on fast mode */
#define SYNQUACER_I2C_CCR_CS_FAST_MAX_18M(rate)			\
	   ((SYNQUACER_I2C_CLK_MASTER_FAST(rate) - 1)		\
					& SYNQUACER_I2C_CCR_CS_MASK)

/* calculate the value of CS bits in CSR register on fast mode */
#define SYNQUACER_I2C_CSR_CS_FAST_MAX_18M(rate)		0x00

/* (clkrate > 18000000) */
/* calculate the value of CS bits in CCR register on standard mode */
#define SYNQUACER_I2C_CCR_CS_STD_MIN_18M(rate)			\
	   ((SYNQUACER_I2C_CLK_MASTER_STD(rate) - 1)		\
					& SYNQUACER_I2C_CCR_CS_MASK)

/* calculate the value of CS bits in CSR register on standard mode */
#define SYNQUACER_I2C_CSR_CS_STD_MIN_18M(rate)			\
	   (((SYNQUACER_I2C_CLK_MASTER_STD(rate) - 1) >> 5)	\
					& SYNQUACER_I2C_CSR_CS_MASK)

/* calculate the value of CS bits in CCR register on fast mode */
#define SYNQUACER_I2C_CCR_CS_FAST_MIN_18M(rate)			\
	   ((SYNQUACER_I2C_CLK_MASTER_FAST(rate) - 1)		\
					& SYNQUACER_I2C_CCR_CS_MASK)

/* calculate the value of CS bits in CSR register on fast mode */
#define SYNQUACER_I2C_CSR_CS_FAST_MIN_18M(rate)			\
	   (((SYNQUACER_I2C_CLK_MASTER_FAST(rate) - 1) >> 5)	\
					& SYNQUACER_I2C_CSR_CS_MASK)

/* min I2C clock frequency 14M */
#define SYNQUACER_I2C_MIN_CLK_RATE	(14 * 1000000)
/* max I2C clock frequency 200M */
#define SYNQUACER_I2C_MAX_CLK_RATE	(200 * 1000000)
/* I2C clock frequency 18M */
#define SYNQUACER_I2C_CLK_RATE_18M	(18 * 1000000)

#define SYNQUACER_I2C_SPEED_FM		400	// Fast Mode
#define SYNQUACER_I2C_SPEED_SM		100	// Standard Mode

enum i2c_state {
	STATE_IDLE,
	STATE_START,
	STATE_READ,
	STATE_WRITE
};

struct synquacer_i2c {
	struct completion	completion;

	struct i2c_msg		*msg;
	u32			msg_num;
	u32			msg_idx;
	u32			msg_ptr;

	u32			irq;
	struct device		*dev;
	void __iomem		*base;
	struct clk		*clk;
	u32			clkrate;
	u32			speed_khz;
	u32			timeout_ms;
	enum i2c_state		state;
	struct i2c_adapter	adapter;

	bool			is_suspended;
};

static inline int is_lastmsg(struct synquacer_i2c *i2c)
{
	return i2c->msg_idx >= (i2c->msg_num - 1);
}

static inline int is_msglast(struct synquacer_i2c *i2c)
{
	return i2c->msg_ptr == (i2c->msg->len - 1);
}

static inline int is_msgend(struct synquacer_i2c *i2c)
{
	return i2c->msg_ptr >= i2c->msg->len;
}

static inline unsigned long calc_timeout_ms(struct synquacer_i2c *i2c,
					    struct i2c_msg *msgs,
					    int num)
{
	unsigned long bit_count = 0;
	int i;

	for (i = 0; i < num; i++, msgs++)
		bit_count += msgs->len;

	return DIV_ROUND_UP(((bit_count * 9) + (10 * num)) * 3, 200) + 10;
}

static void synquacer_i2c_stop(struct synquacer_i2c *i2c, int ret)
{
	dev_dbg(i2c->dev, "STOP\n");

	/*
	 * clear IRQ (INT=0, BER=0)
	 * set Stop Condition (MSS=0)
	 * Interrupt Disable
	 */
	writeb(0, i2c->base + SYNQUACER_I2C_REG_BCR);

	i2c->state = STATE_IDLE;

	i2c->msg_ptr = 0;
	i2c->msg = NULL;
	i2c->msg_idx++;
	i2c->msg_num = 0;
	if (ret)
		i2c->msg_idx = ret;

	complete(&i2c->completion);
}

static void synquacer_i2c_hw_init(struct synquacer_i2c *i2c)
{
	unsigned char ccr_cs, csr_cs;
	u32 rt = i2c->clkrate;

	/* Set own Address */
	writeb(0, i2c->base + SYNQUACER_I2C_REG_ADR);

	/* Set PCLK frequency */
	writeb(SYNQUACER_I2C_BUS_CLK_FR(i2c->clkrate),
	       i2c->base + SYNQUACER_I2C_REG_FSR);

	switch (i2c->speed_khz) {
	case SYNQUACER_I2C_SPEED_FM:
		if (i2c->clkrate <= SYNQUACER_I2C_CLK_RATE_18M) {
			ccr_cs = SYNQUACER_I2C_CCR_CS_FAST_MAX_18M(rt);
			csr_cs = SYNQUACER_I2C_CSR_CS_FAST_MAX_18M(rt);
		} else {
			ccr_cs = SYNQUACER_I2C_CCR_CS_FAST_MIN_18M(rt);
			csr_cs = SYNQUACER_I2C_CSR_CS_FAST_MIN_18M(rt);
		}

		/* Set Clock and enable, Set fast mode*/
		writeb(ccr_cs | SYNQUACER_I2C_CCR_FM |
		       SYNQUACER_I2C_CCR_EN,
		       i2c->base + SYNQUACER_I2C_REG_CCR);
		writeb(csr_cs, i2c->base + SYNQUACER_I2C_REG_CSR);
		break;
	case SYNQUACER_I2C_SPEED_SM:
		if (i2c->clkrate <= SYNQUACER_I2C_CLK_RATE_18M) {
			ccr_cs = SYNQUACER_I2C_CCR_CS_STD_MAX_18M(rt);
			csr_cs = SYNQUACER_I2C_CSR_CS_STD_MAX_18M(rt);
		} else {
			ccr_cs = SYNQUACER_I2C_CCR_CS_STD_MIN_18M(rt);
			csr_cs = SYNQUACER_I2C_CSR_CS_STD_MIN_18M(rt);
		}

		/* Set Clock and enable, Set standard mode */
		writeb(ccr_cs | SYNQUACER_I2C_CCR_EN,
		      i2c->base + SYNQUACER_I2C_REG_CCR);
		writeb(csr_cs, i2c->base + SYNQUACER_I2C_REG_CSR);
		break;
	default:
		BUG();
	}

	/* clear IRQ (INT=0, BER=0), Interrupt Disable */
	writeb(0, i2c->base + SYNQUACER_I2C_REG_BCR);
	writeb(0, i2c->base + SYNQUACER_I2C_REG_BC2R);
}

static void synquacer_i2c_hw_reset(struct synquacer_i2c *i2c)
{
	/* Disable clock */
	writeb(0, i2c->base + SYNQUACER_I2C_REG_CCR);
	writeb(0, i2c->base + SYNQUACER_I2C_REG_CSR);

	WAIT_PCLK(100, i2c->clkrate);

	synquacer_i2c_hw_init(i2c);
}

static int synquacer_i2c_master_start(struct synquacer_i2c *i2c,
				      struct i2c_msg *pmsg)
{
	unsigned char bsr, bcr;

	if (pmsg->flags & I2C_M_RD)
		writeb((pmsg->addr << 1) | 1,
		       i2c->base + SYNQUACER_I2C_REG_DAR);
	else
		writeb(pmsg->addr << 1,
		       i2c->base + SYNQUACER_I2C_REG_DAR);

	dev_dbg(i2c->dev, "%s slave:0x%02x\n", __func__, pmsg->addr);

	/* Generate Start Condition */
	bsr = readb(i2c->base + SYNQUACER_I2C_REG_BSR);
	bcr = readb(i2c->base + SYNQUACER_I2C_REG_BCR);
	dev_dbg(i2c->dev, "%s bsr:0x%08x, bcr:0x%08x\n", __func__, bsr,
		bcr);

	if ((bsr & SYNQUACER_I2C_BSR_BB) &&
	    !(bcr & SYNQUACER_I2C_BCR_MSS)) {
		dev_dbg(i2c->dev, "%s bus is busy", __func__);
		return -EBUSY;
	}

	if (bsr & SYNQUACER_I2C_BSR_BB) { /* Bus is busy */
		dev_dbg(i2c->dev, "%s Continuous Start", __func__);
		writeb(bcr | SYNQUACER_I2C_BCR_SCC,
		       i2c->base + SYNQUACER_I2C_REG_BCR);
	} else {
		if (bcr & SYNQUACER_I2C_BCR_MSS) {
			dev_dbg(i2c->dev, "%s is not in master mode",
				__func__);
			return -EAGAIN;
		}
		dev_dbg(i2c->dev, "%s Start Condition", __func__);
		/* Start Condition + Enable Interrupts */
		writeb(bcr | SYNQUACER_I2C_BCR_MSS |
		       SYNQUACER_I2C_BCR_INTE | SYNQUACER_I2C_BCR_BEIE,
		       i2c->base + SYNQUACER_I2C_REG_BCR);
	}

	WAIT_PCLK(10, i2c->clkrate);

	/* get bsr&bcr register */
	bsr = readb(i2c->base + SYNQUACER_I2C_REG_BSR);
	bcr = readb(i2c->base + SYNQUACER_I2C_REG_BCR);
	dev_dbg(i2c->dev, "%s bsr:0x%08x, bcr:0x%08x\n", __func__, bsr,
		bcr);

	if ((bsr & SYNQUACER_I2C_BSR_AL) ||
	    !(bcr & SYNQUACER_I2C_BCR_MSS)) {
		dev_dbg(i2c->dev, "%s arbitration lost\n", __func__);
		return -EAGAIN;
	}

	return 0;
}

static int synquacer_i2c_master_recover(struct synquacer_i2c *i2c)
{
	unsigned int count = 0;
	unsigned char bc2r;

	/* Disable interrupts */
	writeb(0, i2c->base + SYNQUACER_I2C_REG_BCR);

	/* monitor SDA, SCL */
	bc2r = readb(i2c->base + SYNQUACER_I2C_REG_BC2R);
	dev_dbg(i2c->dev, "%s bc2r:0x%02x\n", __func__, bc2r);

	while (count <= 100) {
		WAIT_PCLK(20, i2c->clkrate);
		bc2r = readb(i2c->base + SYNQUACER_I2C_REG_BC2R);

		/* another master is running */
		if ((bc2r & SYNQUACER_I2C_BC2R_SDAS) ||
		    !(bc2r & SYNQUACER_I2C_BC2R_SCLS)) {
			dev_dbg(i2c->dev, "%s: another master is running?\n",
				__func__);
			return -EAGAIN;
		}
		count++;
	}

	/* Force to make one clock pulse */
	count = 0;
	for (;;) {
		/* SCL = L->H */
		writeb(SYNQUACER_I2C_BC2R_SCLL,
		       i2c->base + SYNQUACER_I2C_REG_BC2R);
		WAIT_PCLK(20, i2c->clkrate);
		writeb(0, i2c->base + SYNQUACER_I2C_REG_BC2R);

		WAIT_PCLK(10, i2c->clkrate);

		bc2r = readb(i2c->base + SYNQUACER_I2C_REG_BC2R);

		WAIT_PCLK(5, i2c->clkrate);

		if (bc2r & SYNQUACER_I2C_BC2R_SDAS)
			break;
		WAIT_PCLK(10, i2c->clkrate);
		if (++count > 9) {
			dev_err(i2c->dev, "%s: count: %i, bc2r: 0x%x\n",
				__func__, count, bc2r);
			return -EIO;
		}
	}

	/* force to make bus-error phase */
	/* SDA = L */
	writeb(SYNQUACER_I2C_BC2R_SDAL,
	       i2c->base + SYNQUACER_I2C_REG_BC2R);
	WAIT_PCLK(10, i2c->clkrate);
	/* SDA = H */
	writeb(0, i2c->base + SYNQUACER_I2C_REG_BC2R);
	WAIT_PCLK(10, i2c->clkrate);

	/* Both SDA & SDL should be H */
	bc2r = readb(i2c->base + SYNQUACER_I2C_REG_BC2R);
	if ((bc2r & (SYNQUACER_I2C_BC2R_SDAS | SYNQUACER_I2C_BC2R_SCLS))
	    != (SYNQUACER_I2C_BC2R_SDAS | SYNQUACER_I2C_BC2R_SCLS)) {
		dev_err(i2c->dev, "%s: bc2r: 0x%x\n", __func__, bc2r);
		return -EIO;
	}

	return 0;
}

static int synquacer_i2c_doxfer(struct synquacer_i2c *i2c,
				struct i2c_msg *msgs, int num)
{
	unsigned char bsr;
	unsigned long timeout, bb_timeout;
	int ret = 0;

	if (i2c->is_suspended)
		return -EBUSY;

	synquacer_i2c_hw_init(i2c);
	bsr = readb(i2c->base + SYNQUACER_I2C_REG_BSR);
	if (bsr & SYNQUACER_I2C_BSR_BB) {
		dev_err(i2c->dev, "cannot get bus (bus busy)\n");
		return -EBUSY;
	}

	init_completion(&i2c->completion);

	i2c->msg = msgs;
	i2c->msg_num = num;
	i2c->msg_ptr = 0;
	i2c->msg_idx = 0;
	i2c->state = STATE_START;

	ret = synquacer_i2c_master_start(i2c, i2c->msg);
	if (ret < 0) {
		dev_dbg(i2c->dev, "Address failed: (0x%08x)\n", ret);
		goto out;
	}

	timeout = wait_for_completion_timeout(&i2c->completion,
				SYNQUACER_I2C_TIMEOUT(i2c->timeout_ms));
	if (timeout <= 0) {
		dev_dbg(i2c->dev, "timeout\n");
		ret = -EAGAIN;
		goto out;
	}

	ret = i2c->msg_idx;
	if (ret != num) {
		dev_dbg(i2c->dev, "incomplete xfer (%d)\n", ret);
		ret = -EAGAIN;
		goto out;
	}

	/* ensure the stop has been through the bus */
	bb_timeout = jiffies + HZ;
	do {
		bsr = readb(i2c->base + SYNQUACER_I2C_REG_BSR);
	} while ((bsr & SYNQUACER_I2C_BSR_BB) &&
		 time_before(jiffies, bb_timeout));
out:
	return ret;
}

static irqreturn_t synquacer_i2c_isr(int irq, void *dev_id)
{
	struct synquacer_i2c *i2c = dev_id;

	unsigned char byte;
	unsigned char bsr, bcr;
	int ret = 0;

	bcr = readb(i2c->base + SYNQUACER_I2C_REG_BCR);
	bsr = readb(i2c->base + SYNQUACER_I2C_REG_BSR);
	dev_dbg(i2c->dev, "%s bsr:0x%02x, bcr:0x%02x\n", __func__,
		bsr, bcr);

	if (bcr & SYNQUACER_I2C_BCR_BER) {
		dev_err(i2c->dev, "%s: bus error\n", __func__);
		synquacer_i2c_stop(i2c, -EAGAIN);
		goto out;
	}
	if ((bsr & SYNQUACER_I2C_BSR_AL) ||
	    !(bcr & SYNQUACER_I2C_BCR_MSS)) {
		dev_dbg(i2c->dev, "%s arbitration lost\n", __func__);
		synquacer_i2c_stop(i2c, -EAGAIN);
		goto out;
	}

	switch (i2c->state) {

	case STATE_START:
		if (bsr & SYNQUACER_I2C_BSR_LRB) {
			dev_dbg(i2c->dev, "ack was not received\n");
			synquacer_i2c_stop(i2c, -EAGAIN);
			goto out;
		}

		if (i2c->msg->flags & I2C_M_RD)
			i2c->state = STATE_READ;
		else
			i2c->state = STATE_WRITE;

		if (is_lastmsg(i2c) && i2c->msg->len == 0) {
			synquacer_i2c_stop(i2c, 0);
			goto out;
		}

		if (i2c->state == STATE_READ)
			goto prepare_read;

		/* fallthru */

	case STATE_WRITE:
		if (bsr & SYNQUACER_I2C_BSR_LRB) {
			dev_dbg(i2c->dev, "WRITE: No Ack\n");
			synquacer_i2c_stop(i2c, -EAGAIN);
			goto out;
		}

		if (!is_msgend(i2c)) {
			writeb(i2c->msg->buf[i2c->msg_ptr++],
			       i2c->base + SYNQUACER_I2C_REG_DAR);

			/* clear IRQ, and continue */
			writeb(SYNQUACER_I2C_BCR_BEIE |
			       SYNQUACER_I2C_BCR_MSS |
			       SYNQUACER_I2C_BCR_INTE,
			       i2c->base + SYNQUACER_I2C_REG_BCR);
			break;
		}
		if (is_lastmsg(i2c)) {
			synquacer_i2c_stop(i2c, 0);
			break;
		}
		dev_dbg(i2c->dev, "WRITE: Next Message\n");

		i2c->msg_ptr = 0;
		i2c->msg_idx++;
		i2c->msg++;

		/* send the new start */
		ret = synquacer_i2c_master_start(i2c, i2c->msg);
		if (ret < 0) {
			dev_dbg(i2c->dev, "restart err:0x%08x\n", ret);
			synquacer_i2c_stop(i2c, -EAGAIN);
			break;
		}
		i2c->state = STATE_START;
		break;

	case STATE_READ:
		if (!(bsr & SYNQUACER_I2C_BSR_FBT)) { /* data */
			byte = readb(i2c->base + SYNQUACER_I2C_REG_DAR);
			i2c->msg->buf[i2c->msg_ptr++] = byte;
		} else /* address */
			dev_dbg(i2c->dev, ", address:0x%08x. ignore it.\n",
				readb(i2c->base + SYNQUACER_I2C_REG_DAR));

prepare_read:
		if (is_msglast(i2c)) {
			writeb(SYNQUACER_I2C_BCR_MSS |
			       SYNQUACER_I2C_BCR_BEIE |
			       SYNQUACER_I2C_BCR_INTE,
			       i2c->base + SYNQUACER_I2C_REG_BCR);
			break;
		}
		if (!is_msgend(i2c)) {
			writeb(SYNQUACER_I2C_BCR_MSS |
			       SYNQUACER_I2C_BCR_BEIE |
			       SYNQUACER_I2C_BCR_INTE |
			       SYNQUACER_I2C_BCR_ACK,
			       i2c->base + SYNQUACER_I2C_REG_BCR);
			break;
		}
		if (is_lastmsg(i2c)) {
			/* last message, send stop and complete */
			dev_dbg(i2c->dev, "READ: Send Stop\n");
			synquacer_i2c_stop(i2c, 0);
			break;
		}
		dev_dbg(i2c->dev, "READ: Next Transfer\n");

		i2c->msg_ptr = 0;
		i2c->msg_idx++;
		i2c->msg++;

		ret = synquacer_i2c_master_start(i2c, i2c->msg);
		if (ret < 0) {
			dev_dbg(i2c->dev, "restart err: 0x%08x\n", ret);
			synquacer_i2c_stop(i2c, -EAGAIN);
		} else
			i2c->state = STATE_START;
		break;
	default:
		dev_err(i2c->dev, "%s: called in err STATE (%d)\n",
			 __func__, i2c->state);
		break;
	}

out:
	WAIT_PCLK(10, i2c->clkrate);
	return IRQ_HANDLED;
}

static int synquacer_i2c_xfer(struct i2c_adapter *adap,
			      struct i2c_msg *msgs, int num)
{
	struct synquacer_i2c *i2c;
	int retry;
	int ret = 0;

	if (!msgs)
		return -EINVAL;
	if (num <= 0)
		return -EINVAL;

	i2c = i2c_get_adapdata(adap);
	i2c->timeout_ms = calc_timeout_ms(i2c, msgs, num);

	dev_dbg(i2c->dev, "calculated timeout %d ms\n",
		i2c->timeout_ms);

	for (retry = 0; retry < adap->retries; retry++) {

		ret = synquacer_i2c_doxfer(i2c, msgs, num);
		if (ret != -EAGAIN)
			return ret;

		dev_dbg(i2c->dev, "Retrying transmission (%d)\n",
			retry);

		synquacer_i2c_master_recover(i2c);
		synquacer_i2c_hw_reset(i2c);
	}
	return -EIO;
}

static u32 synquacer_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm synquacer_i2c_algo = {
	.master_xfer   = synquacer_i2c_xfer,
	.functionality = synquacer_i2c_functionality,
};

static struct i2c_adapter synquacer_i2c_ops = {
	.owner		= THIS_MODULE,
	.name		= "synquacer_i2c-adapter",
	.algo		= &synquacer_i2c_algo,
	.retries	= 5,
};

static int synquacer_i2c_probe(struct platform_device *pdev)
{
	struct synquacer_i2c *i2c;
	struct resource *r;
	int speed_khz;
	int ret;

	ret = device_property_read_u32(&pdev->dev, "clock-frequency",
				       &speed_khz);
	if (ret) {
		dev_err(&pdev->dev,
			"Missing clock-frequency property\n");
		return -EINVAL;
	}
	speed_khz /= 1000;

	i2c = devm_kzalloc(&pdev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	if (dev_of_node(&pdev->dev)) {
		i2c->clk = devm_clk_get(&pdev->dev, "pclk");
		if (IS_ERR(i2c->clk)) {
			dev_err(&pdev->dev, "cannot get clock\n");
			return PTR_ERR(i2c->clk);
		}
		dev_dbg(&pdev->dev, "clock source %p\n", i2c->clk);

		i2c->clkrate = clk_get_rate(i2c->clk);
		dev_dbg(&pdev->dev, "clock rate %d\n", i2c->clkrate);
		clk_prepare_enable(i2c->clk);
	} else {
		ret = device_property_read_u32(&pdev->dev,
					       "socionext,pclk-rate",
					       &i2c->clkrate);
		if (ret)
			return ret;
	}

	if (i2c->clkrate < SYNQUACER_I2C_MIN_CLK_RATE ||
	    i2c->clkrate > SYNQUACER_I2C_MAX_CLK_RATE) {
		dev_err(&pdev->dev, "PCLK rate out of range (%d)\n",
			i2c->clkrate);
		return -EINVAL;
	}

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	i2c->base = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	dev_dbg(&pdev->dev, "registers %p (%p)\n", i2c->base, r);

	i2c->irq = platform_get_irq(pdev, 0);
	if (i2c->irq <= 0) {
		dev_err(&pdev->dev, "no IRQ resource found\n");
		return -ENODEV;
	}

	ret = devm_request_irq(&pdev->dev, i2c->irq, synquacer_i2c_isr,
			       0, dev_name(&pdev->dev), i2c);
	if (ret < 0) {
		dev_err(&pdev->dev, "cannot claim IRQ %d\n", i2c->irq);
		return ret;
	}

	i2c->state = STATE_IDLE;
	i2c->dev = &pdev->dev;
	i2c->msg = NULL;
	i2c->speed_khz = SYNQUACER_I2C_SPEED_SM;
	if (speed_khz == SYNQUACER_I2C_SPEED_FM)
		i2c->speed_khz = SYNQUACER_I2C_SPEED_FM;

	synquacer_i2c_hw_init(i2c);

	i2c->adapter = synquacer_i2c_ops;
	i2c_set_adapdata(&i2c->adapter, i2c);
	i2c->adapter.dev.parent = &pdev->dev;
	i2c->adapter.nr = pdev->id;

	ret = i2c_add_numbered_adapter(&i2c->adapter);
	if (ret) {
		dev_err(&pdev->dev, "failed to add bus to i2c core\n");
		return ret;
	}

	platform_set_drvdata(pdev, i2c);

	dev_info(&pdev->dev, "%s: synquacer_i2c adapter\n",
				dev_name(&i2c->adapter.dev));

	return 0;
}

static int synquacer_i2c_remove(struct platform_device *pdev)
{
	struct synquacer_i2c *i2c = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	i2c_del_adapter(&i2c->adapter);
	clk_disable_unprepare(i2c->clk);

	return 0;
};

#ifdef CONFIG_PM_SLEEP
static int synquacer_i2c_suspend(struct device *dev)
{
	struct synquacer_i2c *i2c = dev_get_drvdata(dev);

	i2c_lock_adapter(&i2c->adapter);
	i2c->is_suspended = true;
	i2c_unlock_adapter(&i2c->adapter);

	clk_disable_unprepare(i2c->clk);

	return 0;
}

static int synquacer_i2c_resume(struct device *dev)
{
	struct synquacer_i2c *i2c = dev_get_drvdata(dev);
	int ret;

	i2c_lock_adapter(&i2c->adapter);

	ret = clk_prepare_enable(i2c->clk);

	if (!ret)
		i2c->is_suspended = false;

	i2c_unlock_adapter(&i2c->adapter);

	return ret;
}

static SIMPLE_DEV_PM_OPS(synquacer_i2c_pm, synquacer_i2c_suspend,
			 synquacer_i2c_resume);
#define SYNQUACER_I2C_PM	(&synquacer_i2c_pm)
#else
#define SYNQUACER_I2C_PM	NULL
#endif

#ifdef CONFIG_OF
static const struct of_device_id synquacer_i2c_dt_ids[] = {
	{ .compatible = "socionext,synquacer-i2c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, synquacer_i2c_dt_ids);
#endif

#ifdef CONFIG_ACPI
static const struct acpi_device_id synquacer_i2c_acpi_ids[] = {
	{ "SCX0003" },
	{ /* sentinel */ }
};
#endif

static struct platform_driver synquacer_i2c_driver = {
	.probe	= synquacer_i2c_probe,
	.remove	= synquacer_i2c_remove,
	.driver	= {
		.owner = THIS_MODULE,
		.name = "synquacer_i2c",
		.of_match_table = of_match_ptr(synquacer_i2c_dt_ids),
		.acpi_match_table = ACPI_PTR(synquacer_i2c_acpi_ids),
		.pm = SYNQUACER_I2C_PM,
	},
};
module_platform_driver(synquacer_i2c_driver);

MODULE_AUTHOR("Fujitsu Semiconductor Ltd");
MODULE_DESCRIPTION("Socionext SynQuacer I2C Driver");
MODULE_LICENSE("GPL v2");
