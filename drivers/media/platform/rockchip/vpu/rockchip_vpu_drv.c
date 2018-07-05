// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Collabora, Ltd.
 * Copyright (C) 2014 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "rockchip_vpu.h"
#include "rockchip_vpu_enc.h"
#include "rockchip_vpu_hw.h"

#define DRIVER_NAME "rockchip-vpu"

int rockchip_vpu_debug;
module_param_named(debug, rockchip_vpu_debug, int, 0644);
MODULE_PARM_DESC(debug,
		 "Debug level - higher value produces more verbose messages");

static inline struct rockchip_vpu_ctx *
rockchip_vpu_set_ctx(struct rockchip_vpu_dev *vpu,
		     struct rockchip_vpu_ctx *new_ctx)
{
	struct rockchip_vpu_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&vpu->irqlock, flags);
	ctx = vpu->running_ctx;
	vpu->running_ctx = new_ctx;
	spin_unlock_irqrestore(&vpu->irqlock, flags);

	return ctx;
}

void rockchip_vpu_irq_done(struct rockchip_vpu_dev *vpu)
{
	struct rockchip_vpu_ctx *ctx = rockchip_vpu_set_ctx(vpu, NULL);

	/* Atomic watchdog cancel. The worker may still be
	 * running after calling this.
	 */
	cancel_delayed_work(&vpu->watchdog_work);
	if (ctx)
		ctx->codec_ops->done(ctx, VB2_BUF_STATE_DONE);
}

void rockchip_vpu_watchdog(struct work_struct *work)
{
	struct rockchip_vpu_dev *vpu = container_of(to_delayed_work(work),
		struct rockchip_vpu_dev, watchdog_work);
	struct rockchip_vpu_ctx *ctx = rockchip_vpu_set_ctx(vpu, NULL);

	if (ctx) {
		vpu_err("frame processing timed out!\n");
		ctx->codec_ops->reset(ctx);
		ctx->codec_ops->done(ctx, VB2_BUF_STATE_ERROR);
	}
}

static void device_run(void *priv)
{
	struct rockchip_vpu_ctx *ctx = priv;
	struct rockchip_vpu_dev *vpu = ctx->dev;

	rockchip_vpu_set_ctx(vpu, ctx);
	ctx->codec_ops->run(ctx);
}

static struct v4l2_m2m_ops vpu_m2m_ops = {
	.device_run = device_run,
};

static int
queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct rockchip_vpu_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &rockchip_vpu_enc_queue_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES |
			    DMA_ATTR_NO_KERNEL_MAPPING;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->vpu_mutex;
	src_vq->dev = ctx->dev->v4l2_dev.dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &rockchip_vpu_enc_queue_ops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->vpu_mutex;
	dst_vq->dev = ctx->dev->v4l2_dev.dev;

	return vb2_queue_init(dst_vq);
}

/*
 * V4L2 file operations.
 */

static int rockchip_vpu_open(struct file *filp)
{
	struct rockchip_vpu_dev *vpu = video_drvdata(filp);
	struct rockchip_vpu_ctx *ctx;
	int ret;

	/*
	 * We do not need any extra locking here, because we operate only
	 * on local data here, except reading few fields from dev, which
	 * do not change through device's lifetime (which is guaranteed by
	 * reference on module from open()) and V4L2 internal objects (such
	 * as vdev and ctx->fh), which have proper locking done in respective
	 * helper functions used here.
	 */

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = vpu;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(vpu->m2m_dev, ctx, &queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		kfree(ctx);
		return ret;
	}
	v4l2_fh_init(&ctx->fh, video_devdata(filp));
	filp->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	ctx->colorspace = V4L2_COLORSPACE_JPEG,
	ctx->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	ctx->quantization = V4L2_QUANTIZATION_DEFAULT;
	ctx->xfer_func = V4L2_XFER_FUNC_DEFAULT;

	ret = rockchip_vpu_enc_init(ctx);
	if (ret) {
		vpu_err("Failed to initialize encoder context\n");
		goto err_fh_free;
	}
	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	return 0;

err_fh_free:
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int rockchip_vpu_release(struct file *filp)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(filp->private_data);

	/*
	 * No need for extra locking because this was the last reference
	 * to this file.
	 */
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	rockchip_vpu_enc_exit(ctx);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations rockchip_vpu_fops = {
	.owner = THIS_MODULE,
	.open = rockchip_vpu_open,
	.release = rockchip_vpu_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct of_device_id of_rockchip_vpu_match[] = {
	{ .compatible = "rockchip,rk3399-vpu", .data = &rk3399_vpu_variant, },
	{ .compatible = "rockchip,rk3288-vpu", .data = &rk3288_vpu_variant, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_rockchip_vpu_match);

static int rockchip_vpu_video_device_register(struct rockchip_vpu_dev *vpu)
{
	struct video_device *vfd;
	int ret;

	vfd = video_device_alloc();
	if (!vfd) {
		v4l2_err(&vpu->v4l2_dev, "Failed to allocate video device\n");
		return -ENOMEM;
	}

	vpu->vfd = vfd;
	vfd->fops = &rockchip_vpu_fops;
	vfd->release = video_device_release;
	vfd->lock = &vpu->vpu_mutex;
	vfd->v4l2_dev = &vpu->v4l2_dev;
	vfd->vfl_dir = VFL_DIR_M2M;
	vfd->ioctl_ops = &rockchip_vpu_enc_ioctl_ops;
	snprintf(vfd->name, sizeof(vfd->name), "%s-enc", DRIVER_NAME);

	video_set_drvdata(vfd, vpu);

	vpu->m2m_dev = v4l2_m2m_init(&vpu_m2m_ops);
	if (IS_ERR(vpu->m2m_dev)) {
		v4l2_err(&vpu->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(vpu->m2m_dev);
		goto err_dev_reg;
	}

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		v4l2_err(&vpu->v4l2_dev, "Failed to register video device\n");
		goto err_m2m_rel;
	}
	return 0;

err_m2m_rel:
	v4l2_m2m_release(vpu->m2m_dev);
err_dev_reg:
	video_device_release(vfd);
	return ret;
}

static int rockchip_vpu_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct rockchip_vpu_dev *vpu;
	struct resource *res;
	int irq, i, ret;

	vpu = devm_kzalloc(&pdev->dev, sizeof(*vpu), GFP_KERNEL);
	if (!vpu)
		return -ENOMEM;

	vpu->dev = &pdev->dev;
	vpu->pdev = pdev;
	mutex_init(&vpu->vpu_mutex);
	spin_lock_init(&vpu->irqlock);

	match = of_match_node(of_rockchip_vpu_match, pdev->dev.of_node);
	vpu->variant = match->data;

	INIT_DELAYED_WORK(&vpu->watchdog_work, rockchip_vpu_watchdog);

	for (i = 0; i < vpu->variant->num_clocks; i++) {
		vpu->clocks[i] = devm_clk_get(&pdev->dev,
					      vpu->variant->clk_names[i]);
		if (IS_ERR(vpu->clocks[i])) {
			dev_err(&pdev->dev, "failed to get clock: %s\n",
				vpu->variant->clk_names[i]);
			return PTR_ERR(vpu->clocks[i]);
		}
	}

	res = platform_get_resource(vpu->pdev, IORESOURCE_MEM, 0);
	vpu->base = devm_ioremap_resource(vpu->dev, res);
	if (IS_ERR(vpu->base))
		return PTR_ERR(vpu->base);
	vpu->enc_base = vpu->base + vpu->variant->enc_offset;

	ret = dma_set_coherent_mask(vpu->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(vpu->dev, "Could not set DMA coherent mask.\n");
		return ret;
	}

	irq = platform_get_irq_byname(vpu->pdev, "vepu");
	if (irq <= 0) {
		dev_err(vpu->dev, "Could not get vepu IRQ.\n");
		return -ENXIO;
	}

	ret = devm_request_irq(vpu->dev, irq, vpu->variant->vepu_irq,
			       0, dev_name(vpu->dev), vpu);
	if (ret) {
		dev_err(vpu->dev, "Could not request vepu IRQ.\n");
		return ret;
	}

	ret = vpu->variant->init(vpu);
	if (ret) {
		dev_err(&pdev->dev, "Failed to init VPU hardware\n");
		return ret;
	}

	ret = v4l2_device_register(&pdev->dev, &vpu->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register v4l2 device\n");
		return ret;
	}

	platform_set_drvdata(pdev, vpu);

	pm_runtime_set_autosuspend_delay(vpu->dev, 100);
	pm_runtime_use_autosuspend(vpu->dev);
	pm_runtime_enable(vpu->dev);
	pm_runtime_get_sync(vpu->dev);

	ret = rockchip_vpu_video_device_register(vpu);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register encoder\n");
		goto err_v4l2_dev_unreg;
	}

	return 0;

err_v4l2_dev_unreg:
	v4l2_device_unregister(&vpu->v4l2_dev);
	pm_runtime_mark_last_busy(vpu->dev);
	pm_runtime_put_autosuspend(vpu->dev);
	pm_runtime_disable(vpu->dev);
	return ret;
}

static int rockchip_vpu_remove(struct platform_device *pdev)
{
	struct rockchip_vpu_dev *vpu = platform_get_drvdata(pdev);

	v4l2_info(&vpu->v4l2_dev, "Removing %s\n", pdev->name);

	video_unregister_device(vpu->vfd);
	video_device_release(vpu->vfd);
	v4l2_device_unregister(&vpu->v4l2_dev);
	pm_runtime_mark_last_busy(vpu->dev);
	pm_runtime_put_autosuspend(vpu->dev);
	pm_runtime_disable(vpu->dev);

	return 0;
}

static int __maybe_unused rockchip_vpu_runtime_suspend(struct device *dev)
{
	struct rockchip_vpu_dev *vpu = dev_get_drvdata(dev);
	int i;

	for (i = vpu->variant->num_clocks - 1; i >= 0; i--)
		clk_disable_unprepare(vpu->clocks[i]);
	return 0;
}

static int __maybe_unused rockchip_vpu_runtime_resume(struct device *dev)
{
	struct rockchip_vpu_dev *vpu = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < vpu->variant->num_clocks; i++) {
		int ret;

		ret = clk_prepare_enable(vpu->clocks[i]);
		if (ret) {
			while (--i >= 0)
				clk_disable_unprepare(vpu->clocks[i]);
			return ret;
		}
	}

	return 0;
}

static const struct dev_pm_ops rockchip_vpu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rockchip_vpu_runtime_suspend,
			   rockchip_vpu_runtime_resume, NULL)
};

static struct platform_driver rockchip_vpu_driver = {
	.probe = rockchip_vpu_probe,
	.remove = rockchip_vpu_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = of_match_ptr(of_rockchip_vpu_match),
		   .pm = &rockchip_vpu_pm_ops,
	},
};
module_platform_driver(rockchip_vpu_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Alpha Lin <Alpha.Lin@Rock-Chips.com>");
MODULE_AUTHOR("Tomasz Figa <tfiga@chromium.org>");
MODULE_AUTHOR("Ezequiel Garcia <ezequiel@collabora.com>");
MODULE_DESCRIPTION("Rockchip VPU codec driver");
