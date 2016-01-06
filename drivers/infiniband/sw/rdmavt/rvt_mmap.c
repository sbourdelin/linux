/*
 * Copyright (c) 2015 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <asm/pgtable.h>

#include "rvt_loc.h"
#include "rvt_queue.h"

void rvt_mmap_release(struct kref *ref)
{
	struct rvt_mmap_info *ip = container_of(ref,
					struct rvt_mmap_info, ref);
	struct rvt_dev *rvt = to_rdev(ip->context->device);

	spin_lock_bh(&rvt->pending_lock);

	if (!list_empty(&ip->pending_mmaps))
		list_del(&ip->pending_mmaps);

	spin_unlock_bh(&rvt->pending_lock);

	vfree(ip->obj);		/* buf */
	kfree(ip);
}

/*
 * open and close keep track of how many times the memory region is mapped,
 * to avoid releasing it.
 */
static void rvt_vma_open(struct vm_area_struct *vma)
{
	struct rvt_mmap_info *ip = vma->vm_private_data;

	kref_get(&ip->ref);
}

static void rvt_vma_close(struct vm_area_struct *vma)
{
	struct rvt_mmap_info *ip = vma->vm_private_data;

	kref_put(&ip->ref, rvt_mmap_release);
}

static struct vm_operations_struct rvt_vm_ops = {
	.open = rvt_vma_open,
	.close = rvt_vma_close,
};

/**
 * rvt_mmap - create a new mmap region
 * @context: the IB user context of the process making the mmap() call
 * @vma: the VMA to be initialized
 * Return zero if the mmap is OK. Otherwise, return an errno.
 */
int rvt_mmap(struct ib_ucontext *context, struct vm_area_struct *vma)
{
	struct rvt_dev *rvt = to_rdev(context->device);
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;
	struct rvt_mmap_info *ip, *pp;
	int ret;

	/*
	 * Search the device's list of objects waiting for a mmap call.
	 * Normally, this list is very short since a call to create a
	 * CQ, QP, or SRQ is soon followed by a call to mmap().
	 */
	spin_lock_bh(&rvt->pending_lock);
	list_for_each_entry_safe(ip, pp, &rvt->pending_mmaps, pending_mmaps) {
		if (context != ip->context || (__u64)offset != ip->info.offset)
			continue;

		/* Don't allow a mmap larger than the object. */
		if (size > ip->info.size) {
			pr_err("mmap region is larger than the object!\n");
			spin_unlock_bh(&rvt->pending_lock);
			ret = -EINVAL;
			goto done;
		}

		goto found_it;
	}
	pr_warn("unable to find pending mmap info\n");
	spin_unlock_bh(&rvt->pending_lock);
	ret = -EINVAL;
	goto done;

found_it:
	list_del_init(&ip->pending_mmaps);
	spin_unlock_bh(&rvt->pending_lock);

	ret = remap_vmalloc_range(vma, ip->obj, 0);
	if (ret) {
		pr_err("rvt: err %d from remap_vmalloc_range\n", ret);
		goto done;
	}

	vma->vm_ops = &rvt_vm_ops;
	vma->vm_private_data = ip;
	rvt_vma_open(vma);
done:
	return ret;
}

/*
 * Allocate information for rvt_mmap
 */
struct rvt_mmap_info *rvt_create_mmap_info(struct rvt_dev *rvt,
					   u32 size,
					   struct ib_ucontext *context,
					   void *obj)
{
	struct rvt_mmap_info *ip;

	ip = kmalloc(sizeof(*ip), GFP_KERNEL);
	if (!ip)
		return NULL;

	size = PAGE_ALIGN(size);

	spin_lock_bh(&rvt->mmap_offset_lock);

	if (rvt->mmap_offset == 0)
		rvt->mmap_offset = PAGE_SIZE;

	ip->info.offset = rvt->mmap_offset;
	rvt->mmap_offset += size;

	spin_unlock_bh(&rvt->mmap_offset_lock);

	INIT_LIST_HEAD(&ip->pending_mmaps);
	ip->info.size = size;
	ip->context = context;
	ip->obj = obj;
	kref_init(&ip->ref);

	return ip;
}
