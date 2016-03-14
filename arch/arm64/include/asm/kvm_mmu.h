/*
 * Copyright (C) 2012,2013 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __ARM64_KVM_MMU_H__
#define __ARM64_KVM_MMU_H__

#include <asm/page.h>
#include <asm/memory.h>
#include <asm/cpufeature.h>

/*
 * As we only have the TTBR0_EL2 register, we cannot express
 * "negative" addresses. This makes it impossible to directly share
 * mappings with the kernel.
 *
 * Instead, give the HYP mode its own VA region at a fixed offset from
 * the kernel by just masking the top bits (which are all ones for a
 * kernel address).
 */
#define HYP_PAGE_OFFSET_SHIFT	VA_BITS
#define HYP_PAGE_OFFSET_MASK	((UL(1) << HYP_PAGE_OFFSET_SHIFT) - 1)
#define HYP_PAGE_OFFSET		(PAGE_OFFSET & HYP_PAGE_OFFSET_MASK)

/*
 * Our virtual mapping for the idmap-ed MMU-enable code. Must be
 * shared across all the page-tables. Conveniently, we use the last
 * possible page, where no kernel mapping will ever exist.
 */
#define TRAMPOLINE_VA		(HYP_PAGE_OFFSET_MASK & PAGE_MASK)

/*
 * KVM_MMU_CACHE_MIN_PAGES is the number of stage2 page table translation
 * levels in addition to the PGD and potentially the PUD which are
 * pre-allocated (we pre-allocate the fake PGD and the PUD when the Stage-2
 * tables use one level of tables less than the kernel.
 */
#ifdef CONFIG_ARM64_64K_PAGES
#define KVM_MMU_CACHE_MIN_PAGES	1
#else
#define KVM_MMU_CACHE_MIN_PAGES	2
#endif

#ifdef __ASSEMBLY__

/*
 * Convert a kernel VA into a HYP VA.
 * reg: VA to be converted.
 */
.macro kern_hyp_va	reg
	and	\reg, \reg, #HYP_PAGE_OFFSET_MASK
.endm

#else

#include <asm/pgalloc.h>
#include <asm/cachetype.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/pgtable.h>
#include <linux/hugetlb.h>

#define KERN_TO_HYP(kva)	((unsigned long)kva - PAGE_OFFSET + HYP_PAGE_OFFSET)

/*
 * We currently only support a 40bit IPA.
 */
#define KVM_PHYS_SHIFT	(40)
#define KVM_PHYS_SIZE	(1UL << KVM_PHYS_SHIFT)
#define KVM_PHYS_MASK	(KVM_PHYS_SIZE - 1UL)

#include <asm/stage2_pgtable.h>

int create_hyp_mappings(void *from, void *to);
int create_hyp_io_mappings(void *from, void *to, phys_addr_t);
void free_boot_hyp_pgd(void);
void free_hyp_pgds(void);

void stage2_unmap_vm(struct kvm *kvm);
int kvm_alloc_stage2_pgd(struct kvm *kvm);
void kvm_free_stage2_pgd(struct kvm *kvm);
int kvm_phys_addr_ioremap(struct kvm *kvm, phys_addr_t guest_ipa,
			  phys_addr_t pa, unsigned long size, bool writable);

int kvm_handle_guest_abort(struct kvm_vcpu *vcpu, struct kvm_run *run);

void kvm_mmu_free_memory_caches(struct kvm_vcpu *vcpu);

phys_addr_t kvm_mmu_get_httbr(void);
phys_addr_t kvm_mmu_get_boot_httbr(void);
phys_addr_t kvm_get_idmap_vector(void);
int kvm_mmu_init(void);
void kvm_clear_hyp_idmap(void);

#define	kvm_set_pte(ptep, pte)		set_pte(ptep, pte)
#define	kvm_set_pmd(pmdp, pmd)		set_pmd(pmdp, pmd)

static inline void kvm_clean_pgd(pgd_t *pgd) {}
static inline void kvm_clean_pmd(pmd_t *pmd) {}
static inline void kvm_clean_pmd_entry(pmd_t *pmd) {}
static inline void kvm_clean_pte(pte_t *pte) {}
static inline void kvm_clean_pte_entry(pte_t *pte) {}

static inline void kvm_set_s2pte_writable(pte_t *pte)
{
	pte_val(*pte) |= PTE_S2_RDWR;
}

static inline void kvm_set_s2pmd_writable(pmd_t *pmd)
{
	pmd_val(*pmd) |= PMD_S2_RDWR;
}

static inline void kvm_set_s2pte_readonly(pte_t *pte)
{
	pte_val(*pte) = (pte_val(*pte) & ~PTE_S2_RDWR) | PTE_S2_RDONLY;
}

static inline bool kvm_s2pte_readonly(pte_t *pte)
{
	return (pte_val(*pte) & PTE_S2_RDWR) == PTE_S2_RDONLY;
}

static inline void kvm_set_s2pmd_readonly(pmd_t *pmd)
{
	pmd_val(*pmd) = (pmd_val(*pmd) & ~PMD_S2_RDWR) | PMD_S2_RDONLY;
}

static inline bool kvm_s2pmd_readonly(pmd_t *pmd)
{
	return (pmd_val(*pmd) & PMD_S2_RDWR) == PMD_S2_RDONLY;
}

static inline int kvm_pud_huge(struct kvm *kvm, pud_t pud)
{
	return kvm ? stage2_pud_huge(pud) : pud_huge(pud);
}

static inline int kvm_pgd_none(struct kvm *kvm, pgd_t pgd)
{
	return kvm ? stage2_pgd_none(pgd) : pgd_none(pgd);
}

static inline void kvm_pgd_clear(struct kvm *kvm, pgd_t *pgdp)
{
	if (kvm)
		stage2_pgd_clear(pgdp);
	else
		pgd_clear(pgdp);
}

static inline int kvm_pgd_present(struct kvm *kvm, pgd_t pgd)
{
	return kvm ? stage2_pgd_present(pgd) : pgd_present(pgd);
}

static inline void
kvm_pgd_populate(struct kvm *kvm, struct mm_struct *mm, pgd_t *pgd, pud_t *pud)
{
	if (kvm)
		stage2_pgd_populate(mm, pgd, pud);
	else
		pgd_populate(mm, pgd, pud);
}

static inline pud_t *
kvm_pud_offset(struct kvm *kvm, pgd_t *pgd, phys_addr_t address)
{
	return kvm ? stage2_pud_offset(pgd, address) : pud_offset(pgd, address);
}

static inline void kvm_pud_free(struct kvm *kvm, struct mm_struct *mm, pud_t *pudp)
{
	if (kvm)
		stage2_pud_free(mm, pudp);
	else
		pud_free(mm, pudp);
}

static inline int kvm_pud_none(struct kvm *kvm, pud_t pud)
{
	return kvm ? stage2_pud_none(pud) : pud_none(pud);
}

static inline void kvm_pud_clear(struct kvm *kvm, pud_t *pudp)
{
	if (kvm)
		stage2_pud_clear(pudp);
	else
		pud_clear(pudp);
}

static inline int kvm_pud_present(struct kvm *kvm, pud_t pud)
{
	return kvm ? stage2_pud_present(pud) : pud_present(pud);
}

static inline void
kvm_pud_populate(struct kvm *kvm, struct mm_struct *mm, pud_t *pud, pmd_t *pmd)
{
	if (kvm)
		stage2_pud_populate(mm, pud, pmd);
	else
		pud_populate(mm, pud, pmd);
}

static inline pmd_t *
kvm_pmd_offset(struct kvm *kvm, pud_t *pud, phys_addr_t address)
{
	return kvm ? stage2_pmd_offset(pud, address) : pmd_offset(pud, address);
}

static inline void kvm_pmd_free(struct kvm *kvm, struct mm_struct *mm, pmd_t *pmd)
{
	if (kvm)
		stage2_pmd_free(mm, pmd);
	else
		pmd_free(mm, pmd);
}

static inline phys_addr_t
kvm_pgd_addr_end(struct kvm *kvm, phys_addr_t addr, phys_addr_t end)
{
	return	kvm ? stage2_pgd_addr_end(addr, end) : pgd_addr_end(addr, end);
}

static inline phys_addr_t
kvm_pud_addr_end(struct kvm *kvm, phys_addr_t addr, phys_addr_t end)
{
	return	kvm ? stage2_pud_addr_end(addr, end) : pud_addr_end(addr, end);
}

static inline phys_addr_t
kvm_pmd_addr_end(struct kvm *kvm, phys_addr_t addr, phys_addr_t end)
{
	return kvm ? stage2_pmd_addr_end(addr, end) : pmd_addr_end(addr, end);
}

static inline phys_addr_t kvm_pgd_index(struct kvm *kvm, phys_addr_t addr)
{
	return kvm ? stage2_pgd_index(addr) : pgd_index(addr);
}

static inline void *kvm_get_hwpgd(struct kvm *kvm)
{
	pgd_t *pgd = kvm->arch.pgd;
	pud_t *pud;

	if (KVM_PREALLOC_LEVEL == 0)
		return pgd;

	pud = pud_offset(pgd, 0);
	if (KVM_PREALLOC_LEVEL == 1)
		return pud;

	BUG_ON(KVM_PREALLOC_LEVEL != 2);
	return pmd_offset(pud, 0);
}

static inline unsigned int kvm_get_hwpgd_size(void)
{
	if (KVM_PREALLOC_LEVEL > 0)
		return PTRS_PER_S2_PGD * PAGE_SIZE;
	return PTRS_PER_S2_PGD * sizeof(pgd_t);
}

/*
 * Allocate fake pgd for the host kernel page table macros to work.
 * This is not used by the hardware and we have no alignment
 * requirement for this allocation.
 */
static inline pgd_t *kvm_setup_fake_pgd(pgd_t *hwpgd)
{
	int i;
	pgd_t *pgd;

	if (!KVM_PREALLOC_LEVEL)
		return hwpgd;

	/*
	 * When KVM_PREALLOC_LEVEL==2, we allocate a single page for
	 * the PMD and the kernel will use folded pud.
	 * When KVM_PREALLOC_LEVEL==1, we allocate 2 consecutive PUD
	 * pages.
	 */

	pgd = kmalloc(PTRS_PER_S2_PGD * sizeof(pgd_t),
			GFP_KERNEL | __GFP_ZERO);
	if (!pgd)
		return ERR_PTR(-ENOMEM);

	/* Plug the HW PGD into the fake one. */
	for (i = 0; i < PTRS_PER_S2_PGD; i++) {
		if (KVM_PREALLOC_LEVEL == 1)
			pgd_populate(NULL, pgd + i,
				     (pud_t *)hwpgd + i * PTRS_PER_PUD);
		else if (KVM_PREALLOC_LEVEL == 2)
			pud_populate(NULL, pud_offset(pgd, 0) + i,
				     (pmd_t *)hwpgd + i * PTRS_PER_PMD);
	}

	return pgd;
}

static inline void kvm_free_fake_pgd(pgd_t *pgd)
{
	if (KVM_PREALLOC_LEVEL > 0)
		kfree(pgd);
}
static inline bool kvm_page_empty(void *ptr)
{
	struct page *ptr_page = virt_to_page(ptr);
	return page_count(ptr_page) == 1;
}

#ifdef __PAGETABLE_PMD_FOLDED
#define hyp_pmd_table_empty(pmdp)	(0)
#else
#define hyp_pmd_table_empty(pmdp)	kvm_page_empty(pmdp)
#endif

#ifdef __PAGETABLE_PUD_FOLDED
#define hyp_pud_table_empty(pudp)	(0)
#else
#define hyp_pud_table_empty(pudp)	kvm_page_empty(pudp)
#endif

static inline bool kvm_pte_table_empty(struct kvm *kvm, pte_t *ptep)
{
	return kvm_page_empty(ptep);
}

static inline bool kvm_pmd_table_empty(struct kvm *kvm, pmd_t *pmdp)
{
	return kvm ? stage2_pmd_table_empty(pmdp) : hyp_pmd_table_empty(pmdp);
}

static inline bool kvm_pud_table_empty(struct kvm *kvm, pud_t *pudp)
{
	return kvm ? stage2_pud_table_empty(pudp) : hyp_pud_table_empty(pudp);
}


struct kvm;

#define kvm_flush_dcache_to_poc(a,l)	__flush_dcache_area((a), (l))

static inline bool vcpu_has_cache_enabled(struct kvm_vcpu *vcpu)
{
	return (vcpu_sys_reg(vcpu, SCTLR_EL1) & 0b101) == 0b101;
}

static inline void __coherent_cache_guest_page(struct kvm_vcpu *vcpu,
					       kvm_pfn_t pfn,
					       unsigned long size,
					       bool ipa_uncached)
{
	void *va = page_address(pfn_to_page(pfn));

	if (!vcpu_has_cache_enabled(vcpu) || ipa_uncached)
		kvm_flush_dcache_to_poc(va, size);

	if (!icache_is_aliasing()) {		/* PIPT */
		flush_icache_range((unsigned long)va,
				   (unsigned long)va + size);
	} else if (!icache_is_aivivt()) {	/* non ASID-tagged VIVT */
		/* any kind of VIPT cache */
		__flush_icache_all();
	}
}

static inline void __kvm_flush_dcache_pte(pte_t pte)
{
	struct page *page = pte_page(pte);
	kvm_flush_dcache_to_poc(page_address(page), PAGE_SIZE);
}

static inline void __kvm_flush_dcache_pmd(pmd_t pmd)
{
	struct page *page = pmd_page(pmd);
	kvm_flush_dcache_to_poc(page_address(page), PMD_SIZE);
}

static inline void __kvm_flush_dcache_pud(pud_t pud)
{
	struct page *page = pud_page(pud);
	kvm_flush_dcache_to_poc(page_address(page), PUD_SIZE);
}

#define kvm_virt_to_phys(x)		__virt_to_phys((unsigned long)(x))

void kvm_set_way_flush(struct kvm_vcpu *vcpu);
void kvm_toggle_cache(struct kvm_vcpu *vcpu, bool was_enabled);

static inline bool __kvm_cpu_uses_extended_idmap(void)
{
	return __cpu_uses_extended_idmap();
}

static inline void __kvm_extend_hypmap(pgd_t *boot_hyp_pgd,
				       pgd_t *hyp_pgd,
				       pgd_t *merged_hyp_pgd,
				       unsigned long hyp_idmap_start)
{
	int idmap_idx;

	/*
	 * Use the first entry to access the HYP mappings. It is
	 * guaranteed to be free, otherwise we wouldn't use an
	 * extended idmap.
	 */
	VM_BUG_ON(pgd_val(merged_hyp_pgd[0]));
	merged_hyp_pgd[0] = __pgd(__pa(hyp_pgd) | PMD_TYPE_TABLE);

	/*
	 * Create another extended level entry that points to the boot HYP map,
	 * which contains an ID mapping of the HYP init code. We essentially
	 * merge the boot and runtime HYP maps by doing so, but they don't
	 * overlap anyway, so this is fine.
	 */
	idmap_idx = hyp_idmap_start >> VA_BITS;
	VM_BUG_ON(pgd_val(merged_hyp_pgd[idmap_idx]));
	merged_hyp_pgd[idmap_idx] = __pgd(__pa(boot_hyp_pgd) | PMD_TYPE_TABLE);
}

static inline unsigned int kvm_get_vmid_bits(void)
{
	int reg = read_system_reg(SYS_ID_AA64MMFR1_EL1);

	return (cpuid_feature_extract_field(reg, ID_AA64MMFR1_VMIDBITS_SHIFT) == 2) ? 16 : 8;
}

#endif /* __ASSEMBLY__ */
#endif /* __ARM64_KVM_MMU_H__ */
