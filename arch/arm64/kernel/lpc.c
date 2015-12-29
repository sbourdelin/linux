#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/version.h>

#define LPC_REG_START            (0x00)
#define LPC_REG_OP_STATUS        (0x04)
#define LPC_REG_IRQ_ST           (0x08)
#define LPC_REG_OP_LEN           (0x10)
#define LPC_REG_CMD              (0x14)
#define LPC_REG_FWH_ID_MSIZE     (0x18)
#define LPC_REG_ADDR             (0x20)
#define LPC_REG_WDATA            (0x24)
#define LPC_REG_RDATA            (0x28)
#define LPC_REG_LONG_CNT         (0x30)
#define LPC_REG_TX_FIFO_ST       (0x50)
#define LPC_REG_RX_FIFO_ST       (0x54)
#define LPC_REG_TIME_OUT         (0x58)
#define LPC_REG_STRQ_CTRL0       (0x80)
#define LPC_REG_STRQ_CTRL1       (0x84)
#define LPC_REG_STRQ_INT         (0x90)
#define LPC_REG_STRQ_INT_MASK    (0x94)
#define LPC_REG_STRQ_STAT        (0xa0)

#define LPC_CMD_SAMEADDR_SING    (0x00000008)
#define LPC_CMD_SAMEADDR_INC     (0x00000000)
#define LPC_CMD_TYPE_IO          (0x00000000)
#define LPC_CMD_TYPE_MEM         (0x00000002)
#define LPC_CMD_TYPE_FWH         (0x00000004)
#define LPC_CMD_WRITE            (0x00000001)
#define LPC_CMD_READ             (0x00000000)

#define LPC_IRQ_CLEAR            (0x02)
#define LPC_IRQ_OCCURRED         (0x02)
#define LPC_STATUS_DILE          (0x01)
#define LPC_OP_FINISHED          (0x02)
#define START_WORK (0x01)

#define LPC_FRAME_LEN (0x10)

#define LPC_CURR_STATUS_IDLE     0
#define LPC_CURR_STATUS_START    1
#define LPC_CURR_STATUS_TYPE_DIR 2
#define LPC_CURR_STATUS_ADDR     3
#define LPC_CURR_STATUS_MSIZE    4
#define LPC_CURR_STATUS_WDATA    5
#define LPC_CURR_STATUS_TARHOST  6
#define LPC_CURR_STATUS_SYNC     7
#define LPC_CURR_STATUS_RDATA    8
#define LPC_CURR_STATUS_TARSLAVE 9
#define LPC_CURR_STATUS_ABORT    10

struct lpc_dev {
	spinlock_t lock;
	void __iomem  *regs;
	struct device *dev;
};

static struct lpc_dev *lpc_dev;

int lpc_master_write(unsigned int slv_access_mode, unsigned int cycle_type,
		      unsigned int addr, unsigned char *buf, unsigned int len)
{
	unsigned int i;
	unsigned int lpc_cmd_value;
	unsigned int lpc_op_state_value;
	unsigned int retry = 0;

	/* para check */
	if (!buf || !len)
		return -EINVAL;

	if (slv_access_mode != LPC_CMD_SAMEADDR_SING &&
		slv_access_mode != LPC_CMD_SAMEADDR_INC) {
		return -EINVAL;
	}

	if ((cycle_type != LPC_CMD_TYPE_IO) &&
		(cycle_type != LPC_CMD_TYPE_MEM) &&
		(cycle_type != LPC_CMD_TYPE_FWH)) {
		return -EINVAL;
	}

	writel(LPC_IRQ_CLEAR, lpc_dev->regs + LPC_REG_IRQ_ST);
	retry = 0;
	while (!(readl(lpc_dev->regs + LPC_REG_OP_STATUS)
		& LPC_STATUS_DILE)) {
		udelay(1);
		retry++;
		if (retry >= 2)
			return -ETIME;
	}

	/* set lpc master write cycle type and slv access mode */
	lpc_cmd_value = LPC_CMD_WRITE | cycle_type | slv_access_mode;
	writel(lpc_cmd_value, lpc_dev->regs + LPC_REG_CMD);

	/* set lpc op len */
	writel(len, lpc_dev->regs + LPC_REG_OP_LEN);

	/* Set write data */
	for (i = 0; i < len; i++)
		writel(buf[i], lpc_dev->regs + LPC_REG_WDATA);

	/* set lpc addr config */
	writel(addr, lpc_dev->regs + LPC_REG_ADDR);

	/* set lpc start work */
	writel(START_WORK, lpc_dev->regs + LPC_REG_START);

	retry = 0;
	while (!(readl(lpc_dev->regs + LPC_REG_IRQ_ST) &
		 LPC_IRQ_OCCURRED)) {
		udelay(1);
		retry++;
		if (retry >= 2)
			return -ETIME;
	}

	writel(LPC_IRQ_CLEAR, lpc_dev->regs + LPC_REG_IRQ_ST);

	lpc_op_state_value = readl(lpc_dev->regs + LPC_REG_OP_STATUS);
	if (lpc_op_state_value & LPC_OP_FINISHED)
		return 0;

	return -EIO;
}

void  lpc_io_write_byte(u8 value, unsigned long addr)
{
	unsigned long flags;

	if (!lpc_dev)
		return;
	spin_lock_irqsave(&lpc_dev->lock, flags);
	(void)lpc_master_write(LPC_CMD_SAMEADDR_SING, LPC_CMD_TYPE_IO,
				 addr, &value, 1);
	spin_unlock_irqrestore(&lpc_dev->lock, flags);
}

int lpc_master_read(unsigned int slv_access_mode, unsigned int cycle_type,
		     unsigned int addr, unsigned char *buf, unsigned int len)
{
	unsigned int i;
	unsigned int lpc_cmd_value;
	unsigned int lpc_op_state_value;
	unsigned int retry = 0;

	/* para check */
	if (!buf || !len)
		return -EINVAL;

	if (slv_access_mode != LPC_CMD_SAMEADDR_SING &&
	    slv_access_mode != LPC_CMD_SAMEADDR_INC) {
		return -EINVAL;
	}

	if (cycle_type != LPC_CMD_TYPE_IO &&
	    cycle_type != LPC_CMD_TYPE_MEM &&
	    cycle_type != LPC_CMD_TYPE_FWH) {
		return -EINVAL;
	}

	writel(LPC_IRQ_CLEAR, lpc_dev->regs + LPC_REG_IRQ_ST);

	retry = 0;
	while (!(readl(lpc_dev->regs + LPC_REG_OP_STATUS) &
	       LPC_STATUS_DILE)) {
		udelay(1);
		retry++;
		if (retry >= 2)
			return -ETIME;
	}

	/* set lpc master read cycle type and slv access mode */
	lpc_cmd_value = LPC_CMD_READ | cycle_type | slv_access_mode;
	writel(lpc_cmd_value, lpc_dev->regs + LPC_REG_CMD);

	/* set lpc op len */
	writel(len, lpc_dev->regs + LPC_REG_OP_LEN);

	/* set lpc addr config */
	writel(addr, lpc_dev->regs + LPC_REG_ADDR);

	/* set lpc start work */
	writel(START_WORK, lpc_dev->regs + LPC_REG_START);

	while (!(readl(lpc_dev->regs + LPC_REG_IRQ_ST) &
	       LPC_IRQ_OCCURRED)) {
		udelay(1);
		retry++;
		if (retry >= 2)
			return -ETIME;
	}

	writel(LPC_IRQ_CLEAR, lpc_dev->regs + LPC_REG_IRQ_ST);

	lpc_op_state_value = readl(lpc_dev->regs + LPC_REG_OP_STATUS);
	/* Get read data */
	if (lpc_op_state_value & LPC_OP_FINISHED) {
		for (i = 0; i < len; i++)
			buf[i] = readl(lpc_dev->regs + LPC_REG_RDATA);
		return 0;
	}
	return -EIO;
}

u8 lpc_io_read_byte(unsigned long addr)
{
	unsigned char value;
	unsigned long flags;
	int ret;

	if (!lpc_dev)
		return 0xff;

	spin_lock_irqsave(&lpc_dev->lock, flags);
	ret = lpc_master_read(LPC_CMD_SAMEADDR_SING,
		LPC_CMD_TYPE_IO, addr, &value, 1);
	spin_unlock_irqrestore(&lpc_dev->lock, flags);
	return ret ? 0xff : value;
}

static const struct arm64_isa_io lpc_io = {
	.inb  = lpc_io_read_byte,
	.outb = lpc_io_write_byte,
};

static int lpc_probe(struct platform_device *pdev)
{
	struct resource *regs = NULL;

	lpc_dev = devm_kzalloc(&pdev->dev,
		sizeof(struct lpc_dev), GFP_KERNEL);
	if (!lpc_dev)
		return -ENOMEM;

	spin_lock_init(&lpc_dev->lock);
	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	lpc_dev->regs = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(lpc_dev->regs))
		return PTR_ERR(lpc_dev->regs);

	dev_info(&pdev->dev, "Low pin count driver initialized successfully\n");

	lpc_dev->dev = &pdev->dev;
	platform_set_drvdata(pdev, lpc_dev);
	arm64_isa_io = lpc_io;

	return 0;
}

static int lpc_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id lpc_pltfm_match[] = {
	{
		.compatible = "low-pin-count",
	},
	{},
};

static struct platform_driver lpc_driver = {
	.driver = {
		.name           = "LPC",
		.owner          = THIS_MODULE,
		.of_match_table = lpc_pltfm_match,
	},
	.probe                = lpc_probe,
	.remove               = lpc_remove,
};

static int __init lpc_init_driver(void)
{
	return platform_driver_register(&lpc_driver);
}

static void __exit lpc_init_exit(void)
{
	platform_driver_unregister(&lpc_driver);
}

arch_initcall(lpc_init_driver);
module_exit(lpc_init_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Huawei Tech. Co., Ltd.");
MODULE_DESCRIPTION("LPC driver for linux");
MODULE_VERSION("v1.0");
