// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare eDMA core driver
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/pm_runtime.h>
#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/dma/edma.h>

#include "dw-edma-core.h"
#include "dw-edma-v0-core.h"
#include "../dmaengine.h"
#include "../virt-dma.h"

#define SET(reg, name, val)			\
	reg.name = val

#define SET_BOTH_CH(name, value)		\
	do {					\
		SET(dw->wr_edma, name, value);	\
		SET(dw->rd_edma, name, value);	\
	} while (0)

static const struct dw_edma_core_ops dw_edma_v0_core_ops = {
	/* eDMA management callbacks */
	.off = dw_edma_v0_core_off,
	.ch_count = dw_edma_v0_core_ch_count,
	.ch_status = dw_edma_v0_core_ch_status,
	.clear_done_int = dw_edma_v0_core_clear_done_int,
	.clear_abort_int = dw_edma_v0_core_clear_abort_int,
	.status_done_int = dw_edma_v0_core_status_done_int,
	.status_abort_int = dw_edma_v0_core_status_abort_int,
	.start = dw_edma_v0_core_start,
	.device_config = dw_edma_v0_core_device_config,
	/* eDMA debug fs callbacks */
	.debugfs_on = dw_edma_v0_core_debugfs_on,
	.debugfs_off = dw_edma_v0_core_debugfs_off,
};

static inline
struct device *dchan2dev(struct dma_chan *dchan)
{
	return &dchan->dev->device;
}

static inline
struct device *chan2dev(struct dw_edma_chan *chan)
{
	return &chan->vc.chan.dev->device;
}

static inline
const struct dw_edma_core_ops *chan2ops(struct dw_edma_chan *chan)
{
	return chan->chip->dw->ops;
}

static inline
struct dw_edma_desc *vd2dw_edma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct dw_edma_desc, vd);
}

static struct dw_edma_burst *dw_edma_alloc_burst(struct dw_edma_chunk *chunk)
{
	struct dw_edma_chan *chan = chunk->chan;
	struct dw_edma_burst *burst;

	burst = kzalloc(sizeof(*burst), GFP_NOWAIT);
	if (unlikely(!burst))
		return NULL;

	INIT_LIST_HEAD(&burst->list);

	if (chunk->burst) {
		chunk->bursts_alloc++;
		dev_dbg(chan2dev(chan), ": alloc new burst element (%d)\n",
			chunk->bursts_alloc);
		list_add_tail(&burst->list, &chunk->burst->list);
	} else {
		chunk->bursts_alloc = 0;
		chunk->burst = burst;
		dev_dbg(chan2dev(chan), ": alloc new burst head\n");
	}

	return burst;
}

static struct dw_edma_chunk *dw_edma_alloc_chunk(struct dw_edma_desc *desc)
{
	struct dw_edma_chan *chan = desc->chan;
	struct dw_edma *dw = chan->chip->dw;
	struct dw_edma_chunk *chunk;

	chunk = kzalloc(sizeof(*chunk), GFP_NOWAIT);
	if (unlikely(!chunk))
		return NULL;

	INIT_LIST_HEAD(&chunk->list);
	chunk->chan = chan;
	chunk->cb = !(desc->chunks_alloc % 2);
	chunk->p_addr = (dma_addr_t)(dw->pa_ll + chan->ll_off);
	chunk->v_addr = (dma_addr_t)(dw->va_ll + chan->ll_off);

	if (desc->chunk) {
		desc->chunks_alloc++;
		dev_dbg(chan2dev(chan), ": alloc new chunk element (%d)\n",
			desc->chunks_alloc);
		list_add_tail(&chunk->list, &desc->chunk->list);
		dw_edma_alloc_burst(chunk);
	} else {
		chunk->burst = NULL;
		desc->chunks_alloc = 0;
		desc->chunk = chunk;
		dev_dbg(chan2dev(chan), ": alloc new chunk head\n");
	}

	return chunk;
}

static struct dw_edma_desc *dw_edma_alloc_desc(struct dw_edma_chan *chan)
{
	struct dw_edma_desc *desc;

	dev_dbg(chan2dev(chan), ": alloc new descriptor\n");

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (unlikely(!desc))
		return NULL;

	desc->chan = chan;
	dw_edma_alloc_chunk(desc);

	return desc;
}

static void dw_edma_free_burst(struct dw_edma_chunk *chunk)
{
	struct dw_edma_burst *child, *_next;

	if (!chunk->burst)
		return;

	/* Remove all the list elements */
	list_for_each_entry_safe(child, _next, &chunk->burst->list, list) {
		list_del(&child->list);
		kfree(child);
		chunk->bursts_alloc--;
	}

	/* Remove the list head */
	kfree(child);
	chunk->burst = NULL;
}

static void dw_edma_free_chunk(struct dw_edma_desc *desc)
{
	struct dw_edma_chan *chan = desc->chan;
	struct dw_edma_chunk *child, *_next;

	if (!desc->chunk)
		return;

	/* Remove all the list elements */
	list_for_each_entry_safe(child, _next, &desc->chunk->list, list) {
		dw_edma_free_burst(child);
		if (child->bursts_alloc)
			dev_dbg(chan2dev(chan),	": %d bursts still allocated\n",
				child->bursts_alloc);
		list_del(&child->list);
		kfree(child);
		desc->chunks_alloc--;
	}

	/* Remove the list head */
	kfree(child);
	desc->chunk = NULL;
}

static void dw_edma_free_desc(struct dw_edma_desc *desc)
{
	struct dw_edma_chan *chan = desc->chan;
	unsigned long flags;

	spin_lock_irqsave(&chan->vc.lock, flags);

	dw_edma_free_chunk(desc);
	if (desc->chunks_alloc)
		dev_dbg(chan2dev(chan), ": %d chunks still allocated\n",
			desc->chunks_alloc);

	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static void vchan_free_desc(struct virt_dma_desc *vdesc)
{
	dw_edma_free_desc(vd2dw_edma_desc(vdesc));
}

static void dw_edma_start_transfer(struct dw_edma_chan *chan)
{
	struct virt_dma_desc *vd;
	struct dw_edma_desc *desc;
	struct dw_edma_chunk *child, *_next;
	const struct dw_edma_core_ops *ops = chan2ops(chan);

	vd = vchan_next_desc(&chan->vc);
	if (!vd)
		return;

	desc = vd2dw_edma_desc(vd);
	if (!desc)
		return;

	list_for_each_entry_safe(child, _next, &desc->chunk->list, list) {
		ops->start(child, !desc->xfer_sz);
		desc->xfer_sz += child->sz;
		dev_dbg(chan2dev(chan), ": transfer of %u bytes started\n", child->sz);

		dw_edma_free_burst(child);
		if (child->bursts_alloc)
			dev_dbg(chan2dev(chan),	": %d bursts still allocated\n",
				child->bursts_alloc);
		list_del(&child->list);
		kfree(child);
		desc->chunks_alloc--;

		return;
	}
}

static int dw_edma_device_config(struct dma_chan *dchan,
				 struct dma_slave_config *config)
{
	struct dw_edma_chan *chan = dchan2dw_edma_chan(dchan);
	const struct dw_edma_core_ops *ops = chan2ops(chan);
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&chan->vc.lock, flags);

	if (!config) {
		err = -EINVAL;
		goto err_config;
	}

	if (chan->configured) {
		dev_err(chan2dev(chan), ": channel already configured\n");
		err = -EPERM;
		goto err_config;
	}

	dev_dbg(chan2dev(chan), ": src_addr(physical) = 0x%.16x\n",
		config->src_addr);
	dev_dbg(chan2dev(chan), ": dst_addr(physical) = 0x%.16x\n",
		config->dst_addr);

	err = ops->device_config(dchan);
	if (!err) {
		chan->configured = true;
		dev_dbg(chan2dev(chan),	": channel configured\n");
	}

err_config:
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	return err;
}

static int dw_edma_device_pause(struct dma_chan *dchan)
{
	struct dw_edma_chan *chan = dchan2dw_edma_chan(dchan);
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&chan->vc.lock, flags);

	if (!chan->configured) {
		dev_err(dchan2dev(dchan), ": channel not configured\n");
		err = -EPERM;
		goto err_pause;
	}

	if (chan->status != EDMA_ST_BUSY) {
		err = -EPERM;
		goto err_pause;
	}

	if (chan->request != EDMA_REQ_NONE) {
		err = -EPERM;
		goto err_pause;
	}

	chan->request = EDMA_REQ_PAUSE;
	dev_dbg(dchan2dev(dchan), ": pause requested\n");

err_pause:
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	return err;
}

static int dw_edma_device_resume(struct dma_chan *dchan)
{
	struct dw_edma_chan *chan = dchan2dw_edma_chan(dchan);
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&chan->vc.lock, flags);

	if (!chan->configured) {
		dev_err(dchan2dev(dchan), ": channel not configured\n");
		err = -EPERM;
		goto err_resume;
	}

	if (chan->status != EDMA_ST_PAUSE) {
		err = -EPERM;
		goto err_resume;
	}

	if (chan->request != EDMA_REQ_NONE) {
		err = -EPERM;
		goto err_resume;
	}

	chan->status = EDMA_ST_BUSY;
	dev_dbg(dchan2dev(dchan), ": transfer resumed\n");
	dw_edma_start_transfer(chan);

err_resume:
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	return err;
}

static int dw_edma_device_terminate_all(struct dma_chan *dchan)
{
	struct dw_edma_chan *chan = dchan2dw_edma_chan(dchan);
	unsigned long flags;
	int err = 0;
	LIST_HEAD(head);

	spin_lock_irqsave(&chan->vc.lock, flags);

	if (!chan->configured) {
		dev_err(dchan2dev(dchan), ": channel not configured\n");
		err = -EPERM;
		goto err_terminate;
	}

	if (chan->status == EDMA_ST_PAUSE) {
		dev_dbg(dchan2dev(dchan), ": channel is paused, stopping immediately\n");
		vchan_get_all_descriptors(&chan->vc, &head);
		vchan_dma_desc_free_list(&chan->vc, &head);
		chan->status = EDMA_ST_IDLE;
		goto err_terminate;
	} else if (chan->status != EDMA_ST_BUSY) {
		err = -EPERM;
		goto err_terminate;
	}

	if (chan->request > EDMA_REQ_PAUSE) {
		err = -EPERM;
		goto err_terminate;
	}

	chan->request = EDMA_REQ_STOP;
	dev_dbg(dchan2dev(dchan), ": termination requested\n");

err_terminate:
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	return err;
}

static void dw_edma_device_issue_pending(struct dma_chan *dchan)
{
	struct dw_edma_chan *chan = dchan2dw_edma_chan(dchan);
	unsigned long flags;

	spin_lock_irqsave(&chan->vc.lock, flags);

	if (chan->configured && chan->request == EDMA_REQ_NONE &&
	    chan->status == EDMA_ST_IDLE && vchan_issue_pending(&chan->vc)) {
		dev_dbg(dchan2dev(dchan), ": transfer issued\n");
		chan->status = EDMA_ST_BUSY;
		dw_edma_start_transfer(chan);
	}

	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static enum dma_status
dw_edma_device_tx_status(struct dma_chan *dchan, dma_cookie_t cookie,
			 struct dma_tx_state *txstate)
{
	struct dw_edma_chan *chan = dchan2dw_edma_chan(dchan);
	const struct dw_edma_core_ops *ops = chan2ops(chan);
	unsigned long flags;
	enum dma_status ret;

	spin_lock_irqsave(&chan->vc.lock, flags);

	ret = ops->ch_status(chan);
	if (ret == DMA_ERROR) {
		goto ret_status;
	} else if (ret == DMA_IN_PROGRESS) {
		chan->status = EDMA_ST_BUSY;
		goto ret_status;
	} else {
		/* DMA_COMPLETE */
		if (chan->status == EDMA_ST_PAUSE)
			ret = DMA_PAUSED;
		else if (chan->status == EDMA_ST_BUSY)
			ret = DMA_IN_PROGRESS;
		else
			ret = DMA_COMPLETE;
	}

ret_status:
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	dma_set_residue(txstate, 0);

	return ret;
}

static struct dma_async_tx_descriptor *
dw_edma_device_prep_slave_sg(struct dma_chan *dchan, struct scatterlist *sgl,
			     unsigned int sg_len,
			     enum dma_transfer_direction direction,
			     unsigned long flags, void *context)
{
	struct dw_edma_chan *chan = dchan2dw_edma_chan(dchan);
	struct dw_edma_desc *desc;
	struct dw_edma_chunk *chunk;
	struct dw_edma_burst *burst;
	struct scatterlist *sg;
	dma_addr_t dev_addr = chan->p_addr;
	unsigned long sflags;
	int i;

	if (sg_len < 1) {
		dev_err(chan2dev(chan), ": invalid sg length %u\n", sg_len);
		return NULL;
	}

	if (direction == DMA_DEV_TO_MEM && chan->dir == EDMA_DIR_WRITE) {
		dev_dbg(chan2dev(chan),	": prepare operation (WRITE)\n");
	} else if (direction == DMA_MEM_TO_DEV && chan->dir == EDMA_DIR_READ) {
		dev_dbg(chan2dev(chan),	": prepare operation (READ)\n");
	} else {
		dev_err(chan2dev(chan), ": invalid direction\n");
		return NULL;
	}

	if (!chan->configured) {
		dev_err(dchan2dev(dchan), ": channel not configured\n");
		return NULL;
	}
	if (chan->status == EDMA_ST_BUSY) {
		dev_err(chan2dev(chan), ": channel is busy or paused\n");
		return NULL;
	}

	spin_lock_irqsave(&chan->vc.lock, sflags);

	desc = dw_edma_alloc_desc(chan);
	if (unlikely(!desc))
		goto err_alloc;

	chunk = dw_edma_alloc_chunk(desc);
	if (unlikely(!chunk))
		goto err_alloc;

	for_each_sg(sgl, sg, sg_len, i) {
		if (chunk->bursts_alloc == chan->ll_max) {
			chunk = dw_edma_alloc_chunk(desc);
			if (unlikely(!chunk))
				goto err_alloc;
		}

		burst = dw_edma_alloc_burst(chunk);

		if (unlikely(!burst))
			goto err_alloc;

		if (direction == DMA_MEM_TO_DEV) {
			burst->sar = sg_dma_address(sg);
			burst->dar = dev_addr;
		} else {
			burst->sar = dev_addr;
			burst->dar = sg_dma_address(sg);
		}

		burst->sz = sg_dma_len(sg);
		chunk->sz += burst->sz;
		desc->alloc_sz += burst->sz;
		dev_addr += burst->sz;

		dev_dbg(chan2dev(chan), "lli %u/%u, sar=0x%.16llx, dar=0x%.16llx, size=%u bytes\n",
			i + 1, sg_len,
			burst->sar, burst->dar,
			burst->sz);
	}

	spin_unlock_irqrestore(&chan->vc.lock, sflags);
	return vchan_tx_prep(&chan->vc, &desc->vd, flags);

err_alloc:
	spin_unlock_irqrestore(&chan->vc.lock, sflags);
	if (desc)
		dw_edma_free_desc(desc);
	return NULL;
}

static void dw_edma_done_interrupt(struct dw_edma_chan *chan)
{
	struct dw_edma *dw = chan->chip->dw;
	const struct dw_edma_core_ops *ops = dw->ops;
	struct virt_dma_desc *vd;
	struct dw_edma_desc *desc;
	unsigned long flags;

	ops->clear_done_int(chan);
	dev_dbg(chan2dev(chan), ": clear done interrupt\n");

	spin_lock_irqsave(&chan->vc.lock, flags);
	vd = vchan_next_desc(&chan->vc);
	switch (chan->request) {
	case EDMA_REQ_NONE:
		desc = vd2dw_edma_desc(vd);
		if (desc->chunks_alloc) {
			dev_dbg(chan2dev(chan),	": sub-transfer complete\n");
			chan->status = EDMA_ST_BUSY;
			dev_dbg(chan2dev(chan), ": transferred %u bytes\n",
				desc->xfer_sz);
			dw_edma_start_transfer(chan);
		} else {
			list_del(&vd->node);
			vchan_cookie_complete(vd);
			chan->status = EDMA_ST_IDLE;
			dev_dbg(chan2dev(chan),	": transfer complete\n");
		}
		break;
	case EDMA_REQ_STOP:
		list_del(&vd->node);
		vchan_cookie_complete(vd);
		chan->request = EDMA_REQ_NONE;
		chan->status = EDMA_ST_IDLE;
		dev_dbg(chan2dev(chan),	": transfer stop\n");
		break;
	case EDMA_REQ_PAUSE:
		chan->request = EDMA_REQ_NONE;
		chan->status = EDMA_ST_PAUSE;
		break;
	default:
		dev_err(chan2dev(chan), ": invalid status state\n");
		break;
	}
	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static void dw_edma_abort_interrupt(struct dw_edma_chan *chan)
{
	struct dw_edma *dw = chan->chip->dw;
	const struct dw_edma_core_ops *ops = dw->ops;
	struct virt_dma_desc *vd;
	unsigned long flags;

	ops->clear_abort_int(chan);
	dev_dbg(chan2dev(chan), ": clear abort interrupt\n");

	spin_lock_irqsave(&chan->vc.lock, flags);
	vd = vchan_next_desc(&chan->vc);
	list_del(&vd->node);
	vchan_cookie_complete(vd);
	chan->request = EDMA_REQ_NONE;
	chan->status = EDMA_ST_IDLE;

	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static irqreturn_t dw_edma_interrupt(int irq, void *data)
{
	struct dw_edma_chip *chip = data;
	struct dw_edma *dw = chip->dw;
	const struct dw_edma_core_ops *ops = dw->ops;
	struct dw_edma_chan *chan;
	u32 i;

	/* Poll, clear and process every chanel interrupt status */
	for (i = 0; i < (dw->wr_ch_count + dw->rd_ch_count); i++) {
		chan = &dw->chan[i];

		if (ops->status_done_int(chan))
			dw_edma_done_interrupt(chan);

		if (ops->status_abort_int(chan))
			dw_edma_abort_interrupt(chan);
	}

	return IRQ_HANDLED;
}

static int dw_edma_alloc_chan_resources(struct dma_chan *dchan)
{
	struct dw_edma_chan *chan = dchan2dw_edma_chan(dchan);

	if (chan->status != EDMA_ST_IDLE) {
		dev_err(chan2dev(chan), ": channel is busy\n");
		return -EBUSY;
	}

	dev_dbg(dchan2dev(dchan), ": allocated\n");

	pm_runtime_get(chan->chip->dev);

	return 0;
}

static void dw_edma_free_chan_resources(struct dma_chan *dchan)
{
	struct dw_edma_chan *chan = dchan2dw_edma_chan(dchan);
	unsigned long timeout = jiffies + msecs_to_jiffies(5000);
	int ret;

	if (chan->status != EDMA_ST_IDLE)
		dev_err(chan2dev(chan), ": channel is busy\n");

	do {
		ret = dw_edma_device_terminate_all(dchan);
		if (!ret)
			break;

		if (time_after_eq(jiffies, timeout)) {
			dev_err(chan2dev(chan), ": timeout\n");
			return;
		}

		cpu_relax();
	} while (1);

	dev_dbg(dchan2dev(dchan), ": freed\n");

	pm_runtime_put(chan->chip->dev);
}

int dw_edma_probe(struct dw_edma_chip *chip)
{
	struct dw_edma *dw = chip->dw;
	const struct dw_edma_core_ops *ops;
	size_t ll_chunk = dw->ll_sz;
	int i, j, err;

	raw_spin_lock_init(&dw->lock);

	switch (dw->version) {
	case 0:
		ops = &dw_edma_v0_core_ops;
		break;
	default:
		dev_err(chip->dev, ": unsupported version\n");
		return -EPERM;
	}
	dw->ops = ops;

	pm_runtime_get_sync(chip->dev);

	dw->wr_ch_count = ops->ch_count(dw, WRITE);
	if (!dw->wr_ch_count) {
		dev_err(chip->dev, ": invalid number of write channels(0)\n");
		return -EINVAL;
	}

	dw->rd_ch_count = ops->ch_count(dw, READ);
	if (!dw->rd_ch_count) {
		dev_err(chip->dev, ": invalid number of read channels(0)\n");
		return -EINVAL;
	}

	dev_dbg(chip->dev, "Channels:\twrite=%d, read=%d\n",
		dw->wr_ch_count, dw->rd_ch_count);

	dw->chan = devm_kcalloc(chip->dev, dw->wr_ch_count + dw->rd_ch_count,
				sizeof(*dw->chan), GFP_KERNEL);
	if (!dw->chan)
		return -ENOMEM;

	ll_chunk /= roundup_pow_of_two(dw->wr_ch_count + dw->rd_ch_count);

	/* Disable eDMA, only to establish the ideal initial conditions */
	ops->off(dw);

	snprintf(dw->name, sizeof(dw->name), "dw-edma-core:%d", chip->id);

	err = devm_request_irq(chip->dev, chip->irq, dw_edma_interrupt,
			       IRQF_SHARED, dw->name, chip);
	if (err)
		return err;

	INIT_LIST_HEAD(&dw->wr_edma.channels);
	for (i = 0; i < dw->wr_ch_count; i++) {
		struct dw_edma_chan *chan = &dw->chan[i];

		chan->chip = chip;
		chan->id = i;
		chan->dir = EDMA_DIR_WRITE;
		chan->configured = false;
		chan->request = EDMA_REQ_NONE;
		chan->status = EDMA_ST_IDLE;

		chan->ll_off = (ll_chunk * i);
		chan->ll_max = (ll_chunk / 24) - 1;

		chan->msi_done_addr = dw->msi_addr;
		chan->msi_abort_addr = dw->msi_addr;
		chan->msi_data = dw->msi_data;

		chan->vc.desc_free = vchan_free_desc;
		vchan_init(&chan->vc, &dw->wr_edma);
	}
	dma_cap_set(DMA_SLAVE, dw->wr_edma.cap_mask);
	dw->wr_edma.directions = BIT(DMA_MEM_TO_DEV);
	dw->wr_edma.chancnt = dw->wr_ch_count;

	INIT_LIST_HEAD(&dw->rd_edma.channels);
	for (j = 0; j < dw->rd_ch_count; j++, i++) {
		struct dw_edma_chan *chan = &dw->chan[i];

		chan->chip = chip;
		chan->id = j;
		chan->dir = EDMA_DIR_READ;
		chan->request = EDMA_REQ_NONE;
		chan->status = EDMA_ST_IDLE;

		chan->ll_off = (ll_chunk * i);
		chan->ll_max = (ll_chunk / 24) - 1;

		chan->msi_done_addr = dw->msi_addr;
		chan->msi_abort_addr = dw->msi_addr;
		chan->msi_data = dw->msi_data;

		chan->vc.desc_free = vchan_free_desc;
		vchan_init(&chan->vc, &dw->rd_edma);
	}
	dma_cap_set(DMA_SLAVE, dw->rd_edma.cap_mask);
	dw->rd_edma.directions = BIT(DMA_DEV_TO_MEM);
	dw->rd_edma.chancnt = dw->rd_ch_count;

	/* Set DMA capabilities */
	SET_BOTH_CH(src_addr_widths, BIT(DMA_SLAVE_BUSWIDTH_4_BYTES));
	SET_BOTH_CH(dst_addr_widths, BIT(DMA_SLAVE_BUSWIDTH_4_BYTES));
	SET_BOTH_CH(residue_granularity, DMA_RESIDUE_GRANULARITY_DESCRIPTOR);

	SET_BOTH_CH(dev, chip->dev);

	SET_BOTH_CH(device_alloc_chan_resources, dw_edma_alloc_chan_resources);
	SET_BOTH_CH(device_free_chan_resources, dw_edma_free_chan_resources);

	SET_BOTH_CH(device_config, dw_edma_device_config);
	SET_BOTH_CH(device_pause, dw_edma_device_pause);
	SET_BOTH_CH(device_resume, dw_edma_device_resume);
	SET_BOTH_CH(device_terminate_all, dw_edma_device_terminate_all);
	SET_BOTH_CH(device_issue_pending, dw_edma_device_issue_pending);
	SET_BOTH_CH(device_tx_status, dw_edma_device_tx_status);
	SET_BOTH_CH(device_prep_slave_sg, dw_edma_device_prep_slave_sg);

	/* Power management */
	pm_runtime_enable(chip->dev);

	/* Register DMA device */
	err = dma_async_device_register(&dw->wr_edma);
	if (err)
		goto err_pm_disable;

	err = dma_async_device_register(&dw->rd_edma);
	if (err)
		goto err_pm_disable;

	/* Turn debugfs on */
	err = ops->debugfs_on(chip);
	if (err) {
		dev_err(chip->dev, ": unable to create debugfs structure\n");
		goto err_pm_disable;
	}

	dev_info(chip->dev, "DesignWare eDMA controller driver loaded completely\n");

	return 0;

err_pm_disable:
	pm_runtime_disable(chip->dev);

	return err;
}
EXPORT_SYMBOL_GPL(dw_edma_probe);

int dw_edma_remove(struct dw_edma_chip *chip)
{
	struct dw_edma *dw = chip->dw;
	const struct dw_edma_core_ops *ops = dw->ops;
	struct dw_edma_chan *chan, *_chan;

	/* Disable eDMA */
	if (ops)
		ops->off(dw);

	/* Free irq */
	devm_free_irq(chip->dev, chip->irq, chip);

	/* Power management */
	pm_runtime_disable(chip->dev);

	list_for_each_entry_safe(chan, _chan, &dw->wr_edma.channels,
				 vc.chan.device_node) {
		list_del(&chan->vc.chan.device_node);
		tasklet_kill(&chan->vc.task);
	}

	list_for_each_entry_safe(chan, _chan, &dw->rd_edma.channels,
				 vc.chan.device_node) {
		list_del(&chan->vc.chan.device_node);
		tasklet_kill(&chan->vc.task);
	}

	/* Deregister eDMA device */
	dma_async_device_unregister(&dw->wr_edma);
	dma_async_device_unregister(&dw->rd_edma);

	/* Turn debugfs off */
	if (ops)
		ops->debugfs_off();

	dev_info(chip->dev, ": DesignWare eDMA controller driver unloaded complete\n");

	return 0;
}
EXPORT_SYMBOL_GPL(dw_edma_remove);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Synopsys DesignWare eDMA controller core driver");
MODULE_AUTHOR("Gustavo Pimentel <gustavo.pimentel@synopsys.com>");
