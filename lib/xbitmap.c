#include <linux/export.h>
#include <linux/xbitmap.h>
#include <linux/bitmap.h>
#include <linux/slab.h>

/*
 * Developer notes:
 * - locks are required to gurantee there is no concurrent
 *   calls of xb_set_bit, xb_clear_bit, xb_clear_bit_range, xb_test_bit,
 *   xb_find_set, or xb_find_clear to operate on the same ida bitmap.
 * - The current implementation of xb_clear_bit_range, xb_find_set and
 *   xb_find_clear may cause long latency when the bit range to operate
 *   on is super large (e.g. [0, ULONG_MAX)).
 */

/**
 *  xb_set_bit - set a bit in the xbitmap
 *  @xb: the xbitmap tree used to record the bit
 *  @bit: index of the bit to set
 *
 * This function is used to set a bit in the xbitmap. If the bitmap that @bit
 * resides in is not there, the per-cpu ida_bitmap will be taken.
 *
 * Returns: 0 on success. -EAGAIN or -ENOMEM indicates that @bit is not set.
 */
int xb_set_bit(struct xb *xb, unsigned long bit)
{
	int err;
	unsigned long index = bit / IDA_BITMAP_BITS;
	struct radix_tree_root *root = &xb->xbrt;
	struct radix_tree_node *node;
	void __rcu **slot;
	struct ida_bitmap *bitmap;

	bit %= IDA_BITMAP_BITS;
	err = __radix_tree_create(root, index, 0, &node, &slot);
	if (err)
		return err;
	bitmap = rcu_dereference_raw(*slot);
	if (!bitmap) {
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
	void __rcu **slot;
	struct ida_bitmap *bitmap;

	bit %= IDA_BITMAP_BITS;
	bitmap = __radix_tree_lookup(root, index, &node, &slot);
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
 * @nbits: number of bits to clear
 *
 * This function is used to clear a range of bits in the xbitmap. If all the
 * bits in the bitmap are 0, the bitmap will be freed.
 */
void xb_clear_bit_range(struct xb *xb, unsigned long start,
			unsigned long nbits)
{
	struct radix_tree_root *root = &xb->xbrt;
	struct radix_tree_node *node;
	void __rcu **slot;
	struct ida_bitmap *bitmap;
	unsigned long index = start / IDA_BITMAP_BITS;
	unsigned long bit = start % IDA_BITMAP_BITS;

	if (nbits > ULONG_MAX - start)
		nbits = ULONG_MAX - start;

	while (nbits) {
		unsigned int __nbits = min(nbits,
					(unsigned long)IDA_BITMAP_BITS - bit);

		bitmap = __radix_tree_lookup(root, index, &node, &slot);
		if (bitmap) {
			if (__nbits != IDA_BITMAP_BITS)
				bitmap_clear(bitmap->bitmap, bit, __nbits);

			if (__nbits == IDA_BITMAP_BITS ||
			    bitmap_empty(bitmap->bitmap, IDA_BITMAP_BITS)) {
				kfree(bitmap);
				__radix_tree_delete(root, node, slot);
			}
		}
		bit = 0;
		index++;
		nbits -= __nbits;
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
	return test_bit(bit, bitmap->bitmap);
}
EXPORT_SYMBOL(xb_test_bit);

/**
 * xb_find_set - find the next set bit in a range of bits
 * @xb: the xbitmap to search from
 * @offset: the offset in the range to start searching
 * @size: the size of the range
 *
 * Returns: the found bit or, @size if no set bit is found.
 */
unsigned long xb_find_set(struct xb *xb, unsigned long size,
			  unsigned long offset)
{
	struct radix_tree_root *root = &xb->xbrt;
	struct radix_tree_node *node;
	void __rcu **slot;
	struct ida_bitmap *bitmap;
	unsigned long index = offset / IDA_BITMAP_BITS;
	unsigned long index_end = size / IDA_BITMAP_BITS;
	unsigned long bit = offset % IDA_BITMAP_BITS;

	if (unlikely(offset >= size))
		return size;

	while (index <= index_end) {
		unsigned long ret;
		unsigned int nbits = size - index * IDA_BITMAP_BITS;

		bitmap = __radix_tree_lookup(root, index, &node, &slot);
		if (!node) {
			index = (index | RADIX_TREE_MAP_MASK) + 1;
			continue;
		}

		if (bitmap) {
			if (nbits > IDA_BITMAP_BITS)
				nbits = IDA_BITMAP_BITS;

			ret = find_next_bit(bitmap->bitmap, nbits, bit);
			if (ret != nbits)
				return ret + index * IDA_BITMAP_BITS;
		}
		bit = 0;
		index++;
	}

	return size;
}
EXPORT_SYMBOL(xb_find_set);

/**
 * xb_find_zero - find the next zero bit in a range of bits
 * @xb: the xbitmap to search from
 * @offset: the offset in the range to start searching
 * @size: the size of the range
 *
 * Returns: the found bit or, @size if no zero bit is found.
 */
unsigned long xb_find_zero(struct xb *xb, unsigned long size,
			   unsigned long offset)
{
	struct radix_tree_root *root = &xb->xbrt;
	struct radix_tree_node *node;
	void __rcu **slot;
	struct ida_bitmap *bitmap;
	unsigned long index = offset / IDA_BITMAP_BITS;
	unsigned long index_end = size / IDA_BITMAP_BITS;
	unsigned long bit = offset % IDA_BITMAP_BITS;

	if (unlikely(offset >= size))
		return size;

	while (index <= index_end) {
		unsigned long ret;
		unsigned int nbits = size - index * IDA_BITMAP_BITS;

		bitmap = __radix_tree_lookup(root, index, &node, &slot);
		if (bitmap) {
			if (nbits > IDA_BITMAP_BITS)
				nbits = IDA_BITMAP_BITS;

			ret = find_next_zero_bit(bitmap->bitmap, nbits, bit);
			if (ret != nbits)
				return ret + index * IDA_BITMAP_BITS;
		} else {
			return bit + index * IDA_BITMAP_BITS;
		}
		bit = 0;
		index++;
	}

	return size;
}
EXPORT_SYMBOL(xb_find_zero);

#ifndef __KERNEL__

static DEFINE_XB(xb1);

void xbitmap_check_bit(unsigned long bit)
{
	xb_preload(GFP_KERNEL);
	assert(!xb_test_bit(&xb1, bit));
	assert(xb_set_bit(&xb1, bit) == 0);
	assert(xb_test_bit(&xb1, bit));
	assert(xb_clear_bit(&xb1, bit) == 0);
	assert(xb_empty(&xb1));
	assert(xb_clear_bit(&xb1, bit) == 0);
	assert(xb_empty(&xb1));
	xb_preload_end();
}

static void xbitmap_check_bit_range(void)
{
	/*
	 * Regular tests
	 * set bit 2000, 2001, 2040
	 * Next 1 in [0, 2048)		--> 2000
	 * Next 1 in [2000, 2002)	--> 2000
	 * Next 1 in [2002, 2041)	--> 2040
	 * Next 1 in [2002, 2040)	--> none
	 * Next 0 in [2000, 2048)	--> 2002
	 * Next 0 in [2048, 2060)	--> 2048
	 */
	xb_preload(GFP_KERNEL);
	assert(!xb_set_bit(&xb1, 2000));
	assert(!xb_set_bit(&xb1, 2001));
	assert(!xb_set_bit(&xb1, 2040));
	assert(xb_find_set(&xb1, 2048, 0) == 2000);
	assert(xb_find_set(&xb1, 2002, 2000) == 2000);
	assert(xb_find_set(&xb1, 2041, 2002) == 2040);
	assert(xb_find_set(&xb1, 2040, 2002) == 2040);
	assert(xb_find_zero(&xb1, 2048, 2000) == 2002);
	assert(xb_find_zero(&xb1, 2060, 2048) == 2048);
	xb_clear_bit_range(&xb1, 0, 2048);
	assert(xb_find_set(&xb1, 2048, 0) == 2048);
	xb_preload_end();

	/*
	 * Overflow tests:
	 * Set bit 1 and ULONG_MAX - 4
	 * Next 1 in [ULONG_MAX - 4, ULONG_MAX)		--> ULONG_MAX - 4
	 * Next 1 [ULONG_MAX - 3, ULONG_MAX + 4)	--> none
	 * Next 0 [ULONG_MAX - 4, ULONG_MAX + 4)	--> none
	 */
	xb_preload(GFP_KERNEL);
	assert(!xb_set_bit(&xb1, 1));
	xb_preload_end();
	xb_preload(GFP_KERNEL);
	assert(!xb_set_bit(&xb1, ULONG_MAX - 4));
	assert(xb_find_set(&xb1, ULONG_MAX, ULONG_MAX - 4) == ULONG_MAX - 4);
	assert(xb_find_set(&xb1, ULONG_MAX + 4, ULONG_MAX - 3) ==
	       ULONG_MAX + 4);
	assert(xb_find_zero(&xb1, ULONG_MAX + 4, ULONG_MAX - 4) ==
	       ULONG_MAX + 4);
	xb_clear_bit_range(&xb1, ULONG_MAX - 4, 4);
	assert(xb_find_set(&xb1, ULONG_MAX, ULONG_MAX - 10) == ULONG_MAX);
	xb_clear_bit_range(&xb1, 0, 2);
	assert(xb_find_set(&xb1, 2, 0) == 2);
	xb_preload_end();
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
