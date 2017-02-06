/*
 * ZTE's zx2967 family i2c bus controller driver
 *
 * Copyright (C) 2017 ZTE Ltd.
 *
 * Author: Baoyou Xie <baoyou.xie@linaro.org>
 *
 * License terms: GNU General Public License (GPL) version 2
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define REG_CMD				0x04
#define REG_DEVADDR_H			0x0C
#define REG_DEVADDR_L			0x10
#define REG_CLK_DIV_FS			0x14
#define REG_CLK_DIV_HS			0x18
#define REG_WRCONF			0x1C
#define REG_RDCONF			0x20
#define REG_DATA			0x24
#define REG_STAT			0x28

#define I2C_STOP			0
#define I2C_MASTER			BIT(0)
#define I2C_ADDR_MODE_TEN		BIT(1)
#define I2C_IRQ_MSK_ENABLE		BIT(3)
#define I2C_RW_READ			BIT(4)
#define I2C_CMB_RW_EN			BIT(5)
#define I2C_START			BIT(6)
#define I2C_ADDR_MODE_TEN		BIT(1)

#define I2C_WFIFO_RESET			BIT(7)
#define I2C_RFIFO_RESET			BIT(7)

#define I2C_IRQ_ACK_CLEAR		BIT(7)
#define I2C_INT_MASK			GENMASK(6, 0)

#define I2C_TRANS_DONE			BIT(0)
#define I2C_ERROR_DEVICE		BIT(1)
#define I2C_ERROR_DATA			BIT(2)
#define I2C_ERROR_MASK			GENMASK(2, 1)

#define I2C_SR_BUSY			BIT(6)

#define I2C_SR_EDEVICE			BIT(1)
#define I2C_SR_EDATA			BIT(2)

#define I2C_FIFO_MAX			16

#define I2C_TIMEOUT			msecs_to_jiffies(1000)

struct zx2967_i2c_info {
	spinlock_t		lock;
	struct device		*dev;
	struct i2c_adapter	adap;
	struct clk		*clk;
	struct completion	complete;
	u32			clk_freq;
	void __iomem		*reg_base;
	size_t			residue;
	int			irq;
	int			msg_rd;
	u8			*buf;
	u8			access_cnt;
	bool			is_suspended;
};

static void zx2967_i2c_writel(struct zx2967_i2c_info *zx_i2c,
			      u32 val, unsigned long reg)
{
	writel_relaxed(val, zx_i2c->reg_base + reg);
}

static u32 zx2967_i2c_readl(struct zx2967_i2c_info *zx_i2c, unsigned long reg)
{
	return readl_relaxed(zx_i2c->reg_base + reg);
}

static void zx2967_i2c_writesb(struct zx2967_i2c_info *zx_i2c,
			       void *data, unsigned long reg, int len)
{
	writesb(zx_i2c->reg_base + reg, data, len);
}

static void zx2967_i2c_readsb(struct zx2967_i2c_info *zx_i2c,
			      void *data, unsigned long reg, int len)
{
	readsb(zx_i2c->reg_base + reg, data, len);
}

static void zx2967_i2c_start_ctrl(struct zx2967_i2c_info *zx_i2c)
{
	u32 status;
	u32 ctl;

	status = zx2967_i2c_readl(zx_i2c, REG_STAT);
	status |= I2C_IRQ_ACK_CLEAR;
	zx2967_i2c_writel(zx_i2c, status, REG_STAT);

	ctl = zx2967_i2c_readl(zx_i2c, REG_CMD);
	if (zx_i2c->msg_rd)
		ctl |= I2C_RW_READ;
	else
		ctl &= ~I2C_RW_READ;
	ctl &= ~I2C_CMB_RW_EN;
	ctl |= I2C_START;
	zx2967_i2c_writel(zx_i2c, ctl, REG_CMD);
}

static void zx2967_i2c_flush_fifos(struct zx2967_i2c_info *zx_i2c)
{
	u32 val;
	u32 offset;

	if (zx_i2c->msg_rd) {
		offset = REG_RDCONF;
		val = I2C_RFIFO_RESET;
	} else {
		offset = REG_WRCONF;
		val = I2C_WFIFO_RESET;
	}

	val |= zx2967_i2c_readl(zx_i2c, offset);
	zx2967_i2c_writel(zx_i2c, val, offset);
}

static int zx2967_i2c_empty_rx_fifo(struct zx2967_i2c_info *zx_i2c, u32 size)
{
	u8 val[I2C_FIFO_MAX] = {0};
	int i;

	if (size > I2C_FIFO_MAX) {
		dev_err(zx_i2c->dev, "fifo size %d over the max value %d\n",
			size, I2C_FIFO_MAX);
		return -EINVAL;
	}

	zx2967_i2c_readsb(zx_i2c, val, REG_DATA, size);
	for (i = 0; i < size; i++) {
		*(zx_i2c->buf++) = val[i];
		zx_i2c->residue--;
		if (zx_i2c->residue <= 0)
			break;
	}

	barrier();

	return 0;
}

static int zx2967_i2c_fill_tx_fifo(struct zx2967_i2c_info *zx_i2c)
{
	u8 *buf = zx_i2c->buf;
	size_t residue = zx_i2c->residue;

	if (residue == 0) {
		dev_err(zx_i2c->dev, "residue is %d\n", (int)residue);
		return -EINVAL;
	}

	if (residue <= I2C_FIFO_MAX) {
		zx2967_i2c_writesb(zx_i2c, buf, REG_DATA, residue);

		/* Again update before writing to FIFO to make sure isr sees. */
		zx_i2c->residue = 0;
		zx_i2c->buf = NULL;
	} else {
		zx2967_i2c_writesb(zx_i2c, buf, REG_DATA, I2C_FIFO_MAX);
		zx_i2c->residue -= I2C_FIFO_MAX;
		zx_i2c->buf += I2C_FIFO_MAX;
	}

	barrier();

	return 0;
}

static int zx2967_i2c_reset_hardware(struct zx2967_i2c_info *zx_i2c)
{
	u32 val;
	u32 clk_div;
	u32 status;

	val = I2C_MASTER | I2C_IRQ_MSK_ENABLE;
	zx2967_i2c_writel(zx_i2c, val, REG_CMD);

	clk_div = clk_get_rate(zx_i2c->clk) / zx_i2c->clk_freq - 1;
	zx2967_i2c_writel(zx_i2c, clk_div, REG_CLK_DIV_FS);
	zx2967_i2c_writel(zx_i2c, clk_div, REG_CLK_DIV_HS);

	zx2967_i2c_writel(zx_i2c, I2C_FIFO_MAX - 1, REG_WRCONF);
	zx2967_i2c_writel(zx_i2c, I2C_FIFO_MAX - 1, REG_RDCONF);
	zx2967_i2c_writel(zx_i2c, 1, REG_RDCONF);

	zx2967_i2c_flush_fifos(zx_i2c);

	status = zx2967_i2c_readl(zx_i2c, REG_STAT);
	if (status & I2C_SR_BUSY)
		return -EBUSY;
	if (status & (I2C_SR_EDEVICE | I2C_SR_EDATA))
		return -EIO;

	enable_irq(zx_i2c->irq);

	return 0;
}

static void zx2967_i2c_isr_clr(struct zx2967_i2c_info *zx_i2c)
{
	u32 status;

	status = zx2967_i2c_readl(zx_i2c, REG_STAT);
	status |= I2C_IRQ_ACK_CLEAR;
	zx2967_i2c_writel(zx_i2c, status, REG_STAT);
}

static irqreturn_t zx2967_i2c_isr(int irq, void *dev_id)
{
	u32 status;
	struct zx2967_i2c_info *zx_i2c = (struct zx2967_i2c_info *)dev_id;
	unsigned long flags;

	spin_lock_irqsave(&zx_i2c->lock, flags);

	status = zx2967_i2c_readl(zx_i2c, REG_STAT) & I2C_INT_MASK;
	zx2967_i2c_isr_clr(zx_i2c);

	if (status & I2C_ERROR_MASK) {
		spin_unlock_irqrestore(&zx_i2c->lock, flags);
		return IRQ_HANDLED;
	}

	if (status & I2C_TRANS_DONE)
		complete(&zx_i2c->complete);

	spin_unlock_irqrestore(&zx_i2c->lock, flags);

	return IRQ_HANDLED;
}

static void zx2967_enable_tenbit(struct zx2967_i2c_info *zx_i2c, __u16 addr)
{
	u16 val = (addr >> 7) & 0x7;

	if (val > 0) {
		zx2967_i2c_writel(zx_i2c, val, REG_DEVADDR_H);
		val = (zx2967_i2c_readl(zx_i2c, REG_CMD)) | I2C_ADDR_MODE_TEN;
		zx2967_i2c_writel(zx_i2c, val, REG_CMD);
	}
}

static int
zx2967_i2c_xfer_read_bytes(struct zx2967_i2c_info *zx_i2c, u32 bytes)
{
	unsigned long time_left;

	reinit_completion(&zx_i2c->complete);
	zx2967_i2c_writel(zx_i2c, bytes - 1, REG_RDCONF);
	zx2967_i2c_start_ctrl(zx_i2c);

	time_left = wait_for_completion_timeout(&zx_i2c->complete,
				I2C_TIMEOUT);
	if (time_left == 0) {
		dev_err(zx_i2c->dev, "read i2c transfer timed out\n");
		disable_irq(zx_i2c->irq);
		zx2967_i2c_reset_hardware(zx_i2c);
		return -EIO;
	}

	return zx2967_i2c_empty_rx_fifo(zx_i2c, bytes);
}

static int zx2967_i2c_xfer_read(struct zx2967_i2c_info *zx_i2c)
{
	int ret;
	int i;

	for (i = 0; i < zx_i2c->access_cnt; i++) {
		ret = zx2967_i2c_xfer_read_bytes(zx_i2c, I2C_FIFO_MAX);
		if (ret)
			return ret;
	}

	if (zx_i2c->residue > 0) {
		ret = zx2967_i2c_xfer_read_bytes(zx_i2c, I2C_FIFO_MAX);
		if (ret)
			return ret;
	}

	zx_i2c->residue = 0;
	zx_i2c->access_cnt = 0;
	return 0;
}

static int
zx2967_i2c_xfer_write_bytes(struct zx2967_i2c_info *zx_i2c, u32 bytes)
{
	unsigned long time_left;
	int ret;

	reinit_completion(&zx_i2c->complete);

	ret = zx2967_i2c_fill_tx_fifo(zx_i2c);
	if (ret)
		return ret;

	zx2967_i2c_start_ctrl(zx_i2c);

	time_left = wait_for_completion_timeout(&zx_i2c->complete,
				I2C_TIMEOUT);
	if (time_left == 0) {
		dev_err(zx_i2c->dev, "write i2c transfer timed out\n");
		disable_irq(zx_i2c->irq);
		zx2967_i2c_reset_hardware(zx_i2c);
		return -EIO;
	}

	return 0;
}

static int zx2967_i2c_xfer_write(struct zx2967_i2c_info *zx_i2c)
{
	int ret;
	int i;

	for (i = 0; i < zx_i2c->access_cnt; i++) {
		ret = zx2967_i2c_xfer_write_bytes(zx_i2c, I2C_FIFO_MAX);
		if (ret)
			return ret;
	}

	if (zx_i2c->residue > 0) {
		ret = zx2967_i2c_xfer_write_bytes(zx_i2c, I2C_FIFO_MAX);
		if (ret)
			return ret;
	}

	zx_i2c->residue = 0;
	zx_i2c->access_cnt = 0;
	return 0;
}

static int zx2967_i2c_xfer_msg(struct zx2967_i2c_info *zx_i2c,
			       struct i2c_msg *msg)
{
	if (msg->len == 0)
		return -EINVAL;

	zx2967_i2c_flush_fifos(zx_i2c);

	zx_i2c->buf = msg->buf;
	zx_i2c->residue = msg->len;
	zx_i2c->access_cnt = msg->len / I2C_FIFO_MAX;
	zx_i2c->msg_rd = (msg->flags & I2C_M_RD);

	if (zx_i2c->msg_rd)
		return zx2967_i2c_xfer_read(zx_i2c);

	return zx2967_i2c_xfer_write(zx_i2c);
}

static int zx2967_i2c_xfer(struct i2c_adapter *adap,
			   struct i2c_msg *msgs, int num)
{
	struct zx2967_i2c_info *zx_i2c = i2c_get_adapdata(adap);
	int ret;
	int i;

	if (zx_i2c->is_suspended)
		return -EBUSY;

	zx2967_i2c_writel(zx_i2c, (msgs->addr & 0x7f), REG_DEVADDR_L);
	zx2967_i2c_writel(zx_i2c, (msgs->addr >> 7) & 0x7, REG_DEVADDR_H);
	if (zx2967_i2c_readl(zx_i2c, REG_DEVADDR_H) > 0)
		zx2967_enable_tenbit(zx_i2c, msgs->addr);

	for (i = 0; i < num; i++) {
		ret = zx2967_i2c_xfer_msg(zx_i2c, &msgs[i]);
		if (ret)
			return ret;
		if (num > 1)
			usleep_range(1000, 2000);
	}

	return num;
}

static void
zx2967_smbus_xfer_prepare(struct zx2967_i2c_info *zx_i2c, u16 addr,
			  char read_write, u8 command, int size,
			  union i2c_smbus_data *data)
{
	u32 val;

	val = zx2967_i2c_readl(zx_i2c, REG_RDCONF);
	val |= I2C_RFIFO_RESET;
	zx2967_i2c_writel(zx_i2c, val, REG_RDCONF);
	zx2967_i2c_writel(zx_i2c, (addr & 0x7f), REG_DEVADDR_L);

	zx2967_enable_tenbit(zx_i2c, addr);
	val = zx2967_i2c_readl(zx_i2c, REG_CMD);
	val &= ~I2C_RW_READ;
	zx2967_i2c_writel(zx_i2c, val, REG_CMD);

	switch (size) {
	case I2C_SMBUS_BYTE:
		zx2967_i2c_writel(zx_i2c, command, REG_DATA);
		break;
	case I2C_SMBUS_BYTE_DATA:
		zx2967_i2c_writel(zx_i2c, command, REG_DATA);
		if (read_write == I2C_SMBUS_WRITE)
			zx2967_i2c_writel(zx_i2c, data->byte, REG_DATA);
		break;
	case I2C_SMBUS_WORD_DATA:
		zx2967_i2c_writel(zx_i2c, command, REG_DATA);
		if (read_write == I2C_SMBUS_WRITE) {
			zx2967_i2c_writel(zx_i2c, (data->word >> 8), REG_DATA);
			zx2967_i2c_writel(zx_i2c, (data->word & 0xff),
					  REG_DATA);
		}
		break;
	}
}

static int zx2967_smbus_xfer_read(struct zx2967_i2c_info *zx_i2c, int size,
				  union i2c_smbus_data *data)
{
	unsigned long time_left;
	u8 buf[2];
	u32 val;

	reinit_completion(&zx_i2c->complete);

	val = zx2967_i2c_readl(zx_i2c, REG_CMD);
	val |= I2C_CMB_RW_EN;
	zx2967_i2c_writel(zx_i2c, val, REG_CMD);

	val = zx2967_i2c_readl(zx_i2c, REG_CMD);
	val |= I2C_START;
	zx2967_i2c_writel(zx_i2c, val, REG_CMD);

	time_left = wait_for_completion_timeout(&zx_i2c->complete,
						I2C_TIMEOUT);
	if (time_left == 0) {
		dev_err(zx_i2c->dev, "i2c read transfer timed out\n");
		disable_irq(zx_i2c->irq);
		zx2967_i2c_reset_hardware(zx_i2c);
		return -EIO;
	}

	usleep_range(1000, 2000);
	switch (size) {
	case I2C_SMBUS_BYTE:
	case I2C_SMBUS_BYTE_DATA:
		val = zx2967_i2c_readl(zx_i2c, REG_DATA);
		data->byte = val;
		break;
	case I2C_SMBUS_WORD_DATA:
	case I2C_SMBUS_PROC_CALL:
		buf[0] = zx2967_i2c_readl(zx_i2c, REG_DATA);
		buf[1] = zx2967_i2c_readl(zx_i2c, REG_DATA);
		data->word = (buf[0] << 8) | buf[1];
		break;
	default:
		dev_warn(zx_i2c->dev, "Unsupported transaction %d\n", size);
		return -EOPNOTSUPP;
	}

	return 0;
}

static int zx2967_smbus_xfer_write(struct zx2967_i2c_info *zx_i2c)
{
	unsigned long time_left;
	u32 val;

	reinit_completion(&zx_i2c->complete);
	val = zx2967_i2c_readl(zx_i2c, REG_CMD);
	val |= I2C_START;
	zx2967_i2c_writel(zx_i2c, val, REG_CMD);

	time_left = wait_for_completion_timeout(&zx_i2c->complete,
						I2C_TIMEOUT);
	if (time_left == 0) {
		dev_err(zx_i2c->dev, "i2c write transfer timed out\n");
		disable_irq(zx_i2c->irq);
		zx2967_i2c_reset_hardware(zx_i2c);
		return -EIO;
	}

	return 0;
}

static int zx2967_smbus_xfer(struct i2c_adapter *adap, u16 addr,
			     unsigned short flags, char read_write,
			     u8 command, int size, union i2c_smbus_data *data)
{
	struct zx2967_i2c_info *zx_i2c = i2c_get_adapdata(adap);

	if (size == I2C_SMBUS_QUICK)
		read_write = I2C_SMBUS_WRITE;

	switch (size) {
	case I2C_SMBUS_QUICK:
	case I2C_SMBUS_BYTE:
	case I2C_SMBUS_BYTE_DATA:
	case I2C_SMBUS_WORD_DATA:
		zx2967_smbus_xfer_prepare(zx_i2c, addr, read_write,
					  command, size, data);
		break;
	default:
		dev_warn(&adap->dev, "Unsupported transaction %d\n", size);
		return -EOPNOTSUPP;
	}

	if (read_write == I2C_SMBUS_READ)
		return zx2967_smbus_xfer_read(zx_i2c, size, data);

	return zx2967_smbus_xfer_write(zx_i2c);
}

#define ZX2967_I2C_FUNCS (I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |	\
		I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |	\
		I2C_FUNC_SMBUS_BLOCK_DATA | I2C_FUNC_SMBUS_PROC_CALL |	\
		I2C_FUNC_I2C | I2C_FUNC_SMBUS_I2C_BLOCK)

static u32 zx2967_i2c_func(struct i2c_adapter *adap)
{
	return ZX2967_I2C_FUNCS;
}

static int __maybe_unused zx2967_i2c_suspend(struct device *dev)
{
	struct zx2967_i2c_info *zx_i2c = dev_get_drvdata(dev);

	zx_i2c->is_suspended = true;
	clk_disable_unprepare(zx_i2c->clk);

	return 0;
}

static int __maybe_unused zx2967_i2c_resume(struct device *dev)
{
	struct zx2967_i2c_info *zx_i2c = dev_get_drvdata(dev);

	zx_i2c->is_suspended = false;
	clk_prepare_enable(zx_i2c->clk);

	return 0;
}

#ifdef CONFIG_PM
static const struct dev_pm_ops zx2967_i2c_dev_pm_ops = {
	.suspend	= zx2967_i2c_suspend,
	.resume		= zx2967_i2c_resume,
};
#define ZX2967_I2C_DEV_PM_OPS	(&zx2967_i2c_dev_pm_ops)
#else
#define	ZX2967_I2C_DEV_PM_OPS	NULL
#endif

static const struct i2c_algorithm zx2967_i2c_algo = {
	.master_xfer = zx2967_i2c_xfer,
	.smbus_xfer = zx2967_smbus_xfer,
	.functionality = zx2967_i2c_func,
};

static const struct of_device_id zx2967_i2c_of_match[] = {
	{ .compatible = "zte,zx296718-i2c", },
	{},
};
MODULE_DEVICE_TABLE(of, zx2967_i2c_of_match);

static int zx2967_i2c_probe(struct platform_device *pdev)
{
	struct zx2967_i2c_info *zx_i2c;
	void __iomem *reg_base;
	struct resource *res;
	struct clk *clk;
	int ret;

	zx_i2c = devm_kzalloc(&pdev->dev, sizeof(*zx_i2c), GFP_KERNEL);
	if (!zx_i2c)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	reg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(reg_base))
		return PTR_ERR(reg_base);

	clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(&pdev->dev, "missing controller clock");
		return PTR_ERR(clk);
	}

	ret = clk_prepare_enable(clk);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable i2c_clk\n");
		return ret;
	}

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	zx_i2c->irq = ret;

	ret = device_property_read_u32(&pdev->dev, "clock-frequency",
				       &zx_i2c->clk_freq);
	if (ret) {
		dev_err(&pdev->dev, "missing clock-frequency");
		return ret;
	}

	zx_i2c->reg_base = reg_base;
	zx_i2c->clk = clk;
	zx_i2c->dev = &pdev->dev;

	spin_lock_init(&zx_i2c->lock);
	init_completion(&zx_i2c->complete);
	platform_set_drvdata(pdev, zx_i2c);

	ret = zx2967_i2c_reset_hardware(zx_i2c);
	if (ret) {
		dev_err(&pdev->dev, "failed to initialize i2c controller\n");
		goto err_clk_unprepare;
	}

	ret = devm_request_irq(&pdev->dev, zx_i2c->irq,
			zx2967_i2c_isr, 0, dev_name(&pdev->dev), zx_i2c);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq %i\n", zx_i2c->irq);
		goto err_clk_unprepare;
	}

	i2c_set_adapdata(&zx_i2c->adap, zx_i2c);
	zx_i2c->adap.owner = THIS_MODULE;
	zx_i2c->adap.class = I2C_CLASS_DEPRECATED;
	strlcpy(zx_i2c->adap.name, "zx2967 i2c adapter",
		sizeof(zx_i2c->adap.name));
	zx_i2c->adap.algo = &zx2967_i2c_algo;
	zx_i2c->adap.dev.parent = &pdev->dev;
	zx_i2c->adap.nr = pdev->id;
	zx_i2c->adap.dev.of_node = pdev->dev.of_node;

	ret = i2c_add_numbered_adapter(&zx_i2c->adap);
	if (ret) {
		dev_err(&pdev->dev, "failed to add zx2967 i2c adapter\n");
		goto err_clk_unprepare;
	}

	return 0;

err_clk_unprepare:
	clk_disable_unprepare(zx_i2c->clk);
	return ret;
}

static int zx2967_i2c_remove(struct platform_device *pdev)
{
	struct zx2967_i2c_info *zx_i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&zx_i2c->adap);
	clk_disable_unprepare(zx_i2c->clk);

	return 0;
}

static struct platform_driver zx2967_i2c_driver = {
	.probe	= zx2967_i2c_probe,
	.remove	= zx2967_i2c_remove,
	.driver	= {
		.name  = "zx2967_i2c",
		.of_match_table = zx2967_i2c_of_match,
		.pm		= ZX2967_I2C_DEV_PM_OPS,
	},
};
module_platform_driver(zx2967_i2c_driver);

MODULE_AUTHOR("Baoyou Xie <baoyou.xie@linaro.org>");
MODULE_DESCRIPTION("ZTE zx2967 I2C Bus Controller driver");
MODULE_LICENSE("GPL v2");
