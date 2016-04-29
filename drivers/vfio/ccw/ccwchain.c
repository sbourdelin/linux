/*
 * ccwchain interfaces
 *
 * Copyright IBM Corp. 2016
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 * Author(s): Dong Jia Shi <bjsdjshi@linux.vnet.ibm.com>
 *            Xiao Feng Ren <renxiaof@linux.vnet.ibm.com>
 */

#include <asm/ccwdev.h>
#include <asm/idals.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include "ccwchain.h"

/*
 * Max length for ccw chain.
 * XXX: Limit to 256, need to check more?
 */
#define CCWCHAIN_LEN_MAX	256
#define CDA_ITEM_SIZE		3 /* sizeof(u64) == (1 << 3) */

struct page_array {
	u64			hva;
	int			nr;
	struct page		**items;
};

struct page_arrays {
	struct page_array	*parray;
	int			nr;
};

struct ccwchain_buf {
	struct ccw1		ccw[CCWCHAIN_LEN_MAX];
	u64			cda[CCWCHAIN_LEN_MAX];
};

struct ccwchain {
	struct ccwchain_buf	buf;

	/* Valid ccw number in chain */
	int			nr;
	/* Pinned PAGEs for the original data. */
	struct page_arrays	*pss;
};

/*
 * Helpers to operate page_array.
 */
/*
 * page_array_pin() - pin user pages in memory
 * @p: page_array on which to perform the operation
 *
 * Attempt to pin user pages in memory.
 *
 * Usage of page_array:
 * @p->hva      starting user address. Assigned by caller.
 * @p->nr       number of pages from @p->hva to pin. Assigned by caller.
 *              number of pages pinned. Assigned by callee.
 * @p->items    array that receives pointers to the pages pinned. Allocated by
 *              caller.
 *
 * Returns:
 *   Number of pages pinned on success. If @p->nr is 0 or negative, returns 0.
 *   If no pages were pinned, returns -errno.
 */
static int page_array_pin(struct page_array *p)
{
	int i, nr;

	nr = get_user_pages_fast(p->hva, p->nr, 1, p->items);
	if (nr <= 0) {
		p->nr = 0;
		return nr;
	} else if (nr != p->nr) {
		for (i = 0; i < nr; i++)
			put_page(p->items[i]);
		p->nr = 0;
		return -ENOMEM;
	}

	return nr;
}

/* Unpin the items before releasing the memory. */
static void page_array_items_unpin_free(struct page_array *p)
{
	int i;

	for (i = 0; i < p->nr; i++)
		put_page(p->items[i]);

	p->nr = 0;
	kfree(p->items);
}

/* Alloc memory for items, then pin pages with them. */
static int page_array_items_alloc_pin(u64 hva,
				      unsigned int len,
				      struct page_array *p)
{
	int ret;

	if (!len || p->nr)
		return -EINVAL;

	p->hva = hva;

	p->nr = ((hva & ~PAGE_MASK) + len + (PAGE_SIZE - 1)) >> PAGE_SHIFT;
	if (!p->nr)
		return -EINVAL;

	p->items = kcalloc(p->nr, sizeof(*p->items), GFP_KERNEL);
	if (!p->items)
		return -ENOMEM;

	ret = page_array_pin(p);
	if (ret <= 0)
		kfree(p->items);

	return ret;
}

static int page_arrays_init(struct page_arrays *ps, int nr)
{
	ps->parray = kcalloc(nr, sizeof(*ps->parray), GFP_KERNEL);
	if (!ps->parray) {
		ps->nr = 0;
		return -ENOMEM;
	}

	ps->nr = nr;
	return 0;
}

static void page_arrays_unpin_free(struct page_arrays *ps)
{
	int i;

	for (i = 0; i < ps->nr; i++)
		page_array_items_unpin_free(ps->parray + i);

	kfree(ps->parray);

	ps->parray = NULL;
	ps->nr = 0;
}

/*
 * Helpers to operate ccwchain.
 */
/* Return the number of idal words needed for an address/length pair. */
static inline unsigned int ccwchain_idal_nr_words(u64 addr, unsigned int length)
{
	/*
	 * User virtual address and its corresponding kernel physical address
	 * are aligned by pages. Thus their offsets to the page boundary will be
	 * the same.
	 * Althought idal_nr_words expects a virtual address as its first param,
	 * it is the offset that matters. It's fine to use either hva or hpa as
	 * the input, since they have the same offset inside a page.
	 */
	return idal_nr_words((void *)(addr), length);
}

/* Create the list idal words for a page_arrays. */
static inline void ccwchain_idal_create_words(unsigned long *idaws,
					      struct page_arrays *ps)
{
	int i, j, k;

	/*
	 * Idal words (execept the first one) rely on the memory being 4k
	 * aligned. If a user virtual address is 4K aligned, then it's
	 * corresponding kernel physical address will also be 4K aligned. Thus
	 * there will be no problem here to simply use the hpa to create an
	 * idaw.
	 */
	k = 0;
	for (i = 0; i < ps->nr; i++)
		for (j = 0; j < ps->parray[i].nr; j++) {
			idaws[k] = page_to_phys(ps->parray[i].items[j]);
			if (k == 0)
				idaws[k] += ps->parray[i].hva & ~PAGE_MASK;
			k++;
		}
}

#define ccw_is_test(_ccw) (((_ccw)->cmd_code & 0x0F) == 0)

#define ccw_is_noop(_ccw) ((_ccw)->cmd_code == CCW_CMD_NOOP)

#define ccw_is_tic(_ccw) ((_ccw)->cmd_code == CCW_CMD_TIC)

#define ccw_is_idal(_ccw) ((_ccw)->flags & CCW_FLAG_IDA)

/* Free resource for a ccw that allocated memory for its cda. */
static void ccw_chain_cda_free(struct ccwchain *chain, int idx)
{
	struct ccw1 *ccw = chain->buf.ccw + idx;

	if (!ccw->count)
		return;

	kfree((void *)(u64)ccw->cda);
}

/* Unpin the pages then free the memory resources. */
static void ccw_chain_unpin_free(struct ccwchain *chain)
{
	int i;

	if (!chain)
		return;

	for (i = 0; i < chain->nr; i++) {
		page_arrays_unpin_free(chain->pss + i);
		ccw_chain_cda_free(chain, i);
	}

	kfree(chain->pss);
	kfree(chain);
}

static int ccw_chain_fetch_tic(struct ccwchain *chain, int idx)
{
	struct ccw1 *ccw = chain->buf.ccw + idx;

	if (ccw->cda >= sizeof(chain->buf.ccw))
		return -EINVAL;

	/*
	 * tic_ccw.cda stores the offset to the address of the first ccw
	 * of the chain. Here we update its value with the the real address.
	 */
	ccw->cda += virt_to_phys(chain->buf.ccw);

	return 0;
}

static int ccw_chain_fetch_direct(struct ccwchain *chain, int idx)
{
	struct ccw1 *ccw;
	struct page_arrays *ps;
	unsigned long *idaws;
	u64 cda_hva;
	int i, cidaw;

	ccw = chain->buf.ccw + idx;

	/*
	 * direct_ccw.cda stores the offset of its cda data in the cda buffer.
	 */
	i = ccw->cda >> CDA_ITEM_SIZE;
	if (i < 0)
		return -EINVAL;
	cda_hva = chain->buf.cda[i];
	if (IS_ERR_VALUE(cda_hva))
		return -EFAULT;

	/*
	 * Pin data page(s) in memory.
	 * The number of pages actually is the count of the idaws which will be
	 * needed when translating a direct ccw to a idal ccw.
	 */
	ps = chain->pss + idx;
	if (page_arrays_init(ps, 1))
		return -ENOMEM;
	cidaw = page_array_items_alloc_pin(cda_hva, ccw->count, ps->parray);
	if (cidaw <= 0)
		return cidaw;

	/* Translate this direct ccw to a idal ccw. */
	idaws = kcalloc(cidaw, sizeof(*idaws), GFP_DMA | GFP_KERNEL);
	if (!idaws) {
		page_arrays_unpin_free(ps);
		return -ENOMEM;
	}
	ccw->cda = (__u32) virt_to_phys(idaws);
	ccw->flags |= CCW_FLAG_IDA;

	ccwchain_idal_create_words(idaws, ps);

	return 0;
}

static int ccw_chain_fetch_idal(struct ccwchain *chain, int idx)
{
	struct ccw1 *ccw;
	struct page_arrays *ps;
	unsigned long *idaws;
	unsigned int cidaw, idaw_len;
	int i, ret;
	u64 cda_hva, idaw_hva;

	ccw = chain->buf.ccw + idx;

	/* idal_ccw.cda stores the offset of its cda data in the cda buffer. */
	i = ccw->cda >> CDA_ITEM_SIZE;
	if (i < 0)
		return -EINVAL;
	cda_hva = chain->buf.cda[i];
	if (IS_ERR_VALUE(cda_hva))
		return -EFAULT;

	/* Calculate size of idaws. */
	ret = copy_from_user(&idaw_hva, (void __user *)cda_hva, sizeof(*idaws));
	if (ret)
		return ret;

	cidaw = ccwchain_idal_nr_words(idaw_hva, ccw->count);
	idaw_len = cidaw * sizeof(*idaws);

	/* Pin data page(s) in memory. */
	ps = chain->pss + idx;
	ret = page_arrays_init(ps, cidaw);
	if (ret)
		return ret;

	/* Translate idal ccw to use new allocated idaws. */
	idaws = kzalloc(idaw_len, GFP_DMA | GFP_KERNEL);
	if (!idaws) {
		ret = -ENOMEM;
		goto out_unpin;
	}

	ret = copy_from_user(idaws, (void __user *)cda_hva, idaw_len);
	if (ret)
		goto out_free_idaws;

	ccw->cda = virt_to_phys(idaws);

	for (i = 0; i < cidaw; i++) {
		idaw_hva = *(idaws + i);
		if (IS_ERR_VALUE(idaw_hva)) {
			ret = -EFAULT;
			goto out_free_idaws;
		}

		ret = page_array_items_alloc_pin(idaw_hva, 1, ps->parray + i);
		if (ret <= 0)
			goto out_free_idaws;
	}

	ccwchain_idal_create_words(idaws, ps);

	return 0;

out_free_idaws:
	kfree(idaws);
out_unpin:
	page_arrays_unpin_free(ps);
	return ret;
}

/*
 * Fetch one ccw.
 * To reduce memory copy, we'll pin the cda page in memory,
 * and to get rid of the cda 2G limitiaion of ccw1, we'll translate
 * direct ccws to idal ccws.
 */
static int ccw_chain_fetch_one(struct ccwchain *chain, int idx)
{
	struct ccw1 *ccw = chain->buf.ccw + idx;

	if (ccw_is_test(ccw) || ccw_is_noop(ccw))
		return 0;

	if (ccw_is_tic(ccw))
		return ccw_chain_fetch_tic(chain, idx);

	if (ccw_is_idal(ccw))
		return ccw_chain_fetch_idal(chain, idx);

	return ccw_chain_fetch_direct(chain, idx);
}

static int ccw_chain_copy_from_user(struct ccwchain_cmd *cmd)
{
	struct ccwchain *chain;
	int ret;

	if (!cmd->nr || cmd->nr > CCWCHAIN_LEN_MAX) {
		ret = -EINVAL;
		goto out_error;
	}

	chain = kzalloc(sizeof(*chain), GFP_DMA | GFP_KERNEL);
	if (!chain) {
		ret = -ENOMEM;
		goto out_error;
	}

	chain->nr = cmd->nr;

	/* Copy current chain from user. */
	ret = copy_from_user(&chain->buf,
			     (void __user *)cmd->u_ccwchain,
			     sizeof(chain->buf));
	if (ret)
		goto out_free_chain;

	/* Alloc memory for page_arrays. */
	chain->pss = kcalloc(chain->nr, sizeof(*chain->pss), GFP_KERNEL);
	if (!chain->pss) {
		ret = -ENOMEM;
		goto out_free_chain;
	}

	cmd->k_ccwchain = chain;

	return 0;

out_free_chain:
	kfree(chain);
out_error:
	cmd->k_ccwchain = NULL;
	return ret;
}

/**
 * ccwchain_alloc() - allocate resources for a ccw chain.
 * @cmd: ccwchain command on which to perform the operation
 *
 * This function is a wrapper around ccw_chain_copy_from_user().
 *
 * This creates a ccwchain and allocates a memory buffer, that could at most
 * contain @cmd->nr ccws, for the ccwchain. Then it copies user-space ccw
 * program from @cmd->u_ccwchain to the buffer, and stores the address of the
 * ccwchain to @cmd->k_ccwchain as the output.
 *
 * Returns:
 *   %0 on success and a negative error value on failure.
 */
int ccwchain_alloc(struct ccwchain_cmd *cmd)
{
	return ccw_chain_copy_from_user(cmd);
}

/**
 * ccwchain_free() - free resources for a ccw chain.
 * @cmd: ccwchain command on which to perform the operation
 *
 * This function is a wrapper around ccw_chain_unpin_free().
 *
 * This unpins the memory pages and frees the memory space occupied by @cmd,
 * which must have been returned by a previous call to ccwchain_alloc().
 * Otherwise, undefined behavior occurs.
 */
void ccwchain_free(struct ccwchain_cmd *cmd)
{
	ccw_chain_unpin_free(cmd->k_ccwchain);
}

/**
 * ccwchain_prefetch() - translate a user-space ccw program to a real-device
 *                       runnable ccw program.
 * @cmd: ccwchain command on which to perform the operation
 *
 * This function translates the user-space ccw program (@cmd->u_ccwchain) and
 * stores the result to @cmd->k_ccwchain. @cmd must have been returned by a
 * previous call to ccwchain_alloc(). Otherwise, undefined behavior occurs.
 *
 * The S/390 CCW Translation APIS (prefixed by 'ccwchain_') are introduced as
 * helpers to do ccw chain translation inside the kernel. Basically they accept
 * a special ccw program issued by a user-space process, and translate the ccw
 * program to a real-device runnable ccw program.
 *
 * The ccws passed in should be well organized in a user-space buffer, using
 * virtual memory addresses and offsets inside the buffer. These APIs will copy
 * the ccws into a kernel-space buffer, and update the virtual addresses and the
 * offsets with their corresponding physical addresses. Then channel I/O device
 * drivers could issue the translated ccw program to real devices to perform an
 * I/O operation.
 *
 * User-space ccw program format:
 * These interfaces are designed to support translation only for special ccw
 * programs, which are generated and formatted by a user-space program. Thus
 * this will make it possible for things like VFIO to leverage the interfaces to
 * realize channel I/O device drivers in user-space.
 *
 * User-space programs should prepare the ccws according to the rules below
 * 1. Alloc a 4K bytes memory buffer in user-space to store all of the ccw
 *    program information.
 * 2. Lower 2K of the buffer are used to store a maximum of 256 ccws.
 * 3. Upper 2K of the buffer are used to store a maximum of 256 corresponding
 *    cda data sets, each having a length of 8 bytes.
 * 4. All of the ccws should be placed one after another.
 * 5. For direct and idal ccw
 *    - Find a free cda data entry, and find its offset to the address of the
 *      cda buffer.
 *    - Store the offset as the CDA value in the ccw.
 *    - Store the virtual address of the data(idaw) as the data of the cda
 *      entry.
 * 6. For tic ccw
 *    - Find the target ccw, and find its offset to the address of the ccw
 *      buffer.
 *    - Store the offset as the CDA value in the ccw.
 *
 * Limitations:
 * 1. Supports only prefetch enabled mode.
 * 2. Supports direct ccw chaining by translating them to idal ccws.
 * 3. Supports idal(c64) ccw chaining.
 *
 * Returns:
 *   %0 on success and a negative error value on failure.
 */
int ccwchain_prefetch(struct ccwchain_cmd *cmd)
{
	int ret, i;
	struct ccwchain *chain = cmd->k_ccwchain;

	for (i = 0; i < chain->nr; i++) {
		ret = ccw_chain_fetch_one(chain, i);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * ccwchain_get_cpa() - get the ccw program address of a ccwchain
 * @cmd: ccwchain command on which to perform the operation
 *
 * This function returns the address of the translated kernel ccw program.
 * Channel I/O device drivers could issue this address to real devices to
 * perform an I/O operation.
 */
struct ccw1 *ccwchain_get_cpa(struct ccwchain_cmd *cmd)
{
	return ((struct ccwchain *)cmd->k_ccwchain)->buf.ccw;
}

/**
 * ccwchain_update_scsw() - update scsw for a ccw chain.
 * @cmd: ccwchain command on which to perform the operation
 * @scsw: I/O result of the ccw program and also the target to be updated
 *
 * @scsw contains the I/O results of the ccw program that pointed to by @cmd.
 * However what @scsw->cpa stores is a kernel physical address, which is
 * meaningless for a user-space program, which is waiting for the I/O results.
 *
 * This function updates @scsw->cpa to its coressponding user-space ccw address
 * (an offset inside the user-space ccw buffer).
 */
void ccwchain_update_scsw(struct ccwchain_cmd *cmd, union scsw *scsw)
{
	u32 cpa = scsw->cmd.cpa;
	struct ccwchain *chain = cmd->k_ccwchain;

	/*
	 * LATER:
	 * For now, only update the cmd.cpa part. We may need to deal with
	 * other portions of the schib as well, even if we don't return them
	 * in the ioctl directly. Path status changes etc.
	 */
	cpa = cpa - (u32)(u64)(chain->buf.ccw);
	if (cpa & (1 << 31))
		cpa &= (1 << 31) - 1U;

	scsw->cmd.cpa = cpa;
}
