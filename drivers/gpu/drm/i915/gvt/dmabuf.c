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

#define GEN8_DECODE_PTE(pte) \
	((dma_addr_t)(((((u64)pte) >> 12) & 0x7ffffffULL) << 12))

static struct sg_table *intel_vgpu_gem_get_pages(
		struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *dev_priv = to_i915(obj->base.dev);
	struct sg_table *st;
	struct scatterlist *sg;
	int i, ret;
	gen8_pte_t __iomem *gtt_entries;
	unsigned int fb_gma = 0, fb_size = 0;
	struct intel_vgpu_plane_info *plane_info;

	plane_info = (struct intel_vgpu_plane_info *)obj->gvt_plane_info;
	if (WARN_ON(!plane_info))
		return ERR_PTR(-EINVAL);

	fb_gma = plane_info->start;
	fb_size = plane_info->size;

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st) {
		ret = -ENOMEM;
		return ERR_PTR(ret);
	}

	ret = sg_alloc_table(st, fb_size, GFP_KERNEL);
	if (ret) {
		kfree(st);
		return ERR_PTR(ret);
	}
	gtt_entries = (gen8_pte_t __iomem *)dev_priv->ggtt.gsm +
		(fb_gma >> PAGE_SHIFT);
	for_each_sg(st->sgl, sg, fb_size, i) {
		sg->offset = 0;
		sg->length = PAGE_SIZE;
		sg_dma_address(sg) =
			GEN8_DECODE_PTE(readq(&gtt_entries[i]));
		sg_dma_len(sg) = PAGE_SIZE;
	}

	return st;
}

static void intel_vgpu_gem_put_pages(struct drm_i915_gem_object *obj,
		struct sg_table *pages)
{
	struct intel_vgpu_plane_info *plane_info;

	plane_info = (struct intel_vgpu_plane_info *)obj->gvt_plane_info;
	if (WARN_ON(!plane_info))
		return;

	sg_free_table(pages);
	kfree(pages);
}

static const struct drm_i915_gem_object_ops intel_vgpu_gem_ops = {
	.flags = I915_GEM_OBJECT_IS_GVT_DMABUF,
	.get_pages = intel_vgpu_gem_get_pages,
	.put_pages = intel_vgpu_gem_put_pages,
};

static struct drm_i915_gem_object *intel_vgpu_create_gem(struct drm_device *dev,
		struct intel_vgpu_plane_info *info)
{
	struct drm_i915_private *pri = dev->dev_private;
	struct drm_i915_gem_object *obj;

	obj = i915_gem_object_alloc(pri);
	if (obj == NULL)
		return NULL;

	drm_gem_private_object_init(dev, &obj->base,
		info->size << PAGE_SHIFT);
	i915_gem_object_init(obj, &intel_vgpu_gem_ops);

	obj->base.read_domains = I915_GEM_DOMAIN_GTT;
	obj->base.write_domain = 0;
	obj->framebuffer_references++;
	obj->gvt_plane_info = info;

	if (IS_SKYLAKE(pri)) {
		unsigned int tiling_mode = 0;

		switch (info->drm_format_mod << 10) {
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
			gvt_dbg_core("not supported tiling mode\n");
		}
		obj->tiling_and_stride = tiling_mode | info->stride;
	} else {
		obj->tiling_and_stride = info->drm_format_mod ?
					I915_TILING_X : 0;
	}

	return obj;
}

static struct intel_vgpu_plane_info *intel_vgpu_get_plane_info(
		struct drm_device *dev,
		struct intel_vgpu *vgpu, int plane_id)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_vgpu_primary_plane_format *p;
	struct intel_vgpu_cursor_plane_format *c;
	struct intel_vgpu_plane_info *info;

	info = kmalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return NULL;

	if (plane_id == INTEL_GVT_PLANE_PRIMARY) {
		p = (struct intel_vgpu_primary_plane_format *)
			intel_vgpu_decode_plane(dev, vgpu, plane_id);
		if (p != NULL) {
			info->start = p->base;
			info->width = p->width;
			info->height = p->height;
			info->stride = p->stride;
			info->drm_format = p->drm_format;
			info->drm_format_mod = p->tiled;
			info->size = (((p->stride * p->height * p->bpp) / 8) +
					(PAGE_SIZE - 1)) >> PAGE_SHIFT;
		} else {
			kfree(info);
			gvt_vgpu_err("invalid primary plane\n");
			return NULL;
		}
	} else if (plane_id == INTEL_GVT_PLANE_CURSOR) {
		c = (struct intel_vgpu_cursor_plane_format *)
			intel_vgpu_decode_plane(dev, vgpu, plane_id);
		if (c != NULL) {
			info->start = c->base;
			info->width = c->width;
			info->height = c->height;
			info->stride = c->width * (c->bpp / 8);
			info->drm_format_mod = 0;
			info->x_pos = c->x_pos;
			info->y_pos = c->y_pos;
			info->size = (((info->stride * c->height * c->bpp) / 8)
					+ (PAGE_SIZE - 1)) >> PAGE_SHIFT;
		} else {
			kfree(info);
			gvt_vgpu_err("invalid cursor plane\n");
			return NULL;
		}
	} else {
		kfree(info);
		gvt_vgpu_err("invalid plane id:%d\n", plane_id);
		return NULL;
	}

	if (info->size == 0) {
		kfree(info);
		gvt_vgpu_err("fb size is zero\n");
		return NULL;
	}

	if (info->start & (PAGE_SIZE - 1)) {
		kfree(info);
		gvt_vgpu_err("Not aligned fb address:0x%x\n", info->start);
		return NULL;
	}
	if (((info->start >> PAGE_SHIFT) + info->size) >
		ggtt_total_entries(&dev_priv->ggtt)) {
		kfree(info);
		gvt_vgpu_err("Invalid GTT offset or size\n");
		return NULL;
	}

	if (!intel_gvt_ggtt_validate_range(vgpu, info->start, info->size)) {
		kfree(info);
		gvt_vgpu_err("invalid gma addr\n");
		return NULL;
	}

	return info;
}

int intel_vgpu_query_dmabuf(struct intel_vgpu *vgpu, void *args)
{
	struct drm_device *dev = &vgpu->gvt->dev_priv->drm;
	struct intel_vgpu_dmabuf *gvt_dmabuf = args;
	struct intel_vgpu_plane_info *info;

	info = intel_vgpu_get_plane_info(dev, vgpu, gvt_dmabuf->plane_id);
	if (info == NULL)
		return -EINVAL;

	gvt_dmabuf->plane_info = *info;

	return 0;
}

int intel_vgpu_create_dmabuf(struct intel_vgpu *vgpu, void *args)
{
	struct dma_buf *dmabuf;
	struct drm_i915_gem_object *obj;
	struct drm_device *dev = &vgpu->gvt->dev_priv->drm;
	struct intel_vgpu_dmabuf *gvt_dmabuf = args;
	struct intel_vgpu_plane_info *info;
	int ret;

	info = intel_vgpu_get_plane_info(dev, vgpu, gvt_dmabuf->plane_id);
	if (info == NULL)
		return -EINVAL;

	obj = intel_vgpu_create_gem(dev, info);
	if (obj == NULL) {
		gvt_vgpu_err("create gvt gem obj failed:%d\n", vgpu->id);
		return -EINVAL;
	}

	dmabuf = i915_gem_prime_export(dev, &obj->base, DRM_CLOEXEC | DRM_RDWR);

	if (IS_ERR(dmabuf)) {
		gvt_vgpu_err("export dma-buf failed\n");
		return -EINVAL;
	}

	ret = dma_buf_fd(dmabuf, DRM_CLOEXEC | DRM_RDWR);
	if (ret < 0) {
		gvt_vgpu_err("create dma-buf fd failed ret:%d\n", ret);
		return -EINVAL;
	}
	gvt_dmabuf->fd = ret;
	gvt_dmabuf->plane_info = *info;

	return 0;
}
