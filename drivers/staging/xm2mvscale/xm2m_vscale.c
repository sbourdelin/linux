/*
 * Xilinx Memory-to-Memory Video Scaler IP
 *
 * Copyright (C) 2018 Xilinx, Inc. All rights reserved.
 *
 * Description:
 * This driver is developed for the Xilinx M2M Video Scaler IP. It allows
 * userspace to operate upon the IP and takes care of interrupt handling
 * and framebuffer programming within the driver.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/cdev.h>
#include <linux/dma-buf.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>

#include "scaler_hw_xm2m.h"
#include "ioctl_xm2mvsc.h"

/* Forward Declaration */
static int xm2mvsc_ioctl_stop(struct xm2m_vscale_dev *xm2mvsc);

/* Module Parameters */
static struct class *xm2mvsc_class;
static dev_t xm2mvsc_devt;
static atomic_t xm2mvsc_ndevs = ATOMIC_INIT(0);

#define DRIVER_NAME	"xilinx-m2m-scaler"
#define DRIVER_VERSION	"0.4"
#define DRIVER_MAX_DEV	(10)

static int xm2mvsc_open(struct inode *iptr, struct file *fptr)
{
	struct xm2m_vscale_dev *xm2mvsc;

	xm2mvsc = container_of(iptr->i_cdev, struct xm2m_vscale_dev, chdev);
	if (!xm2mvsc) {
		pr_err("%s: failed to get xm2mvsc driver handle", __func__);
		return -EAGAIN;
	}
	fptr->private_data = xm2mvsc;
	xm2mvsc->batch_size = XSCALER_BATCH_SIZE_MIN;
	atomic_inc(&xm2mvsc->user_count);
	return 0;
}

static int xm2mvsc_release(struct inode *iptr, struct file *fptr)
{
	struct xm2m_vscale_dev *xm2mvsc;

	xm2mvsc = container_of(iptr->i_cdev, struct xm2m_vscale_dev, chdev);
	if (!xm2mvsc) {
		pr_err("%s: failed to get xm2mvsc driver handle", __func__);
		return -EAGAIN;
	}
	if (atomic_dec_and_test(&xm2mvsc->user_count)) {
		/* Reset IP and clear driver state */
		dev_dbg(xm2mvsc->dev,
			"%s: Stopping and clearing device", __func__);
		(void)xm2mvsc_ioctl_stop(xm2mvsc);
		atomic_set(&xm2mvsc->desc_count, 0);
		atomic_set(&xm2mvsc->ongoing_count, 0);
	}
	dev_dbg(xm2mvsc->dev, "%s: user count = %d",
		__func__, atomic_read(&xm2mvsc->user_count));
	return 0;
}

#define XM2MVSC_MAX_WIDTH	(3840)
#define XM2MVSC_MAX_HEIGHT	(2160)
#define XM2MVSC_MIN_WIDTH	(32)
#define XM2MVSC_MIN_HEIGHT	(32)
static int xm2mvsc_verify_desc(struct xm2m_vscale_desc *desc)
{
	if (!desc)
		return -EIO;
	if (desc->data.srcbuf_ht > XM2MVSC_MAX_HEIGHT ||
	    desc->data.srcbuf_ht < XM2MVSC_MIN_HEIGHT ||
	    desc->data.dstbuf_ht > XM2MVSC_MAX_HEIGHT ||
	    desc->data.dstbuf_ht < XM2MVSC_MIN_HEIGHT)
		return -EINVAL;
	if (desc->data.srcbuf_wt > XM2MVSC_MAX_WIDTH ||
	    desc->data.srcbuf_wt < XM2MVSC_MIN_WIDTH ||
	    desc->data.dstbuf_wt > XM2MVSC_MAX_WIDTH ||
	    desc->data.dstbuf_wt < XM2MVSC_MIN_WIDTH)
		return -EINVAL;
	return 0;
}

static int xm2mvsc_ioctl_batch_size(struct xm2m_vscale_dev *xm2mvsc,
				    void __user *arg)
{
	int ret;
	struct xm2mvsc_batch *batch;

	batch = kzalloc(sizeof(*batch), GFP_KERNEL);
	if (!batch)
		return -ENOMEM;
	ret = copy_from_user(batch, arg, sizeof(*batch));
	if (ret) {
		dev_err(xm2mvsc->dev,
			"%s: Failed to copy from user", __func__);
		kfree(batch);
		return -EFAULT;
	}

	if (!batch->batch_size || batch->batch_size > xm2mvsc->hw.max_chan) {
		dev_err(xm2mvsc->dev,
			"Invalid batch size passed %d", batch->batch_size);
		kfree(batch);
		return -EINVAL;
	}
	xm2mvsc->batch_size = batch->batch_size;
	kfree(batch);
	return 0;
}

static int xm2mvsc_ioctl_enqueue(struct xm2m_vscale_dev *xm2mvsc,
				 void __user *arg)
{
	struct xm2m_vscale_desc *desc;
	int ret;
	unsigned long flags;

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	ret = copy_from_user(&desc->data, arg, sizeof(desc->data));
	if (ret)  {
		dev_err(xm2mvsc->dev, "%s: Failed to copy from user", __func__);
		return -EFAULT;
	}
	ret = xm2mvsc_verify_desc(desc);
	if (ret < 0)
		return ret;
	/* Assign xm2m_vscale_dev handle */
	desc->xm2mvsc_dev = xm2mvsc;
	desc->data.desc_id = atomic_add_return(1, &xm2mvsc->desc_count);
	desc->src_kaddr = dma_alloc_coherent(xm2mvsc->dev,
					     desc->data.srcbuf_size,
					     &desc->srcbuf_addr,
					     GFP_KERNEL | GFP_DMA32);
	if (!desc->src_kaddr)
		return -ENOMEM;
	desc->dst_kaddr = dma_alloc_coherent(xm2mvsc->dev,
					     desc->data.dstbuf_size,
					     &desc->dstbuf_addr,
					     GFP_KERNEL | GFP_DMA32);
	if (!desc->dst_kaddr)
		return -ENOMEM;
	spin_lock_irqsave(&xm2mvsc->lock, flags);
	list_add_tail(&desc->node, &xm2mvsc->pending_list);
	spin_unlock_irqrestore(&xm2mvsc->lock, flags);
	if (copy_to_user(arg, &desc->data, sizeof(desc->data))) {
		dev_err(xm2mvsc->dev,
			"%s : Failed to copy to user for desc_id = %d",
			__func__, desc->data.desc_id);
		return -EFAULT;
	}
	dev_dbg(xm2mvsc->dev, "%s: Desc_id = %d", __func__, desc->data.desc_id);
	return 0;
}

static int xm2mvsc_complete(struct xm2m_vscale_dev *xm2mvsc)
{
	struct xm2m_vscale_desc *desc, *next;
	unsigned long flags;

	spin_lock_irqsave(&xm2mvsc->lock, flags);
	list_for_each_entry_safe(desc, next, &xm2mvsc->ongoing_list, node) {
		list_del(&desc->node);
		list_add_tail(&desc->node, &xm2mvsc->done_list);
		atomic_dec(&xm2mvsc->ongoing_count);
	}
	spin_unlock_irqrestore(&xm2mvsc->lock, flags);
	dev_dbg(xm2mvsc->dev, "%s: ongoing_count = %d",
		__func__, atomic_read(&xm2mvsc->ongoing_count));
	return 0;
}

static int xm2mvsc_ready(struct xm2m_vscale_dev *xm2mvsc)
{
	unsigned long flags;
	struct xm2m_vscale_desc *desc, *next;

	spin_lock_irqsave(&xm2mvsc->lock, flags);
	if (list_empty_careful(&xm2mvsc->pending_list)) {
		spin_unlock_irqrestore(&xm2mvsc->lock, flags);
		return -EAGAIN;
	}
	if (atomic_read(&xm2mvsc->ongoing_count) < xm2mvsc->batch_size) {
		list_for_each_entry_safe(desc, next,
					 &xm2mvsc->pending_list, node) {
			list_del(&desc->node);
			desc->channel_offset =
				atomic_read(&xm2mvsc->ongoing_count);
			WARN(desc->channel_offset > xm2mvsc->hw.max_chan,
			     "%s: Channel offset is beyond supported max",
			     __func__);
			list_add_tail(&desc->node, &xm2mvsc->ongoing_list);
			atomic_inc(&xm2mvsc->ongoing_count);
			dev_dbg(xm2mvsc->dev,
				"%s: Desc_id=%d offset=%d ongoing count=%d",
				__func__, desc->data.desc_id,
				desc->channel_offset,
				atomic_read(&xm2mvsc->ongoing_count));
		}
	}
	spin_unlock_irqrestore(&xm2mvsc->lock, flags);

	if (atomic_read(&xm2mvsc->ongoing_count) == xm2mvsc->batch_size) {
		list_for_each_entry_safe(desc, next,
					 &xm2mvsc->ongoing_list, node) {
			xm2mvsc_write_desc(desc);
		}
		dev_dbg(xm2mvsc->dev, "%s: xm2mvsc_start_scaling", __func__);
		/* Start the IP */
		xm2mvsc_start_scaling(&xm2mvsc->hw, xm2mvsc->batch_size);
	}
	return 0;
}

/* Can be called from IRQ Handler, not allowed to sleep */
static int xm2mvsc_start_running(struct xm2m_vscale_dev *xm2mvsc)
{
	/* Process and make ready */
	return xm2mvsc_ready(xm2mvsc);
}

/*
 * Implementation may need to change to coalesce
 * completion of multiple buffers
 */
static int xm2mvsc_ioctl_dequeue(struct xm2m_vscale_dev *xm2mvsc,
				 void __user *arg)
{
	struct xm2mvsc_dqdata *dqdata;
	struct xm2m_vscale_desc *desc, *next;
	unsigned long flags;

	dqdata = kzalloc(sizeof(*dqdata), GFP_KERNEL);
	if (!dqdata)
		return -ENOMEM;

	if (copy_from_user(dqdata, arg, sizeof(*dqdata))) {
		dev_err(xm2mvsc->dev, "%s: Failed to copy from user", __func__);
		return -EFAULT;
	}

	/* Underflow or ioctl called too early, try later */
	spin_lock_irqsave(&xm2mvsc->lock, flags);
	if (list_empty_careful(&xm2mvsc->done_list)) {
		spin_unlock_irqrestore(&xm2mvsc->lock, flags);
		dev_err(xm2mvsc->dev,
			"%s: failed as done list empty", __func__);
		return -EAGAIN;
	}
	/* Search through the done list, move to free list if found */
	list_for_each_entry_safe(desc, next, &xm2mvsc->done_list, node) {
		if (desc->data.desc_id == dqdata->desc_id) {
			list_del(&desc->node);
			list_add_tail(&desc->node, &xm2mvsc->free_list);
			break;
		}
	}
	spin_unlock_irqrestore(&xm2mvsc->lock, flags);

	/* Reached end of the list */
	if (!desc || desc->data.desc_id != dqdata->desc_id) {
		dev_err(xm2mvsc->dev,
			"%s: Unable to find desc_id = %d in done list",
			__func__, dqdata->desc_id);
		return -EIO;
	}

	return 0;
}

static int xm2mvsc_ioctl_start(struct xm2m_vscale_dev *xm2mvsc)
{
	return xm2mvsc_start_running(xm2mvsc);
}

static void xm2mvsc_free_desc_list(struct list_head *list)
{
	struct xm2m_vscale_desc *desc, *next;

	list_for_each_entry_safe(desc, next, list, node) {
		list_del(&desc->node);
		kfree(desc);
	}
}

/*  PS GPIO RESET MACROS */
#define XM2MVSC_RESET_ASSERT	(0x1)
#define XM2MVSC_RESET_DEASSERT	(0x0)

static void xm2mvsc_reset(struct xm2m_vscale_dev *xm2mvsc)
{
	gpiod_set_value_cansleep(xm2mvsc->rst_gpio, XM2MVSC_RESET_ASSERT);
	gpiod_set_value_cansleep(xm2mvsc->rst_gpio, XM2MVSC_RESET_DEASSERT);
}

static void xm2mvsc_clear_state(struct xm2m_vscale_dev *xm2mvsc)
{
	unsigned long flags;

	spin_lock_irqsave(&xm2mvsc->lock, flags);
	xm2mvsc_free_desc_list(&xm2mvsc->pending_list);
	xm2mvsc_free_desc_list(&xm2mvsc->ongoing_list);
	xm2mvsc_free_desc_list(&xm2mvsc->done_list);
	xm2mvsc_free_desc_list(&xm2mvsc->free_list);
	spin_unlock_irqrestore(&xm2mvsc->lock, flags);

	spin_lock_irqsave(&xm2mvsc->lock, flags);
	INIT_LIST_HEAD(&xm2mvsc->pending_list);
	INIT_LIST_HEAD(&xm2mvsc->ongoing_list);
	INIT_LIST_HEAD(&xm2mvsc->done_list);
	INIT_LIST_HEAD(&xm2mvsc->free_list);
	spin_unlock_irqrestore(&xm2mvsc->lock, flags);
}

static int xm2mvsc_ioctl_stop(struct xm2m_vscale_dev *xm2mvsc)
{
	xm2mvsc_clear_state(xm2mvsc);
	/* Reset IP */
	xm2mvsc_stop_scaling(&xm2mvsc->hw);
	xm2mvsc_reset(xm2mvsc);
	return 0;
}

static int xm2mvsc_ioctl_free(struct xm2m_vscale_dev *xm2mvsc,
			      void __user *arg)
{
	struct xm2mvsc_dqdata *dqdata;
	struct xm2m_vscale_desc *desc, *next;
	int ret;

	dqdata = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!dqdata)
		return -ENOMEM;

	ret = copy_from_user(dqdata, arg, sizeof(*dqdata));
	if (ret < 0) {
		dev_err(xm2mvsc->dev,
			"%s: Failed to copy from user", __func__);
		return -EFAULT;
	}

	list_for_each_entry_safe(desc, next, &xm2mvsc->free_list, node) {
		if (desc->data.desc_id == dqdata->desc_id) {
			list_del(&desc->node);
			break;
		}
	}

	if (!desc || desc->data.desc_id != dqdata->desc_id) {
		dev_err(xm2mvsc->dev,
			"%s: Desc_id = %d not found in free list",
			__func__, dqdata->desc_id);
		kfree(dqdata);
		return -EBADF;
	}

	dma_free_coherent(xm2mvsc->dev, desc->data.srcbuf_size,
			  desc->src_kaddr, desc->srcbuf_addr);
	dma_free_coherent(xm2mvsc->dev, desc->data.dstbuf_size,
			  desc->dst_kaddr, desc->dstbuf_addr);
	kfree(dqdata);
	kfree(desc);
	return 0;
}

static long xm2mvsc_ioctl(struct file *fptr,
			  unsigned int cmd, unsigned long data)
{
	struct xm2m_vscale_dev *xm2mvsc;
	void __user *arg;
	int ret;

	xm2mvsc = fptr->private_data;
	arg = (void __user *)data;

	if (!xm2mvsc || !arg) {
		pr_err("%s: file op error", __func__);
		return -EIO;
	}

	switch (cmd) {
	case XM2MVSC_ENQUEUE:
		ret = xm2mvsc_ioctl_enqueue(xm2mvsc, arg);
		if (ret < 0)
			return ret;
		return 0;
	case XM2MVSC_DEQUEUE:
		ret = xm2mvsc_ioctl_dequeue(xm2mvsc, arg);
		if (ret < 0)
			return ret;
		return 0;
	case XM2MVSC_START:
		ret = xm2mvsc_ioctl_start(xm2mvsc);
		if (ret < 0)
			return ret;
		return 0;
	case XM2MVSC_STOP:
		ret = xm2mvsc_ioctl_stop(xm2mvsc);
		if (ret < 0)
			return ret;
		return 0;
	case XM2MVSC_FREE:
		ret = xm2mvsc_ioctl_free(xm2mvsc, arg);
		if (ret < 0)
			return ret;
		return 0;
	case XM2MVSC_BATCH_SIZE:
		ret = xm2mvsc_ioctl_batch_size(xm2mvsc, arg);
		if (ret < 0)
			return ret;
		return 0;
	default:
		dev_err(xm2mvsc->dev, "Unsupported ioctl cmd");
		return -EINVAL;
	}
}

/*
 * First call  maps the source buffer,
 * second call maps the destination buffer
 */
static int xm2mvsc_mmap(struct file *fptr, struct vm_area_struct *vma)
{
	struct xm2m_vscale_dev *xm2mvsc = fptr->private_data;
	struct xm2m_vscale_desc *desc, *next;
	int ret, desc_id;
	unsigned long flags;

	if (!xm2mvsc) {
		pr_err("xm2mvsc file private data is NULL");
		return -EIO;
	}

	desc_id = vma->vm_pgoff;

	spin_lock_irqsave(&xm2mvsc->lock, flags);
	list_for_each_entry_safe(desc, next, &xm2mvsc->pending_list, node) {
		if (desc->data.desc_id == desc_id)
			break;
	}
	spin_unlock_irqrestore(&xm2mvsc->lock, flags);
	if (!desc || desc->data.desc_id != desc_id) {
		dev_err(xm2mvsc->dev,
			"Unable to find desc_id = %d in pending list",
			desc_id);
		return -EIO;
	}
	if (!desc->src_kaddr && !desc->dst_kaddr) {
		dev_err(xm2mvsc->dev, "Enqueue before mmap for desc_id = %d",
			desc->data.desc_id);
	}
	if (desc->data.srcbuf_mmap && desc->data.dstbuf_mmap) {
		dev_err(xm2mvsc->dev,
			"Src and Dest buffs already mmap'ed for desc_id = %d",
			desc->data.desc_id);
		return -EIO;
	}
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	if (!desc->data.srcbuf_mmap) {
		ret = remap_pfn_range(vma, vma->vm_start,
				      desc->srcbuf_addr >> PAGE_SHIFT,
				      vma->vm_end - vma->vm_start,
				      vma->vm_page_prot);
		if (ret) {
			dev_err(xm2mvsc->dev,
				"mmap op failed for srcbuf of desc_id = %d",
				desc->data.desc_id);
			ret = -EAGAIN;
			goto error_mmap;
		}
		desc->data.srcbuf_mmap = true;
		goto success_mmap;
	}
	if (!desc->data.dstbuf_mmap) {
		ret = remap_pfn_range(vma, vma->vm_start,
				      desc->dstbuf_addr >> PAGE_SHIFT,
				      vma->vm_end - vma->vm_start,
				      vma->vm_page_prot);
		if (ret) {
			dev_err(xm2mvsc->dev,
				"mmap op failed for dstbuf of desc_id = %d",
				desc->data.desc_id);
			ret = -EAGAIN;
			goto error_mmap;
		}
		desc->data.dstbuf_mmap = true;
		goto success_mmap;
	}
success_mmap:
	vma->vm_private_data = xm2mvsc;
	return 0;
error_mmap:
	dev_err(xm2mvsc->dev, "%s: failed %d", __func__, ret);
	return ret;
}

static unsigned int xm2mvsc_poll(struct file *fptr, poll_table *wait)
{
	struct xm2m_vscale_dev *xm2mvsc = fptr->private_data;

	if (!xm2mvsc)
		return 0;

	poll_wait(fptr, &xm2mvsc->waitq, wait);
	if (!list_empty_careful(&xm2mvsc->done_list))
		return POLLIN | POLLPRI;
	return 0;
}

static const struct file_operations xm2mvsc_fops = {
	.open = xm2mvsc_open,
	.release = xm2mvsc_release,
	.unlocked_ioctl = xm2mvsc_ioctl,
	.poll = xm2mvsc_poll,
	.mmap = xm2mvsc_mmap,
};

static irqreturn_t xm2mvsc_intr_handler(int irq, void *ctx)
{
	u32 status;
	struct xm2m_vscale_dev *xm2mvsc = (struct xm2m_vscale_dev *)ctx;

	WARN(!xm2mvsc, "%s: xm2mvsc is NULL", __func__);
	WARN(xm2mvsc->irq != irq,
	     "IRQ registered %d does not match IRQ received %d",
	     xm2mvsc->irq, irq);

	status = xm2mvsc_get_irq_status(&xm2mvsc->hw);
	if (status) {
		/* The ongoing descriptors list should be cleared */
		(void)xm2mvsc_complete(xm2mvsc);
		wake_up_interruptible(&xm2mvsc->waitq);
		/* Program next operation if any*/
		(void)xm2mvsc_start_running(xm2mvsc);
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

#define XM2MVSC_OF_TAPS		"xlnx,scaler-num-taps"
#define XM2MVSC_OF_MAX_CHAN	"xlnx,scaler-max-chan"
static int xm2m_vscale_parse_dt_prop(struct xm2m_vscale_dev *xm2mvsc)
{
	struct device_node *node;
	int ret;

	if (!xm2mvsc)
		return -EIO;
	node = xm2mvsc->dev->of_node;

	ret = of_property_read_u32(node, XM2MVSC_OF_TAPS,
				   &xm2mvsc->hw.num_taps);
	if (ret < 0)
		return ret;
	switch (xm2mvsc->hw.num_taps) {
	case XV_SCALER_TAPS_6:
	case XV_SCALER_TAPS_8:
	case XV_SCALER_TAPS_10:
	case XV_SCALER_TAPS_12:
		break;
	default:
		dev_err(xm2mvsc->dev,
			"Unsupported M2M Scaler taps : %d",
			xm2mvsc->hw.num_taps);
		return -EINVAL;
	}

	ret = of_property_read_u32(node, XM2MVSC_OF_MAX_CHAN,
				   &xm2mvsc->hw.max_chan);
	if (ret < 0)
		return ret;
	if (xm2mvsc->hw.max_chan < XSCALER_BATCH_SIZE_MIN ||
	    xm2mvsc->hw.max_chan > XSCALER_BATCH_SIZE_MAX) {
		dev_err(xm2mvsc->dev,
			"Invalid maximum scaler channels : %d",
			xm2mvsc->hw.max_chan);
		return -EINVAL;
	}
	/* Reset PS GPIO specifier is optional for now */
	xm2mvsc->rst_gpio = devm_gpiod_get(xm2mvsc->dev,
					   "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(xm2mvsc->rst_gpio)) {
		if (PTR_ERR(xm2mvsc->rst_gpio) != -EPROBE_DEFER) {
			dev_err(xm2mvsc->dev,
				"Reset GPIO specifier not setup in DT");
		}
		return PTR_ERR(xm2mvsc->rst_gpio);
	}

	xm2mvsc->irq = irq_of_parse_and_map(node, 0);
	if (xm2mvsc->irq < 0) {
		dev_err(xm2mvsc->dev, "Unable to get IRQ");
		return xm2mvsc->irq;
	}

	return 0;
}

static int xm2m_vscale_probe(struct platform_device *pdev)
{
	struct xm2m_vscale_dev *xm2mvsc;
	struct device *dc;
	struct resource *res;
	int ret;

	if (atomic_read(&xm2mvsc_ndevs) >= DRIVER_MAX_DEV) {
		dev_err(&pdev->dev,
			"Unable to create xm2mvsc devices beyond max %d",
			DRIVER_MAX_DEV);
		return -EIO;
	}

	xm2mvsc = devm_kzalloc(&pdev->dev, sizeof(*xm2mvsc), GFP_KERNEL);
	if (!xm2mvsc)
		return -ENOMEM;
	xm2mvsc->dev = &pdev->dev;
	xm2mvsc->hw.dev = &pdev->dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	xm2mvsc->hw.regs = devm_ioremap_resource(xm2mvsc->dev, res);
	if (IS_ERR(xm2mvsc->hw.regs))
		return PTR_ERR(xm2mvsc->hw.regs);
	ret = xm2m_vscale_parse_dt_prop(xm2mvsc);
	if (ret < 0)
		return ret;
	xm2mvsc_reset(xm2mvsc);

	/* Initialize Scaler Properties */
	xm2mvsc->hw.max_lines = XM2MVSC_MAX_HEIGHT;
	xm2mvsc->hw.max_pixels = XM2MVSC_MAX_WIDTH;
	xm2mvsc_initialize_coeff_banks(&xm2mvsc->hw);

	init_waitqueue_head(&xm2mvsc->waitq);
	spin_lock_init(&xm2mvsc->lock);
	INIT_LIST_HEAD(&xm2mvsc->pending_list);
	INIT_LIST_HEAD(&xm2mvsc->ongoing_list);
	INIT_LIST_HEAD(&xm2mvsc->done_list);
	INIT_LIST_HEAD(&xm2mvsc->free_list);
	ret = devm_request_irq(xm2mvsc->dev, xm2mvsc->irq,
			       xm2mvsc_intr_handler, IRQF_SHARED,
			       DRIVER_NAME, xm2mvsc);
	if (ret < 0) {
		dev_err(xm2mvsc->dev, "Unable to register IRQ");
		return ret;
	}

	cdev_init(&xm2mvsc->chdev, &xm2mvsc_fops);
	xm2mvsc->chdev.owner = THIS_MODULE;
	xm2mvsc->id = atomic_read(&xm2mvsc_ndevs);
	ret = cdev_add(&xm2mvsc->chdev,
		       MKDEV(MAJOR(xm2mvsc_devt), xm2mvsc->id), 1);
	if (ret < 0) {
		dev_err(xm2mvsc->dev, "cdev_add failed");
		return ret;
	}

	if (!xm2mvsc_class) {
		dev_err(xm2mvsc->dev, "xm2mvsc device class not created");
		goto err_cdev;
	}
	dc = device_create(xm2mvsc_class, xm2mvsc->dev,
			   MKDEV(MAJOR(xm2mvsc_devt), xm2mvsc->id),
			   xm2mvsc, "xm2mvsc%d", xm2mvsc->id);
	if (IS_ERR(dc)) {
		ret = PTR_ERR(dc);
		dev_err(xm2mvsc->dev, "Unable to create device");
		goto err_cdev;
	}
	platform_set_drvdata(pdev, xm2mvsc);
	dev_info(xm2mvsc->dev,
		 "Xilinx M2M Video Scaler %d tap %d channel device probe complete",
		 xm2mvsc->hw.num_taps, xm2mvsc->hw.max_chan);
	atomic_inc(&xm2mvsc_ndevs);
	return 0;
err_cdev:
	cdev_del(&xm2mvsc->chdev);
	return ret;
}

static int xm2m_vscale_remove(struct platform_device *pdev)
{
	struct xm2m_vscale_dev *xm2mvsc;

	xm2mvsc = platform_get_drvdata(pdev);
	if (!xm2mvsc || !xm2mvsc_class)
		return -EIO;
	device_destroy(xm2mvsc_class,
		       MKDEV(MAJOR(xm2mvsc_devt), xm2mvsc->id));
	cdev_del(&xm2mvsc->chdev);
	atomic_dec(&xm2mvsc_ndevs);
	return 0;
}

static const struct of_device_id xm2mvsc_of_match[] = {
	{ .compatible = "xlnx,v-m2m-scaler", },
	{ /* end of table*/ }
};
MODULE_DEVICE_TABLE(of, xm2mvsc_of_match);

static struct platform_driver xm2mvsc_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = xm2mvsc_of_match,
	},
	.probe = xm2m_vscale_probe,
	.remove = xm2m_vscale_remove,
};

static int __init xm2mvsc_init_mod(void)
{
	int err;

	xm2mvsc_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(xm2mvsc_class)) {
		pr_err("%s : Unable to create xm2mvsc class", __func__);
		return PTR_ERR(xm2mvsc_class);
	}
	err = alloc_chrdev_region(&xm2mvsc_devt, 0,
				  DRIVER_MAX_DEV, DRIVER_NAME);
	if (err < 0) {
		pr_err("%s: Unable to get major number for xm2mvsc", __func__);
		goto err_class;
	}
	err = platform_driver_register(&xm2mvsc_driver);
	if (err < 0) {
		pr_err("%s: Unable to register %s driver",
		       __func__, DRIVER_NAME);
		goto err_pdrv;
	}
	return 0;
err_pdrv:
	unregister_chrdev_region(xm2mvsc_devt, DRIVER_MAX_DEV);
err_class:
	class_destroy(xm2mvsc_class);
	return err;
}

static void __exit xm2mvsc_cleanup_mod(void)
{
	platform_driver_unregister(&xm2mvsc_driver);
	unregister_chrdev_region(xm2mvsc_devt, DRIVER_MAX_DEV);
	class_destroy(xm2mvsc_class);
	xm2mvsc_class = NULL;
}
module_init(xm2mvsc_init_mod);
module_exit(xm2mvsc_cleanup_mod);

MODULE_AUTHOR("Xilinx Inc.");
MODULE_DESCRIPTION("Xilinx M2M Video Scaler IP Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRIVER_VERSION);
