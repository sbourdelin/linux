/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */
/*
 * Authors:
 *    Christian König <christian.koenig@amd.com>
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/hmm.h>
#include <drm/drmP.h>
#include <drm/drm.h>

#include "radeon.h"

struct radeon_mn {
	/* constant after initialisation */
	struct radeon_device	*rdev;
	struct mm_struct	*mm;
	struct hmm_mirror	mirror;

	/* only used on destruction */
	struct work_struct	work;

	/* protected by rdev->mn_lock */
	struct hlist_node	node;

	/* objects protected by lock */
	struct mutex		lock;
	struct rb_root_cached	objects;
};

struct radeon_mn_node {
	struct interval_tree_node	it;
	struct list_head		bos;
};

/**
 * radeon_mn_destroy - destroy the rmn
 *
 * @work: previously sheduled work item
 *
 * Lazy destroys the notifier from a work item
 */
static void radeon_mn_destroy(struct work_struct *work)
{
	struct radeon_mn *rmn = container_of(work, struct radeon_mn, work);
	struct radeon_device *rdev = rmn->rdev;
	struct radeon_mn_node *node, *next_node;
	struct radeon_bo *bo, *next_bo;

	mutex_lock(&rdev->mn_lock);
	mutex_lock(&rmn->lock);
	hash_del(&rmn->node);
	rbtree_postorder_for_each_entry_safe(node, next_node,
					     &rmn->objects.rb_root, it.rb) {

		interval_tree_remove(&node->it, &rmn->objects);
		list_for_each_entry_safe(bo, next_bo, &node->bos, mn_list) {
			bo->mn = NULL;
			list_del_init(&bo->mn_list);
		}
		kfree(node);
	}
	mutex_unlock(&rmn->lock);
	mutex_unlock(&rdev->mn_lock);
	hmm_mirror_unregister(&rmn->mirror);
	kfree(rmn);
}

/**
 * radeon_mn_release - callback to notify about mm destruction
 *
 * @mirror: our mirror struct
 *
 * Shedule a work item to lazy destroy our notifier.
 */
static void radeon_mirror_release(struct hmm_mirror *mirror)
{
	struct radeon_mn *rmn = container_of(mirror, struct radeon_mn, mirror);
	INIT_WORK(&rmn->work, radeon_mn_destroy);
	schedule_work(&rmn->work);
}

/**
 * radeon_sync_cpu_device_pagetables - callback to synchronize with mm changes
 *
 * @mirror: our HMM mirror
 * @update: update informations (start, end, event, blockable, ...)
 *
 * We block for all BOs between start and end to be idle and unmap them by
 * moving them into system domain again (trigger a call to ttm_backend_func.
 * unbind see radeon_ttm.c).
 */
static int radeon_sync_cpu_device_pagetables(struct hmm_mirror *mirror,
					     const struct hmm_update *update)
{
	struct radeon_mn *rmn = container_of(mirror, struct radeon_mn, mirror);
	struct ttm_operation_ctx ctx = { false, false };
	struct interval_tree_node *it;
	unsigned long end;
	int ret = 0;

	/* notification is exclusive, but interval is inclusive */
	end = update->end - 1;

	/* TODO we should be able to split locking for interval tree and
	 * the tear down.
	 */
	if (update->blockable)
		mutex_lock(&rmn->lock);
	else if (!mutex_trylock(&rmn->lock))
		return -EAGAIN;

	it = interval_tree_iter_first(&rmn->objects, update->start, end);
	while (it) {
		struct radeon_mn_node *node;
		struct radeon_bo *bo;
		long r;

		if (!update->blockable) {
			ret = -EAGAIN;
			goto out_unlock;
		}

		node = container_of(it, struct radeon_mn_node, it);
		it = interval_tree_iter_next(it, update->start, end);

		list_for_each_entry(bo, &node->bos, mn_list) {

			if (!bo->tbo.ttm || bo->tbo.ttm->state != tt_bound)
				continue;

			r = radeon_bo_reserve(bo, true);
			if (r) {
				DRM_ERROR("(%ld) failed to reserve user bo\n", r);
				continue;
			}

			r = reservation_object_wait_timeout_rcu(bo->tbo.resv,
				true, false, MAX_SCHEDULE_TIMEOUT);
			if (r <= 0)
				DRM_ERROR("(%ld) failed to wait for user bo\n", r);

			radeon_ttm_placement_from_domain(bo, RADEON_GEM_DOMAIN_CPU);
			r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
			if (r)
				DRM_ERROR("(%ld) failed to validate user bo\n", r);

			radeon_bo_unreserve(bo);
		}
	}

out_unlock:
	mutex_unlock(&rmn->lock);

	return ret;
}

static const struct hmm_mirror_ops radeon_mirror_ops = {
	.sync_cpu_device_pagetables = &radeon_sync_cpu_device_pagetables,
	.release = &radeon_mirror_release,
};

/**
 * radeon_mn_get - create notifier context
 *
 * @rdev: radeon device pointer
 *
 * Creates a notifier context for current->mm.
 */
static struct radeon_mn *radeon_mn_get(struct radeon_device *rdev)
{
	struct mm_struct *mm = current->mm;
	struct radeon_mn *rmn, *new;
	int r;

	mutex_lock(&rdev->mn_lock);
	hash_for_each_possible(rdev->mn_hash, rmn, node, (unsigned long)mm) {
		if (rmn->mm == mm) {
			mutex_unlock(&rdev->mn_lock);
			return rmn;
		}
	}
	mutex_unlock(&rdev->mn_lock);

	new = kzalloc(sizeof(*rmn), GFP_KERNEL);
	if (!new) {
		return ERR_PTR(-ENOMEM);
	}
	new->mm = mm;
	new->rdev = rdev;
	mutex_init(&new->lock);
	new->objects = RB_ROOT_CACHED;
	new->mirror.ops = &radeon_mirror_ops;

	if (down_write_killable(&mm->mmap_sem)) {
		kfree(new);
		return ERR_PTR(-EINTR);
	}
	r = hmm_mirror_register(&new->mirror, mm);
	up_write(&mm->mmap_sem);
	if (r) {
		kfree(new);
		return ERR_PTR(r);
	}

	mutex_lock(&rdev->mn_lock);
	/* Check again in case some other thread raced with us ... */
	hash_for_each_possible(rdev->mn_hash, rmn, node, (unsigned long)mm) {
		if (rmn->mm == mm) {
			mutex_unlock(&rdev->mn_lock);
			hmm_mirror_unregister(&new->mirror);
			kfree(new);
			return rmn;
		}
	}
	hash_add(rdev->mn_hash, &new->node, (unsigned long)mm);
	mutex_unlock(&rdev->mn_lock);

	return new;
}

/**
 * radeon_mn_register - register a BO for notifier updates
 *
 * @bo: radeon buffer object
 * @addr: userptr addr we should monitor
 *
 * Registers an MMU notifier for the given BO at the specified address.
 * Returns 0 on success, -ERRNO if anything goes wrong.
 */
int radeon_mn_register(struct radeon_bo *bo, unsigned long addr)
{
	unsigned long end = addr + radeon_bo_size(bo) - 1;
	struct radeon_device *rdev = bo->rdev;
	struct radeon_mn *rmn;
	struct radeon_mn_node *node = NULL;
	struct list_head bos;
	struct interval_tree_node *it;

	bo->userptr = addr;
	bo->pfns = kvmalloc_array(bo->tbo.num_pages, sizeof(uint64_t),
				  GFP_KERNEL | __GFP_ZERO);
	if (bo->pfns == NULL)
		return -ENOMEM;

	rmn = radeon_mn_get(rdev);
	if (IS_ERR(rmn)) {
		kvfree(bo->pfns);
		bo->pfns = NULL;
		return PTR_ERR(rmn);
	}

	INIT_LIST_HEAD(&bos);

	mutex_lock(&rmn->lock);

	while ((it = interval_tree_iter_first(&rmn->objects, addr, end))) {
		kfree(node);
		node = container_of(it, struct radeon_mn_node, it);
		interval_tree_remove(&node->it, &rmn->objects);
		addr = min(it->start, addr);
		end = max(it->last, end);
		list_splice(&node->bos, &bos);
	}

	if (!node) {
		node = kmalloc(sizeof(struct radeon_mn_node), GFP_KERNEL);
		if (!node) {
			mutex_unlock(&rmn->lock);
			kvfree(bo->pfns);
			bo->pfns = NULL;
			return -ENOMEM;
		}
	}

	bo->mn = rmn;

	node->it.start = addr;
	node->it.last = end;
	INIT_LIST_HEAD(&node->bos);
	list_splice(&bos, &node->bos);
	list_add(&bo->mn_list, &node->bos);

	interval_tree_insert(&node->it, &rmn->objects);

	mutex_unlock(&rmn->lock);

	return 0;
}

/**
 * radeon_mn_unregister - unregister a BO for notifier updates
 *
 * @bo: radeon buffer object
 *
 * Remove any registration of MMU notifier updates from the buffer object.
 */
void radeon_mn_unregister(struct radeon_bo *bo)
{
	struct radeon_device *rdev = bo->rdev;
	struct radeon_mn *rmn;
	struct list_head *head;

	mutex_lock(&rdev->mn_lock);
	rmn = bo->mn;
	if (rmn == NULL) {
		mutex_unlock(&rdev->mn_lock);
		return;
	}

	mutex_lock(&rmn->lock);
	/* save the next list entry for later */
	head = bo->mn_list.next;

	bo->mn = NULL;
	list_del(&bo->mn_list);

	if (list_empty(head)) {
		struct radeon_mn_node *node;
		node = container_of(head, struct radeon_mn_node, bos);
		interval_tree_remove(&node->it, &rmn->objects);
		kfree(node);
	}

	mutex_unlock(&rmn->lock);
	mutex_unlock(&rdev->mn_lock);

	kvfree(bo->pfns);
	bo->pfns = NULL;
}

/**
 * radeon_mn_bo_map - map range of virtual address as buffer object
 *
 * @bo: radeon buffer object
 * @ttm: ttm_tt object in which holds mirroring result
 * @write: can GPU write to the range ?
 * Returns: 0 on success, error code otherwise
 *
 * Use HMM to mirror a range of virtual address as a buffer object mapped into
 * GPU address space (thus allowing transparent GPU access to this range). It
 * does not pin pages for range but rely on HMM and underlying synchronizations
 * to make sure that both CPU and GPU points to same physical memory for the
 * range.
 */
int radeon_mn_bo_map(struct radeon_bo *bo, struct ttm_dma_tt *dma, bool write)
{
	static const uint64_t radeon_range_flags[HMM_PFN_FLAG_MAX] = {
		(1 << 0), /* HMM_PFN_VALID */
		(1 << 1), /* HMM_PFN_WRITE */
		0 /* HMM_PFN_DEVICE_PRIVATE */
	};
	static const uint64_t radeon_range_values[HMM_PFN_VALUE_MAX] = {
		0xfffffffffffffffeUL, /* HMM_PFN_ERROR */
		0, /* HMM_PFN_NONE */
		0xfffffffffffffffcUL /* HMM_PFN_SPECIAL */
	};

	unsigned long i, npages = bo->tbo.num_pages;
	enum dma_data_direction direction = write ?
		DMA_BIDIRECTIONAL : DMA_TO_DEVICE;
	struct radeon_device *rdev = bo->rdev;
	struct ttm_tt *ttm = &dma->ttm;
	struct hmm_range range;
	struct radeon_mn *rmn;
	int ret;

	/*
	 * FIXME This whole protection shouldn't be needed as we should only
	 * reach that code with a valid reserved bo that can not under go a
	 * concurrent radeon_mn_unregister().
	 */
	mutex_lock(&rdev->mn_lock);
	if (bo->mn == NULL) {
		mutex_unlock(&rdev->mn_lock);
		return -EINVAL;
	}
	rmn = bo->mn;
	mutex_unlock(&rdev->mn_lock);

	range.pfn_shift = 12;
	range.pfns = bo->pfns;
	range.start = bo->userptr;
	range.flags = radeon_range_flags;
	range.values = radeon_range_values;
	range.end = bo->userptr + radeon_bo_size(bo);

	range.vma = find_vma(rmn->mm, bo->userptr);
	if (!range.vma || range.vma->vm_file || range.vma->vm_end < range.end)
		return -EPERM;

	memset(ttm->pages, 0, sizeof(void*) * npages);

again:
	for (i = 0; i < npages; ++i) {
		range.pfns[i] = range.flags[HMM_PFN_VALID];
		range.pfns[i] |= write ? range.flags[HMM_PFN_WRITE] : 0;
	}

	ret = hmm_vma_fault(&range, true);
	if (ret)
		goto err_unmap;

	for (i = 0; i < npages; ++i) {
		struct page *page = hmm_pfn_to_page(&range, range.pfns[i]);

		if (page == NULL)
			goto again;

		if (ttm->pages[i] == page)
			continue;

		if (ttm->pages[i])
			dma_unmap_page(rdev->dev, dma->dma_address[i],
				       PAGE_SIZE, direction);
		ttm->pages[i] = page;

		dma->dma_address[i] = dma_map_page(rdev->dev, page, 0,
						   PAGE_SIZE, direction);
		if (dma_mapping_error(rdev->dev, dma->dma_address[i])) {
			hmm_vma_range_done(&range);
			ttm->pages[i] = NULL;
			ret = -ENOMEM;
			goto err_unmap;
		}
	}

	/*
	 * Taking rmn->lock is not necessary here as we are protected from any
	 * concurrent invalidation through ttm object reservation. Involved
	 * functions: radeon_sync_cpu_device_pagetables()
	 *            radeon_bo_list_validate()
	 *            radeon_gem_userptr_ioctl()
	 */
	if (!hmm_vma_range_done(&range))
		goto again;

	return 0;

err_unmap:
	radeon_mn_bo_unmap(bo, dma, write);
	return ret;
}

/**
 * radeon_mn_bo_unmap - unmap range of virtual address as buffer object
 *
 * @bo: radeon buffer object
 * @ttm: ttm_tt object in which holds mirroring result
 * @write: can GPU write to the range ?
 * Returns: 0 on success, error code otherwise
 */
void radeon_mn_bo_unmap(struct radeon_bo *bo, struct ttm_dma_tt *dma,
			bool write)
{
	unsigned long i, npages = bo->tbo.num_pages;
	enum dma_data_direction direction = write ?
		DMA_BIDIRECTIONAL : DMA_TO_DEVICE;
	struct radeon_device *rdev = bo->rdev;
	struct ttm_tt *ttm = &dma->ttm;

	for (i = 0; i < npages; ++i) {
		/* No need to go beyond first NULL page */
		if (ttm->pages[i] == NULL)
			break;

		dma_unmap_page(rdev->dev, dma->dma_address[i],
			       PAGE_SIZE, direction);
		ttm->pages[i] = NULL;
	}
}
