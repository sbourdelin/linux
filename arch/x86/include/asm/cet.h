/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CET_H
#define _ASM_X86_CET_H

#ifndef __ASSEMBLY__
#include <linux/types.h>

struct task_struct;
/*
 * Per-thread CET status
 */
struct cet_status {
	unsigned long	shstk_base;
	unsigned long	shstk_size;
	unsigned int	shstk_enabled:1;
};

#ifdef CONFIG_X86_INTEL_CET
int cet_setup_shstk(void);
void cet_disable_shstk(void);
void cet_disable_free_shstk(struct task_struct *p);
#else
static inline int cet_setup_shstk(void) { return 0; }
static inline void cet_disable_shstk(void) {}
static inline void cet_disable_free_shstk(struct task_struct *p) {}
#endif

#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_CET_H */
