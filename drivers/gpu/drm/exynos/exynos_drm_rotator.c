/*
 * Copyright (C) 2012 Samsung Electronics Co.Ltd
 * Authors:
 *	YoungJun Cho <yj44.cho@samsung.com>
 *	Eunchul Kim <chulspro.kim@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundationr
 */

#include <linux/kernel.h>
#include <linux/component.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>

#include <drm/drmP.h>
#include <drm/exynos_drm.h>
#include "regs-rotator.h"
#include "exynos_drm_fb.h"
#include "exynos_drm_drv.h"
#include "exynos_drm_iommu.h"
#include "exynos_drm_pp.h"

/*
 * Rotator supports image crop/rotator and input/output DMA operations.
 * input DMA reads image data from the memory.
 * output DMA writes image data to memory.
 */

#define get_rot_context(dev)	platform_get_drvdata(to_platform_device(dev))
#define rot_read(offset)		readl(rot->regs + (offset))
#define rot_write(cfg, offset)	writel(cfg, rot->regs + (offset))

enum rot_irq_status {
	ROT_IRQ_STATUS_COMPLETE	= 8,
	ROT_IRQ_STATUS_ILLEGAL	= 9,
};

/*
 * A structure of limitation.
 *
 * @min_w: minimum width.
 * @min_h: minimum height.
 * @max_w: maximum width.
 * @max_h: maximum height.
 * @align: align size.
 */
struct rot_limit {
	u32	min_w;
	u32	min_h;
	u32	max_w;
	u32	max_h;
	u32	align;
};

/*
 * A structure of limitation table.
 *
 * @ycbcr420_2p: case of YUV.
 * @rgb888: case of RGB.
 */
struct rot_limit_table {
	struct rot_limit	ycbcr420_2p;
	struct rot_limit	rgb888;
};

/*
 * A structure of rotator context.
 * @ippdrv: prepare initialization using ippdrv.
 * @regs: memory mapped io registers.
 * @clock: rotator gate clock.
 * @limit_tbl: limitation of rotator.
 * @suspended: suspended state.
 */
struct rot_context {
	struct exynos_drm_pp pp;
	struct drm_device *drm_dev;
	struct device	*dev;
	void __iomem	*regs;
	struct clk	*clock;
	struct rot_limit_table	*limit_tbl;
	bool	suspended;
	struct exynos_drm_pp_task	*task;
};

static void rotator_reg_set_irq(struct rot_context *rot, bool enable)
{
	u32 val = rot_read(ROT_CONFIG);

	if (enable == true)
		val |= ROT_CONFIG_IRQ;
	else
		val &= ~ROT_CONFIG_IRQ;

	rot_write(val, ROT_CONFIG);
}

static u32 rotator_reg_get_fmt(struct rot_context *rot)
{
	u32 val = rot_read(ROT_CONTROL);

	val &= ROT_CONTROL_FMT_MASK;

	return val;
}

static enum rot_irq_status rotator_reg_get_irq_status(struct rot_context *rot)
{
	u32 val = rot_read(ROT_STATUS);

	val = ROT_STATUS_IRQ(val);

	if (val == ROT_STATUS_IRQ_VAL_COMPLETE)
		return ROT_IRQ_STATUS_COMPLETE;

	return ROT_IRQ_STATUS_ILLEGAL;
}

static irqreturn_t rotator_irq_handler(int irq, void *arg)
{
	struct rot_context *rot = arg;
	enum rot_irq_status irq_status;
	u32 val;

	/* Get execution result */
	irq_status = rotator_reg_get_irq_status(rot);

	/* clear status */
	val = rot_read(ROT_STATUS);
	val |= ROT_STATUS_IRQ_PENDING((u32)irq_status);
	rot_write(val, ROT_STATUS);

	if (rot->task) {
		struct exynos_drm_pp_task *task = rot->task;

		rot->task = NULL;
		pm_runtime_put(rot->dev);
		exynos_drm_pp_task_done(task,
			irq_status == ROT_IRQ_STATUS_COMPLETE ? 0 : -EINVAL);
	}

	return IRQ_HANDLED;
}

static void rotator_align_size(struct rot_context *rot, u32 fmt, u32 *hsize,
		u32 *vsize)
{
	struct rot_limit_table *limit_tbl = rot->limit_tbl;
	struct rot_limit *limit;
	u32 mask, val;

	/* Get size limit */
	if (fmt == ROT_CONTROL_FMT_RGB888)
		limit = &limit_tbl->rgb888;
	else
		limit = &limit_tbl->ycbcr420_2p;

	/* Get mask for rounding to nearest aligned val */
	mask = ~((1 << limit->align) - 1);

	/* Set aligned width */
	val = ROT_ALIGN(*hsize, limit->align, mask);
	if (val < limit->min_w)
		*hsize = ROT_MIN(limit->min_w, mask);
	else if (val > limit->max_w)
		*hsize = ROT_MAX(limit->max_w, mask);
	else
		*hsize = val;

	/* Set aligned height */
	val = ROT_ALIGN(*vsize, limit->align, mask);
	if (val < limit->min_h)
		*vsize = ROT_MIN(limit->min_h, mask);
	else if (val > limit->max_h)
		*vsize = ROT_MAX(limit->max_h, mask);
	else
		*vsize = val;
}

static int rotator_src_set_fmt(struct device *dev, u32 fmt)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	u32 val;

	val = rot_read(ROT_CONTROL);
	val &= ~ROT_CONTROL_FMT_MASK;

	switch (fmt) {
	case DRM_FORMAT_NV12:
		val |= ROT_CONTROL_FMT_YCBCR420_2P;
		break;
	case DRM_FORMAT_XRGB8888:
		val |= ROT_CONTROL_FMT_RGB888;
		break;
	}

	rot_write(val, ROT_CONTROL);

	return 0;
}

static int rotator_src_set_buf(struct device *dev, struct drm_exynos_pos *pos,
			       struct drm_framebuffer *fb)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	u32 fmt, hsize, vsize;
	u32 val;

	fmt = rotator_reg_get_fmt(rot);

	/* Align buffer size */
	hsize = fb->width;
	vsize = fb->height;
	rotator_align_size(rot, fmt, &hsize, &vsize);

	/* Set buffer size configuration */
	val = ROT_SET_BUF_SIZE_H(vsize) | ROT_SET_BUF_SIZE_W(hsize);
	rot_write(val, ROT_SRC_BUF_SIZE);

	/* Set crop image position configuration */
	val = ROT_CROP_POS_Y(pos->y) | ROT_CROP_POS_X(pos->x);
	rot_write(val, ROT_SRC_CROP_POS);
	val = ROT_SRC_CROP_SIZE_H(pos->h) | ROT_SRC_CROP_SIZE_W(pos->w);
	rot_write(val, ROT_SRC_CROP_SIZE);

	/* Set buffer DMA address */
	rot_write(exynos_drm_fb_dma_addr(fb, 0), ROT_SRC_BUF_ADDR(0));
	rot_write(exynos_drm_fb_dma_addr(fb, 1), ROT_SRC_BUF_ADDR(1));

	return 0;
}

static int rotator_dst_set_transf(struct device *dev, unsigned int rotation)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	u32 val;

	/* Set transform configuration */
	val = rot_read(ROT_CONTROL);

	val &= ~ROT_CONTROL_FLIP_MASK;

	if (rotation & DRM_REFLECT_Y)
		val |= ROT_CONTROL_FLIP_VERTICAL;
	if (rotation & DRM_REFLECT_X)
		val |= ROT_CONTROL_FLIP_HORIZONTAL;

	val &= ~ROT_CONTROL_ROT_MASK;

	if (rotation & DRM_ROTATE_90)
		val |= ROT_CONTROL_ROT_90;
	else if (rotation & DRM_ROTATE_180)
		val |= ROT_CONTROL_ROT_180;
	else if (rotation & DRM_ROTATE_270)
		val |= ROT_CONTROL_ROT_270;

	rot_write(val, ROT_CONTROL);

	return 0;
}

static int rotator_dst_set_buf(struct device *dev, struct drm_exynos_pos *pos,
			       struct drm_framebuffer *fb)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	u32 fmt, hsize, vsize;
	u32 val;

	fmt = rotator_reg_get_fmt(rot);

	/* Align buffer size */
	hsize = fb->width;
	vsize = fb->height;
	rotator_align_size(rot, fmt, &hsize, &vsize);

	/* Set buffer size configuration */
	val = ROT_SET_BUF_SIZE_H(vsize) | ROT_SET_BUF_SIZE_W(hsize);
	rot_write(val, ROT_DST_BUF_SIZE);

	/* Set crop image position configuration */
	val = ROT_CROP_POS_Y(pos->y) | ROT_CROP_POS_X(pos->x);
	rot_write(val, ROT_DST_CROP_POS);

	/* Set buffer DMA address */
	rot_write(exynos_drm_fb_dma_addr(fb, 0), ROT_DST_BUF_ADDR(0));
	rot_write(exynos_drm_fb_dma_addr(fb, 1), ROT_DST_BUF_ADDR(1));

	return 0;
}

static int rotator_start(struct device *dev)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	u32 val;

	/* Set interrupt enable */
	rotator_reg_set_irq(rot, true);

	val = rot_read(ROT_CONTROL);
	val |= ROT_CONTROL_START;
	rot_write(val, ROT_CONTROL);

	return 0;
}

static struct rot_limit_table rot_limit_tbl_4210 = {
	.ycbcr420_2p = {
		.min_w = 32,
		.min_h = 32,
		.max_w = SZ_64K,
		.max_h = SZ_64K,
		.align = 3,
	},
	.rgb888 = {
		.min_w = 8,
		.min_h = 8,
		.max_w = SZ_16K,
		.max_h = SZ_16K,
		.align = 2,
	},
};

static struct rot_limit_table rot_limit_tbl_4x12 = {
	.ycbcr420_2p = {
		.min_w = 32,
		.min_h = 32,
		.max_w = SZ_32K,
		.max_h = SZ_32K,
		.align = 3,
	},
	.rgb888 = {
		.min_w = 8,
		.min_h = 8,
		.max_w = SZ_8K,
		.max_h = SZ_8K,
		.align = 2,
	},
};

static struct rot_limit_table rot_limit_tbl_5250 = {
	.ycbcr420_2p = {
		.min_w = 32,
		.min_h = 32,
		.max_w = SZ_32K,
		.max_h = SZ_32K,
		.align = 3,
	},
	.rgb888 = {
		.min_w = 8,
		.min_h = 8,
		.max_w = SZ_8K,
		.max_h = SZ_8K,
		.align = 1,
	},
};

static const struct of_device_id exynos_rotator_match[] = {
	{
		.compatible = "samsung,exynos4210-rotator",
		.data = &rot_limit_tbl_4210,
	},
	{
		.compatible = "samsung,exynos4212-rotator",
		.data = &rot_limit_tbl_4x12,
	},
	{
		.compatible = "samsung,exynos5250-rotator",
		.data = &rot_limit_tbl_5250,
	},
	{},
};
MODULE_DEVICE_TABLE(of, exynos_rotator_match);

static int rotator_commit(struct exynos_drm_pp *pp,
			  struct exynos_drm_pp_task *task)
{
	struct rot_context *rot =
			container_of(pp, struct rot_context, pp);
	struct device *dev = rot->dev;
	struct drm_exynos_pos src_pos = {
		task->src_x >> 16, task->src_y >> 16,
		task->src_w >> 16, task->src_h >> 16,
	};
	struct drm_exynos_pos dst_pos = {
		task->dst_x >> 16, task->dst_y >> 16,
		task->dst_w >> 16, task->dst_h >> 16,
	};

	pm_runtime_get_sync(dev);
	rot->task = task;

	rotator_src_set_fmt(dev, task->src_fb->format->format);
	rotator_src_set_buf(dev, &src_pos, task->src_fb);
	rotator_dst_set_transf(dev, task->rotation);
	rotator_dst_set_buf(dev, &dst_pos, task->dst_fb);
	rotator_start(dev);

	return 0;
}

struct exynos_drm_pp_funcs pp_funcs = {
	.commit = rotator_commit,
};

static const uint32_t rotator_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_NV12,
};

static int rotator_bind(struct device *dev, struct device *master, void *data)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	struct exynos_drm_pp *pp = &rot->pp;

	rot->drm_dev = drm_dev;
	drm_iommu_attach_device(drm_dev, dev);

	exynos_drm_pp_register(drm_dev, pp, &pp_funcs,
			   DRM_EXYNOS_PP_CAP_CROP | DRM_EXYNOS_PP_CAP_ROTATE,
			   rotator_formats, ARRAY_SIZE(rotator_formats),
			   rotator_formats, ARRAY_SIZE(rotator_formats),
			   DRM_ROTATE_0 | DRM_ROTATE_90 | DRM_ROTATE_180 |
			   DRM_ROTATE_270 | DRM_REFLECT_X | DRM_REFLECT_Y,
			   "rotator");

	dev_info(dev, "The exynos rotator has been probed successfully\n");

	return 0;
}

static void rotator_unbind(struct device *dev, struct device *master,
			void *data)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	struct exynos_drm_pp *pp = &rot->pp;

	exynos_drm_pp_unregister(drm_dev, pp);
	drm_iommu_detach_device(rot->drm_dev, rot->dev);
}

static const struct component_ops rotator_component_ops = {
	.bind	= rotator_bind,
	.unbind = rotator_unbind,
};

static int rotator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource	*regs_res;
	struct rot_context *rot;
	int irq;
	int ret;

	if (!dev->of_node) {
		dev_err(dev, "cannot find of_node.\n");
		return -ENODEV;
	}

	rot = devm_kzalloc(dev, sizeof(*rot), GFP_KERNEL);
	if (!rot)
		return -ENOMEM;

	rot->limit_tbl = (struct rot_limit_table *)
				of_device_get_match_data(dev);
	rot->dev = dev;
	regs_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	rot->regs = devm_ioremap_resource(dev, regs_res);
	if (IS_ERR(rot->regs))
		return PTR_ERR(rot->regs);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "failed to get irq\n");
		return irq;
	}

	ret = devm_request_threaded_irq(dev, irq, NULL,	rotator_irq_handler,
					IRQF_ONESHOT, "drm_rotator", rot);
	if (ret < 0) {
		dev_err(dev, "failed to request irq\n");
		return ret;
	}

	rot->clock = devm_clk_get(dev, "rotator");
	if (IS_ERR(rot->clock)) {
		dev_err(dev, "failed to get clock\n");
		return PTR_ERR(rot->clock);
	}

	pm_runtime_enable(dev);
	platform_set_drvdata(pdev, rot);

	ret = component_add(dev, &rotator_component_ops);
	if (ret)
		goto err_ippdrv_register;

	return 0;

err_ippdrv_register:
	pm_runtime_disable(dev);
	return ret;
}

static int rotator_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	component_del(dev, &rotator_component_ops);
	pm_runtime_disable(dev);

	return 0;
}

#ifdef CONFIG_PM
static int rotator_clk_crtl(struct rot_context *rot, bool enable)
{
	if (enable) {
		clk_prepare_enable(rot->clock);
		rot->suspended = false;
	} else {
		clk_disable_unprepare(rot->clock);
		rot->suspended = true;
	}

	return 0;
}

static int rotator_runtime_suspend(struct device *dev)
{
	struct rot_context *rot = dev_get_drvdata(dev);

	return  rotator_clk_crtl(rot, false);
}

static int rotator_runtime_resume(struct device *dev)
{
	struct rot_context *rot = dev_get_drvdata(dev);

	return  rotator_clk_crtl(rot, true);
}
#endif

static const struct dev_pm_ops rotator_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rotator_runtime_suspend, rotator_runtime_resume,
									NULL)
};

struct platform_driver rotator_driver = {
	.probe		= rotator_probe,
	.remove		= rotator_remove,
	.driver		= {
		.name	= "exynos-rot",
		.owner	= THIS_MODULE,
		.pm	= &rotator_pm_ops,
		.of_match_table = exynos_rotator_match,
	},
};
