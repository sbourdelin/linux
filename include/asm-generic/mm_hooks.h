/*
 * Define generic no-op hooks for mmap and protection related routines
 * to be included in asm-FOO/mmu_context.h for any arch FOO which doesn't
 * need to hook these.
 */
#ifndef _ASM_GENERIC_MM_HOOKS_H
#define _ASM_GENERIC_MM_HOOKS_H

static inline unsigned long arch_pre_mmap_flags(struct file *file,
						unsigned long flags,
						vm_flags_t *vm_flags)
{
	return 0;	/* no errors */
}

static inline void arch_post_mmap(struct mm_struct *mm, unsigned long addr,
					vm_flags_t vm_flags)
{
}

static inline void arch_dup_mmap(struct mm_struct *oldmm,
				 struct mm_struct *mm)
{
}

static inline void arch_exit_mmap(struct mm_struct *mm)
{
}

static inline void arch_unmap(struct mm_struct *mm,
			struct vm_area_struct *vma,
			unsigned long start, unsigned long end)
{
}

static inline void arch_bprm_mm_init(struct mm_struct *mm,
				     struct vm_area_struct *vma)
{
}

static inline bool arch_vma_access_permitted(struct vm_area_struct *vma,
		bool write, bool execute, bool foreign)
{
	/* by default, allow everything */
	return true;
}

static inline bool arch_pte_access_permitted(pte_t pte, bool write)
{
	/* by default, allow everything */
	return true;
}
#endif	/* _ASM_GENERIC_MM_HOOKS_H */
