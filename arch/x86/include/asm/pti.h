// SPDX-License-Identifier: GPL-2.0
#ifndef _ASM_X86_PTI_H
#define _ASM_X86_PTI_H
#ifndef __ASSEMBLY__

#include <asm/desc.h>

#define PTI_DISABLE_OFF			(0)
#define PTI_DISABLE_IA32		(1 << 0)

#ifdef CONFIG_PAGE_TABLE_ISOLATION
static inline unsigned short mm_pti_disable(struct mm_struct *mm)
{
	if (mm == NULL)
		return 0;

	return mm->context.pti_disable;
}

static inline void pti_update_user_cs64(unsigned short prev_pti_disable,
					unsigned short next_pti_disable)
{
	struct desc_struct user_cs, *d;

	if ((prev_pti_disable ^ next_pti_disable) & PTI_DISABLE_IA32)
		return;

	d = get_cpu_gdt_rw(smp_processor_id());
	user_cs = d[GDT_ENTRY_DEFAULT_USER_CS];
	user_cs.p = !(next_pti_disable & PTI_DISABLE_IA32);
	write_gdt_entry(d, GDT_ENTRY_DEFAULT_USER_CS, &user_cs, DESCTYPE_S);
}

extern void pti_init(void);
extern void pti_check_boottime_disable(void);
#else
static inline unsigned short mm_pti_disable(struct mm_struct *mm) { return 0; }
static inline unsigned short mm_pti_disable(struct mm_struct *mm);
static inline void pti_update_user_cs64(unsigned short prev_pti_disable,
					unsigned short next_pti_disable) { }
static inline void pti_check_boottime_disable(void) { }
#endif

#endif /* __ASSEMBLY__ */
#endif /* _ASM_X86_PTI_H */
