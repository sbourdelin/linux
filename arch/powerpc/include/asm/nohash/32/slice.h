/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_NOHASH_32_SLICE_H
#define _ASM_POWERPC_NOHASH_32_SLICE_H

#ifdef CONFIG_PPC_MM_SLICES

#define SLICE_LOW_SHIFT		26	/* 64 slices */
#define SLICE_LOW_TOP		(0x100000000ull)
#define SLICE_NUM_LOW		(SLICE_LOW_TOP >> SLICE_LOW_SHIFT)
#define GET_LOW_SLICE_INDEX(addr)	((addr) >> SLICE_LOW_SHIFT)

#define SLICE_HIGH_SHIFT	0
#define SLICE_NUM_HIGH		0ul
#define GET_HIGH_SLICE_INDEX(addr)	(addr & 0)

#ifndef __ASSEMBLY__

static inline void slice_bitmap_zero(unsigned long *dst, unsigned int nbits)
{
}

static inline int slice_bitmap_and(unsigned long *dst,
				   const unsigned long *src1,
				   const unsigned long *src2,
				   unsigned int nbits)
{
	return 0;
}

static inline void slice_bitmap_or(unsigned long *dst,
				   const unsigned long *src1,
				   const unsigned long *src2,
				   unsigned int nbits)
{
}

static inline int slice_bitmap_andnot(unsigned long *dst,
				      const unsigned long *src1,
				      const unsigned long *src2,
				      unsigned int nbits)
{
	return 0;
}

static inline int slice_bitmap_equal(const unsigned long *src1,
				     const unsigned long *src2,
				     unsigned int nbits)
{
	return 1;
}

static inline int slice_bitmap_empty(const unsigned long *src, unsigned nbits)
{
	return 1;
}

static inline void slice_bitmap_set(unsigned long *map, unsigned int start,
				    unsigned int nbits)
{
}
#endif /* __ASSEMBLY__ */

#endif /* CONFIG_PPC_MM_SLICES */

#endif /* _ASM_POWERPC_NOHASH_32_SLICE_H */
