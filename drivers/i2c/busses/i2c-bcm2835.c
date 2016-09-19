/*
 * BCM2835 master mode driver
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define BCM2835_I2C_C		0x0
#define BCM2835_I2C_S		0x4
#define BCM2835_I2C_DLEN	0x8
#define BCM2835_I2C_A		0xc
#define BCM2835_I2C_FIFO	0x10
#define BCM2835_I2C_DIV		0x14
#define BCM2835_I2C_DEL		0x18
#define BCM2835_I2C_CLKT	0x1c

#define BCM2835_I2C_C_READ	BIT(0)
#define BCM2835_I2C_C_CLEAR	BIT(4) /* bits 4 and 5 both clear */
#define BCM2835_I2C_C_ST	BIT(7)
#define BCM2835_I2C_C_INTD	BIT(8)
#define BCM2835_I2C_C_INTT	BIT(9)
#define BCM2835_I2C_C_INTR	BIT(10)
#define BCM2835_I2C_C_I2CEN	BIT(15)

#define BCM2835_I2C_S_TA	BIT(0)
#define BCM2835_I2C_S_DONE	BIT(1)
#define BCM2835_I2C_S_TXW	BIT(2)
#define BCM2835_I2C_S_RXR	BIT(3)
#define BCM2835_I2C_S_TXD	BIT(4)
#define BCM2835_I2C_S_RXD	BIT(5)
#define BCM2835_I2C_S_TXE	BIT(6)
#define BCM2835_I2C_S_RXF	BIT(7)
#define BCM2835_I2C_S_ERR	BIT(8)
#define BCM2835_I2C_S_CLKT	BIT(9)
#define BCM2835_I2C_S_LEN	BIT(10) /* Fake bit for SW error reporting */

#define BCM2835_I2C_BITMSK_S	0x03FF

#define BCM2835_I2C_CDIV_MIN	0x0002
#define BCM2835_I2C_CDIV_MAX	0xFFFE

#define BCM2835_I2C_TIMEOUT (msecs_to_jiffies(1000))

struct bcm2835_i2c_dev {
	struct device *dev;
	void __iomem *regs;
	struct clk *clk;
	int irq;
	struct i2c_adapter adapter;
	struct completion completion;
	struct i2c_msg *curr_msg;
	u32 msg_err;
	u8 *msg_buf;
	size_t msg_buf_remaining;
};

static inline void bcm2835_i2c_writel(struct bcm2835_i2c_dev *i2c_dev,
				      u32 reg, u32 val)
{
	writel(val, i2c_dev->regs + reg);
}

static inline u32 bcm2835_i2c_readl(struct bcm2835_i2c_dev *i2c_dev, u32 reg)
{
	return readl(i2c_dev->regs + reg);
}

static void bcm2835_fill_txfifo(struct bcm2835_i2c_dev *i2c_dev)
{
	u32 val;

	while (i2c_dev->msg_buf_remaining) {
		val = bcm2835_i2c_readl(i2c_dev, BCM2835_I2C_S);
		if (!(val & BCM2835_I2C_S_TXD))
			break;
		bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_FIFO,
				   *i2c_dev->msg_buf);
		i2c_dev->msg_buf++;
		i2c_dev->msg_buf_remaining--;
	}
}

static void bcm2835_drain_rxfifo(struct bcm2835_i2c_dev *i2c_dev)
{
	u32 val;

	while (i2c_dev->msg_buf_remaining) {
		val = bcm2835_i2c_readl(i2c_dev, BCM2835_I2C_S);
		if (!(val & BCM2835_I2C_S_RXD))
			break;
		*i2c_dev->msg_buf = bcm2835_i2c_readl(i2c_dev,
						      BCM2835_I2C_FIFO);
		i2c_dev->msg_buf++;
		i2c_dev->msg_buf_remaining--;
	}
}

static irqreturn_t bcm2835_i2c_isr(int this_irq, void *data)
{
	struct bcm2835_i2c_dev *i2c_dev = data;
	u32 val, err;

	val = bcm2835_i2c_readl(i2c_dev, BCM2835_I2C_S);
	val &= BCM2835_I2C_BITMSK_S;
	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_S, val);

	err = val & (BCM2835_I2C_S_CLKT | BCM2835_I2C_S_ERR);
	if (err) {
		i2c_dev->msg_err = err;
		complete(&i2c_dev->completion);
		return IRQ_HANDLED;
	}

	if (val & BCM2835_I2C_S_DONE) {
		if (i2c_dev->curr_msg->flags & I2C_M_RD) {
			bcm2835_drain_rxfifo(i2c_dev);
			val = bcm2835_i2c_readl(i2c_dev, BCM2835_I2C_S);
		}

		if ((val & BCM2835_I2C_S_RXD) || i2c_dev->msg_buf_remaining)
			i2c_dev->msg_err = BCM2835_I2C_S_LEN;
		else
			i2c_dev->msg_err = 0;
		complete(&i2c_dev->completion);
		return IRQ_HANDLED;
	}

	if (val & BCM2835_I2C_S_TXW) {
		bcm2835_fill_txfifo(i2c_dev);
		return IRQ_HANDLED;
	}

	if (val & BCM2835_I2C_S_RXR) {
		bcm2835_drain_rxfifo(i2c_dev);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

/*
 * Repeated Start Condition (Sr)
 * The BCM2835 ARM Peripherals datasheet mentions a way to trigger a Sr when it
 * talks about reading from a slave with 10 bit address. This is achieved by
 * issuing a write (without enabling interrupts), poll the I2CS.TA flag and
 * wait for it to be set, and then issue a read.
 * https://github.com/raspberrypi/linux/issues/254 shows how the firmware does
 * it and states that it's a workaround for a problem in the state machine.
 * This is the comment in the firmware code:
 *
 *     The I2C peripheral samples the values for rw_bit and xfer_count in the
 *     IDLE state if start is set.
 *
 *     We want to generate a ReSTART not a STOP at the end of the TX phase. In
 *     order to do that we must ensure the state machine goes
 *     RACK1 -> RACK2 -> SRSTRT1 (not RACK1 -> RACK2 -> SSTOP1).
 *
 *     So, in the RACK2 state when (TX) xfer_count==0 we must therefore have
 *     already set, ready to be sampled:
 *     READ; rw_bit     <= I2CC bit 0 - must be "read"
 *     ST;   start      <= I2CC bit 7 - must be "Go" in order to not issue STOP
 *     DLEN; xfer_count <= I2CDLEN    - must be equal to our read amount
 *
 *     The plan to do this is:
 *     1. Start the sub-address write, but don't let it finish (keep
 *        xfer_count > 0)
 *     2. Populate READ, DLEN and ST in preparation for ReSTART read sequence
 *     3. Let TX finish (write the rest of the data)
 *     4. Read back data as it arrives
 */

static int bcm2835_i2c_xfer_msg(struct bcm2835_i2c_dev *i2c_dev,
				struct i2c_msg *msg, struct i2c_msg *msg2)
{
	u32 c;
	unsigned long time_left;

	i2c_dev->curr_msg = msg;
	i2c_dev->msg_buf = msg->buf;
	i2c_dev->msg_buf_remaining = msg->len;
	reinit_completion(&i2c_dev->completion);

	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_C, BCM2835_I2C_C_CLEAR);

	if (!(msg->flags & I2C_M_RD))
		bcm2835_fill_txfifo(i2c_dev);

	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_A, msg->addr);
	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_DLEN, msg->len);

	if (!msg2) {
		if (msg->flags & I2C_M_RD)
			c = BCM2835_I2C_C_READ | BCM2835_I2C_C_INTR;
		else
			c = BCM2835_I2C_C_INTT;

		c |= BCM2835_I2C_C_ST | BCM2835_I2C_C_INTD |
		     BCM2835_I2C_C_I2CEN;
		bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_C, c);
	} else {
		unsigned long flags;
		u32 stat, err = 0;

		local_irq_save(flags);

		/* Start write message */
		c = BCM2835_I2C_C_ST | BCM2835_I2C_C_I2CEN;
		bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_C, c);

		/* Wait for the transfer to become active */
		for (time_left = 100; time_left > 0; time_left--) {
			stat = bcm2835_i2c_readl(i2c_dev, BCM2835_I2C_S);

			err = stat & (BCM2835_I2C_S_CLKT | BCM2835_I2C_S_ERR);
			if (err)
				break;

			if (stat & BCM2835_I2C_S_TA)
				break;
		}

		if (err || !time_left) {
			i2c_dev->msg_err = err;
			local_irq_restore(flags);
			goto error;
		}

		/* Start read message */
		i2c_dev->curr_msg = msg2;
		i2c_dev->msg_buf = msg2->buf;
		i2c_dev->msg_buf_remaining = msg2->len;
		bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_DLEN, msg2->len);

		c = BCM2835_I2C_C_READ | BCM2835_I2C_C_INTR |
		    BCM2835_I2C_C_INTD | BCM2835_I2C_C_ST |
		    BCM2835_I2C_C_I2CEN;

		bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_C, c);

		local_irq_restore(flags);
	}

	time_left = wait_for_completion_timeout(&i2c_dev->completion,
						BCM2835_I2C_TIMEOUT);
error:
	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_C, BCM2835_I2C_C_CLEAR);
	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_S, BCM2835_I2C_S_CLKT |
			   BCM2835_I2C_S_ERR | BCM2835_I2C_S_DONE);
	if (!time_left) {
		dev_err(i2c_dev->dev, "i2c transfer timed out\n");
		return -ETIMEDOUT;
	}

	if (likely(!i2c_dev->msg_err))
		return 0;

	if ((i2c_dev->msg_err & BCM2835_I2C_S_ERR) &&
	    (msg->flags & I2C_M_IGNORE_NAK))
		return 0;

	dev_err_ratelimited(i2c_dev->dev, "i2c transfer failed: %x\n",
			    i2c_dev->msg_err);

	if (i2c_dev->msg_err & BCM2835_I2C_S_ERR)
		return -EREMOTEIO;
	else
		return -EIO;
}

static int bcm2835_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[],
			    int num)
{
	struct bcm2835_i2c_dev *i2c_dev = i2c_get_adapdata(adap);
	int i;
	int ret = 0;

	/* Combined write-read to the same address (smbus) */
	if (num == 2 && (msgs[0].addr == msgs[1].addr) &&
	    !(msgs[0].flags & I2C_M_RD) && (msgs[1].flags & I2C_M_RD) &&
	    (msgs[0].len <= 16)) {
		ret = bcm2835_i2c_xfer_msg(i2c_dev, &msgs[0], &msgs[1]);

		return ret ? ret : 2;
	}

	for (i = 0; i < num; i++) {
		ret = bcm2835_i2c_xfer_msg(i2c_dev, &msgs[i], NULL);
		if (ret)
			break;
	}

	return ret ?: i;
}

static u32 bcm2835_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm bcm2835_i2c_algo = {
	.master_xfer	= bcm2835_i2c_xfer,
	.functionality	= bcm2835_i2c_func,
};

static int bcm2835_i2c_probe(struct platform_device *pdev)
{
	struct bcm2835_i2c_dev *i2c_dev;
	struct resource *mem, *irq;
	u32 bus_clk_rate, divider;
	int ret;
	struct i2c_adapter *adap;

	i2c_dev = devm_kzalloc(&pdev->dev, sizeof(*i2c_dev), GFP_KERNEL);
	if (!i2c_dev)
		return -ENOMEM;
	platform_set_drvdata(pdev, i2c_dev);
	i2c_dev->dev = &pdev->dev;
	init_completion(&i2c_dev->completion);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	i2c_dev->regs = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(i2c_dev->regs))
		return PTR_ERR(i2c_dev->regs);

	i2c_dev->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(i2c_dev->clk)) {
		dev_err(&pdev->dev, "Could not get clock\n");
		return PTR_ERR(i2c_dev->clk);
	}

	ret = of_property_read_u32(pdev->dev.of_node, "clock-frequency",
				   &bus_clk_rate);
	if (ret < 0) {
		dev_warn(&pdev->dev,
			 "Could not read clock-frequency property\n");
		bus_clk_rate = 100000;
	}

	divider = DIV_ROUND_UP(clk_get_rate(i2c_dev->clk), bus_clk_rate);
	/*
	 * Per the datasheet, the register is always interpreted as an even
	 * number, by rounding down. In other words, the LSB is ignored. So,
	 * if the LSB is set, increment the divider to avoid any issue.
	 */
	if (divider & 1)
		divider++;
	if ((divider < BCM2835_I2C_CDIV_MIN) ||
	    (divider > BCM2835_I2C_CDIV_MAX)) {
		dev_err(&pdev->dev, "Invalid clock-frequency\n");
		return -ENODEV;
	}
	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_DIV, divider);

	irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!irq) {
		dev_err(&pdev->dev, "No IRQ resource\n");
		return -ENODEV;
	}
	i2c_dev->irq = irq->start;

	ret = request_irq(i2c_dev->irq, bcm2835_i2c_isr, IRQF_SHARED,
			  dev_name(&pdev->dev), i2c_dev);
	if (ret) {
		dev_err(&pdev->dev, "Could not request IRQ\n");
		return -ENODEV;
	}

	adap = &i2c_dev->adapter;
	i2c_set_adapdata(adap, i2c_dev);
	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_DEPRECATED;
	strlcpy(adap->name, "bcm2835 I2C adapter", sizeof(adap->name));
	adap->algo = &bcm2835_i2c_algo;
	adap->dev.parent = &pdev->dev;
	adap->dev.of_node = pdev->dev.of_node;

	bcm2835_i2c_writel(i2c_dev, BCM2835_I2C_C, 0);

	ret = i2c_add_adapter(adap);
	if (ret)
		free_irq(i2c_dev->irq, i2c_dev);

	return ret;
}

static int bcm2835_i2c_remove(struct platform_device *pdev)
{
	struct bcm2835_i2c_dev *i2c_dev = platform_get_drvdata(pdev);

	free_irq(i2c_dev->irq, i2c_dev);
	i2c_del_adapter(&i2c_dev->adapter);

	return 0;
}

static const struct of_device_id bcm2835_i2c_of_match[] = {
	{ .compatible = "brcm,bcm2835-i2c" },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_i2c_of_match);

static struct platform_driver bcm2835_i2c_driver = {
	.probe		= bcm2835_i2c_probe,
	.remove		= bcm2835_i2c_remove,
	.driver		= {
		.name	= "i2c-bcm2835",
		.of_match_table = bcm2835_i2c_of_match,
	},
};
module_platform_driver(bcm2835_i2c_driver);

MODULE_AUTHOR("Stephen Warren <swarren@wwwdotorg.org>");
MODULE_DESCRIPTION("BCM2835 I2C bus adapter");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c-bcm2835");
