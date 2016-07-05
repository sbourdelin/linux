/*
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mailbox_controller.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <dt-bindings/mailbox/tegra186-hsp.h>

#define HSP_INT_DIMENSIONING	0x380
#define HSP_nSM_OFFSET		0
#define HSP_nSS_OFFSET		4
#define HSP_nAS_OFFSET		8
#define HSP_nDB_OFFSET		12
#define HSP_nSI_OFFSET		16
#define HSP_nINT_MASK		0xf

#define HSP_DB_REG_TRIGGER	0x0
#define HSP_DB_REG_ENABLE	0x4
#define HSP_DB_REG_RAW		0x8
#define HSP_DB_REG_PENDING	0xc

#define HSP_DB_CCPLEX		1
#define HSP_DB_BPMP		3

#define MAX_NUM_HSP_CHAN 32
#define MAX_NUM_HSP_DB 7

#define hsp_db_offset(i, d) \
	(d->base + ((1 + (d->nr_sm >> 1) + d->nr_ss + d->nr_as) << 16) + \
	(i) * 0x100)

struct tegra_hsp_db_chan {
	int master_id;
	int db_id;
};

struct tegra_hsp_mbox_chan {
	int type;
	union {
		struct tegra_hsp_db_chan db_chan;
	};
};

struct tegra_hsp_mbox {
	struct mbox_controller *mbox;
	void __iomem *base;
	void __iomem *db_base[MAX_NUM_HSP_DB];
	int db_irq;
	int nr_sm;
	int nr_as;
	int nr_ss;
	int nr_db;
	int nr_si;
	spinlock_t lock;
};

static inline u32 hsp_readl(void __iomem *base, int reg)
{
	return readl(base + reg);
}

static inline void hsp_writel(void __iomem *base, int reg, u32 val)
{
	writel(val, base + reg);
	readl(base + reg);
}

static int hsp_db_can_ring(void __iomem *db_base)
{
	u32 reg;

	reg = hsp_readl(db_base, HSP_DB_REG_ENABLE);

	return !!(reg & BIT(HSP_DB_MASTER_CCPLEX));
}

static irqreturn_t hsp_db_irq(int irq, void *p)
{
	struct tegra_hsp_mbox *hsp_mbox = p;
	ulong val;
	int master_id;

	val = (ulong)hsp_readl(hsp_mbox->db_base[HSP_DB_CCPLEX],
			       HSP_DB_REG_PENDING);
	hsp_writel(hsp_mbox->db_base[HSP_DB_CCPLEX], HSP_DB_REG_PENDING, val);

	spin_lock(&hsp_mbox->lock);
	for_each_set_bit(master_id, &val, MAX_NUM_HSP_CHAN) {
		struct mbox_chan *chan;
		struct tegra_hsp_mbox_chan *mchan;
		int i;

		for (i = 0; i < MAX_NUM_HSP_CHAN; i++) {
			chan = &hsp_mbox->mbox->chans[i];

			if (!chan->con_priv)
				continue;

			mchan = chan->con_priv;
			if (mchan->type == HSP_MBOX_TYPE_DB &&
			    mchan->db_chan.master_id == master_id)
				break;
			chan = NULL;
		}

		if (chan)
			mbox_chan_received_data(chan, NULL);
	}
	spin_unlock(&hsp_mbox->lock);

	return IRQ_HANDLED;
}

static int hsp_db_send_data(struct mbox_chan *chan, void *data)
{
	struct tegra_hsp_mbox_chan *mchan = chan->con_priv;
	struct tegra_hsp_db_chan *db_chan = &mchan->db_chan;
	struct tegra_hsp_mbox *hsp_mbox = dev_get_drvdata(chan->mbox->dev);

	hsp_writel(hsp_mbox->db_base[db_chan->db_id], HSP_DB_REG_TRIGGER, 1);

	return 0;
}

static int hsp_db_startup(struct mbox_chan *chan)
{
	struct tegra_hsp_mbox_chan *mchan = chan->con_priv;
	struct tegra_hsp_db_chan *db_chan = &mchan->db_chan;
	struct tegra_hsp_mbox *hsp_mbox = dev_get_drvdata(chan->mbox->dev);
	u32 val;
	unsigned long flag;

	if (db_chan->master_id >= MAX_NUM_HSP_CHAN) {
		dev_err(chan->mbox->dev, "invalid HSP chan: master ID: %d\n",
			db_chan->master_id);
		return -EINVAL;
	}

	spin_lock_irqsave(&hsp_mbox->lock, flag);
	val = hsp_readl(hsp_mbox->db_base[HSP_DB_CCPLEX], HSP_DB_REG_ENABLE);
	val |= BIT(db_chan->master_id);
	hsp_writel(hsp_mbox->db_base[HSP_DB_CCPLEX], HSP_DB_REG_ENABLE, val);
	spin_unlock_irqrestore(&hsp_mbox->lock, flag);

	if (!hsp_db_can_ring(hsp_mbox->db_base[db_chan->db_id]))
		return -ENODEV;

	return 0;
}

static void hsp_db_shutdown(struct mbox_chan *chan)
{
	struct tegra_hsp_mbox_chan *mchan = chan->con_priv;
	struct tegra_hsp_db_chan *db_chan = &mchan->db_chan;
	struct tegra_hsp_mbox *hsp_mbox = dev_get_drvdata(chan->mbox->dev);
	u32 val;
	unsigned long flag;

	spin_lock_irqsave(&hsp_mbox->lock, flag);
	val = hsp_readl(hsp_mbox->db_base[HSP_DB_CCPLEX], HSP_DB_REG_ENABLE);
	val &= ~BIT(db_chan->master_id);
	hsp_writel(hsp_mbox->db_base[HSP_DB_CCPLEX], HSP_DB_REG_ENABLE, val);
	spin_unlock_irqrestore(&hsp_mbox->lock, flag);
}

static bool hsp_db_last_tx_done(struct mbox_chan *chan)
{
	return true;
}

static int tegra_hsp_db_init(struct tegra_hsp_mbox *hsp_mbox,
			     struct mbox_chan *mchan, int master_id)
{
	struct platform_device *pdev = to_platform_device(hsp_mbox->mbox->dev);
	struct tegra_hsp_mbox_chan *hsp_mbox_chan;
	int ret;

	if (!hsp_mbox->db_irq) {
		int i;

		hsp_mbox->db_irq = platform_get_irq_byname(pdev, "doorbell");
		ret = devm_request_irq(&pdev->dev, hsp_mbox->db_irq,
				       hsp_db_irq, IRQF_NO_SUSPEND,
				       dev_name(&pdev->dev), hsp_mbox);
		if (ret)
			return ret;

		for (i = 0; i < MAX_NUM_HSP_DB; i++)
			hsp_mbox->db_base[i] = hsp_db_offset(i, hsp_mbox);
	}

	hsp_mbox_chan = devm_kzalloc(&pdev->dev, sizeof(*hsp_mbox_chan),
				     GFP_KERNEL);
	if (!hsp_mbox_chan)
		return -ENOMEM;

	hsp_mbox_chan->type = HSP_MBOX_TYPE_DB;
	hsp_mbox_chan->db_chan.master_id = master_id;
	switch (master_id) {
	case HSP_DB_MASTER_BPMP:
		hsp_mbox_chan->db_chan.db_id = HSP_DB_BPMP;
		break;
	default:
		hsp_mbox_chan->db_chan.db_id = MAX_NUM_HSP_DB;
		break;
	}

	mchan->con_priv = hsp_mbox_chan;

	return 0;
}

static int hsp_send_data(struct mbox_chan *chan, void *data)
{
	struct tegra_hsp_mbox_chan *hsp_mbox_chan = chan->con_priv;
	int ret = 0;

	switch (hsp_mbox_chan->type) {
	case HSP_MBOX_TYPE_DB:
		ret = hsp_db_send_data(chan, data);
		break;
	default:
		break;
	}

	return ret;
}

static int hsp_startup(struct mbox_chan *chan)
{
	struct tegra_hsp_mbox_chan *hsp_mbox_chan = chan->con_priv;
	int ret = 0;

	switch (hsp_mbox_chan->type) {
	case HSP_MBOX_TYPE_DB:
		ret = hsp_db_startup(chan);
		break;
	default:
		break;
	}

	return ret;
}

static void hsp_shutdown(struct mbox_chan *chan)
{
	struct tegra_hsp_mbox_chan *hsp_mbox_chan = chan->con_priv;

	switch (hsp_mbox_chan->type) {
	case HSP_MBOX_TYPE_DB:
		hsp_db_shutdown(chan);
		break;
	default:
		break;
	}

	chan->con_priv = NULL;
}

static bool hsp_last_tx_done(struct mbox_chan *chan)
{
	struct tegra_hsp_mbox_chan *hsp_mbox_chan = chan->con_priv;
	bool ret = true;

	switch (hsp_mbox_chan->type) {
	case HSP_MBOX_TYPE_DB:
		ret = hsp_db_last_tx_done(chan);
		break;
	default:
		break;
	}

	return ret;
}

static const struct mbox_chan_ops tegra_hsp_ops = {
	.send_data = hsp_send_data,
	.startup = hsp_startup,
	.shutdown = hsp_shutdown,
	.last_tx_done = hsp_last_tx_done,
};

static const struct of_device_id tegra_hsp_match[] = {
	{ .compatible = "nvidia,tegra186-hsp" },
	{ }
};

static struct mbox_chan *
of_hsp_mbox_xlate(struct mbox_controller *mbox,
		  const struct of_phandle_args *sp)
{
	int mbox_id = sp->args[0];
	int hsp_type = (mbox_id >> 16) & 0xf;
	int master_id = mbox_id & 0xff;
	struct tegra_hsp_mbox *hsp_mbox = dev_get_drvdata(mbox->dev);
	struct mbox_chan *free_chan;
	int i, ret = 0;

	spin_lock(&hsp_mbox->lock);

	for (i = 0; i < mbox->num_chans; i++) {
		free_chan = &mbox->chans[i];
		if (!free_chan->con_priv)
			break;
		free_chan = NULL;
	}

	if (!free_chan) {
		spin_unlock(&hsp_mbox->lock);
		return ERR_PTR(-EFAULT);
	}

	switch (hsp_type) {
	case HSP_MBOX_TYPE_DB:
		ret = tegra_hsp_db_init(hsp_mbox, free_chan, master_id);
		break;
	default:
		break;
	}

	spin_unlock(&hsp_mbox->lock);

	if (ret)
		free_chan = ERR_PTR(-EFAULT);

	return free_chan;
}

static int tegra_hsp_probe(struct platform_device *pdev)
{
	struct tegra_hsp_mbox *hsp_mbox;
	struct resource *res;
	int ret = 0;
	u32 reg;

	hsp_mbox = devm_kzalloc(&pdev->dev, sizeof(*hsp_mbox), GFP_KERNEL);
	if (!hsp_mbox)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	hsp_mbox->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(hsp_mbox->base))
		return PTR_ERR(hsp_mbox->base);

	reg = hsp_readl(hsp_mbox->base, HSP_INT_DIMENSIONING);
	hsp_mbox->nr_sm = (reg >> HSP_nSM_OFFSET) & HSP_nINT_MASK;
	hsp_mbox->nr_ss = (reg >> HSP_nSS_OFFSET) & HSP_nINT_MASK;
	hsp_mbox->nr_as = (reg >> HSP_nAS_OFFSET) & HSP_nINT_MASK;
	hsp_mbox->nr_db = (reg >> HSP_nDB_OFFSET) & HSP_nINT_MASK;
	hsp_mbox->nr_si = (reg >> HSP_nSI_OFFSET) & HSP_nINT_MASK;

	hsp_mbox->mbox = devm_kzalloc(&pdev->dev,
				      sizeof(*hsp_mbox->mbox), GFP_KERNEL);
	if (!hsp_mbox->mbox)
		return -ENOMEM;

	hsp_mbox->mbox->chans =
		devm_kcalloc(&pdev->dev, MAX_NUM_HSP_CHAN,
			     sizeof(*hsp_mbox->mbox->chans), GFP_KERNEL);
	if (!hsp_mbox->mbox->chans)
		return -ENOMEM;

	hsp_mbox->mbox->of_xlate = of_hsp_mbox_xlate;
	hsp_mbox->mbox->num_chans = MAX_NUM_HSP_CHAN;
	hsp_mbox->mbox->dev = &pdev->dev;
	hsp_mbox->mbox->txdone_irq = false;
	hsp_mbox->mbox->txdone_poll = false;
	hsp_mbox->mbox->ops = &tegra_hsp_ops;
	platform_set_drvdata(pdev, hsp_mbox);

	ret = mbox_controller_register(hsp_mbox->mbox);
	if (ret) {
		pr_err("tegra-hsp mbox: fail to register mailbox %d.\n", ret);
		return ret;
	}

	spin_lock_init(&hsp_mbox->lock);

	return 0;
}

static int tegra_hsp_remove(struct platform_device *pdev)
{
	struct tegra_hsp_mbox *hsp_mbox = platform_get_drvdata(pdev);

	if (hsp_mbox->mbox)
		mbox_controller_unregister(hsp_mbox->mbox);

	return 0;
}

static struct platform_driver tegra_hsp_driver = {
	.driver = {
		.name = "tegra-hsp",
		.of_match_table = tegra_hsp_match,
	},
	.probe = tegra_hsp_probe,
	.remove = tegra_hsp_remove,
};

static int __init tegra_hsp_init(void)
{
	return platform_driver_register(&tegra_hsp_driver);
}
core_initcall(tegra_hsp_init);
