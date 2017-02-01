#ifndef _ASM_X86_MPX_H
#define _ASM_X86_MPX_H

#include <linux/types.h>
#include <asm/ptrace.h>
#include <asm/insn.h>

/*
 * These get stored into mm_context_t->mpx_directory_info.
 * We could theoretically use bits 0 and 1, but those are
 * used in the BNDCFGU register that also holds the bounds
 * directory pointer.  To avoid confusion, use different bits.
 */
#define MPX_INVALID_BOUNDS_DIR	(1UL<<2)
#define MPX_LARGE_BOUNDS_DIR	(1UL<<3)

#define MPX_BNDCFG_ENABLE_FLAG	0x1
#define MPX_BD_ENTRY_VALID_FLAG	0x1

/*
 * The uppermost bits [56:20] of the virtual address in 64-bit
 * are used to index into bounds directory (BD).  On processors
 * with support for smaller virtual address space size, the "56"
 * is obviously smaller.
 *
 * When using 47-bit virtual addresses, the directory is 2G
 * (2^31) bytes in size, and with 8-byte entries it has 2^28
 * entries.  With 56-bit virtual addresses, it goes to 1T in size
 * and has 2^37 entries.
 *
 * Needs to be ULL so we can use this in 32-bit kernels without
 * warnings.
 */
#define MPX_BD_BASE_SIZE_BYTES_64	(1ULL<<31)
#define MPX_BD_ENTRY_BYTES_64	8
/*
 * Note: size of tables on 64-bit is not constant, so we have no
 * fixed definition for MPX_BD_NR_ENTRIES_64.
 *
 * The 5-Level Paging Whitepaper says:  "A bound directory
 * comprises 2^(28+MAWA) 64-bit entries."  Since MAWA=0 in
 * legacy mode:
 */
#define MPX_BD_LEGACY_NR_ENTRIES_64	(1UL<<28)

/*
 * When the hardware "MAWA" feature is enabled, we have a larger
 * bounds directory.  There are only two sizes supported: large
 * and small, so we only need a single value here.
 */
#define MPX_LARGE_BOUNDS_DIR_SHIFT	9

/*
 * The 32-bit directory is 4MB (2^22) in size, and with 4-byte
 * entries it has 2^20 entries.
 */
#define MPX_BD_SIZE_BYTES_32	(1UL<<22)
#define MPX_BD_ENTRY_BYTES_32	4
#define MPX_BD_NR_ENTRIES_32	(MPX_BD_SIZE_BYTES_32/MPX_BD_ENTRY_BYTES_32)

/*
 * A 64-bit table is 4MB total in size, and an entry is
 * 4 64-bit pointers in size.
 */
#define MPX_BT_SIZE_BYTES_64	(1UL<<22)
#define MPX_BT_ENTRY_BYTES_64	32
#define MPX_BT_NR_ENTRIES_64	(MPX_BT_SIZE_BYTES_64/MPX_BT_ENTRY_BYTES_64)

/*
 * A 32-bit table is 16kB total in size, and an entry is
 * 4 32-bit pointers in size.
 */
#define MPX_BT_SIZE_BYTES_32	(1UL<<14)
#define MPX_BT_ENTRY_BYTES_32	16
#define MPX_BT_NR_ENTRIES_32	(MPX_BT_SIZE_BYTES_32/MPX_BT_ENTRY_BYTES_32)

#define MPX_BNDSTA_TAIL		2
#define MPX_BNDCFG_TAIL		12
#define MPX_BNDSTA_ADDR_MASK	(~((1UL<<MPX_BNDSTA_TAIL)-1))
#define MPX_BNDCFG_ADDR_MASK	(~((1UL<<MPX_BNDCFG_TAIL)-1))
#define MPX_BNDSTA_ERROR_CODE	0x3

#ifdef CONFIG_X86_INTEL_MPX
siginfo_t *mpx_generate_siginfo(struct pt_regs *regs);
int mpx_handle_bd_fault(void);
static inline void __user *mpx_bounds_dir_addr(struct mm_struct *mm)
{
	/*
	 * The only bit that can be set in a valid bounds
	 * directory is MPX_LARGE_BOUNDS_DIR, so only mask
	 * it back off.
	 */
	return (void __user *)
		(mm->context.mpx_directory_info & ~MPX_LARGE_BOUNDS_DIR);
}
static inline int kernel_managing_mpx_tables(struct mm_struct *mm)
{
	return (mm->context.mpx_directory_info != MPX_INVALID_BOUNDS_DIR);
}
static inline void mpx_mm_init(struct mm_struct *mm)
{
	/*
	 * MPX starts out off (invalid) and with a legacy-size
	 * bounds directory (cleared MPX_LARGE_BOUNDS_DIR bit).
	 */
	mm->context.mpx_directory_info = MPX_INVALID_BOUNDS_DIR;
}
void mpx_notify_unmap(struct mm_struct *mm, struct vm_area_struct *vma,
		      unsigned long start, unsigned long end);
static inline int mpx_bd_size_shift(struct mm_struct *mm)
{
	if (!kernel_managing_mpx_tables(mm))
		return 0;
	if (mm->context.mpx_directory_info & MPX_LARGE_BOUNDS_DIR)
		return MPX_LARGE_BOUNDS_DIR_SHIFT;
	return 0;
}
static inline siginfo_t *mpx_generate_siginfo(struct pt_regs *regs)
{
	return NULL;
}
static inline int mpx_handle_bd_fault(void)
{
	return -EINVAL;
}
static inline int kernel_managing_mpx_tables(struct mm_struct *mm)
{
	return 0;
}
static inline void mpx_mm_init(struct mm_struct *mm)
{
}
static inline void mpx_notify_unmap(struct mm_struct *mm,
				    struct vm_area_struct *vma,
				    unsigned long start, unsigned long end)
{
}
/* Should never be called, but need stub to avoid an #ifdef */
static inline int mpx_bd_size_shift(struct mm_struct *mm)
{
	WARN_ON(1);
	return 0;
}
#endif /* CONFIG_X86_INTEL_MPX */

#endif /* _ASM_X86_MPX_H */
