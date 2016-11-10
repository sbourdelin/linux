#ifndef _ASM_POWERPC_BOOK3S_64_HUGETLB_H
#define _ASM_POWERPC_BOOK3S_64_HUGETLB_H
/*
 * For radix we want generic code to handle hugetlb. But then if we want
 * both hash and radix to be enabled together we need to workaround the
 * limitations.
 */
void radix__flush_hugetlb_page(struct vm_area_struct *vma, unsigned long vmaddr);
void radix__local_flush_hugetlb_page(struct vm_area_struct *vma, unsigned long vmaddr);
extern unsigned long
radix__hugetlb_get_unmapped_area(struct file *file, unsigned long addr,
				unsigned long len, unsigned long pgoff,
				unsigned long flags);

static inline int hstate_get_psize(struct hstate *hstate)
{
	unsigned long shift;

	shift = huge_page_shift(hstate);
	if (shift == mmu_psize_defs[MMU_PAGE_2M].shift)
		return MMU_PAGE_2M;
	else if (shift == mmu_psize_defs[MMU_PAGE_1G].shift)
		return MMU_PAGE_1G;
	else if (shift == mmu_psize_defs[MMU_PAGE_16M].shift)
		return MMU_PAGE_16M;
	else {
		WARN(1, "Wrong huge page shift\n");
		return mmu_virtual_psize;
	}
}

static inline unsigned long huge_pte_update(struct vm_area_struct *vma, unsigned long addr,
					    pte_t *ptep, unsigned long clr,
					    unsigned long set)
{
	unsigned long pg_sz;

	VM_WARN_ON(!is_vm_hugetlb_page(vma));
	pg_sz = huge_page_size(hstate_vma(vma));

	if (radix_enabled())
		return radix__pte_update(vma->vm_mm, addr, ptep, clr, set, pg_sz);
	return hash__pte_update(vma->vm_mm, addr, ptep, clr, set, true);
}

static inline void huge_ptep_set_wrprotect(struct vm_area_struct *vma,
					   unsigned long addr, pte_t *ptep)
{
	if ((pte_raw(*ptep) & cpu_to_be64(_PAGE_WRITE)) == 0)
		return;

	huge_pte_update(vma, addr, ptep, _PAGE_WRITE, 0);
}

#endif
