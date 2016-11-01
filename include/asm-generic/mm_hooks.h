/*
 * Define generic hooks for arch_dup_mmap, arch_exit_mmap and arch_unmap to be
 * included in asm-FOO/mmu_context.h for any arch FOO which doesn't need to
 * specially hook these.
 *
 * arch_remap originally from include/linux-mm-arch-hooks.h
 * arch_unmap originally from arch/powerpc/include/asm/mmu_context.h
 * Copyright (C) 2015, IBM Corporation
 * Author: Laurent Dufour <ldufour@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#ifndef _ASM_GENERIC_MM_HOOKS_H
#define _ASM_GENERIC_MM_HOOKS_H

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
#ifdef CONFIG_GENERIC_VDSO
	if (start <= mm->context.vdso && mm->context.vdso < end)
		mm->context.vdso = 0;
#endif /* CONFIG_GENERIC_VDSO */
}

static inline void arch_remap(struct mm_struct *mm,
			      unsigned long old_start, unsigned long old_end,
			      unsigned long new_start, unsigned long new_end)
{
#ifdef CONFIG_GENERIC_VDSO
	/*
	 * mremap() doesn't allow moving multiple vmas so we can limit the
	 * check to old_addr == vdso.
	 */
	if (old_addr == mm->context.vdso)
		mm->context.vdso = new_addr;

#endif /* CONFIG_GENERIC_VDSO */
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
