/*
 * Copyright 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software")
 * to deal in the software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * them Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTIBILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT, OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/dma-buf.h>
#include <linux/reservation.h>

#include "vgem_drv.h"

struct vgem_fence {
	struct fence base;
	struct spinlock lock;
};

static const char *vgem_fence_get_driver_name(struct fence *fence)
{
	return "vgem";
}

static const char *vgem_fence_get_timeline_name(struct fence *fence)
{
	return "file";
}

static bool vgem_fence_signaled(struct fence *fence)
{
	return false;
}

static bool vgem_fence_enable_signaling(struct fence *fence)
{
	return true;
}

static void vgem_fence_value_str(struct fence *fence, char *str, int size)
{
	snprintf(str, size, "%u", fence->seqno);
}

static void vgem_fence_timeline_value_str(struct fence *fence, char *str,
					  int size)
{
	snprintf(str, size, "%u", 0);
}

const struct fence_ops vgem_fence_ops = {
	.get_driver_name = vgem_fence_get_driver_name,
	.get_timeline_name = vgem_fence_get_timeline_name,
	.enable_signaling = vgem_fence_enable_signaling,
	.signaled = vgem_fence_signaled,
	.wait = fence_default_wait,
	.fence_value_str = vgem_fence_value_str,
	.timeline_value_str = vgem_fence_timeline_value_str,
};

static u32 vgem_fence_next_seqno(struct vgem_file *vfile)
{
	u32 seqno;

	seqno = atomic_inc_return(&vfile->fence_seqno);
	if (seqno == 0)
		seqno = atomic_inc_return(&vfile->fence_seqno);

	return seqno;
}

static struct fence *vgem_fence_create(struct vgem_file *vfile)
{
	struct vgem_fence *fence;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return NULL;

	spin_lock_init(&fence->lock);
	fence_init(&fence->base,
		   &vgem_fence_ops,
		   &fence->lock,
		   vfile->fence_context,
		   vgem_fence_next_seqno(vfile));

	return &fence->base;
}

static int attach_dmabuf(struct drm_device *dev,
			 struct drm_gem_object *obj)
{
	struct dma_buf *dmabuf;

	if (obj->dma_buf)
		return 0;

	dmabuf = dev->driver->gem_prime_export(dev, obj, 0);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	obj->dma_buf = dmabuf;
	drm_gem_object_reference(obj);
	return 0;
}

int vgem_fence_attach_ioctl(struct drm_device *dev,
			    void *data,
			    struct drm_file *file)
{
	struct drm_vgem_fence_attach *arg = data;
	struct vgem_file *vfile = file->driver_priv;
	struct reservation_object *resv;
	struct drm_gem_object *obj;
	struct fence *fence;
	int ret;

	if (arg->flags & ~VGEM_FENCE_WRITE)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, arg->handle);
	if (!obj)
		return -ENOENT;

	ret = attach_dmabuf(dev, obj);
	if (ret)
		goto out;

	fence = vgem_fence_create(vfile);
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}

	ret = 0;
	resv = obj->dma_buf->resv;
	mutex_lock(&resv->lock.base);
	if (arg->flags & VGEM_FENCE_WRITE)
		reservation_object_add_excl_fence(resv, fence);
	else if ((ret = reservation_object_reserve_shared(resv)) == 0)
		reservation_object_add_shared_fence(resv, fence);
	mutex_unlock(&resv->lock.base);

	if (ret == 0) {
		mutex_lock(&vfile->fence_mutex);
		ret = idr_alloc(&vfile->fence_idr, fence, 1, 0, GFP_KERNEL);
		mutex_unlock(&vfile->fence_mutex);
		if (ret > 0) {
			arg->out_fence = ret;
			ret = 0;
		}
	}
	if (ret)
		fence_put(fence);
out:
	drm_gem_object_unreference_unlocked(obj);
	return ret;
}

int vgem_fence_signal_ioctl(struct drm_device *dev,
			    void *data,
			    struct drm_file *file)
{
	struct vgem_file *vfile = file->driver_priv;
	struct drm_vgem_fence_signal *arg = data;
	struct fence *fence;

	if (arg->flags)
		return -EINVAL;

	mutex_lock(&vfile->fence_mutex);
	fence = idr_replace(&vfile->fence_idr, NULL, arg->fence);
	mutex_unlock(&vfile->fence_mutex);
	if (!fence)
		return -ENOENT;
	if (IS_ERR(fence))
		return PTR_ERR(fence);

	fence_signal(fence);
	fence_put(fence);
	return 0;
}

int vgem_fence_open(struct vgem_file *vfile)
{
	mutex_init(&vfile->fence_mutex);
	idr_init(&vfile->fence_idr);
	vfile->fence_context = fence_context_alloc(1);

	return 0;
}

static int __vgem_fence_idr_fini(int id, void *p, void *data)
{
	fence_signal(p);
	fence_put(p);
	return 0;
}

void vgem_fence_close(struct vgem_file *vfile)
{
	idr_for_each(&vfile->fence_idr, __vgem_fence_idr_fini, vfile);
	idr_destroy(&vfile->fence_idr);
}
