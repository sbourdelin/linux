#ifndef _ASM_PPC64_PKEYS_H
#define _ASM_PPC64_PKEYS_H

extern bool pkey_inited;
#define arch_max_pkey()  32
#define AMR_RD_BIT 0x1UL
#define AMR_WR_BIT 0x2UL
#define IAMR_EX_BIT 0x1UL
#define AMR_BITS_PER_PKEY 2
#define ARCH_VM_PKEY_FLAGS (VM_PKEY_BIT0 | VM_PKEY_BIT1 | VM_PKEY_BIT2 | \
				VM_PKEY_BIT3 | VM_PKEY_BIT4)
#define AMR_BITS_PER_PKEY 2
/*
 * Bits are in BE format.
 * NOTE: key 31, 1, 0 are not used.
 * key 0 is used by default. It give read/write/execute permission.
 * key 31 is reserved by the hypervisor.
 * key 1 is recommended to be not used.
 * PowerISA(3.0) page 1015, programming note.
 */
#define PKEY_INITIAL_ALLOCAION  0xc0000001

#define pkeybit_mask(pkey) (0x1 << (arch_max_pkey() - pkey - 1))

#define mm_pkey_allocation_map(mm)	(mm->context.pkey_allocation_map)

#define mm_set_pkey_allocated(mm, pkey) {	\
	mm_pkey_allocation_map(mm) |= pkeybit_mask(pkey); \
}

#define mm_set_pkey_free(mm, pkey) {	\
	mm_pkey_allocation_map(mm) &= ~pkeybit_mask(pkey);	\
}

#define mm_set_pkey_is_allocated(mm, pkey)	\
	(mm_pkey_allocation_map(mm) & pkeybit_mask(pkey))

#define mm_set_pkey_is_reserved(mm, pkey) (PKEY_INITIAL_ALLOCAION & \
					pkeybit_mask(pkey))

static inline bool mm_pkey_is_allocated(struct mm_struct *mm, int pkey)
{
	/* a reserved key is never considered as 'explicitly allocated' */
	return ((pkey < arch_max_pkey()) &&
		!mm_set_pkey_is_reserved(mm, pkey) &&
		mm_set_pkey_is_allocated(mm, pkey));
}

extern void __arch_activate_pkey(int pkey);
extern void __arch_deactivate_pkey(int pkey);
/*
 * Returns a positive, 5-bit key on success, or -1 on failure.
 */
static inline int mm_pkey_alloc(struct mm_struct *mm)
{
	/*
	 * Note: this is the one and only place we make sure
	 * that the pkey is valid as far as the hardware is
	 * concerned.  The rest of the kernel trusts that
	 * only good, valid pkeys come out of here.
	 */
	u32 all_pkeys_mask = (u32)(~(0x0));
	int ret;

	if (!pkey_inited)
		return -1;
	/*
	 * Are we out of pkeys?  We must handle this specially
	 * because ffz() behavior is undefined if there are no
	 * zeros.
	 */
	if (mm_pkey_allocation_map(mm) == all_pkeys_mask)
		return -1;

	ret = arch_max_pkey() -
		ffz((u32)mm_pkey_allocation_map(mm))
		- 1;
	mm_set_pkey_allocated(mm, ret);

	/*
	 * enable the key in the hardware
	 */
	if (ret > 0)
		__arch_activate_pkey(ret);
	return ret;
}

static inline int mm_pkey_free(struct mm_struct *mm, int pkey)
{
	if (!pkey_inited)
		return -1;

	if (!mm_pkey_is_allocated(mm, pkey))
		return -EINVAL;

	/*
	 * Disable the key in the hardware
	 */
	__arch_deactivate_pkey(pkey);
	mm_set_pkey_free(mm, pkey);

	return 0;
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

extern int __arch_set_user_pkey_access(struct task_struct *tsk, int pkey,
		unsigned long init_val);
static inline int arch_set_user_pkey_access(struct task_struct *tsk, int pkey,
		unsigned long init_val)
{
	if (!pkey_inited)
		return -1;
	return __arch_set_user_pkey_access(tsk, pkey, init_val);
}

static inline void pkey_mm_init(struct mm_struct *mm)
{
	if (!pkey_inited)
		return;
	mm_pkey_allocation_map(mm) = PKEY_INITIAL_ALLOCAION;
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
