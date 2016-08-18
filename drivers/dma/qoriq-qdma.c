/*
 * drivers/dma/qoriq-qdma.c
 *
 * Copyright 2015-2016 NXP Semiconductor, Inc.
 *
 * Driver for the QorIQ qDMA engine with software command queue mode.
 * Channel virtualization is supported through enqueuing of DMA jobs to,
 * or dequeuing DMA jobs from, different work queues.
 * This module can be found on some QorIQ SoCs.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <asm/cacheflush.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_dma.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "qoriq-qdma.h"

static unsigned int channels = 2;
module_param(channels, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(channels, "Number of channels supported by driver");

static unsigned int status_sizes[FSL_QDMA_MAX_BLOCK], status_num;
module_param_array(status_sizes, uint, &status_num,
		S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(status_sizes, "Size of each status queue in bytes");

static unsigned int queue_sizes[FSL_QDMA_MAX_QUEUE], queue_num;
module_param_array(queue_sizes, uint, &queue_num,
		S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(queue_sizes, "Size of each command queue in bytes");

static void fsl_qdma_free_chan_resources(struct dma_chan *chan)
{
	struct fsl_qdma_chan *fsl_chan = to_fsl_qdma_chan(chan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&fsl_chan->vchan.lock, flags);
	vchan_get_all_descriptors(&fsl_chan->vchan, &head);
	spin_unlock_irqrestore(&fsl_chan->vchan.lock, flags);

	vchan_dma_desc_free_list(&fsl_chan->vchan, &head);
}

static void fsl_qdma_comp_fill_memcpy(struct fsl_qdma_comp *fsl_comp,
					dma_addr_t dst, dma_addr_t src, u32 len)
{
	struct fsl_qdma_frame *frame;

	memset(fsl_comp->virt_addr, 0, FSL_QDMA_BASE_BUFFER_SIZE);
	frame = (struct fsl_qdma_frame *)fsl_comp->virt_addr;
	/* Head Command Descriptor(Frame Descriptor) */
	frame->ccdf.addr_low = lower_32_bits(fsl_comp->bus_addr + 16);
	frame->ccdf.dd_q_addr_high = (upper_32_bits(fsl_comp->bus_addr + 16))
				     & QDMA_CCDF_ADDR_HIGH_MASK;
	/* Compound S/G format */
	frame->ccdf.format_offset = (0 << QDMA_CCDF_OFFSET_SHIFT) |
				(0x1 << QDMA_CCDF_FORMAT_SHIFT);
	/* Status notification is enqueued to status queue. */
	frame->ccdf.ser_status = QDMA_CCDF_SER;

	/* Compound Command Descriptor(Frame List Table) */
	frame->csgf_desc.addr_low = lower_32_bits(fsl_comp->bus_addr + 64);
	frame->csgf_desc.addr_high = upper_32_bits(fsl_comp->bus_addr + 64);
	/* It must be 32 as Compound S/G Descriptor */
	frame->csgf_desc.e_f_length = 32;
	frame->csgf_src.addr_low = lower_32_bits(src);
	frame->csgf_src.addr_high = upper_32_bits(src);
	frame->csgf_src.e_f_length = len;
	frame->csgf_dest.addr_low = lower_32_bits(dst);
	frame->csgf_dest.addr_high = upper_32_bits(dst);
	frame->csgf_dest.e_f_length = len;
	/* This entry is the last entry. */
	frame->csgf_dest.e_f_length |= QDMA_CSGF_F;
	/* Descriptor Buffer */
	frame->sdf.cmd = FSL_QDMA_CMD_RWTTYPE << FSL_QDMA_CMD_RWTTYPE_OFFSET;
	frame->ddf.cmd = FSL_QDMA_CMD_RWTTYPE << FSL_QDMA_CMD_RWTTYPE_OFFSET;
}

static void fsl_qdma_comp_fill_sg(
		struct fsl_qdma_comp *fsl_comp,
		struct scatterlist *dst_sg, unsigned int dst_nents,
		struct scatterlist *src_sg, unsigned int src_nents)
{
	struct fsl_qdma_csgf *csgf_sg;
	struct fsl_qdma_sg *sg_block, *temp;
	struct scatterlist *sg;
	struct fsl_qdma_frame *frame;
	dma_addr_t dma_address;
	u64 total_src_len = 0;
	u64 total_dst_len = 0;
	u32 i;

	memset(fsl_comp->virt_addr, 0, FSL_QDMA_BASE_BUFFER_SIZE);
	frame = (struct fsl_qdma_frame *)fsl_comp->virt_addr;
	/* Head Command Descriptor(Frame Descriptor) */
	frame->ccdf.addr_low = lower_32_bits(fsl_comp->bus_addr + 16);
	frame->ccdf.dd_q_addr_high = (upper_32_bits(fsl_comp->bus_addr + 16))
				     & QDMA_CCDF_ADDR_HIGH_MASK;
	/* Compound S/G format */
	frame->ccdf.format_offset |= 0x1 << QDMA_CCDF_FORMAT_SHIFT;
	/* Status notification is enqueued to status queue. */
	frame->ccdf.ser_status |= QDMA_CCDF_SER;

	/* Compound Command Descriptor(Frame List Table) */
	frame->csgf_desc.addr_low = lower_32_bits(fsl_comp->bus_addr + 64);
	frame->csgf_desc.addr_high = upper_32_bits(fsl_comp->bus_addr + 64);
	/* It must be 32 as Compound S/G Descriptor */
	frame->csgf_desc.e_f_length = 32;

	sg_block = fsl_comp->sg_block;
	frame->csgf_src.addr_low = lower_32_bits(sg_block->bus_addr);
	frame->csgf_src.addr_high = upper_32_bits(sg_block->bus_addr);
	/* This entry link to the s/g entry. */
	frame->csgf_src.e_f_length |= QDMA_CSGF_E;

	temp = sg_block + fsl_comp->sg_block_src;
	frame->csgf_dest.addr_low = lower_32_bits(temp->bus_addr);
	frame->csgf_dest.addr_high = upper_32_bits(temp->bus_addr);
	/* This entry is the last entry and link to the s/g entry. */
	frame->csgf_dest.e_f_length |= QDMA_CSGF_F | QDMA_CSGF_E;

	for_each_sg(src_sg, sg, src_nents, i) {
		temp = sg_block + i / (FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1);
		csgf_sg = (struct fsl_qdma_csgf *)temp->virt_addr +
			  i % (FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1);
		dma_address = sg_dma_address(sg);
		csgf_sg->addr_low = lower_32_bits(dma_address);
		csgf_sg->addr_high = upper_32_bits(dma_address);
		csgf_sg->e_f_length |= sg_dma_len(sg);
		total_src_len += sg_dma_len(sg);

		if (i == src_nents - 1)
			csgf_sg->e_f_length |= QDMA_CSGF_F;
		if (i % (FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1) ==
		    FSL_QDMA_EXPECT_SG_ENTRY_NUM - 2) {
			csgf_sg = (struct fsl_qdma_csgf *)temp->virt_addr +
				  FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1;
			temp = sg_block +
				i / (FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1) + 1;
			csgf_sg->addr_low = lower_32_bits(temp->bus_addr);
			csgf_sg->addr_high = upper_32_bits(temp->bus_addr);
			csgf_sg->e_f_length |= QDMA_CSGF_E;
		}
	}

	sg_block += fsl_comp->sg_block_src;
	for_each_sg(dst_sg, sg, dst_nents, i) {
		temp = sg_block + i / (FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1);
		csgf_sg = (struct fsl_qdma_csgf *)temp->virt_addr +
			  i % (FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1);
		dma_address = sg_dma_address(sg);
		csgf_sg->addr_low = lower_32_bits(dma_address);
		csgf_sg->addr_high = upper_32_bits(dma_address);
		csgf_sg->e_f_length |= sg_dma_len(sg);
		total_dst_len += sg_dma_len(sg);

		if (i == dst_nents - 1)
			csgf_sg->e_f_length |= QDMA_CSGF_F;
		if (i % (FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1) ==
		    FSL_QDMA_EXPECT_SG_ENTRY_NUM - 2) {
			csgf_sg = (struct fsl_qdma_csgf *)temp->virt_addr +
				  FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1;
			temp = sg_block +
				i / (FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1) + 1;
			csgf_sg->addr_low = lower_32_bits(temp->bus_addr);
			csgf_sg->addr_high = upper_32_bits(temp->bus_addr);
			csgf_sg->e_f_length |= QDMA_CSGF_E;
		}
	}

	if (total_src_len != total_dst_len)
		dev_err(&fsl_comp->qchan->vchan.chan.dev->device,
			"The data length for src and dst isn't match.\n");

	frame->csgf_src.e_f_length |= total_src_len;
	frame->csgf_dest.e_f_length |= total_dst_len;

	/* Descriptor Buffer */
	frame->sdf.cmd = FSL_QDMA_CMD_RWTTYPE << FSL_QDMA_CMD_RWTTYPE_OFFSET;
	frame->ddf.cmd = FSL_QDMA_CMD_RWTTYPE << FSL_QDMA_CMD_RWTTYPE_OFFSET;
}

/*
 * Request a command descriptor for enqueue.
 */
static struct fsl_qdma_comp *fsl_qdma_request_enqueue_desc(
					struct fsl_qdma_chan *fsl_chan,
					unsigned int dst_nents,
					unsigned int src_nents)
{
	struct fsl_qdma_comp *comp_temp;
	struct fsl_qdma_sg *sg_block;
	struct fsl_qdma_queue *queue = fsl_chan->queue;
	unsigned long flags;
	unsigned int dst_sg_entry_block, src_sg_entry_block, sg_entry_total, i;

	spin_lock_irqsave(&queue->queue_lock, flags);
	if (list_empty(&queue->comp_free)) {
		spin_unlock_irqrestore(&queue->queue_lock, flags);
		comp_temp = kzalloc(sizeof(*comp_temp), GFP_KERNEL);
		if (!comp_temp)
			return NULL;
		comp_temp->virt_addr = dma_pool_alloc(queue->comp_pool,
						      GFP_NOWAIT,
						      &comp_temp->bus_addr);
		if (!comp_temp->virt_addr)
			return NULL;
	} else {
		comp_temp = list_first_entry(&queue->comp_free,
					     struct fsl_qdma_comp,
					     list);
		list_del(&comp_temp->list);
		spin_unlock_irqrestore(&queue->queue_lock, flags);
	}

	if (dst_nents != 0)
		dst_sg_entry_block = dst_nents /
					(FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1) + 1;
	else
		dst_sg_entry_block = 0;

	if (src_nents != 0)
		src_sg_entry_block = src_nents /
					(FSL_QDMA_EXPECT_SG_ENTRY_NUM - 1) + 1;
	else
		src_sg_entry_block = 0;

	sg_entry_total = dst_sg_entry_block + src_sg_entry_block;
	if (sg_entry_total) {
		sg_block = kzalloc(sizeof(*sg_block) *
					      sg_entry_total,
					      GFP_KERNEL);
		if (!sg_block)
			return NULL;
		comp_temp->sg_block = sg_block;
		for (i = 0; i < sg_entry_total; i++) {
			sg_block->virt_addr = dma_pool_alloc(queue->sg_pool,
							GFP_NOWAIT,
							&sg_block->bus_addr);
			memset(sg_block->virt_addr, 0,
					FSL_QDMA_EXPECT_SG_ENTRY_NUM * 16);
			sg_block++;
		}
	}

	comp_temp->sg_block_src = src_sg_entry_block;
	comp_temp->sg_block_dst = dst_sg_entry_block;
	comp_temp->qchan = fsl_chan;

	return comp_temp;
}

static struct fsl_qdma_queue *fsl_qdma_alloc_queue_resources(
					struct platform_device *pdev,
					enum qdma_queue_type type)
{
	struct fsl_qdma_queue *queue_head, *queue_temp;
	unsigned int *qdma_queue_sizes, qdma_queue_num;
	int len, i;

	if (type == QDMA_QUEUE) {
		if (queue_num > FSL_QDMA_MAX_QUEUE) {
			dev_warn(&pdev->dev,
				 "The max number of the queues is: %d\n",
				 FSL_QDMA_MAX_QUEUE);
			queue_num = FSL_QDMA_MAX_QUEUE;
		}

		if (queue_num == 0) {
			dev_warn(&pdev->dev,
				 "The number of the queues can't be 0\n");
			queue_num = 1;
		}
		qdma_queue_sizes = queue_sizes;
		qdma_queue_num = queue_num;
	} else if (type == QDMA_STATUS) {
		if (status_num > FSL_QDMA_MAX_BLOCK) {
			dev_warn(&pdev->dev,
				 "The max number of the queues is: %d\n",
				 FSL_QDMA_MAX_BLOCK);
			status_num = FSL_QDMA_MAX_BLOCK;
		}

		if (status_num == 0) {
			dev_warn(&pdev->dev,
				 "The number of the queues can't be 0\n");
			status_num = 1;
		}
		qdma_queue_sizes = status_sizes;
		qdma_queue_num = status_num;
	} else {
		return NULL;
	}

	len = sizeof(*queue_head) * qdma_queue_num;
	queue_head = devm_kzalloc(&pdev->dev, len, GFP_KERNEL);
	if (!queue_head)
		return NULL;

	for (i = 0; i < qdma_queue_num; i++) {
		if (qdma_queue_sizes[i] > FSL_QDMA_CIRCULAR_SIZE_MAX
		    || qdma_queue_sizes[i] < FSL_QDMA_CIRCULAR_SIZE_MIN) {
			dev_warn(&pdev->dev, "The wrong queue sizes\n");
			qdma_queue_sizes[i] = FSL_QDMA_CIRCULAR_SIZE_MIN;
		}
		queue_temp = queue_head + i;
		queue_temp->cq = dma_alloc_coherent(&pdev->dev,
						sizeof(struct fsl_qdma_ccdf) *
						qdma_queue_sizes[i],
						&queue_temp->bus_addr,
						GFP_KERNEL);
		if (!queue_temp->cq)
			return NULL;
		queue_temp->n_cq = qdma_queue_sizes[i];
		queue_temp->id = i;
		queue_temp->virt_head = queue_temp->cq;
		queue_temp->virt_tail = queue_temp->cq;

		/*
		 * There is no dma pool need for status queue.
		 */
		if (type == QDMA_STATUS) {
			queue_temp->comp_pool = NULL;
			queue_temp->sg_pool = NULL;
			continue;
		}

		/*
		 * The dma pool for queue command buffer.
		 */
		queue_temp->comp_pool = dma_pool_create("comp_pool",
						&pdev->dev,
						FSL_QDMA_BASE_BUFFER_SIZE,
						16, 0);
		if (!queue_temp->comp_pool) {
			dma_free_coherent(&pdev->dev,
						sizeof(struct fsl_qdma_ccdf) *
						qdma_queue_sizes[i],
						queue_temp->cq,
						queue_temp->bus_addr);
			return NULL;
		}
		/*
		 * The dma pool for queue command buffer.
		 */
		queue_temp->sg_pool = dma_pool_create("sg_pool",
					&pdev->dev,
					FSL_QDMA_EXPECT_SG_ENTRY_NUM * 16,
					64, 0);
		if (!queue_temp->sg_pool) {
			dma_free_coherent(&pdev->dev,
						sizeof(struct fsl_qdma_ccdf) *
						qdma_queue_sizes[i],
						queue_temp->cq,
						queue_temp->bus_addr);
			dma_pool_destroy(queue_temp->comp_pool);
			return NULL;
		}
		/*
		 * List for queue command buffer.
		 */
		INIT_LIST_HEAD(&queue_temp->comp_used);
		INIT_LIST_HEAD(&queue_temp->comp_free);
		spin_lock_init(&queue_temp->queue_lock);
	}

	return queue_head;
}

static int fsl_qdma_halt(struct fsl_qdma_engine *fsl_qdma)
{
	void __iomem *ctrl = fsl_qdma->ctrl_base;
	void __iomem *block = fsl_qdma->block_base;
	int i, count = 5;
	u32 reg;

	/* Disable the command queue and wait for idle state. */
	reg = qdma_readl(fsl_qdma, ctrl + FSL_QDMA_DMR);
	reg |= FSL_QDMA_DMR_DQD;
	qdma_writel(fsl_qdma, reg, ctrl + FSL_QDMA_DMR);
	for (i = 0; i < FSL_QDMA_MAX_QUEUE; i++)
		qdma_writel(fsl_qdma, 0, block + FSL_QDMA_BCQMR(i));

	while (1) {
		reg = qdma_readl(fsl_qdma, ctrl + FSL_QDMA_DSR);
		if (!(reg & FSL_QDMA_DSR_DB))
			break;
		if (count-- < 0)
			return -EBUSY;
		udelay(100);
	}

	/* Disable status queue. */
	qdma_writel(fsl_qdma, 0, block + FSL_QDMA_BSQMR);

	/*
	 * Clear the command queue interrupt detect register for all queues.
	 */
	qdma_writel(fsl_qdma, 0xffffffff, block + FSL_QDMA_BCQIDR(0));

	return 0;
}

static void fsl_qdma_queue_complete(struct fsl_qdma_engine *fsl_qdma,
		enum dma_status status)
{
	struct fsl_qdma_queue *fsl_queue = fsl_qdma->queue;
	struct fsl_qdma_queue *fsl_status = fsl_qdma->status;
	struct fsl_qdma_queue *temp_queue;
	struct fsl_qdma_comp *fsl_comp;
	struct fsl_qdma_ccdf *status_addr;
	void __iomem *block = fsl_qdma->block_base;
	u64 bus_addr;
	u32 reg, i;

	while (1) {
		status_addr = fsl_status->virt_head;
		/*
		 * Sacn all the queues.
		 * Match which queue completed this transfer.
		 */
		for (i = 0; i < fsl_qdma->n_queues; i++) {
			temp_queue = fsl_queue + i;
			if (list_empty(&temp_queue->comp_used))
				continue;
			fsl_comp = list_first_entry(&temp_queue->comp_used,
							struct fsl_qdma_comp,
							list);
			bus_addr = status_addr->dd_q_addr_high
				   & QDMA_CCDF_ADDR_HIGH_MASK;
			bus_addr = bus_addr << 32 | status_addr->addr_low;
			if (fsl_comp->bus_addr + 16 != (dma_addr_t)bus_addr)
				continue;
			spin_lock(&temp_queue->queue_lock);
			list_del(&fsl_comp->list);
			spin_unlock(&temp_queue->queue_lock);

			reg = qdma_readl(fsl_qdma, block + FSL_QDMA_BSQMR);
			reg |= FSL_QDMA_BSQMR_DI;
			qdma_writel(fsl_qdma, reg, block + FSL_QDMA_BSQMR);
			fsl_status->virt_head++;
			if (fsl_status->virt_head == fsl_status->cq +
						     fsl_status->n_cq)
				fsl_status->virt_head = fsl_status->cq;

			spin_lock(&fsl_comp->qchan->vchan.lock);
			if (status == DMA_COMPLETE)
				vchan_cookie_complete(&fsl_comp->vdesc);
			fsl_comp->qchan->status = status;

			spin_unlock(&fsl_comp->qchan->vchan.lock);
			break;
		}
		reg = qdma_readl(fsl_qdma, block + FSL_QDMA_BSQSR);
		if (reg & FSL_QDMA_BSQSR_QE)
			break;
		if (i == fsl_qdma->n_queues) {
			/*
			 * QDMA can't find the corresponding completed queue.
			 */
			return;
		}
	}
}

static irqreturn_t fsl_qdma_error_handler(int irq, void *dev_id)
{
	struct fsl_qdma_engine *fsl_qdma = dev_id;
	unsigned int intr;
	void __iomem *ctrl = fsl_qdma->ctrl_base;

	intr = qdma_readl(fsl_qdma, ctrl + FSL_QDMA_DEDR);

	if (!intr)
		return IRQ_NONE;

	fsl_qdma_queue_complete(fsl_qdma, DMA_ERROR);
	qdma_writel(fsl_qdma, 0xffffffff, ctrl + FSL_QDMA_DEDR);
	return IRQ_HANDLED;
}

static irqreturn_t fsl_qdma_queue_handler(int irq, void *dev_id)
{
	struct fsl_qdma_engine *fsl_qdma = dev_id;
	unsigned int intr, intr_err;
	void __iomem *block = fsl_qdma->block_base;
	void __iomem *ctrl = fsl_qdma->ctrl_base;
	int ret = IRQ_NONE;

	intr = qdma_readl(fsl_qdma, block + FSL_QDMA_BCQIDR(0));
	intr_err = qdma_readl(fsl_qdma, ctrl + FSL_QDMA_DEDR);

	if ((intr & FSL_QDMA_CQIDR_SQT) != 0) {
		if (intr_err) {
			fsl_qdma_queue_complete(fsl_qdma, DMA_ERROR);
			qdma_writel(fsl_qdma, 0xffffffff, ctrl + FSL_QDMA_DEDR);
		} else
			fsl_qdma_queue_complete(fsl_qdma, DMA_COMPLETE);
		ret = IRQ_HANDLED;
	}

	qdma_writel(fsl_qdma, 0xffffffff, block + FSL_QDMA_BCQIDR(0));
	return ret;
}

static int
fsl_qdma_irq_init(struct platform_device *pdev,
		  struct fsl_qdma_engine *fsl_qdma)
{
	int ret;

	fsl_qdma->error_irq = platform_get_irq_byname(pdev,
							"qdma-error");
	if (fsl_qdma->error_irq < 0) {
		dev_err(&pdev->dev, "Can't get qdma controller irq.\n");
		return fsl_qdma->error_irq;
	}

	fsl_qdma->queue_irq = platform_get_irq_byname(pdev, "qdma-queue");
	if (fsl_qdma->queue_irq < 0) {
		dev_err(&pdev->dev, "Can't get qdma queue irq.\n");
		return fsl_qdma->queue_irq;
	}

	ret = devm_request_irq(&pdev->dev, fsl_qdma->error_irq,
			fsl_qdma_error_handler, 0, "qDMA error", fsl_qdma);
	if (ret) {
		dev_err(&pdev->dev, "Can't register qDMA controller IRQ.\n");
		return  ret;
	}
	ret = devm_request_irq(&pdev->dev, fsl_qdma->queue_irq,
			fsl_qdma_queue_handler, 0, "qDMA queue", fsl_qdma);
	if (ret) {
		dev_err(&pdev->dev, "Can't register qDMA queue IRQ.\n");
		return  ret;
	}

	return 0;
}

static int fsl_qdma_reg_init(struct fsl_qdma_engine *fsl_qdma)
{
	struct fsl_qdma_queue *fsl_queue = fsl_qdma->queue;
	struct fsl_qdma_queue *temp;
	void __iomem *ctrl = fsl_qdma->ctrl_base;
	void __iomem *block = fsl_qdma->block_base;
	int i, ret;
	u32 reg;

	/* Try to halt the qDMA engine first. */
	ret = fsl_qdma_halt(fsl_qdma);
	if (ret) {
		dev_err(fsl_qdma->dma_dev.dev, "DMA halt failed!");
		return ret;
	}

	/*
	 * Clear the command queue interrupt detect register for all queues.
	 */
	qdma_writel(fsl_qdma, 0xffffffff, block + FSL_QDMA_BCQIDR(0));

	for (i = 0; i < fsl_qdma->n_queues; i++) {
		temp = fsl_queue + i;
		/*
		 * Initialize Command Queue registers to point to the first
		 * command descriptor in memory.
		 * Dequeue Pointer Address Registers
		 * Enqueue Pointer Address Registers
		 */
		qdma_writel(fsl_qdma, temp->bus_addr,
				block + FSL_QDMA_BCQDPA_SADDR(i));
		qdma_writel(fsl_qdma, temp->bus_addr,
				block + FSL_QDMA_BCQEPA_SADDR(i));

		/* Initialize the queue mode. */
		reg = FSL_QDMA_BCQMR_EN;
		reg |= FSL_QDMA_BCQMR_CD_THLD(ilog2(temp->n_cq)-4);
		reg |= FSL_QDMA_BCQMR_CQ_SIZE(ilog2(temp->n_cq)-6);
		qdma_writel(fsl_qdma, reg, block + FSL_QDMA_BCQMR(i));
	}

	/*
	 * Initialize status queue registers to point to the first
	 * command descriptor in memory.
	 * Dequeue Pointer Address Registers
	 * Enqueue Pointer Address Registers
	 */
	qdma_writel(fsl_qdma, fsl_qdma->status->bus_addr,
					block + FSL_QDMA_SQEPAR);
	qdma_writel(fsl_qdma, fsl_qdma->status->bus_addr,
					block + FSL_QDMA_SQDPAR);
	/* Initialize status queue interrupt. */
	qdma_writel(fsl_qdma, FSL_QDMA_BCQIER_CQTIE,
			      block + FSL_QDMA_BCQIER(0));
	qdma_writel(fsl_qdma, FSL_QDMA_BSQICR_ICEN | FSL_QDMA_BSQICR_ICST(1),
			      block + FSL_QDMA_BSQICR);
	qdma_writel(fsl_qdma, FSL_QDMA_CQIER_MEIE | FSL_QDMA_CQIER_TEIE,
			      block + FSL_QDMA_CQIER);
	/* Initialize controller interrupt register. */
	qdma_writel(fsl_qdma, 0xffffffff, ctrl + FSL_QDMA_DEDR);
	qdma_writel(fsl_qdma, 0xffffffff, ctrl + FSL_QDMA_DEIER);

	/* Initialize the status queue mode. */
	reg = FSL_QDMA_BSQMR_EN;
	reg |= FSL_QDMA_BSQMR_CQ_SIZE(ilog2(fsl_qdma->status->n_cq)-6);
	qdma_writel(fsl_qdma, reg, block + FSL_QDMA_BSQMR);

	reg = qdma_readl(fsl_qdma, ctrl + FSL_QDMA_DMR);
	reg &= ~FSL_QDMA_DMR_DQD;
	qdma_writel(fsl_qdma, reg, ctrl + FSL_QDMA_DMR);

	return 0;
}

static struct dma_async_tx_descriptor *fsl_qdma_prep_dma_sg(
		struct dma_chan *chan,
		struct scatterlist *dst_sg, unsigned int dst_nents,
		struct scatterlist *src_sg, unsigned int src_nents,
		unsigned long flags)
{
	struct fsl_qdma_chan *fsl_chan = to_fsl_qdma_chan(chan);
	struct fsl_qdma_comp *fsl_comp;

	fsl_comp = fsl_qdma_request_enqueue_desc(fsl_chan,
						 dst_nents,
						 src_nents);
	fsl_qdma_comp_fill_sg(fsl_comp, dst_sg, dst_nents, src_sg, src_nents);

	return vchan_tx_prep(&fsl_chan->vchan, &fsl_comp->vdesc, flags);
}

static struct dma_async_tx_descriptor *
fsl_qdma_prep_memcpy(struct dma_chan *chan, dma_addr_t dst,
		dma_addr_t src, size_t len, unsigned long flags)
{
	struct fsl_qdma_chan *fsl_chan = to_fsl_qdma_chan(chan);
	struct fsl_qdma_comp *fsl_comp;

	fsl_comp = fsl_qdma_request_enqueue_desc(fsl_chan, 0, 0);
	fsl_qdma_comp_fill_memcpy(fsl_comp, dst, src, len);

	return vchan_tx_prep(&fsl_chan->vchan, &fsl_comp->vdesc, flags);
}

static void fsl_qdma_enqueue_desc(struct fsl_qdma_chan *fsl_chan)
{
	void __iomem *block = fsl_chan->qdma->block_base;
	struct fsl_qdma_queue *fsl_queue = fsl_chan->queue;
	struct fsl_qdma_comp *fsl_comp;
	struct virt_dma_desc *vdesc;
	u32 reg;

	reg = qdma_readl(fsl_chan->qdma, block + FSL_QDMA_BCQSR(fsl_queue->id));
	if (reg & FSL_QDMA_BCQSR_QF)
		return;
	vdesc = vchan_next_desc(&fsl_chan->vchan);
	if (!vdesc)
		return;
	list_del(&vdesc->node);
	fsl_comp = to_fsl_qdma_comp(vdesc);

	memcpy(fsl_queue->virt_head++, fsl_comp->virt_addr, 16);
	if (fsl_queue->virt_head == fsl_queue->cq + fsl_queue->n_cq)
		fsl_queue->virt_head = fsl_queue->cq;

	list_add_tail(&fsl_comp->list, &fsl_queue->comp_used);
	reg = qdma_readl(fsl_chan->qdma, block + FSL_QDMA_BCQMR(fsl_queue->id));
	reg |= FSL_QDMA_BCQMR_EI;
	qdma_writel(fsl_chan->qdma, reg, block + FSL_QDMA_BCQMR(fsl_queue->id));
	fsl_chan->status = DMA_IN_PROGRESS;
}

static enum dma_status fsl_qdma_tx_status(struct dma_chan *chan,
		dma_cookie_t cookie, struct dma_tx_state *txstate)
{
	return dma_cookie_status(chan, cookie, txstate);
}

static void fsl_qdma_free_desc(struct virt_dma_desc *vdesc)
{
	struct fsl_qdma_comp *fsl_comp;
	struct fsl_qdma_queue *fsl_queue;
	struct fsl_qdma_sg *sg_block;
	unsigned long flags;
	unsigned int i;

	fsl_comp = to_fsl_qdma_comp(vdesc);
	fsl_queue = fsl_comp->qchan->queue;

	if (fsl_comp->sg_block) {
		for (i = 0; i < fsl_comp->sg_block_src +
				fsl_comp->sg_block_dst; i++) {
			sg_block = fsl_comp->sg_block + i;
			dma_pool_free(fsl_queue->sg_pool,
				      sg_block->virt_addr,
				      sg_block->bus_addr);
		}
		kfree(fsl_comp->sg_block);
	}

	spin_lock_irqsave(&fsl_queue->queue_lock, flags);
	list_add_tail(&fsl_comp->list, &fsl_queue->comp_free);
	spin_unlock_irqrestore(&fsl_queue->queue_lock, flags);
}

static void fsl_qdma_issue_pending(struct dma_chan *chan)
{
	struct fsl_qdma_chan *fsl_chan = to_fsl_qdma_chan(chan);
	struct fsl_qdma_queue *fsl_queue = fsl_chan->queue;
	unsigned long flags;

	spin_lock_irqsave(&fsl_queue->queue_lock, flags);
	spin_lock(&fsl_chan->vchan.lock);
	if (vchan_issue_pending(&fsl_chan->vchan))
		fsl_qdma_enqueue_desc(fsl_chan);
	spin_unlock(&fsl_chan->vchan.lock);
	spin_unlock_irqrestore(&fsl_queue->queue_lock, flags);
}

static int fsl_qdma_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct fsl_qdma_engine *fsl_qdma;
	struct fsl_qdma_chan *fsl_chan;
	struct resource *res;
	int len, ret, i;

	if (channels > FSL_QDMA_MAX_BLOCK * FSL_QDMA_MAX_QUEUE) {
		dev_warn(&pdev->dev,
			 "The max number of the channels is: %d\n",
			 FSL_QDMA_MAX_BLOCK * FSL_QDMA_MAX_QUEUE);
		channels = FSL_QDMA_MAX_BLOCK * FSL_QDMA_MAX_QUEUE;
	}

	len = sizeof(*fsl_qdma) + sizeof(*fsl_chan) * channels;
	fsl_qdma = devm_kzalloc(&pdev->dev, len, GFP_KERNEL);
	if (!fsl_qdma)
		return -ENOMEM;

	fsl_qdma->queue = fsl_qdma_alloc_queue_resources(pdev, QDMA_QUEUE);
	if (!fsl_qdma->queue)
		return -ENOMEM;

	fsl_qdma->status = fsl_qdma_alloc_queue_resources(pdev, QDMA_STATUS);
	if (!fsl_qdma->status)
		return -ENOMEM;

	fsl_qdma->n_chans = channels;
	fsl_qdma->n_queues = queue_num;
	mutex_init(&fsl_qdma->fsl_qdma_mutex);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	fsl_qdma->ctrl_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(fsl_qdma->ctrl_base))
		return PTR_ERR(fsl_qdma->ctrl_base);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	fsl_qdma->block_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(fsl_qdma->block_base))
		return PTR_ERR(fsl_qdma->block_base);

	ret = fsl_qdma_irq_init(pdev, fsl_qdma);
	if (ret)
		return ret;

	fsl_qdma->big_endian = of_property_read_bool(np, "big-endian");
	INIT_LIST_HEAD(&fsl_qdma->dma_dev.channels);
	for (i = 0; i < fsl_qdma->n_chans; i++) {
		struct fsl_qdma_chan *fsl_chan = &fsl_qdma->chans[i];

		fsl_chan->qdma = fsl_qdma;
		fsl_chan->queue = fsl_qdma->queue;
		fsl_chan->vchan.desc_free = fsl_qdma_free_desc;
		INIT_LIST_HEAD(&fsl_chan->qcomp);
		vchan_init(&fsl_chan->vchan, &fsl_qdma->dma_dev);
	}

	dma_cap_set(DMA_MEMCPY, fsl_qdma->dma_dev.cap_mask);
	dma_cap_set(DMA_SG, fsl_qdma->dma_dev.cap_mask);

	fsl_qdma->dma_dev.dev = &pdev->dev;
	fsl_qdma->dma_dev.device_free_chan_resources
		= fsl_qdma_free_chan_resources;
	fsl_qdma->dma_dev.device_tx_status = fsl_qdma_tx_status;
	fsl_qdma->dma_dev.device_prep_dma_memcpy = fsl_qdma_prep_memcpy;
	fsl_qdma->dma_dev.device_prep_dma_sg = fsl_qdma_prep_dma_sg;
	fsl_qdma->dma_dev.device_issue_pending = fsl_qdma_issue_pending;

	dma_set_mask(&pdev->dev, DMA_BIT_MASK(40));

	platform_set_drvdata(pdev, fsl_qdma);

	ret = dma_async_device_register(&fsl_qdma->dma_dev);
	if (ret) {
		dev_err(&pdev->dev, "Can't register QorIQ qDMA engine.\n");
		return ret;
	}

	ret = fsl_qdma_reg_init(fsl_qdma);
	if (ret) {
		dev_err(&pdev->dev, "Can't Initialize the qDMA engine.\n");
		return ret;
	}


	return 0;
}

static int fsl_qdma_remove(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct fsl_qdma_engine *fsl_qdma = platform_get_drvdata(pdev);
	struct fsl_qdma_queue *queue_temp;
	struct fsl_qdma_queue *status = fsl_qdma->status;
	struct fsl_qdma_comp *comp_temp, *_comp_temp;
	int i;

	of_dma_controller_free(np);
	dma_async_device_unregister(&fsl_qdma->dma_dev);

	/* Free descriptor areas */
	for (i = 0; i < fsl_qdma->n_queues; i++) {
		queue_temp = fsl_qdma->queue + i;
		list_for_each_entry_safe(comp_temp, _comp_temp,
					 &queue_temp->comp_used, list) {
			dma_pool_free(queue_temp->comp_pool,
				      comp_temp->virt_addr,
				      comp_temp->bus_addr);
			list_del(&comp_temp->list);
			kfree(comp_temp);
		}
		list_for_each_entry_safe(comp_temp, _comp_temp,
					 &queue_temp->comp_free, list) {
			dma_pool_free(queue_temp->comp_pool,
				      comp_temp->virt_addr,
				      comp_temp->bus_addr);
			list_del(&comp_temp->list);
			kfree(comp_temp);
		}
		dma_free_coherent(&pdev->dev, sizeof(struct fsl_qdma_ccdf) *
				  queue_temp->n_cq, queue_temp->cq,
				  queue_temp->bus_addr);
		dma_pool_destroy(queue_temp->comp_pool);
	}

	dma_free_coherent(&pdev->dev, sizeof(struct fsl_qdma_ccdf) *
				status->n_cq, status->cq, status->bus_addr);
	return 0;
}

static const struct of_device_id fsl_qdma_dt_ids[] = {
	{ .compatible = "fsl,ls1021a-qdma", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fsl_qdma_dt_ids);

static struct platform_driver fsl_qdma_driver = {
	.driver		= {
		.name	= "fsl-qdma",
		.owner  = THIS_MODULE,
		.of_match_table = fsl_qdma_dt_ids,
	},
	.probe          = fsl_qdma_probe,
	.remove		= fsl_qdma_remove,
};

static int __init fsl_qdma_init(void)
{
	return platform_driver_register(&fsl_qdma_driver);
}
subsys_initcall(fsl_qdma_init);

static void __exit fsl_qdma_exit(void)
{
	platform_driver_unregister(&fsl_qdma_driver);
}
module_exit(fsl_qdma_exit);

MODULE_DESCRIPTION("QorIQ qDMA engine driver");
MODULE_AUTHOR("Yuan Yao <yao.yuan@nxp.com>");
MODULE_LICENSE("GPL v2");
