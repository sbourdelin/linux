/*
 * Copyright © 2015 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <drm/drmP.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"

static void __i915_gem_object_free_pages(struct sg_table *st)
{
	struct sgt_iter iter;
	struct page *page;

	for_each_sgt_page(page, iter, st)
		put_page(page);

	sg_free_table(st);
	kfree(st);
}

static int i915_gem_object_get_pages_internal(struct drm_i915_gem_object *obj)
{
	const unsigned int npages = obj->base.size / PAGE_SIZE;
	struct sg_table *st;
	struct scatterlist *sg;
	unsigned long last_pfn = 0;	/* suppress gcc warning */
	gfp_t gfp;
	int i;

	st = kmalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	if (sg_alloc_table(st, npages, GFP_KERNEL)) {
		kfree(st);
		return -ENOMEM;
	}

	sg = st->sgl;
	st->nents = 0;

	gfp = GFP_KERNEL | __GFP_HIGHMEM;
	gfp |= __GFP_NORETRY | __GFP_NOWARN;
	gfp &= ~(__GFP_IO | __GFP_RECLAIM);
	for (i = 0; i < npages; i++) {
		struct page *page;

		page = alloc_page(gfp);
		if (!page) {
			i915_gem_shrink_all(to_i915(obj->base.dev));
			page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM);
			if (!page)
				goto err;
		}

#ifdef CONFIG_SWIOTLB
		if (swiotlb_nr_tbl()) {
			st->nents++;
			sg_set_page(sg, page, PAGE_SIZE, 0);
			sg = sg_next(sg);
			continue;
		}
#endif
		if (!i || page_to_pfn(page) != last_pfn + 1) {
			if (i)
				sg = sg_next(sg);
			st->nents++;
			sg_set_page(sg, page, PAGE_SIZE, 0);
		} else {
			sg->length += PAGE_SIZE;
		}
		last_pfn = page_to_pfn(page);
	}
#ifdef CONFIG_SWIOTLB
	if (!swiotlb_nr_tbl())
#endif
		sg_mark_end(sg);
	obj->mm.pages = st;

	if (i915_gem_gtt_prepare_object(obj)) {
		obj->mm.pages = NULL;
		goto err;
	}

	obj->mm.madv = I915_MADV_DONTNEED;
	return 0;

err:
	sg_mark_end(sg);
	__i915_gem_object_free_pages(st);
	return -ENOMEM;
}

static void i915_gem_object_put_pages_internal(struct drm_i915_gem_object *obj)
{
	__i915_gem_object_free_pages(obj->mm.pages);

	obj->mm.dirty = false;
	obj->mm.madv = I915_MADV_WILLNEED;
}

static const struct drm_i915_gem_object_ops i915_gem_object_internal_ops = {
	.flags = I915_GEM_OBJECT_HAS_STRUCT_PAGE,
	.get_pages = i915_gem_object_get_pages_internal,
	.put_pages = i915_gem_object_put_pages_internal,
};

/**
 * Creates a new object that wraps some internal memory for private use.
 * This object is not backed by swappable storage, and as such its contents
 * are volatile and only valid whilst pinned. If the object is reaped by the
 * shrinker, its pages and data will be discarded. Equally, it is not a full
 * GEM object and so not valid for access from userspace. This makes it useful
 * for hardware interfaces like ringbuffers (which are pinned from the time
 * the request is written to the time the hardware stops accessing it), but
 * not for contexts (which need to be preserved when not active for later
 * reuse).
 */
struct drm_i915_gem_object *
i915_gem_object_create_internal(struct drm_device *dev,
				unsigned int size)
{
	struct drm_i915_gem_object *obj;

	obj = i915_gem_object_alloc(dev);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	drm_gem_private_object_init(dev, &obj->base, size);
	i915_gem_object_init(obj, &i915_gem_object_internal_ops);

	obj->base.write_domain = I915_GEM_DOMAIN_CPU;
	obj->base.read_domains = I915_GEM_DOMAIN_CPU;
	obj->cache_level = HAS_LLC(dev) ? I915_CACHE_LLC : I915_CACHE_NONE;

	return obj;
}
