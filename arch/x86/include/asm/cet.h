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
	unsigned long	ibt_bitmap_addr;
	unsigned long	ibt_bitmap_size;
	unsigned int	shstk_enabled:1;
	unsigned int	ibt_enabled:1;
	unsigned int	locked:1;
};

#ifdef CONFIG_X86_INTEL_CET
int prctl_cet(int option, unsigned long arg2);
int cet_setup_shstk(void);
int cet_setup_thread_shstk(struct task_struct *p);
int cet_alloc_shstk(unsigned long *arg);
void cet_disable_shstk(void);
void cet_disable_free_shstk(struct task_struct *p);
int cet_restore_signal(unsigned long ssp);
int cet_setup_signal(bool ia32, unsigned long rstor, unsigned long *new_ssp);
int cet_setup_ibt(void);
int cet_setup_ibt_bitmap(void);
void cet_disable_ibt(void);
#else
static inline int prctl_cet(int option, unsigned long arg2) { return 0; }
static inline int cet_setup_shstk(void) { return 0; }
static inline int cet_setup_thread_shstk(struct task_struct *p) { return 0; }
static inline int cet_alloc_shstk(unsigned long *arg) { return -EINVAL; }
static inline void cet_disable_shstk(void) {}
static inline void cet_disable_free_shstk(struct task_struct *p) {}
static inline int cet_restore_signal(unsigned long ssp) { return 0; }
static inline int cet_setup_signal(bool ia32, unsigned long rstor,
				   unsigned long *new_ssp) { return 0; }
static inline int cet_setup_ibt(void) { return 0; }
static inline void cet_disable_ibt(void) {}
#endif

#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_CET_H */
