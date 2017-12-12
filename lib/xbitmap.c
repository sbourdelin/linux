#include <linux/export.h>
#include <linux/xbitmap.h>
#include <linux/bitmap.h>
#include <linux/slab.h>

/*
 * Developer notes: locks are required to gurantee there is no concurrent
 * calls of xb_set_bit, xb_clear_bit, xb_clear_bit_range, xb_test_bit,
 * xb_find_next_set_bit, or xb_find_next_clear_bit to operate on the same
 * ida bitamp.
 */

/**
 *  xb_set_bit - set a bit in the xbitmap
 *  @xb: the xbitmap tree used to record the bit
 *  @bit: index of the bit to set
 *
 * This function is used to set a bit in the xbitmap. If the bitmap that @bit
 * resides in is not there, the per-cpu ida_bitmap will be taken.
 *
 * Returns: 0 on success. %-EAGAIN indicates that @bit was not set.
 */
int xb_set_bit(struct xb *xb, unsigned long bit)
{
	int err;
	unsigned long index = bit / IDA_BITMAP_BITS;
	struct radix_tree_root *root = &xb->xbrt;
	struct radix_tree_node *node;
	void **slot;
	struct ida_bitmap *bitmap;
	unsigned long ebit;

	bit %= IDA_BITMAP_BITS;
	ebit = bit + 2;

	err = __radix_tree_create(root, index, 0, &node, &slot);
	if (err)
		return err;
	bitmap = rcu_dereference_raw(*slot);
	if (radix_tree_exception(bitmap)) {
		unsigned long tmp = (unsigned long)bitmap;

		if (ebit < BITS_PER_LONG) {
			tmp |= 1UL << ebit;
			rcu_assign_pointer(*slot, (void *)tmp);
			return 0;
		}
		bitmap = this_cpu_xchg(ida_bitmap, NULL);
		if (!bitmap) {
			__radix_tree_delete(root, node, slot);
			return -EAGAIN;
		}
		memset(bitmap, 0, sizeof(*bitmap));
		bitmap->bitmap[0] = tmp >> RADIX_TREE_EXCEPTIONAL_SHIFT;
		rcu_assign_pointer(*slot, bitmap);
	}

	if (!bitmap) {
		if (ebit < BITS_PER_LONG) {
			bitmap = (void *)((1UL << ebit) |
					RADIX_TREE_EXCEPTIONAL_ENTRY);
			__radix_tree_replace(root, node, slot, bitmap, NULL);
			return 0;
		}
		bitmap = this_cpu_xchg(ida_bitmap, NULL);
		if (!bitmap) {
			__radix_tree_delete(root, node, slot);
			return -EAGAIN;
		}
		memset(bitmap, 0, sizeof(*bitmap));
		__radix_tree_replace(root, node, slot, bitmap, NULL);
	}

	__set_bit(bit, bitmap->bitmap);
	return 0;
}
EXPORT_SYMBOL(xb_set_bit);

/**
 *  xb_preload_and_set_bit - preload the memory and set a bit in the xbitmap
 *  @xb: the xbitmap tree used to record the bit
 *  @bit: index of the bit to set
 *
 * A wrapper of the xb_preload() and xb_set_bit().
 * Returns: 0 on success; -EAGAIN or -ENOMEM on error.
 */
int xb_preload_and_set_bit(struct xb *xb, unsigned long bit, gfp_t gfp)
{
	int ret = 0;

	if (!xb_preload(gfp))
		return -ENOMEM;

	ret = xb_set_bit(xb, bit);
	xb_preload_end();

	return ret;
}
EXPORT_SYMBOL(xb_preload_and_set_bit);

/**
 * xb_clear_bit - clear a bit in the xbitmap
 * @xb: the xbitmap tree used to record the bit
 * @bit: index of the bit to clear
 *
 * This function is used to clear a bit in the xbitmap. If all the bits of the
 * bitmap are 0, the bitmap will be freed.
 */
void xb_clear_bit(struct xb *xb, unsigned long bit)
{
	unsigned long index = bit / IDA_BITMAP_BITS;
	struct radix_tree_root *root = &xb->xbrt;
	struct radix_tree_node *node;
	void **slot;
	struct ida_bitmap *bitmap;
	unsigned long ebit;

	bit %= IDA_BITMAP_BITS;
	ebit = bit + 2;

	bitmap = __radix_tree_lookup(root, index, &node, &slot);
	if (radix_tree_exception(bitmap)) {
		unsigned long tmp = (unsigned long)bitmap;

		if (ebit >= BITS_PER_LONG)
			return;
		tmp &= ~(1UL << ebit);
		if (tmp == RADIX_TREE_EXCEPTIONAL_ENTRY)
			__radix_tree_delete(root, node, slot);
		else
			rcu_assign_pointer(*slot, (void *)tmp);
		return;
	}

	if (!bitmap)
		return;

	__clear_bit(bit, bitmap->bitmap);
	if (bitmap_empty(bitmap->bitmap, IDA_BITMAP_BITS)) {
		kfree(bitmap);
		__radix_tree_delete(root, node, slot);
	}
}
EXPORT_SYMBOL(xb_clear_bit);

/**
 * xb_clear_bit_range - clear a range of bits in the xbitmap
 * @start: the start of the bit range, inclusive
 * @end: the end of the bit range, exclusive
 *
 * This function is used to clear a bit in the xbitmap. If all the bits of the
 * bitmap are 0, the bitmap will be freed.
 */
void xb_clear_bit_range(struct xb *xb, unsigned long start, unsigned long end)
{
	struct radix_tree_root *root = &xb->xbrt;
	struct radix_tree_node *node;
	void **slot;
	struct ida_bitmap *bitmap;
	unsigned int nbits;

	for (; start < end; start = (start | (IDA_BITMAP_BITS - 1)) + 1) {
		unsigned long index = start / IDA_BITMAP_BITS;
		unsigned long bit = start % IDA_BITMAP_BITS;

		bitmap = __radix_tree_lookup(root, index, &node, &slot);
		if (radix_tree_exception(bitmap)) {
			unsigned long ebit = bit + 2;
			unsigned long tmp = (unsigned long)bitmap;

			nbits = min(end - start + 1, BITS_PER_LONG - ebit);

			if (ebit >= BITS_PER_LONG)
				continue;
			bitmap_clear(&tmp, ebit, nbits);
			if (tmp == RADIX_TREE_EXCEPTIONAL_ENTRY)
				__radix_tree_delete(root, node, slot);
			else
				rcu_assign_pointer(*slot, (void *)tmp);
		} else if (bitmap) {
			nbits = min(end - start + 1, IDA_BITMAP_BITS - bit);

			if (nbits != IDA_BITMAP_BITS)
				bitmap_clear(bitmap->bitmap, bit, nbits);

			if (nbits == IDA_BITMAP_BITS ||
			    bitmap_empty(bitmap->bitmap, IDA_BITMAP_BITS)) {
				kfree(bitmap);
				__radix_tree_delete(root, node, slot);
			}
		}

		/*
		 * Already reached the last usable ida bitmap, so just return,
		 * otherwise overflow will happen.
		 */
		if (index == ULONG_MAX / IDA_BITMAP_BITS)
			break;
	}
}
EXPORT_SYMBOL(xb_clear_bit_range);

/**
 * xb_test_bit - test a bit in the xbitmap
 * @xb: the xbitmap tree used to record the bit
 * @bit: index of the bit to test
 *
 * This function is used to test a bit in the xbitmap.
 *
 * Returns: true if the bit is set, or false otherwise.
 */
bool xb_test_bit(const struct xb *xb, unsigned long bit)
{
	unsigned long index = bit / IDA_BITMAP_BITS;
	const struct radix_tree_root *root = &xb->xbrt;
	struct ida_bitmap *bitmap = radix_tree_lookup(root, index);

	bit %= IDA_BITMAP_BITS;

	if (!bitmap)
		return false;
	if (radix_tree_exception(bitmap)) {
		bit += RADIX_TREE_EXCEPTIONAL_SHIFT;
		if (bit >= BITS_PER_LONG)
			return false;
		return (unsigned long)bitmap & (1UL << bit);
	}
	return test_bit(bit, bitmap->bitmap);
}
EXPORT_SYMBOL(xb_test_bit);

static unsigned long xb_find_next_bit(struct xb *xb, unsigned long start,
				      unsigned long end, bool set)
{
	struct radix_tree_root *root = &xb->xbrt;
	struct radix_tree_node *node;
	void **slot;
	struct ida_bitmap *bmap;
	unsigned long ret = end;

	for (; start < end; start = (start | (IDA_BITMAP_BITS - 1)) + 1) {
		unsigned long index = start / IDA_BITMAP_BITS;
		unsigned long bit = start % IDA_BITMAP_BITS;

		bmap = __radix_tree_lookup(root, index, &node, &slot);
		if (radix_tree_exception(bmap)) {
			unsigned long tmp = (unsigned long)bmap;
			unsigned long ebit = bit + 2;

			if (ebit >= BITS_PER_LONG)
				continue;
			if (set)
				ret = find_next_bit(&tmp, BITS_PER_LONG, ebit);
			else
				ret = find_next_zero_bit(&tmp, BITS_PER_LONG,
							 ebit);
			if (ret < BITS_PER_LONG)
				return ret - 2 + IDA_BITMAP_BITS * index;
		} else if (bmap) {
			if (set)
				ret = find_next_bit(bmap->bitmap,
						    IDA_BITMAP_BITS, bit);
			else
				ret = find_next_zero_bit(bmap->bitmap,
							 IDA_BITMAP_BITS, bit);
			if (ret < IDA_BITMAP_BITS)
				return ret + index * IDA_BITMAP_BITS;
		} else if (!bmap && !set) {
			return start;
		}

		/*
		 * Already reached the last searchable ida bitmap. Return
		 * ULONG_MAX, otherwise overflow will happen.
		 */
		if (index == ULONG_MAX / IDA_BITMAP_BITS)
			return ULONG_MAX;
	}

	return ret;
}

/**
 * xb_find_next_set_bit - find the next set bit in a range
 * @xb: the xbitmap to search
 * @start: the start of the range, inclusive
 * @end: the end of the range, exclusive
 *
 * Returns: the index of the found bit, or @end + 1 if no such bit is found.
 */
unsigned long xb_find_next_set_bit(struct xb *xb, unsigned long start,
				   unsigned long end)
{
	return xb_find_next_bit(xb, start, end, 1);
}
EXPORT_SYMBOL(xb_find_next_set_bit);

/**
 * xb_find_next_zero_bit - find the next zero bit in a range
 * @xb: the xbitmap to search
 * @start: the start of the range, inclusive
 * @end: the end of the range, exclusive
 *
 * Returns: the index of the found bit, or @end + 1 if no such bit is found.
 */
unsigned long xb_find_next_zero_bit(struct xb *xb, unsigned long start,
				    unsigned long end)
{
	return xb_find_next_bit(xb, start, end, 0);
}
EXPORT_SYMBOL(xb_find_next_zero_bit);

#ifndef __KERNEL__

static DEFINE_XB(xb1);

void xbitmap_check_bit(unsigned long bit)
{
	xb_preload(GFP_KERNEL);
	assert(!xb_test_bit(&xb1, bit));
	assert(xb_set_bit(&xb1, bit) == 0);
	assert(xb_test_bit(&xb1, bit));
	xb_clear_bit(&xb1, bit);
	assert(xb_empty(&xb1));
	xb_clear_bit(&xb1, bit);
	assert(xb_empty(&xb1));
	xb_preload_end();
}

static void xbitmap_check_bit_range(void)
{
	/*
	 * Regular tests
	 * ebit tests: set 1030, 1031, 1034, 1035
	 * Next 1 in [0, 10000)    --> 1030
	 * Next 1 in [1030, 1034)  --> 1030
	 * Next 1 in [1032, 1034)  --> none (1034)
	 * Next 0 in [1030, 1032)  --> none (1032)
	 * Next 0 in [1030, 1033)  --> 1032
	 *
	 * ida bitmap tests: set 8260, 8261, 8264, 8265
	 * Next 1 in [2000, 10000) --> 8260
	 * Next 1 in [8260, 8264)  --> 8260
	 * Next 1 in [8262, 8264)  --> none (8264)
	 * Next 0 in [8260, 8262)  --> none (8262)
	 * Next 0 in [8260, 8263)  --> 8262
	 */
	assert(!xb_preload_and_set_bit(&xb1, 1030, GFP_KERNEL));
	assert(!xb_preload_and_set_bit(&xb1, 1031, GFP_KERNEL));
	assert(!xb_preload_and_set_bit(&xb1, 1034, GFP_KERNEL));
	assert(!xb_preload_and_set_bit(&xb1, 1035, GFP_KERNEL));
	assert(!xb_preload_and_set_bit(&xb1, 8260, GFP_KERNEL));
	assert(!xb_preload_and_set_bit(&xb1, 8261, GFP_KERNEL));
	assert(!xb_preload_and_set_bit(&xb1, 8264, GFP_KERNEL));
	assert(!xb_preload_and_set_bit(&xb1, 8265, GFP_KERNEL));

	assert(xb_find_next_set_bit(&xb1, 0, 10000) == 1030);
	assert(xb_find_next_set_bit(&xb1, 1030, 1034) == 1030);
	assert(xb_find_next_set_bit(&xb1, 1032, 1034) == 1034);
	assert(xb_find_next_zero_bit(&xb1, 1030, 1032) == 1032);
	assert(xb_find_next_zero_bit(&xb1, 1030, 1033) == 1032);

	assert(xb_find_next_set_bit(&xb1, 2000, 10000) == 8260);
	assert(xb_find_next_set_bit(&xb1, 8260, 8264) == 8260);
	assert(xb_find_next_set_bit(&xb1, 8262, 8264) == 8264);
	assert(xb_find_next_zero_bit(&xb1, 8260, 8262) == 8262);
	assert(xb_find_next_zero_bit(&xb1, 8260, 8263) == 8262);

	xb_clear_bit_range(&xb1, 0, 10000);
	assert(xb_find_next_set_bit(&xb1, 0, 10000) == 10000);

	/*
	 * Overflow tests:
	 * Next 1 in [0, ULONG_MAX)		--> none (ULONG_MAX)
	 * Set ULONG_MAX - 4
	 * Next 1 in [0, ULONG_MAX)		--> ULONG_MAX - 4
	 * Next 1 in [ULONG_MAX - 3, ULONG_MAX)	--> none (ULONG_MAX)
	 * Next 0 in [ULONG_MAX - 4, ULONG_MAX)	--> ULONG_MAX - 3
	 */
	assert(!xb_preload_and_set_bit(&xb1, ULONG_MAX - 4, GFP_KERNEL));
	assert(xb_find_next_set_bit(&xb1, ULONG_MAX - 10, ULONG_MAX) ==
	       ULONG_MAX - 4);
	assert(xb_find_next_set_bit(&xb1, ULONG_MAX - 3, ULONG_MAX) ==
	       ULONG_MAX);
	assert(xb_find_next_zero_bit(&xb1, ULONG_MAX - 4, ULONG_MAX) ==
	       ULONG_MAX - 3);
	xb_clear_bit_range(&xb1, ULONG_MAX - 10, ULONG_MAX);
}

void xbitmap_checks(void)
{
	xb_init(&xb1);
	xbitmap_check_bit(0);
	xbitmap_check_bit(30);
	xbitmap_check_bit(31);
	xbitmap_check_bit(1023);
	xbitmap_check_bit(1024);
	xbitmap_check_bit(1025);
	xbitmap_check_bit((1UL << 63) | (1UL << 24));
	xbitmap_check_bit((1UL << 63) | (1UL << 24) | 70);

	xbitmap_check_bit_range();
}

int __weak main(void)
{
	radix_tree_init();
	xbitmap_checks();
}
#endif
