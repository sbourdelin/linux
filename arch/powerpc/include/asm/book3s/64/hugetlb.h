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
	else if (shift == mmu_psize_defs[MMU_PAGE_16G].shift)
		return MMU_PAGE_16G;
	else {
		WARN(1, "Wrong huge page shift\n");
		return mmu_virtual_psize;
	}
}

static inline unsigned long huge_pte_update(struct mm_struct *mm, unsigned long addr,
					    pte_t *ptep, unsigned long clr,
					    unsigned long set)
{
	if (radix_enabled()) {
		unsigned long old_pte;

		if (cpu_has_feature(CPU_FTR_POWER9_DD1)) {

			unsigned long new_pte;

			old_pte = __radix_pte_update(ptep, ~0, 0);
			asm volatile("ptesync" : : : "memory");
			/*
			 * new value of pte
			 */
			new_pte = (old_pte | set) & ~clr;
			/*
			 * For now let's do heavy pid flush
			 * radix__flush_tlb_page_psize(mm, addr, mmu_virtual_psize);
			 */
			radix__flush_tlb_mm(mm);

			__radix_pte_update(ptep, 0, new_pte);
		} else
			old_pte = __radix_pte_update(ptep, clr, set);
		asm volatile("ptesync" : : : "memory");
		return old_pte;
	}
	return hash__pte_update(mm, addr, ptep, clr, set, true);
}

static inline void huge_ptep_set_wrprotect(struct mm_struct *mm,
					   unsigned long addr, pte_t *ptep)
{
	if ((pte_raw(*ptep) & cpu_to_be64(_PAGE_WRITE)) == 0)
		return;

	huge_pte_update(mm, addr, ptep, _PAGE_WRITE, 0);
}

#endif
