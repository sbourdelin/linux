// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2016-18 Intel Corporation.

#include <linux/device.h>
#include <linux/freezer.h>
#include <linux/highmem.h>
#include <linux/kthread.h>
#include <linux/ratelimit.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include "sgx.h"

static inline struct sgx_encl_page *to_encl_page(struct sgx_epc_page *epc_page)
{
	WARN_ON_ONCE(1);
	return NULL;
}

bool sgx_encl_page_get(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = to_encl_page(epc_page);
	struct sgx_encl *encl = encl_page->encl;

	return kref_get_unless_zero(&encl->refcount) != 0;
}

void sgx_encl_page_put(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = to_encl_page(epc_page);
	struct sgx_encl *encl = encl_page->encl;

	kref_put(&encl->refcount, sgx_encl_release);
}

bool sgx_encl_page_reclaim(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = to_encl_page(epc_page);
	struct sgx_encl *encl = encl_page->encl;
	bool ret;

	down_read(&encl->mm->mmap_sem);
	mutex_lock(&encl->lock);

	if (encl->flags & SGX_ENCL_DEAD)
		ret = true;
	else if (encl_page->desc & SGX_ENCL_PAGE_RESERVED)
		ret = false;
	else
		ret = !sgx_test_and_clear_young(encl_page);
	if (ret)
		encl_page->desc |= SGX_ENCL_PAGE_RECLAIMED;

	mutex_unlock(&encl->lock);
	up_read(&encl->mm->mmap_sem);

	return ret;
}

void sgx_encl_page_block(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = to_encl_page(epc_page);
	struct sgx_encl *encl = encl_page->encl;

	down_read(&encl->mm->mmap_sem);
	mutex_lock(&encl->lock);
	sgx_encl_block(encl_page);
	mutex_unlock(&encl->lock);
	up_read(&encl->mm->mmap_sem);
}

static int sgx_ewb(struct sgx_encl *encl, struct sgx_epc_page *epc_page,
		   struct sgx_va_page *va_page, unsigned int va_offset)
{
	struct sgx_encl_page *encl_page = to_encl_page(epc_page);
	int pcmd_offset = sgx_encl_page_pcmd_offset(encl_page, encl);
	struct sgx_pageinfo pginfo;
	pgoff_t backing_index;
	struct page *backing;
	struct page *pcmd;
	int ret;

	backing_index = sgx_encl_page_backing_index(encl_page, encl);

	backing = sgx_get_backing(encl->backing, backing_index);
	if (IS_ERR(backing)) {
		ret = PTR_ERR(backing);
		return ret;
	}

	pcmd = sgx_get_backing(encl->pcmd, backing_index >> 5);
	if (IS_ERR(pcmd)) {
		ret = PTR_ERR(pcmd);
		sgx_put_backing(backing, true);
		return ret;
	}

	pginfo.addr = 0;
	pginfo.contents = (unsigned long)kmap_atomic(backing);
	pginfo.metadata = (unsigned long)kmap_atomic(pcmd) + pcmd_offset;
	pginfo.secs = 0;
	ret = __ewb(&pginfo, sgx_epc_addr(epc_page),
		    sgx_epc_addr(va_page->epc_page) + va_offset);
	kunmap_atomic((void *)(unsigned long)(pginfo.metadata - pcmd_offset));
	kunmap_atomic((void *)(unsigned long)pginfo.contents);

	sgx_put_backing(pcmd, true);
	sgx_put_backing(backing, true);

	return ret;
}

/**
 * sgx_write_page - write a page to the regular memory
 *
 * Writes an EPC page to the shmem file associated with the enclave. Flushes
 * CPUs and retries if there are hardware threads that can potentially have TLB
 * entries to the page (indicated by SGX_NOT_TRACKED). Clears the reserved flag
 * after the page is swapped.
 *
 * @epc_page:	an EPC page
 */
static void sgx_write_page(struct sgx_epc_page *epc_page, bool do_free)
{
	struct sgx_encl_page *encl_page = to_encl_page(epc_page);
	struct sgx_encl *encl = encl_page->encl;
	struct sgx_va_page *va_page;
	unsigned int va_offset;
	int ret;

	encl_page->desc &= ~(SGX_ENCL_PAGE_LOADED | SGX_ENCL_PAGE_RECLAIMED);

	if (!(encl->flags & SGX_ENCL_DEAD)) {
		va_page = list_first_entry(&encl->va_pages, struct sgx_va_page,
					   list);
		va_offset = sgx_alloc_va_slot(va_page);
		if (sgx_va_page_full(va_page))
			list_move_tail(&va_page->list, &encl->va_pages);

		ret = sgx_ewb(encl, epc_page, va_page, va_offset);
		if (ret == SGX_NOT_TRACKED) {
			sgx_encl_track(encl);
			ret = sgx_ewb(encl, epc_page, va_page, va_offset);
			if (ret == SGX_NOT_TRACKED) {
				/* slow path, IPI needed */
				sgx_flush_cpus(encl);
				ret = sgx_ewb(encl, epc_page, va_page,
					      va_offset);
			}
		}
		SGX_INVD(ret, encl, "EWB returned %d (0x%x)", ret, ret);

		SGX_INVD(encl_page->desc & SGX_ENCL_PAGE_VA_OFFSET_MASK, encl,
			"Flags set in VA offset area: %lx", encl_page->desc);
		encl_page->desc |= va_offset;
		encl_page->va_page = va_page;
	} else if (!do_free) {
		ret = __eremove(sgx_epc_addr(epc_page));
		WARN(ret, "EREMOVE returned %d\n", ret);
	}

	if (do_free)
		sgx_free_page(epc_page);
}

void sgx_encl_page_write(struct sgx_epc_page *epc_page)
{
	struct sgx_encl_page *encl_page = to_encl_page(epc_page);
	struct sgx_encl *encl = encl_page->encl;

	down_read(&encl->mm->mmap_sem);
	mutex_lock(&encl->lock);
	sgx_write_page(epc_page, false);
	encl->secs_child_cnt--;
	if (!encl->secs_child_cnt &&
	    (encl->flags & (SGX_ENCL_INITIALIZED | SGX_ENCL_DEAD)))
		sgx_write_page(encl->secs.epc_page, true);
	mutex_unlock(&encl->lock);
	up_read(&encl->mm->mmap_sem);
}
