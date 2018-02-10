/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_BOOK3S_64_SLICE_H
#define _ASM_POWERPC_BOOK3S_64_SLICE_H

#ifdef CONFIG_PPC_MM_SLICES

#define SLICE_LOW_SHIFT		28
#define SLICE_LOW_TOP		(0x100000000ul)
#define SLICE_NUM_LOW		(SLICE_LOW_TOP >> SLICE_LOW_SHIFT)
#define GET_LOW_SLICE_INDEX(addr)	((addr) >> SLICE_LOW_SHIFT)

#define SLICE_HIGH_SHIFT	40
#define SLICE_NUM_HIGH		(H_PGTABLE_RANGE >> SLICE_HIGH_SHIFT)
#define GET_HIGH_SLICE_INDEX(addr)	((addr) >> SLICE_HIGH_SHIFT)

#ifndef __ASSEMBLY__

#include <linux/bitmap.h>

static inline void slice_bitmap_zero(unsigned long *dst, unsigned int nbits)
{
	bitmap_zero(dst, nbits);
}

static inline int slice_bitmap_and(unsigned long *dst,
				   const unsigned long *src1,
				   const unsigned long *src2,
				   unsigned int nbits)
{
	return bitmap_and(dst, src1, src2, nbits);
}

static inline void slice_bitmap_or(unsigned long *dst,
				   const unsigned long *src1,
				   const unsigned long *src2,
				   unsigned int nbits)
{
	bitmap_or(dst, src1, src2, nbits);
}

static inline int slice_bitmap_andnot(unsigned long *dst,
				      const unsigned long *src1,
				      const unsigned long *src2,
				      unsigned int nbits)
{
	return bitmap_andnot(dst, src1, src2, nbits);
}

static inline int slice_bitmap_equal(const unsigned long *src1,
				     const unsigned long *src2,
				     unsigned int nbits)
{
	return bitmap_equal(src1, src2, nbits);
}

static inline int slice_bitmap_empty(const unsigned long *src, unsigned nbits)
{
	return bitmap_empty(src, nbits);
}

static inline void slice_bitmap_set(unsigned long *map, unsigned int start,
				    unsigned int nbits)
{
	bitmap_set(map, start, nbits);
}
#endif /* __ASSEMBLY__ */

#else /* CONFIG_PPC_MM_SLICES */

#define get_slice_psize(mm, addr)	((mm)->context.user_psize)
#define slice_set_user_psize(mm, psize)		\
do {						\
	(mm)->context.user_psize = (psize);	\
	(mm)->context.sllp = SLB_VSID_USER | mmu_psize_defs[(psize)].sllp; \
} while (0)

#endif /* CONFIG_PPC_MM_SLICES */

#endif /* _ASM_POWERPC_BOOK3S_64_SLICE_H */
