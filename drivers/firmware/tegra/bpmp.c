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

#include <linux/mailbox_client.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/semaphore.h>

#include <soc/tegra/bpmp.h>
#include <soc/tegra/bpmp_abi.h>
#include <soc/tegra/ivc.h>

#define BPMP_MSG_SZ		128
#define BPMP_MSG_DATA_SZ	120

#define __MRQ_ATTRS		0xff000000
#define __MRQ_INDEX(id)		((id) & ~__MRQ_ATTRS)

#define DO_ACK			BIT(0)
#define RING_DOORBELL		BIT(1)

struct tegra_bpmp_soc_data {
	u32 ch_index;		/* channel index */
	u32 thread_ch_index;	/* thread channel index */
	u32 cpu_rx_ch_index;	/* CPU Rx channel index */
	u32 nr_ch;		/* number of total channels */
	u32 nr_thread_ch;	/* number of thread channels */
	u32 ch_timeout;		/* channel timeout */
	u32 thread_ch_timeout;	/* thread channel timeout */
};

struct channel_info {
	u32 tch_free;
	u32 tch_to_complete;
	struct semaphore tch_sem;
};

struct mb_data {
	s32 code;
	s32 flags;
	u8 data[BPMP_MSG_DATA_SZ];
} __packed;

struct channel_data {
	struct mb_data *ib;
	struct mb_data *ob;
};

struct mrq {
	struct list_head list;
	u32 mrq_code;
	bpmp_mrq_handler handler;
	void *data;
};

struct tegra_bpmp {
	struct device *dev;
	const struct tegra_bpmp_soc_data *soc_data;
	void __iomem *tx_base;
	void __iomem *rx_base;
	struct mbox_client cl;
	struct mbox_chan *chan;
	struct ivc *ivc_channels;
	struct channel_data *ch_area;
	struct channel_info ch_info;
	struct completion *ch_completion;
	struct list_head mrq_list;
	struct tegra_bpmp_ops *ops;
	spinlock_t lock;
	bool init_done;
};

static struct tegra_bpmp *bpmp;

static int bpmp_get_thread_ch(int idx)
{
	return bpmp->soc_data->thread_ch_index + idx;
}

static int bpmp_get_thread_ch_index(int ch)
{
	if (ch < bpmp->soc_data->thread_ch_index ||
	    ch >= bpmp->soc_data->cpu_rx_ch_index)
		return -1;
	return ch - bpmp->soc_data->thread_ch_index;
}

static int bpmp_get_ob_channel(void)
{
	return smp_processor_id() + bpmp->soc_data->ch_index;
}

static struct completion *bpmp_get_completion_obj(int ch)
{
	int i = bpmp_get_thread_ch_index(ch);

	return i < 0 ? NULL : bpmp->ch_completion + i;
}

static int bpmp_valid_txfer(void *ob_data, int ob_sz, void *ib_data, int ib_sz)
{
	return ob_sz >= 0 && ob_sz <= BPMP_MSG_DATA_SZ &&
	       ib_sz >= 0 && ib_sz <= BPMP_MSG_DATA_SZ &&
	       (!ob_sz || ob_data) && (!ib_sz || ib_data);
}

static bool bpmp_master_acked(int ch)
{
	struct ivc *ivc_chan;
	void *frame;
	bool ready;

	ivc_chan = bpmp->ivc_channels + ch;
	frame = tegra_ivc_read_get_next_frame(ivc_chan);
	ready = !IS_ERR_OR_NULL(frame);
	bpmp->ch_area[ch].ib = ready ? frame : NULL;

	return ready;
}

static int bpmp_wait_ack(int ch)
{
	ktime_t t;

	t = ns_to_ktime(local_clock());

	do {
		if (bpmp_master_acked(ch))
			return 0;
	} while (ktime_us_delta(ns_to_ktime(local_clock()), t) <
		 bpmp->soc_data->ch_timeout);

	return -ETIMEDOUT;
}

static bool bpmp_master_free(int ch)
{
	struct ivc *ivc_chan;
	void *frame;
	bool ready;

	ivc_chan = bpmp->ivc_channels + ch;
	frame = tegra_ivc_write_get_next_frame(ivc_chan);
	ready = !IS_ERR_OR_NULL(frame);
	bpmp->ch_area[ch].ob = ready ? frame : NULL;

	return ready;
}

static int bpmp_wait_master_free(int ch)
{
	ktime_t t;

	t = ns_to_ktime(local_clock());

	do {
		if (bpmp_master_free(ch))
			return 0;
	} while (ktime_us_delta(ns_to_ktime(local_clock()), t)
		 < bpmp->soc_data->ch_timeout);

	return -ETIMEDOUT;
}

static int __read_ch(int ch, void *data, int sz)
{
	struct ivc *ivc_chan;
	struct mb_data *p;

	ivc_chan = bpmp->ivc_channels + ch;
	p = bpmp->ch_area[ch].ib;
	if (data)
		memcpy_fromio(data, p->data, sz);

	return tegra_ivc_read_advance(ivc_chan);
}

static int bpmp_read_ch(int ch, void *data, int sz)
{
	unsigned long flags;
	int i, ret;

	i = bpmp_get_thread_ch_index(ch);

	spin_lock_irqsave(&bpmp->lock, flags);
	ret = __read_ch(ch, data, sz);
	bpmp->ch_info.tch_free |= (1 << i);
	spin_unlock_irqrestore(&bpmp->lock, flags);

	up(&bpmp->ch_info.tch_sem);

	return ret;
}

static int __write_ch(int ch, int mrq_code, int flags, void *data, int sz)
{
	struct ivc *ivc_chan;
	struct mb_data *p;

	ivc_chan = bpmp->ivc_channels + ch;
	p = bpmp->ch_area[ch].ob;

	p->code = mrq_code;
	p->flags = flags;
	if (data)
		memcpy_toio(p->data, data, sz);

	return tegra_ivc_write_advance(ivc_chan);
}

static int bpmp_write_threaded_ch(int *ch, int mrq_code, void *data, int sz)
{
	unsigned long flags;
	int ret, i;

	ret = down_timeout(&bpmp->ch_info.tch_sem,
			   usecs_to_jiffies(bpmp->soc_data->thread_ch_timeout));
	if (ret)
		return ret;

	spin_lock_irqsave(&bpmp->lock, flags);

	i = __ffs(bpmp->ch_info.tch_free);
	*ch = bpmp_get_thread_ch(i);
	ret = bpmp_master_free(*ch) ? 0 : -EFAULT;
	if (!ret) {
		bpmp->ch_info.tch_free &= ~(1 << i);
		__write_ch(*ch, mrq_code, DO_ACK | RING_DOORBELL, data, sz);
		bpmp->ch_info.tch_to_complete |= (1 << *ch);
	}

	spin_unlock_irqrestore(&bpmp->lock, flags);

	return ret;
}

static int bpmp_write_ch(int ch, int mrq_code, int flags, void *data, int sz)
{
	int ret;

	ret = bpmp_wait_master_free(ch);
	if (ret)
		return ret;

	return __write_ch(ch, mrq_code, flags, data, sz);
}

static int bpmp_send_receive_atomic(int mrq_code, void *ob_data, int ob_sz,
				    void *ib_data, int ib_sz)
{
	int ch, ret;

	if (WARN_ON(!irqs_disabled()))
		return -EPERM;

	if (!bpmp_valid_txfer(ob_data, ob_sz, ib_data, ib_sz))
		return -EINVAL;

	if (!bpmp->init_done)
		return -ENODEV;

	ch = bpmp_get_ob_channel();
	ret = bpmp_write_ch(ch, mrq_code, DO_ACK, ob_data, ob_sz);
	if (ret)
		return ret;

	ret = mbox_send_message(bpmp->chan, NULL);
	if (ret < 0)
		return ret;
	mbox_client_txdone(bpmp->chan, 0);

	ret = bpmp_wait_ack(ch);
	if (ret)
		return ret;

	return __read_ch(ch, ib_data, ib_sz);
}

static int bpmp_send_receive(int mrq_code, void *ob_data, int ob_sz,
			     void *ib_data, int ib_sz)
{
	struct completion *comp_obj;
	unsigned long timeout;
	int ch, ret;

	if (WARN_ON(irqs_disabled()))
		return -EPERM;

	if (!bpmp_valid_txfer(ob_data, ob_sz, ib_data, ib_sz))
		return -EINVAL;

	if (!bpmp->init_done)
		return -ENODEV;

	ret = bpmp_write_threaded_ch(&ch, mrq_code, ob_data, ob_sz);
	if (ret)
		return ret;

	ret = mbox_send_message(bpmp->chan, NULL);
	if (ret < 0)
		return ret;
	mbox_client_txdone(bpmp->chan, 0);

	comp_obj = bpmp_get_completion_obj(ch);
	timeout = usecs_to_jiffies(bpmp->soc_data->thread_ch_timeout);
	if (!wait_for_completion_timeout(comp_obj, timeout))
		return -ETIMEDOUT;

	return bpmp_read_ch(ch, ib_data, ib_sz);
}

static struct mrq *bpmp_find_mrq(u32 mrq_code)
{
	struct mrq *mrq;

	list_for_each_entry(mrq, &bpmp->mrq_list, list) {
		if (mrq_code == mrq->mrq_code)
			return mrq;
	}

	return NULL;
}

static void bpmp_mrq_return_data(int ch, int code, void *data, int sz)
{
	int flags = bpmp->ch_area[ch].ib->flags;
	struct ivc *ivc_chan;
	struct mb_data *frame;
	int ret;

	if (WARN_ON(sz > BPMP_MSG_DATA_SZ))
		return;

	ivc_chan = bpmp->ivc_channels + ch;
	ret = tegra_ivc_read_advance(ivc_chan);
	WARN_ON(ret);

	if (!(flags & DO_ACK))
		return;

	frame = tegra_ivc_write_get_next_frame(ivc_chan);
	if (IS_ERR_OR_NULL(frame)) {
		WARN_ON(1);
		return;
	}

	frame->code = code;
	if (data != NULL)
		memcpy_toio(frame->data, data, sz);
	ret = tegra_ivc_write_advance(ivc_chan);
	WARN_ON(ret);

	if (flags & RING_DOORBELL) {
		ret = mbox_send_message(bpmp->chan, NULL);
		if (ret < 0) {
			WARN_ON(1);
			return;
		}
		mbox_client_txdone(bpmp->chan, 0);
	}
}

static void bpmp_mail_return(int ch, int ret_code, int val)
{
	bpmp_mrq_return_data(ch, ret_code, &val, sizeof(val));
}

static void bpmp_handle_mrq(int mrq_code, int ch)
{
	struct mrq *mrq;

	spin_lock(&bpmp->lock);

	mrq = bpmp_find_mrq(mrq_code);
	if (!mrq) {
		spin_unlock(&bpmp->lock);
		bpmp_mail_return(ch, -EINVAL, 0);
		return;
	}

	mrq->handler(mrq_code, mrq->data, ch);

	spin_unlock(&bpmp->lock);
}

static int bpmp_request_mrq(int mrq_code, bpmp_mrq_handler handler, void *data)
{
	struct mrq *mrq;
	unsigned long flags;

	if (!handler)
		return -EINVAL;

	mrq = devm_kzalloc(bpmp->dev, sizeof(*mrq), GFP_KERNEL);
	if (!mrq)
		return -ENOMEM;

	spin_lock_irqsave(&bpmp->lock, flags);

	mrq->mrq_code = __MRQ_INDEX(mrq_code);
	mrq->handler = handler;
	mrq->data = data;
	list_add(&mrq->list, &bpmp->mrq_list);

	spin_unlock_irqrestore(&bpmp->lock, flags);

	return 0;
}

static void bpmp_mrq_handle_ping(int mrq_code, void *data, int ch)
{
	int challenge;
	int reply;

	challenge = *(int *)bpmp->ch_area[ch].ib->data;
	reply = challenge << (smp_processor_id() + 1);
	bpmp_mail_return(ch, 0, reply);
}

static int bpmp_mailman_init(void)
{
	return bpmp_request_mrq(MRQ_PING, bpmp_mrq_handle_ping, NULL);
}

static int bpmp_ping(void)
{
	unsigned long flags;
	ktime_t t;
	int challenge = 1;
	int reply = 0;
	int ret;

	t = ktime_get();
	local_irq_save(flags);
	ret = bpmp_send_receive_atomic(MRQ_PING, &challenge, sizeof(challenge),
				       &reply, sizeof(reply));
	local_irq_restore(flags);
	t = ktime_sub(ktime_get(), t);

	if (!ret)
		dev_info(bpmp->dev,
			 "ping ok: challenge: %d, reply: %d, time: %lld\n",
			 challenge, reply, ktime_to_us(t));

	return ret;
}

static int bpmp_get_fwtag(void)
{
	unsigned long flags;
	void *vaddr;
	dma_addr_t paddr;
	u32 addr;
	int ret;

	vaddr = dma_alloc_coherent(bpmp->dev, BPMP_MSG_DATA_SZ, &paddr,
				   GFP_KERNEL);
	if (!vaddr)
		return -ENOMEM;
	addr = paddr;

	local_irq_save(flags);
	ret = bpmp_send_receive_atomic(MRQ_QUERY_TAG, &addr, sizeof(addr),
				       NULL, 0);
	local_irq_restore(flags);

	if (!ret)
		dev_info(bpmp->dev, "fwtag: %s\n", (char *)vaddr);

	dma_free_coherent(bpmp->dev, BPMP_MSG_DATA_SZ, vaddr, paddr);

	return ret;
}

static void bpmp_signal_thread(int ch)
{
	int flags = bpmp->ch_area[ch].ob->flags;
	struct completion *comp_obj;

	if (!(flags & RING_DOORBELL))
		return;

	comp_obj = bpmp_get_completion_obj(ch);
	if (!comp_obj) {
		WARN_ON(1);
		return;
	}

	complete(comp_obj);
}

static void bpmp_handle_rx(struct mbox_client *cl, void *data)
{
	int i, rx_ch;

	rx_ch = bpmp->soc_data->cpu_rx_ch_index;

	if (bpmp_master_acked(rx_ch))
		bpmp_handle_mrq(bpmp->ch_area[rx_ch].ib->code, rx_ch);

	spin_lock(&bpmp->lock);

	for (i = 0; i < bpmp->soc_data->nr_thread_ch &&
			bpmp->ch_info.tch_to_complete; i++) {
		int ch = bpmp_get_thread_ch(i);

		if ((bpmp->ch_info.tch_to_complete & (1 << ch)) &&
		    bpmp_master_acked(ch)) {
			bpmp->ch_info.tch_to_complete &= ~(1 << ch);
			bpmp_signal_thread(ch);
		}
	}

	spin_unlock(&bpmp->lock);
}

static void bpmp_ivc_notify(struct ivc *ivc)
{
	int ret;

	ret = mbox_send_message(bpmp->chan, NULL);
	if (ret < 0)
		return;

	mbox_send_message(bpmp->chan, NULL);
}

static int bpmp_msg_chan_init(int ch)
{
	struct ivc *ivc_chan;
	u32 hdr_sz, msg_sz, que_sz;
	uintptr_t rx_base, tx_base;
	int ret;

	msg_sz = tegra_ivc_align(BPMP_MSG_SZ);
	hdr_sz = tegra_ivc_total_queue_size(0);
	que_sz = tegra_ivc_total_queue_size(msg_sz);

	rx_base =  (uintptr_t)(bpmp->rx_base + que_sz * ch);
	tx_base =  (uintptr_t)(bpmp->tx_base + que_sz * ch);

	ivc_chan = bpmp->ivc_channels + ch;
	ret = tegra_ivc_init(ivc_chan, rx_base, DMA_ERROR_CODE, tx_base,
			     DMA_ERROR_CODE, 1, msg_sz, bpmp->dev,
			     bpmp_ivc_notify);
	if (ret) {
		dev_err(bpmp->dev, "%s fail: ch %d returned %d\n",
			__func__, ch, ret);
		return ret;
	}

	/* reset the channel state */
	tegra_ivc_channel_reset(ivc_chan);

	/* sync the channel state with BPMP */
	while (tegra_ivc_channel_notified(ivc_chan))
		;

	return 0;
}

struct tegra_bpmp_ops *tegra_bpmp_get_ops(void)
{
	if (bpmp->init_done && bpmp->ops)
		return bpmp->ops;
	return NULL;
}
EXPORT_SYMBOL(tegra_bpmp_get_ops);

static struct tegra_bpmp_ops bpmp_ops = {
	.send_receive = bpmp_send_receive,
	.send_receive_atomic = bpmp_send_receive_atomic,
	.request_mrq = bpmp_request_mrq,
	.mrq_return = bpmp_mail_return,
};

static const struct tegra_bpmp_soc_data soc_data_tegra186 = {
	.ch_index = 0,
	.thread_ch_index = 6,
	.cpu_rx_ch_index = 13,
	.nr_ch = 14,
	.nr_thread_ch = 7,
	.ch_timeout = 60 * USEC_PER_SEC,
	.thread_ch_timeout = 600 * USEC_PER_SEC,
};

static const struct of_device_id tegra_bpmp_match[] = {
	{ .compatible = "nvidia,tegra186-bpmp", .data = &soc_data_tegra186 },
	{ }
};

static int tegra_bpmp_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct resource shmem_res;
	struct device_node *shmem_np;
	int i, ret;

	bpmp = devm_kzalloc(&pdev->dev, sizeof(*bpmp), GFP_KERNEL);
	if (!bpmp)
		return -ENOMEM;
	bpmp->dev = &pdev->dev;

	match = of_match_device(tegra_bpmp_match, &pdev->dev);
	if (!match)
		return -EINVAL;
	bpmp->soc_data = match->data;

	shmem_np = of_parse_phandle(pdev->dev.of_node, "shmem", 0);
	of_address_to_resource(shmem_np, 0, &shmem_res);
	bpmp->tx_base = devm_ioremap_resource(&pdev->dev, &shmem_res);
	if (IS_ERR(bpmp->tx_base))
		return PTR_ERR(bpmp->tx_base);

	shmem_np = of_parse_phandle(pdev->dev.of_node, "shmem", 1);
	of_address_to_resource(shmem_np, 0, &shmem_res);
	bpmp->rx_base = devm_ioremap_resource(&pdev->dev, &shmem_res);
	if (IS_ERR(bpmp->rx_base))
		return PTR_ERR(bpmp->rx_base);

	bpmp->ivc_channels = devm_kcalloc(&pdev->dev, bpmp->soc_data->nr_ch,
					  sizeof(*bpmp->ivc_channels),
					  GFP_KERNEL);
	if (!bpmp->ivc_channels)
		return -ENOMEM;

	bpmp->ch_area = devm_kcalloc(&pdev->dev, bpmp->soc_data->nr_ch,
				     sizeof(*bpmp->ch_area), GFP_KERNEL);
	if (!bpmp->ch_area)
		return -ENOMEM;

	bpmp->ch_completion = devm_kcalloc(&pdev->dev,
					   bpmp->soc_data->nr_thread_ch,
					   sizeof(*bpmp->ch_completion),
					   GFP_KERNEL);
	if (!bpmp->ch_completion)
		return -ENOMEM;

	/* mbox registration */
	bpmp->cl.dev = &pdev->dev;
	bpmp->cl.rx_callback = bpmp_handle_rx;
	bpmp->cl.tx_block = false;
	bpmp->cl.knows_txdone = false;
	bpmp->chan = mbox_request_channel(&bpmp->cl, 0);
	if (IS_ERR(bpmp->chan)) {
		if (PTR_ERR(bpmp->chan) != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"fail to get HSP mailbox, bpmp init fail.\n");
		return PTR_ERR(bpmp->chan);
	}

	/* message channel initialization */
	for (i = 0; i < bpmp->soc_data->nr_ch; i++) {
		struct completion *comp_obj;

		ret = bpmp_msg_chan_init(i);
		if (ret)
			return ret;

		comp_obj = bpmp_get_completion_obj(i);
		if (comp_obj)
			init_completion(comp_obj);
	}

	bpmp->ch_info.tch_free = (1 << bpmp->soc_data->nr_thread_ch) - 1;
	sema_init(&bpmp->ch_info.tch_sem, bpmp->soc_data->nr_thread_ch);

	spin_lock_init(&bpmp->lock);
	INIT_LIST_HEAD(&bpmp->mrq_list);
	if (bpmp_mailman_init())
		return -ENODEV;

	bpmp->init_done = true;

	ret = bpmp_ping();
	if (ret)
		dev_err(&pdev->dev, "ping failed: %d\n", ret);

	ret = bpmp_get_fwtag();
	if (ret)
		dev_err(&pdev->dev, "get fwtag failed: %d\n", ret);

	/* BPMP is ready now. */
	bpmp->ops = &bpmp_ops;

	return 0;
}

static struct platform_driver tegra_bpmp_driver = {
	.driver = {
		.name = "tegra-bpmp",
		.of_match_table = tegra_bpmp_match,
	},
	.probe = tegra_bpmp_probe,
};

static int __init tegra_bpmp_init(void)
{
	return platform_driver_register(&tegra_bpmp_driver);
}
core_initcall(tegra_bpmp_init);
