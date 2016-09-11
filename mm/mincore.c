/*
 *	linux/mm/mincore.c
 *
 * Copyright (C) 1994-2006  Linus Torvalds
 */

/*
 * The mincore() system call.
 */
#include <linux/pagemap.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/syscalls.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/hugetlb.h>
#include <linux/dax.h>

#include <asm/uaccess.h>
#include <asm/pgtable.h>

#define MINCORE_DAX_MASK 2
#define MINCORE_DAX_SHIFT 1

#define MINCORE_ORDER_MASK 0x7c
#define MINCORE_ORDER_SHIFT 2

struct mincore_params {
	unsigned char *vec;
	int flags;
};

static void mincore_set(unsigned char *vec, struct vm_area_struct *vma, int nr,
		int flags)
{
	unsigned char mincore = 1;

	if (!nr) {
		*vec = 0;
		return;
	}

	if ((flags & MINCORE_DAX) && vma_is_dax(vma))
		mincore |= 1 << MINCORE_DAX_SHIFT;
	if (flags & MINCORE_ORDER) {
		unsigned char order = ilog2(nr);

		WARN_ON((order << MINCORE_ORDER_SHIFT) & ~MINCORE_ORDER_MASK);
		mincore |= order << MINCORE_ORDER_SHIFT;
	}
	memset(vec, mincore, nr);
}

static int mincore_hugetlb(pte_t *pte, unsigned long hmask, unsigned long addr,
			unsigned long end, struct mm_walk *walk)
{
#ifdef CONFIG_HUGETLB_PAGE
	struct mincore_params *p = walk->private;
	int nr = (end - addr) >> PAGE_SHIFT;
	unsigned char *vec = p->vec;
	unsigned char present;

	/*
	 * Hugepages under user process are always in RAM and never
	 * swapped out, but theoretically it needs to be checked.
	 */
	present = pte && !huge_pte_none(huge_ptep_get(pte));
	if (!present)
		memset(vec, 0, nr);
	else
		mincore_set(vec, walk->vma, nr, p->flags);
	p->vec = vec + nr;
#else
	BUG();
#endif
	return 0;
}

/*
 * Later we can get more picky about what "in core" means precisely.
 * For now, simply check to see if the page is in the page cache,
 * and is up to date; i.e. that no page-in operation would be required
 * at this time if an application were to map and access this page.
 */
static unsigned char mincore_page(struct address_space *mapping, pgoff_t pgoff)
{
	unsigned char present = 0;
	struct page *page;

	/*
	 * When tmpfs swaps out a page from a file, any process mapping that
	 * file will not get a swp_entry_t in its pte, but rather it is like
	 * any other file mapping (ie. marked !present and faulted in with
	 * tmpfs's .fault). So swapped out tmpfs mappings are tested here.
	 */
#ifdef CONFIG_SWAP
	if (shmem_mapping(mapping)) {
		page = find_get_entry(mapping, pgoff);
		/*
		 * shmem/tmpfs may return swap: account for swapcache
		 * page too.
		 */
		if (radix_tree_exceptional_entry(page)) {
			swp_entry_t swp = radix_to_swp_entry(page);
			page = find_get_page(swap_address_space(swp), swp.val);
		}
	} else
		page = find_get_page(mapping, pgoff);
#else
	page = find_get_page(mapping, pgoff);
#endif
	if (page) {
		present = PageUptodate(page);
		put_page(page);
	}

	return present;
}

static int __mincore_unmapped_range(unsigned long addr, unsigned long end,
				struct vm_area_struct *vma, unsigned char *vec,
				int flags)
{
	unsigned long nr = (end - addr) >> PAGE_SHIFT;
	unsigned char present;
	int i;

	if (vma->vm_file) {
		pgoff_t pgoff;

		pgoff = linear_page_index(vma, addr);
		for (i = 0; i < nr; i++, pgoff++) {
			present = mincore_page(vma->vm_file->f_mapping, pgoff);
			mincore_set(vec + i, vma, present, flags);
		}
	} else {
		for (i = 0; i < nr; i++)
			mincore_set(vec + i, vma, 0, flags);
	}
	return nr;
}

static int mincore_unmapped_range(unsigned long addr, unsigned long end,
				   struct mm_walk *walk)
{
	struct mincore_params *p = walk->private;
	int nr = __mincore_unmapped_range(addr, end, walk->vma, p->vec,
			p->flags);

	p->vec += nr;
	return 0;
}

static int mincore_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			struct mm_walk *walk)
{
	spinlock_t *ptl;
	struct vm_area_struct *vma = walk->vma;
	pte_t *ptep;
	struct mincore_params *p = walk->private;
	unsigned char *vec = p->vec;
	int nr = (end - addr) >> PAGE_SHIFT;
	int flags = p->flags;

	ptl = pmd_trans_huge_lock(pmd, vma);
	if (ptl) {
		mincore_set(vec, vma, nr, flags);
		spin_unlock(ptl);
		goto out;
	}

	if (pmd_trans_unstable(pmd)) {
		__mincore_unmapped_range(addr, end, vma, vec, flags);
		goto out;
	}

	ptep = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	for (; addr != end; ptep++, addr += PAGE_SIZE) {
		pte_t pte = *ptep;

		if (pte_none(pte))
			__mincore_unmapped_range(addr, addr + PAGE_SIZE,
						 vma, vec, flags);
		else if (pte_present(pte))
			mincore_set(vec, vma, 1, flags);
		else { /* pte is a swap entry */
			swp_entry_t entry = pte_to_swp_entry(pte);

			if (non_swap_entry(entry)) {
				/*
				 * migration or hwpoison entries are always
				 * uptodate
				 */
				mincore_set(vec, vma, 1, flags);
			} else {
#ifdef CONFIG_SWAP
				unsigned char present;

				present = mincore_page(swap_address_space(entry),
						entry.val);
				mincore_set(vec, vma, present, flags);
#else
				WARN_ON(1);
				mincore_set(vec, vma, 1, flags);
#endif
			}
		}
		vec++;
	}
	pte_unmap_unlock(ptep - 1, ptl);
out:
	p->vec = vec + nr;
	cond_resched();
	return 0;
}

/*
 * Do a chunk of "sys_mincore()". We've already checked
 * all the arguments, we hold the mmap semaphore: we should
 * just return the amount of info we're asked for.
 */
static long do_mincore(unsigned long addr, unsigned long pages,
		unsigned char *vec, int flags)
{
	struct vm_area_struct *vma;
	unsigned long end;
	int err;
	struct mincore_params p = {
		.vec = vec,
		.flags = flags,
	};
	struct mm_walk mincore_walk = {
		.pmd_entry = mincore_pte_range,
		.pte_hole = mincore_unmapped_range,
		.hugetlb_entry = mincore_hugetlb,
		.private = &p,
	};

	vma = find_vma(current->mm, addr);
	if (!vma || addr < vma->vm_start)
		return -ENOMEM;
	mincore_walk.mm = vma->vm_mm;
	end = min(vma->vm_end, addr + (pages << PAGE_SHIFT));
	err = walk_page_range(addr, end, &mincore_walk);
	if (err < 0)
		return err;
	return (end - addr) >> PAGE_SHIFT;
}

/*
 * The mincore2(2) system call.
 *
 * mincore2() returns the memory residency status of the pages in the
 * current process's address space specified by [addr, addr + len).
 * The status is returned in a vector of bytes.  The least significant
 * bit of each byte is 1 if the referenced page is in memory, otherwise
 * it is zero.  When 'flags' is non-zero each byte additionally contains
 * an indication of whether the referenced page in memory is a DAX
 * mapping (bit 2 of each vector byte), and/or the order of the mapping
 * (bits 3 through 7 of each vector byte).  Where the order relates to
 * the hardware mapping size backing the given logical-page.  For
 * example, a 2MB-dax-mapped-huge-page would correspond to 512 vector
 * entries with the value 0x27.
 *
 * Because the status of a page can change after mincore() checks it
 * but before it returns to the application, the returned vector may
 * contain stale information.  Only locked pages are guaranteed to
 * remain in memory.
 *
 * return values:
 *  zero    - success
 *  -EFAULT - vec points to an illegal address
 *  -EINVAL - addr is not a multiple of PAGE_SIZE
 *  -ENOMEM - Addresses in the range [addr, addr + len] are
 *		invalid for the address space of this process, or
 *		specify one or more pages which are not currently
 *		mapped
 *  -EAGAIN - A kernel resource was temporarily unavailable.
 */
SYSCALL_DEFINE4(mincore2, unsigned long, start, size_t, len,
		unsigned char __user *, vec, int, flags)
{
	long retval;
	unsigned long pages;
	unsigned char *tmp;

	/* Check the start address: needs to be page-aligned.. */
	if (start & ~PAGE_MASK)
		return -EINVAL;

	/* Check that undefined flags are zero */
	if (flags & ~(MINCORE_DAX | MINCORE_ORDER))
		return -EINVAL;

	/* ..and we need to be passed a valid user-space range */
	if (!access_ok(VERIFY_READ, (void __user *) start, len))
		return -ENOMEM;

	/* This also avoids any overflows on PAGE_ALIGN */
	pages = len >> PAGE_SHIFT;
	pages += (offset_in_page(len)) != 0;

	if (!access_ok(VERIFY_WRITE, vec, pages))
		return -EFAULT;

	tmp = (void *) __get_free_page(GFP_USER);
	if (!tmp)
		return -EAGAIN;

	retval = 0;
	while (pages) {
		/*
		 * Do at most PAGE_SIZE entries per iteration, due to
		 * the temporary buffer size.
		 */
		down_read(&current->mm->mmap_sem);
		retval = do_mincore(start, min(pages, PAGE_SIZE), tmp, flags);
		up_read(&current->mm->mmap_sem);

		if (retval <= 0)
			break;
		if (copy_to_user(vec, tmp, retval)) {
			retval = -EFAULT;
			break;
		}
		pages -= retval;
		vec += retval;
		start += retval << PAGE_SHIFT;
		retval = 0;
	}
	free_page((unsigned long) tmp);
	return retval;
}

SYSCALL_DEFINE3(mincore, unsigned long, start, size_t, len,
		unsigned char __user *, vec)
{
	return sys_mincore2(start, len, vec, 0);
}
