// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 ARM Ltd.
 * Author: Samarth Parikh <samarth.parikh@arm.com>
 *
 */

#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/amba/bus.h>
#include <linux/mailbox_controller.h>
#include <linux/of_device.h>
#include <linux/of_address.h>

#define MHU_V2_REG_STAT_OFS		0x0
#define MHU_V2_REG_CLR_OFS		0x8
#define MHU_V2_REG_SET_OFS		0xC
#define MHU_V2_REG_MSG_NO_CAP_OFS	0xF80
#define MHU_V2_REG_ACC_REQ_OFS		0xF88
#define MHU_V2_REG_ACC_RDY_OFS		0xF8C

#define MHU_V2_LP_OFFSET		0x20
#define MHU_V2_HP_OFFSET		0x0

#define MHU_V2_CHANS			3

#define mbox_to_mhuv2_drv_data(c) container_of(c, struct mhuv2_drv_data, mbox)

enum mhuv2_regs {
	MHU_V2_REG_STAT,
	MHU_V2_REG_SET,
	MHU_V2_REG_CLR,
	MHU_V2_REG_END
};

enum mhuv2_access_regs {
	MHU_V2_REG_MSG_NO_CAP,
	MHU_V2_REG_ACC_REQ,
	MHU_V2_REG_ACC_RDY,
	MHU_V2_REG_ACC_END
};

enum mhuv2_channels {
	MHU_V2_CHAN_LOW,
	MHU_V2_CHAN_HIGH,
	MHU_V2_CHAN_SEC,
	MHU_V2_CHAN_END
};

/**
 * ARM MHUv2 Mailbox device specific data
 *
 * @regs: MHUv2 specific array of register offset for STAT, SET & CLEAR
 *        registers.
 * @chans: MHUv2 specific array of channel offset for Low
 *         Priority, High Priority & Secure channels.
 * @acc_regs: An array of access register offsets.
 */
struct mhuv2_dev_data {
	int regs[MHU_V2_REG_END]; /* STAT, SET, CLEAR */
	int chans[MHU_V2_CHAN_END]; /* LP, HP, Sec */
	int acc_regs[MHU_V2_REG_ACC_END];
};

/**
 * ARM MHUv2 link specific data
 *
 * @irq: MHUv2 receive channel IRQ number
 * @tx_reg: Transmit channel register
 * @rx_reg: Receive channel register
 * @pchan: No. of physical channels
 */
struct mhuv2_link {
	unsigned int irq;
	void __iomem *tx_reg;
	void __iomem *rx_reg;
	unsigned int pchan;
};

/**
 * ARM MHUv2 Mailbox driver specific data
 *
 * @mlink: MHUv2 link specific data
 * @chan: Mbox specific data
 * @mbox: Mbox controller specific data
 * @drvdata: MHUv2 device specific data
 */
struct mhuv2_drv_data {
	struct mhuv2_link mlink[MHU_V2_CHANS];
	struct mbox_chan chan[MHU_V2_CHANS];
	struct mbox_controller mbox;
	const struct mhuv2_dev_data *drvdata;
};

static irqreturn_t mhuv2_rx_interrupt(int irq, void *p)
{
	struct mbox_chan *chan = p;
	struct mhuv2_link *mlink = chan->con_priv;
	struct mhuv2_drv_data *mhu = mbox_to_mhuv2_drv_data(chan->mbox);
	const struct mhuv2_dev_data *mdata = mhu->drvdata;
	u32 val;

	val = readl_relaxed(mlink->rx_reg + mdata->regs[MHU_V2_REG_STAT]);
	if (!val)
		return IRQ_NONE;

	mbox_chan_received_data(chan, (void *)&val);
	writel_relaxed(val, mlink->rx_reg + mdata->regs[MHU_V2_REG_CLR]);
	return IRQ_HANDLED;
}

static bool mhuv2_last_tx_done(struct mbox_chan *chan)
{
	struct mhuv2_link *mlink = chan->con_priv;
	struct mhuv2_drv_data *mhu = mbox_to_mhuv2_drv_data(chan->mbox);
	const struct mhuv2_dev_data *mdata = mhu->drvdata;
	u32 val = readl_relaxed(mlink->tx_reg + mdata->regs[MHU_V2_REG_STAT]);

	return (val == 0);
}

static int mhuv2_send_data(struct mbox_chan *chan, void *data)
{
	struct mhuv2_link *mlink = chan->con_priv;
	struct mhuv2_drv_data *mhu = mbox_to_mhuv2_drv_data(chan->mbox);
	const struct mhuv2_dev_data *mdata = mhu->drvdata;
	u32 *arg = data;

	writel_relaxed(*arg, mlink->tx_reg + mdata->regs[MHU_V2_REG_SET]);
	return 0;
}

static int mhuv2_startup(struct mbox_chan *chan)
{
	struct mhuv2_link *mlink = chan->con_priv;
	struct mhuv2_drv_data *mhu = mbox_to_mhuv2_drv_data(chan->mbox);
	const struct mhuv2_dev_data *mdata = mhu->drvdata;
	u32 val;
	int ret;

	writel_relaxed(0x1, mlink->tx_reg
		+ (mdata->acc_regs[MHU_V2_REG_ACC_REQ]
		- (mdata->chans[mlink->pchan])));

	val = readl_relaxed(mlink->tx_reg + mdata->regs[MHU_V2_REG_STAT]);
	writel_relaxed(val, mlink->tx_reg + mdata->regs[MHU_V2_REG_CLR]);
	ret = request_irq(mlink->irq, mhuv2_rx_interrupt,
			  IRQF_SHARED, "mhuv2_link", chan);
	if (ret) {
		dev_err(chan->mbox->dev,
			"unable to acquire IRQ %d\n", mlink->irq);
		return ret;
	}
	return 0;
}

static void mhuv2_shutdown(struct mbox_chan *chan)
{
	struct mhuv2_link *mlink = chan->con_priv;
	struct mhuv2_drv_data *mhu = mbox_to_mhuv2_drv_data(chan->mbox);
	const struct mhuv2_dev_data *mdata = mhu->drvdata;

	writel_relaxed(0x0, mlink->tx_reg
		+ (mdata->acc_regs[MHU_V2_REG_ACC_REQ]
		- (mdata->chans[mlink->pchan])));

	free_irq(mlink->irq, chan);
}

static const struct mbox_chan_ops mhuv2_ops = {
	.send_data = mhuv2_send_data,
	.startup = mhuv2_startup,
	.shutdown = mhuv2_shutdown,
	.last_tx_done = mhuv2_last_tx_done,
};

static int mhuv2_probe(struct amba_device *adev, const struct amba_id *id)
{
	int i, err, irq;
	struct mhuv2_drv_data *mhu;
	struct device *dev = &adev->dev;
	void __iomem *rx_base;
	void __iomem *tx_base;
	const struct device_node *np = dev->of_node;
	const struct mhuv2_dev_data *mdata = id->data;
	unsigned int pchans;

	if (!mdata) {
		dev_err(dev, "device data not found\n");
		return -EINVAL;
	}

	/* Allocate memory for device */
	mhu = devm_kzalloc(dev, sizeof(*mhu), GFP_KERNEL);
	if (!mhu)
		return -ENOMEM;

	rx_base = of_iomap((struct device_node *)np, 0);
	if (!rx_base) {
		dev_err(dev, "failed to map rx registers\n");
		return -ENOMEM;
	}

	tx_base = of_iomap((struct device_node *)np, 1);
	if (!tx_base) {
		dev_err(dev, "failed to map tx registers\n");
		return -ENOMEM;
	}

	pchans = readl_relaxed(tx_base
			+ mdata->acc_regs[MHU_V2_REG_MSG_NO_CAP]);
	if (pchans == 0 || pchans > MHU_V2_CHANS) {
		dev_err(dev, "invalid number of channels %d\n", pchans);
		iounmap(tx_base);
		return -EINVAL;
	}

	for (i = 0; i < pchans; i++) {
		mhu->chan[i].con_priv = &mhu->mlink[i];
		mhu->mlink[i].pchan = i;
		irq = mhu->mlink[i].irq = adev->irq[i];

		if (irq <= 0) {
			dev_err(dev, "no IRQ found for channel %d\n", i);
			iounmap(tx_base);
			return -EINVAL;
		}

		mhu->mlink[i].rx_reg = rx_base + mdata->chans[i];
		mhu->mlink[i].tx_reg = tx_base + mdata->chans[i];
	}

	mhu->mbox.dev = dev;
	mhu->mbox.chans = &mhu->chan[0];
	mhu->mbox.num_chans = pchans;
	mhu->mbox.ops = &mhuv2_ops;
	mhu->mbox.txdone_irq = false;
	mhu->mbox.txdone_poll = true;
	mhu->mbox.txpoll_period = 1;
	mhu->drvdata = mdata;

	amba_set_drvdata(adev, mhu);

	err = mbox_controller_register(&mhu->mbox);
	if (err) {
		dev_err(dev, "failed to register mailboxes %d\n", err);
		iounmap(tx_base);
		return err;
	}

	dev_info(dev, "ARM MHUv2 Mailbox driver registered\n");
	return 0;
}

static int mhuv2_remove(struct amba_device *adev)
{
	struct mhuv2_drv_data *mhu = amba_get_drvdata(adev);

	mbox_controller_unregister(&mhu->mbox);
	return 0;
}

static struct mhuv2_dev_data arm_mhuv2_data = {
	.regs = { MHU_V2_REG_STAT_OFS, MHU_V2_REG_SET_OFS, MHU_V2_REG_CLR_OFS },
	.chans = { MHU_V2_LP_OFFSET, MHU_V2_HP_OFFSET },
	.acc_regs = { MHU_V2_REG_MSG_NO_CAP_OFS, MHU_V2_REG_ACC_REQ_OFS,
		MHU_V2_REG_ACC_RDY_OFS },
};

static struct amba_id mhuv2_ids[] = {
	{
		.id     = 0x4b0d1,
		.mask   = 0xfffff,
		.data	= (void *)&arm_mhuv2_data,
	},
	{
		.id     = 0xbb0d1,
		.mask   = 0xfffff,
		.data	= (void *)&arm_mhuv2_data,
	},
	{ 0, 0 },
};
MODULE_DEVICE_TABLE(amba, mhuv2_ids);

static struct amba_driver arm_mhuv2_driver = {
	.drv = {
		.name	= "mhuv2",
	},
	.id_table	= mhuv2_ids,
	.probe		= mhuv2_probe,
	.remove		= mhuv2_remove,
};
module_amba_driver(arm_mhuv2_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ARM MHUv2 Driver");
MODULE_AUTHOR("Samarth Parikh <samarthp@ymail.com>");
