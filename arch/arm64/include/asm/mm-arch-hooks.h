/*
 * Architecture specific mm hooks
 *
 * Copyright (C) 2015, IBM Corporation
 * Author: Laurent Dufour <ldufour@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ASM_MM_ARCH_HOOKS_H
#define __ASM_MM_ARCH_HOOKS_H

static inline void arch_remap(struct mm_struct *mm,
			      unsigned long old_start, unsigned long old_end,
			      unsigned long new_start, unsigned long new_end)
{
	/*
	 * mremap() doesn't allow moving multiple vmas so we can limit the
	 * check to old_start == vdso_base.
	 */
	if ((void *)old_start == mm->context.vdso)
		mm->context.vdso = (void *)new_start;
}
#define arch_remap arch_remap

#endif /* __ASM_MM_ARCH_HOOKS_H */
