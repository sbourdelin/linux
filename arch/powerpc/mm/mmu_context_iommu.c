/*
 *  IOMMU helpers in MMU context.
 *
 *  Copyright (C) 2015 IBM Corp. <aik@ozlabs.ru>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 */

#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/rculist.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/migrate.h>
#include <linux/hugetlb.h>
#include <linux/swap.h>
#include <asm/mmu_context.h>
#include <asm/pte-walk.h>
#include <linux/mm_inline.h>

static DEFINE_MUTEX(mem_list_mutex);

struct mm_iommu_table_group_mem_t {
	struct list_head next;
	struct rcu_head rcu;
	unsigned long used;
	atomic64_t mapped;
	unsigned int pageshift;
	u64 ua;			/* userspace address */
	u64 entries;		/* number of entries in hpages[] */
	/*
	 * in mm_iommu_get we temporarily use this to store
	 * struct page address.
	 *
	 * We need to convert ua to hpa in real mode. Make it
	 * simpler by storing physicall address.
	 */
	union {
		struct page **hpages;	/* vmalloc'ed */
		phys_addr_t *hpas;
	};
};

static long mm_iommu_adjust_locked_vm(struct mm_struct *mm,
		unsigned long npages, bool incr)
{
	long ret = 0, locked, lock_limit;

	if (!npages)
		return 0;

	down_write(&mm->mmap_sem);

	if (incr) {
		locked = mm->locked_vm + npages;
		lock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;
		if (locked > lock_limit && !capable(CAP_IPC_LOCK))
			ret = -ENOMEM;
		else
			mm->locked_vm += npages;
	} else {
		if (WARN_ON_ONCE(npages > mm->locked_vm))
			npages = mm->locked_vm;
		mm->locked_vm -= npages;
	}

	pr_debug("[%d] RLIMIT_MEMLOCK HASH64 %c%ld %ld/%ld\n",
			current ? current->pid : 0,
			incr ? '+' : '-',
			npages << PAGE_SHIFT,
			mm->locked_vm << PAGE_SHIFT,
			rlimit(RLIMIT_MEMLOCK));
	up_write(&mm->mmap_sem);

	return ret;
}

bool mm_iommu_preregistered(struct mm_struct *mm)
{
	return !list_empty(&mm->context.iommu_group_mem_list);
}
EXPORT_SYMBOL_GPL(mm_iommu_preregistered);

long mm_iommu_get(struct mm_struct *mm, unsigned long ua, unsigned long entries,
		struct mm_iommu_table_group_mem_t **pmem)
{
	struct mm_iommu_table_group_mem_t *mem;
	long i, ret = 0, locked_entries = 0;
	unsigned int pageshift;

	mutex_lock(&mem_list_mutex);

	list_for_each_entry_rcu(mem, &mm->context.iommu_group_mem_list,
			next) {
		if ((mem->ua == ua) && (mem->entries == entries)) {
			++mem->used;
			*pmem = mem;
			goto unlock_exit;
		}

		/* Overlap? */
		if ((mem->ua < (ua + (entries << PAGE_SHIFT))) &&
				(ua < (mem->ua +
				       (mem->entries << PAGE_SHIFT)))) {
			ret = -EINVAL;
			goto unlock_exit;
		}

	}

	ret = mm_iommu_adjust_locked_vm(mm, entries, true);
	if (ret)
		goto unlock_exit;

	locked_entries = entries;

	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem) {
		ret = -ENOMEM;
		goto unlock_exit;
	}

	/*
	 * For a starting point for a maximum page size calculation
	 * we use @ua and @entries natural alignment to allow IOMMU pages
	 * smaller than huge pages but still bigger than PAGE_SIZE.
	 */
	mem->pageshift = __ffs(ua | (entries << PAGE_SHIFT));
	mem->hpas = vzalloc(array_size(entries, sizeof(mem->hpas[0])));
	if (!mem->hpas) {
		kfree(mem);
		ret = -ENOMEM;
		goto unlock_exit;
	}

	ret = get_user_pages_cma_migrate(ua, entries, 1, mem->hpages);
	if (ret != entries) {
		/* free the reference taken */
		for (i = 0; i < ret; i++)
			put_page(mem->hpages[i]);

		vfree(mem->hpas);
		kfree(mem);
		ret = -EFAULT;
		goto unlock_exit;
	} else
		ret = 0;

	pageshift = PAGE_SHIFT;
	for (i = 0; i < entries; ++i) {
		struct page *page = mem->hpages[i];
		/*
		 * Allow to use larger than 64k IOMMU pages. Only do that
		 * if we are backed by hugetlb.
		 */
		if ((mem->pageshift > PAGE_SHIFT) && PageHuge(page)) {
			struct page *head = compound_head(page);
			pageshift = compound_order(head) + PAGE_SHIFT;
		}
		mem->pageshift = min(mem->pageshift, pageshift);
		/*
		 * We don't need struct page reference any more, switch
		 * physicall address.
		 */
		mem->hpas[i] = page_to_pfn(page) << PAGE_SHIFT;

	}

	atomic64_set(&mem->mapped, 1);
	mem->used = 1;
	mem->ua = ua;
	mem->entries = entries;
	*pmem = mem;

	list_add_rcu(&mem->next, &mm->context.iommu_group_mem_list);

unlock_exit:
	if (locked_entries && ret)
		mm_iommu_adjust_locked_vm(mm, locked_entries, false);

	mutex_unlock(&mem_list_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mm_iommu_get);

static void mm_iommu_unpin(struct mm_iommu_table_group_mem_t *mem)
{
	long i;
	struct page *page = NULL;

	for (i = 0; i < mem->entries; ++i) {
		if (!mem->hpas[i])
			continue;

		page = pfn_to_page(mem->hpas[i] >> PAGE_SHIFT);
		if (!page)
			continue;

		put_page(page);
		mem->hpas[i] = 0;
	}
}

static void mm_iommu_do_free(struct mm_iommu_table_group_mem_t *mem)
{

	mm_iommu_unpin(mem);
	vfree(mem->hpas);
	kfree(mem);
}

static void mm_iommu_free(struct rcu_head *head)
{
	struct mm_iommu_table_group_mem_t *mem = container_of(head,
			struct mm_iommu_table_group_mem_t, rcu);

	mm_iommu_do_free(mem);
}

static void mm_iommu_release(struct mm_iommu_table_group_mem_t *mem)
{
	list_del_rcu(&mem->next);
	call_rcu(&mem->rcu, mm_iommu_free);
}

long mm_iommu_put(struct mm_struct *mm, struct mm_iommu_table_group_mem_t *mem)
{
	long ret = 0;

	mutex_lock(&mem_list_mutex);

	if (mem->used == 0) {
		ret = -ENOENT;
		goto unlock_exit;
	}

	--mem->used;
	/* There are still users, exit */
	if (mem->used)
		goto unlock_exit;

	/* Are there still mappings? */
	if (atomic_cmpxchg(&mem->mapped, 1, 0) != 1) {
		++mem->used;
		ret = -EBUSY;
		goto unlock_exit;
	}

	/* @mapped became 0 so now mappings are disabled, release the region */
	mm_iommu_release(mem);

	mm_iommu_adjust_locked_vm(mm, mem->entries, false);

unlock_exit:
	mutex_unlock(&mem_list_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mm_iommu_put);

struct mm_iommu_table_group_mem_t *mm_iommu_lookup(struct mm_struct *mm,
		unsigned long ua, unsigned long size)
{
	struct mm_iommu_table_group_mem_t *mem, *ret = NULL;

	list_for_each_entry_rcu(mem, &mm->context.iommu_group_mem_list, next) {
		if ((mem->ua <= ua) &&
				(ua + size <= mem->ua +
				 (mem->entries << PAGE_SHIFT))) {
			ret = mem;
			break;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mm_iommu_lookup);

struct mm_iommu_table_group_mem_t *mm_iommu_lookup_rm(struct mm_struct *mm,
		unsigned long ua, unsigned long size)
{
	struct mm_iommu_table_group_mem_t *mem, *ret = NULL;

	list_for_each_entry_lockless(mem, &mm->context.iommu_group_mem_list,
			next) {
		if ((mem->ua <= ua) &&
				(ua + size <= mem->ua +
				 (mem->entries << PAGE_SHIFT))) {
			ret = mem;
			break;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mm_iommu_lookup_rm);

struct mm_iommu_table_group_mem_t *mm_iommu_find(struct mm_struct *mm,
		unsigned long ua, unsigned long entries)
{
	struct mm_iommu_table_group_mem_t *mem, *ret = NULL;

	list_for_each_entry_rcu(mem, &mm->context.iommu_group_mem_list, next) {
		if ((mem->ua == ua) && (mem->entries == entries)) {
			ret = mem;
			break;
		}
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mm_iommu_find);

long mm_iommu_ua_to_hpa(struct mm_iommu_table_group_mem_t *mem,
		unsigned long ua, unsigned int pageshift, unsigned long *hpa)
{
	const long entry = (ua - mem->ua) >> PAGE_SHIFT;
	u64 *va = &mem->hpas[entry];

	if (entry >= mem->entries)
		return -EFAULT;

	if (pageshift > mem->pageshift)
		return -EFAULT;

	*hpa = *va | (ua & ~PAGE_MASK);

	return 0;
}
EXPORT_SYMBOL_GPL(mm_iommu_ua_to_hpa);

long mm_iommu_ua_to_hpa_rm(struct mm_iommu_table_group_mem_t *mem,
		unsigned long ua, unsigned int pageshift, unsigned long *hpa)
{
	const long entry = (ua - mem->ua) >> PAGE_SHIFT;
	void *va = &mem->hpas[entry];
	unsigned long *pa;

	if (entry >= mem->entries)
		return -EFAULT;

	if (pageshift > mem->pageshift)
		return -EFAULT;

	pa = (void *) vmalloc_to_phys(va);
	if (!pa)
		return -EFAULT;

	*hpa = *pa | (ua & ~PAGE_MASK);

	return 0;
}
EXPORT_SYMBOL_GPL(mm_iommu_ua_to_hpa_rm);

long mm_iommu_mapped_inc(struct mm_iommu_table_group_mem_t *mem)
{
	if (atomic64_inc_not_zero(&mem->mapped))
		return 0;

	/* Last mm_iommu_put() has been called, no more mappings allowed() */
	return -ENXIO;
}
EXPORT_SYMBOL_GPL(mm_iommu_mapped_inc);

void mm_iommu_mapped_dec(struct mm_iommu_table_group_mem_t *mem)
{
	atomic64_add_unless(&mem->mapped, -1, 1);
}
EXPORT_SYMBOL_GPL(mm_iommu_mapped_dec);

void mm_iommu_init(struct mm_struct *mm)
{
	INIT_LIST_HEAD_RCU(&mm->context.iommu_group_mem_list);
}
