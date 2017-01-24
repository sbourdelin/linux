#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#include "internal.h"

static inline bool check_pmd(struct page_check_walk *pcw)
{
	pmd_t pmde = *pcw->pmd;
	barrier();
	return pmd_present(pmde) && !pmd_trans_huge(pmde);
}

static inline bool not_found(struct page_check_walk *pcw)
{
	page_check_walk_done(pcw);
	return false;
}

static inline bool map_pte(struct page_check_walk *pcw)
{
	pcw->pte = pte_offset_map(pcw->pmd, pcw->address);
	if (!(pcw->flags & PAGE_CHECK_WALK_SYNC)) {
		if (pcw->flags & PAGE_CHECK_WALK_MIGRATION) {
			if (!is_swap_pte(*pcw->pte))
				return false;
		} else {
			if (!pte_present(*pcw->pte))
				return false;
		}
	}
	pcw->ptl = pte_lockptr(pcw->vma->vm_mm, pcw->pmd);
	spin_lock(pcw->ptl);
	return true;
}

static inline bool check_pte(struct page_check_walk *pcw)
{
	if (pcw->flags & PAGE_CHECK_WALK_MIGRATION) {
		swp_entry_t entry;
		if (!is_swap_pte(*pcw->pte))
			return false;
		entry = pte_to_swp_entry(*pcw->pte);
		if (!is_migration_entry(entry))
			return false;
		if (migration_entry_to_page(entry) - pcw->page >=
				hpage_nr_pages(pcw->page)) {
			return false;
		}
		if (migration_entry_to_page(entry) < pcw->page)
			return false;
	} else {
		if (!pte_present(*pcw->pte))
			return false;

		/* THP can be referenced by any subpage */
		if (pte_page(*pcw->pte) - pcw->page >=
				hpage_nr_pages(pcw->page)) {
			return false;
		}
		if (pte_page(*pcw->pte) < pcw->page)
			return false;
	}

	return true;
}

bool __page_check_walk(struct page_check_walk *pcw)
{
	struct mm_struct *mm = pcw->vma->vm_mm;
	struct page *page = pcw->page;
	pgd_t *pgd;
	pud_t *pud;

	/* For THP, seek to next pte entry */
	if (pcw->pte)
		goto next_pte;

	if (unlikely(PageHuge(pcw->page))) {
		/* when pud is not present, pte will be NULL */
		pcw->pte = huge_pte_offset(mm, pcw->address);
		if (!pcw->pte)
			return false;

		pcw->ptl = huge_pte_lockptr(page_hstate(page), mm, pcw->pte);
		spin_lock(pcw->ptl);
		if (!check_pte(pcw))
			return not_found(pcw);
		return true;
	}
restart:
	pgd = pgd_offset(mm, pcw->address);
	if (!pgd_present(*pgd))
		return false;
	pud = pud_offset(pgd, pcw->address);
	if (!pud_present(*pud))
		return false;
	pcw->pmd = pmd_offset(pud, pcw->address);
	if (pmd_trans_huge(*pcw->pmd)) {
		pcw->ptl = pmd_lock(mm, pcw->pmd);
		if (!pmd_present(*pcw->pmd))
			return not_found(pcw);
		if (likely(pmd_trans_huge(*pcw->pmd))) {
			if (pcw->flags & PAGE_CHECK_WALK_MIGRATION)
				return not_found(pcw);
			if (pmd_page(*pcw->pmd) != page)
				return not_found(pcw);
			return true;
		} else {
			/* THP pmd was split under us: handle on pte level */
			spin_unlock(pcw->ptl);
			pcw->ptl = NULL;
		}
	} else {
		if (!check_pmd(pcw))
			return false;
	}
	if (!map_pte(pcw))
		goto next_pte;
	while (1) {
		if (check_pte(pcw))
			return true;
next_pte:	do {
			pcw->address += PAGE_SIZE;
			if (pcw->address >= __vma_address(pcw->page, pcw->vma) +
					hpage_nr_pages(pcw->page) * PAGE_SIZE)
				return not_found(pcw);
			/* Did we cross page table boundary? */
			if (pcw->address % PMD_SIZE == 0) {
				pte_unmap(pcw->pte);
				if (pcw->ptl) {
					spin_unlock(pcw->ptl);
					pcw->ptl = NULL;
				}
				goto restart;
			} else {
				pcw->pte++;
			}
		} while (pte_none(*pcw->pte));

		if (!pcw->ptl) {
			pcw->ptl = pte_lockptr(mm, pcw->pmd);
			spin_lock(pcw->ptl);
		}
	}
}
