// SPDX-License-Identifier: GPL-2.0
#ifndef _ASM_X86_PTI_H
#define _ASM_X86_PTI_H
#ifndef __ASSEMBLY__

#ifdef CONFIG_PAGE_TABLE_ISOLATION
static inline unsigned short mm_pti_disable(struct mm_struct *mm)
{
	if (mm == NULL)
		return 0;

	return mm->context.pti_disable;
}

extern void pti_init(void);
extern void pti_check_boottime_disable(void);
#else
static inline unsigned short mm_pti_disable(struct mm_struct *mm) { return 0; }
static inline void pti_check_boottime_disable(void) { }
#endif

#endif /* __ASSEMBLY__ */
#endif /* _ASM_X86_PTI_H */
