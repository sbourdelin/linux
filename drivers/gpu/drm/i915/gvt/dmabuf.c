/*
 * Copyright 2017 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Zhiyuan Lv <zhiyuan.lv@intel.com>
 *
 * Contributors:
 *    Xiaoguang Chen <xiaoguang.chen@intel.com>
 */

#include <linux/dma-buf.h>
#include <drm/drmP.h>

#include "i915_drv.h"
#include "gvt.h"

static struct sg_table *intel_vgpu_gem_get_pages(
		struct drm_i915_gem_object *obj)
{
	WARN_ON(1);
	return NULL;
}

static void intel_vgpu_gem_put_pages(struct drm_i915_gem_object *obj,
		struct sg_table *pages)
{
	/* like stolen memory, this should only be called during free
	 * after clearing pin count.
	 */
	sg_free_table(pages);
	kfree(pages);
}

static const struct drm_i915_gem_object_ops intel_vgpu_gem_ops = {
	.get_pages = intel_vgpu_gem_get_pages,
	.put_pages = intel_vgpu_gem_put_pages,
};

#define GEN8_DECODE_PTE(pte) \
	((dma_addr_t)(((((u64)pte) >> 12) & 0x7ffffffULL) << 12))

#define GEN7_DECODE_PTE(pte) \
	((dma_addr_t)(((((u64)pte) & 0x7f0) << 28) | (u64)(pte & 0xfffff000)))

static struct sg_table *
intel_vgpu_create_sg_pages(struct drm_device *dev, u32 start, u32 num_pages)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct sg_table *st;
	struct scatterlist *sg;
	int i;
	gen8_pte_t __iomem *gtt_entries;

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (st == NULL)
		return NULL;

	if (sg_alloc_table(st, num_pages, GFP_KERNEL)) {
		kfree(st);
		return NULL;
	}

	gtt_entries = (gen8_pte_t __iomem *)dev_priv->ggtt.gsm +
		(start >> PAGE_SHIFT);
	for_each_sg(st->sgl, sg, num_pages, i) {
		sg->offset = 0;
		sg->length = PAGE_SIZE;
		sg_dma_address(sg) =
			GEN8_DECODE_PTE(readq(&gtt_entries[i]));
		sg_dma_len(sg) = PAGE_SIZE;
	}

	return st;
}

static struct drm_i915_gem_object *intel_vgpu_create_gem(struct drm_device *dev,
		struct intel_vgpu_dmabuf *info)
{
	struct drm_i915_gem_object *obj;
	struct drm_i915_private *pri = dev->dev_private;

	obj = i915_gem_object_alloc(pri);
	if (obj == NULL)
		return NULL;

	drm_gem_private_object_init(dev, &obj->base, info->size << PAGE_SHIFT);
	i915_gem_object_init(obj, &intel_vgpu_gem_ops);
	obj->mm.pages = intel_vgpu_create_sg_pages(dev, info->start,
			info->size);
	if (obj->mm.pages == NULL) {
		i915_gem_object_free(obj);
		return NULL;
	}
	obj->cache_level = I915_CACHE_L3_LLC;
	if (IS_SKYLAKE(pri)) {
		unsigned int tiling_mode = 0;

		switch (info->tiled << 10) {
		case PLANE_CTL_TILED_LINEAR:
			tiling_mode = I915_TILING_NONE;
			break;
		case PLANE_CTL_TILED_X:
			tiling_mode = I915_TILING_X;
			break;
		case PLANE_CTL_TILED_Y:
			tiling_mode = I915_TILING_Y;
			break;
		default:
			gvt_dbg_core("tile %d not supported\n", info->tiled);
		}
		obj->tiling_and_stride = tiling_mode | info->stride;
	} else {
		obj->tiling_and_stride = (info->tiled ? I915_TILING_X :
			I915_TILING_NONE) | (info->tiled ? info->stride : 0);
	}

	return obj;
}

static int intel_vgpu_get_plane_info(struct drm_device *dev,
		struct intel_vgpu *vgpu,
		struct intel_vgpu_dmabuf *info)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_vgpu_primary_plane_format *p;
	struct intel_vgpu_cursor_plane_format *c;

	if (info->plane_id == INTEL_GVT_PLANE_PRIMARY) {
		p = (struct intel_vgpu_primary_plane_format *)
			intel_vgpu_decode_plane(dev, vgpu, info->plane_id);
		if (p != NULL) {
			info->start = p->base;
			info->width = p->width;
			info->height = p->height;
			info->stride = p->stride;
			info->drm_format = p->drm_format;
			info->tiled = p->tiled;
			info->size = (((p->stride * p->height * p->bpp) / 8) +
					(PAGE_SIZE - 1)) >> PAGE_SHIFT;
		} else {
			gvt_dbg_core("invalid primary plane\n");
			return -EINVAL;
		}
	} else if (info->plane_id == INTEL_GVT_PLANE_CURSOR) {
		c = (struct intel_vgpu_cursor_plane_format *)
			intel_vgpu_decode_plane(dev, vgpu, info->plane_id);
		if (c != NULL) {
			info->start = c->base;
			info->width = c->width;
			info->height = c->height;
			info->stride = c->width * (c->bpp / 8);
			info->tiled = 0;
			info->x_pos = c->x_pos;
			info->y_pos = c->y_pos;
			info->size = (((info->stride * c->height * c->bpp) / 8) +
					(PAGE_SIZE - 1)) >> PAGE_SHIFT;
		} else {
			gvt_dbg_core("invalid cursor plane\n");
			return -EINVAL;
		}
	} else {
		gvt_vgpu_err("invalid plane id:%d\n", info->plane_id);
		return -EINVAL;
	}

	if (info->start & (PAGE_SIZE - 1)) {
		gvt_vgpu_err("Not aligned fb address:0x%x\n", info->start);
		return -EINVAL;
	}
	if (((info->start >> PAGE_SHIFT) + info->size) >
		ggtt_total_entries(&dev_priv->ggtt)) {
		gvt_vgpu_err("Invalid GTT offset or size\n");
		return -EINVAL;
	}

	return 0;
}

static struct drm_i915_gem_object *intel_vgpu_create_gem_from_vgpuid(
		struct drm_device *dev, struct intel_vgpu *vgpu,
		struct intel_vgpu_dmabuf *info)
{
	struct drm_i915_gem_object *obj;
	int ret;

	ret = intel_vgpu_get_plane_info(dev, vgpu, info);
	if (ret) {
		gvt_vgpu_err("get plane info failed:%d\n", info->plane_id);
		return NULL;
	}
	obj = intel_vgpu_create_gem(dev, info);

	return obj;
}

int intel_vgpu_query_dmabuf(struct intel_vgpu *vgpu, void *args)
{
	struct drm_device *dev = &vgpu->gvt->dev_priv->drm;
	int ret;
	struct intel_vgpu_dmabuf *info = args;

	ret = intel_vgpu_get_plane_info(dev, vgpu, info);
	if (ret) {
		gvt_vgpu_err("get plane info failed:%d\n", info->plane_id);
		return -EINVAL;
	}

	return 0;
}

int intel_vgpu_generate_dmabuf(struct intel_vgpu *vgpu, void *args)
{
	struct dma_buf *dmabuf;
	struct drm_i915_gem_object *obj;
	struct drm_device *dev = &vgpu->gvt->dev_priv->drm;
	int ret;
	struct intel_vgpu_dmabuf *info = args;
	struct dma_buf_export_info exp_info = {
		.exp_name = KBUILD_MODNAME,
		.owner = THIS_MODULE };

	obj = intel_vgpu_create_gem_from_vgpuid(dev, vgpu, info);
	if (obj == NULL) {
		gvt_vgpu_err("create gvt gem obj failed:%d\n", vgpu->id);
		return -EINVAL;
	}

	exp_info.ops = &i915_dmabuf_ops;
	exp_info.size = obj->base.size;
	exp_info.flags = DRM_CLOEXEC | DRM_RDWR;
	exp_info.priv = &obj->base;
	exp_info.resv = obj->resv;

	dmabuf = drm_gem_dmabuf_export(dev, &exp_info);
	if (IS_ERR(dmabuf)) {
		gvt_vgpu_err("intel vgpu export dma-buf failed\n");
		mutex_unlock(&dev->object_name_lock);
		return -EINVAL;
	}

	ret = dma_buf_fd(dmabuf, exp_info.flags);
	if (ret < 0) {
		gvt_vgpu_err("intel vgpu create dma-buf fd failed\n");
		return ret;
	}
	info->fd = ret;

	return 0;
}
