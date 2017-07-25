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
 *    Tina Zhang <tina.zhang@intel.com>
 */

#include <linux/dma-buf.h>
#include <drm/drmP.h>
#include <linux/vfio.h>

#include "i915_drv.h"
#include "gvt.h"

#define GEN8_DECODE_PTE(pte) (pte & GENMASK_ULL(63, 12))

#define VBLNAK_TIMER_PERIOD 16000000

static struct sg_table *intel_vgpu_gem_get_pages(
		struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *dev_priv = to_i915(obj->base.dev);
	struct sg_table *st;
	struct scatterlist *sg;
	int i, ret;
	gen8_pte_t __iomem *gtt_entries;
	struct intel_vgpu_fb_info *fb_info;

	fb_info = (struct intel_vgpu_fb_info *)obj->gvt_info;
	if (WARN_ON(!fb_info))
		return ERR_PTR(-ENODEV);

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(st, fb_info->size, GFP_KERNEL);
	if (ret) {
		kfree(st);
		return ERR_PTR(ret);
	}
	gtt_entries = (gen8_pte_t __iomem *)dev_priv->ggtt.gsm +
		(fb_info->start >> PAGE_SHIFT);
	for_each_sg(st->sgl, sg, fb_info->size, i) {
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
	sg_free_table(pages);
	kfree(pages);
}

static void intel_vgpu_gem_release(struct drm_i915_gem_object *obj)
{
	struct intel_vgpu_dmabuf_obj *dmabuf_obj;
	struct intel_vgpu_fb_info *fb_info;
	struct intel_vgpu *vgpu;
	struct list_head *pos;

	fb_info = (struct intel_vgpu_fb_info *)obj->gvt_info;
	if (WARN_ON(!fb_info || !fb_info->vgpu)) {
		gvt_err("gvt info is invalid\n");
		goto out;
	}

	vgpu = fb_info->vgpu;
	mutex_lock(&vgpu->dmabuf_list_lock);
	list_for_each(pos, &vgpu->dmabuf_obj_list_head) {
		dmabuf_obj = container_of(pos, struct intel_vgpu_dmabuf_obj,
						list);
		if ((dmabuf_obj != NULL) && (dmabuf_obj->obj == obj)) {
			kfree(dmabuf_obj);
			list_del(pos);
			break;
		}
	}
	mutex_unlock(&vgpu->dmabuf_list_lock);
	intel_gvt_hypervisor_put_vfio_device(vgpu);
out:
	kfree(obj->gvt_info);
}

static const struct drm_i915_gem_object_ops intel_vgpu_gem_ops = {
	.flags = I915_GEM_OBJECT_IS_PROXY,
	.get_pages = intel_vgpu_gem_get_pages,
	.put_pages = intel_vgpu_gem_put_pages,
	.release = intel_vgpu_gem_release,
};

static struct drm_i915_gem_object *intel_vgpu_create_gem(struct drm_device *dev,
		struct intel_vgpu_fb_info *info)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_gem_object *obj;

	obj = i915_gem_object_alloc(dev_priv);
	if (obj == NULL)
		return NULL;

	drm_gem_private_object_init(dev, &obj->base,
		info->size << PAGE_SHIFT);
	i915_gem_object_init(obj, &intel_vgpu_gem_ops);

	obj->base.read_domains = I915_GEM_DOMAIN_GTT;
	obj->base.write_domain = 0;
	if (IS_SKYLAKE(dev_priv)) {
		unsigned int tiling_mode = 0;
		unsigned int stride = 0;

		switch (info->drm_format_mod << 10) {
		case PLANE_CTL_TILED_LINEAR:
			tiling_mode = I915_TILING_NONE;
			break;
		case PLANE_CTL_TILED_X:
			tiling_mode = I915_TILING_X;
			stride = info->stride;
			break;
		case PLANE_CTL_TILED_Y:
			tiling_mode = I915_TILING_Y;
			stride = info->stride;
			break;
		default:
			gvt_dbg_core("not supported tiling mode\n");
		}
		obj->tiling_and_stride = tiling_mode | stride;
	} else {
		obj->tiling_and_stride = info->drm_format_mod ?
					I915_TILING_X : 0;
	}

	return obj;
}

static int intel_vgpu_get_plane_info(struct drm_device *dev,
		struct intel_vgpu *vgpu,
		struct intel_vgpu_fb_info *info,
		int plane_id)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct intel_vgpu_primary_plane_format p;
	struct intel_vgpu_cursor_plane_format c;
	int ret;

	if (plane_id == DRM_PLANE_TYPE_PRIMARY) {
		ret = intel_vgpu_decode_primary_plane(vgpu, &p);
		if (ret)
			return ret;
		info->start = p.base;
		info->start_gpa = p.base_gpa;
		info->width = p.width;
		info->height = p.height;
		info->stride = p.stride;
		info->drm_format = p.drm_format;
		info->drm_format_mod = p.tiled;
		info->size = (((p.stride * p.height * p.bpp) / 8) +
				(PAGE_SIZE - 1)) >> PAGE_SHIFT;
	} else if (plane_id == DRM_PLANE_TYPE_CURSOR) {
		ret = intel_vgpu_decode_cursor_plane(vgpu, &c);
		if (ret)
			return ret;
		info->start = c.base;
		info->start_gpa = c.base_gpa;
		info->width = c.width;
		info->height = c.height;
		info->stride = c.width * (c.bpp / 8);
		info->drm_format = c.drm_format;
		info->drm_format_mod = 0;
		info->x_pos = c.x_pos;
		info->y_pos = c.y_pos;
		info->size = (((info->stride * c.height * c.bpp) / 8)
				+ (PAGE_SIZE - 1)) >> PAGE_SHIFT;
	} else {
		gvt_vgpu_err("invalid plane id:%d\n", plane_id);
		return -EINVAL;
	}

	if (info->size == 0) {
		gvt_vgpu_err("fb size is zero\n");
		return -EINVAL;
	}

	if (info->start & (PAGE_SIZE - 1)) {
		gvt_vgpu_err("Not aligned fb address:0x%llx\n", info->start);
		return -EFAULT;
	}
	if (((info->start >> PAGE_SHIFT) + info->size) >
		ggtt_total_entries(&dev_priv->ggtt)) {
		gvt_vgpu_err("Invalid GTT offset or size\n");
		return -EFAULT;
	}

	if (!intel_gvt_ggtt_validate_range(vgpu, info->start, info->size)) {
		gvt_vgpu_err("invalid gma addr\n");
		return -EFAULT;
	}

	return 0;
}

static int intel_vgpu_pick_exposed_dmabuf(struct intel_vgpu *vgpu,
		struct intel_vgpu_fb_info *latest_info)
{
	struct list_head *pos;
	struct intel_vgpu_fb_info *fb_info;
	struct intel_vgpu_dmabuf_obj *dmabuf_obj;
	struct dma_buf *dmabuf;
	int ret = -1;

	mutex_lock(&vgpu->dmabuf_list_lock);
	list_for_each(pos, &vgpu->dmabuf_obj_list_head) {
		dmabuf_obj = container_of(pos, struct intel_vgpu_dmabuf_obj,
						list);
		if ((dmabuf_obj == NULL) ||
		    (dmabuf_obj->obj == NULL) ||
		    (dmabuf_obj->obj->gvt_info == NULL))
			continue;

		fb_info = dmabuf_obj->obj->gvt_info;

		if ((fb_info->start == latest_info->start) &&
		    (fb_info->start_gpa == latest_info->start_gpa) &&
		    (fb_info->size == latest_info->size) &&
		    (fb_info->drm_format_mod == latest_info->drm_format_mod) &&
		    (fb_info->drm_format == latest_info->drm_format) &&
		    (fb_info->width == latest_info->width) &&
		    (fb_info->height == latest_info->height) &&
		    (fb_info->stride == latest_info->stride)) {
			dmabuf = dma_buf_get(dmabuf_obj->fd);
			if (IS_ERR(dmabuf)) {
				continue;
			}
			dma_buf_put(dmabuf);
			ret = dmabuf_obj->fd;
			break;
		}
	}
	mutex_unlock(&vgpu->dmabuf_list_lock);

	return ret;
}

static void update_fb_info(struct vfio_device_gfx_plane_info *gvt_dmabuf,
		      struct intel_vgpu_fb_info *fb_info)
{
	gvt_dmabuf->drm_format = fb_info->drm_format;
	gvt_dmabuf->width = fb_info->width;
	gvt_dmabuf->height = fb_info->height;
	gvt_dmabuf->stride = fb_info->stride;
	gvt_dmabuf->size = fb_info->size;
	gvt_dmabuf->x_pos = fb_info->x_pos;
	gvt_dmabuf->y_pos = fb_info->y_pos;
}

int intel_vgpu_query_plane(struct intel_vgpu *vgpu, void *args)
{
	struct drm_device *dev = &vgpu->gvt->dev_priv->drm;
	struct vfio_device_gfx_plane_info *gvt_dmabuf = args;
	struct intel_vgpu_dmabuf_obj *dmabuf_obj;
	struct drm_i915_gem_object *obj;
	struct intel_vgpu_fb_info fb_info;
	struct dma_buf *dmabuf;
	int ret;

	ret = intel_vgpu_get_plane_info(dev, vgpu, &fb_info,
					gvt_dmabuf->drm_plane_type);
	if (ret != 0)
		goto out;

	/* If exists, pick up the exposed dmabuf fd */
	ret = intel_vgpu_pick_exposed_dmabuf(vgpu, &fb_info);
	if (ret > 0) {
		update_fb_info(gvt_dmabuf, &fb_info);
		gvt_dmabuf->fd = ret;
		ret = 0;
		goto out;
	}

	/* Need to expose a new one*/
	dmabuf_obj = kmalloc(sizeof(struct intel_vgpu_dmabuf_obj), GFP_KERNEL);
	if (!dmabuf_obj) {
		gvt_vgpu_err("alloc dmabuf_obj failed\n");
		ret = -ENOMEM;
		goto out;
	}

	obj = intel_vgpu_create_gem(dev, &fb_info);
	if (obj == NULL) {
		gvt_vgpu_err("create gvt gem obj failed:%d\n", vgpu->id);
		ret = -ENOMEM;
		goto out_free_dmabuf;
	}

	dmabuf_obj->obj = obj;

	obj->gvt_info = kmalloc(sizeof(struct intel_vgpu_fb_info), GFP_KERNEL);
	if (!obj->gvt_info) {
		gvt_vgpu_err("allocate intel vgpu fb info failed\n");
		ret = -ENOMEM;
		goto out_free_gem;
	}

	fb_info.vgpu = vgpu;
	dmabuf_obj->vgpu = vgpu;
	memcpy(obj->gvt_info, &fb_info, sizeof(struct intel_vgpu_fb_info));

	dmabuf = i915_gem_prime_export(dev, &obj->base, DRM_CLOEXEC | DRM_RDWR);
	if (IS_ERR(dmabuf)) {
		gvt_vgpu_err("export dma-buf failed\n");
		ret = PTR_ERR(dmabuf);
		goto out_free_info;
	}
	i915_gem_object_put(obj);
	obj->base.dma_buf = dmabuf;

	ret = dma_buf_fd(dmabuf, DRM_CLOEXEC | DRM_RDWR);
	if (ret < 0) {
		gvt_vgpu_err("create dma-buf fd failed ret:%d\n", ret);
		goto out_free;
	}

	if (intel_gvt_hypervisor_get_vfio_device(vgpu)) {
		gvt_vgpu_err("get vfio device failed\n");
		put_unused_fd(ret);
		goto out_free;
	}

	update_fb_info(gvt_dmabuf, &fb_info);
	gvt_dmabuf->fd = ret;
	dmabuf_obj->fd = ret;

	INIT_LIST_HEAD(&dmabuf_obj->list);
	mutex_lock(&vgpu->dmabuf_list_lock);
	list_add_tail(&dmabuf_obj->list, &vgpu->dmabuf_obj_list_head);
	mutex_unlock(&vgpu->dmabuf_list_lock);

	return 0;

out_free:
	dma_buf_put(dmabuf);
out_free_info:
	kfree(obj->gvt_info);
out_free_gem:
	i915_gem_object_put(obj);
out_free_dmabuf:
	kfree(dmabuf_obj);
out:
	return ret;
}
