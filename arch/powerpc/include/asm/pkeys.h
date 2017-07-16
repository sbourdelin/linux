#ifndef _ASM_PPC64_PKEYS_H
#define _ASM_PPC64_PKEYS_H

extern bool pkey_inited;
#define ARCH_VM_PKEY_FLAGS 0

static inline bool mm_pkey_is_allocated(struct mm_struct *mm, int pkey)
{
	return (pkey == 0);
}

static inline int mm_pkey_alloc(struct mm_struct *mm)
{
	return -1;
}

static inline int mm_pkey_free(struct mm_struct *mm, int pkey)
{
	return -EINVAL;
}

/*
 * Try to dedicate one of the protection keys to be used as an
 * execute-only protection key.
 */
static inline int execute_only_pkey(struct mm_struct *mm)
{
	return 0;
}

static inline int arch_override_mprotect_pkey(struct vm_area_struct *vma,
		int prot, int pkey)
{
	return 0;
}

static inline int arch_set_user_pkey_access(struct task_struct *tsk, int pkey,
		unsigned long init_val)
{
	return 0;
}

static inline void pkey_initialize(void)
{
#ifdef CONFIG_PPC_64K_PAGES
	pkey_inited = !radix_enabled();
#else
	pkey_inited = false;
#endif
}
#endif /*_ASM_PPC64_PKEYS_H */
