#include <linux/slab.h>
#include <linux/xbitmap.h>

/*
 * The xbitmap implementation supports up to ULONG_MAX bits, and it is
 * implemented based on ida bitmaps. So, given an unsigned long index,
 * the high order XB_INDEX_BITS bits of the index is used to find the
 * corresponding iteam (i.e. ida bitmap) from the radix tree, and the low
 * order (i.e. ilog2(IDA_BITMAP_BITS)) bits of the index are indexed into
 * the ida bitmap to find the bit.
 */
#define XB_INDEX_BITS		(BITS_PER_LONG - ilog2(IDA_BITMAP_BITS))
#define XB_MAX_PATH		(DIV_ROUND_UP(XB_INDEX_BITS, \
					      RADIX_TREE_MAP_SHIFT))
#define XB_PRELOAD_SIZE		(XB_MAX_PATH * 2 - 1)

enum xb_ops {
	XB_SET,
	XB_CLEAR,
	XB_TEST
};

static int xb_bit_ops(struct xb *xb, unsigned long bit, enum xb_ops ops)
{
	int ret = 0;
	unsigned long index = bit / IDA_BITMAP_BITS;
	struct radix_tree_root *root = &xb->xbrt;
	struct radix_tree_node *node;
	void **slot;
	struct ida_bitmap *bitmap;
	unsigned long ebit, tmp;

	bit %= IDA_BITMAP_BITS;
	ebit = bit + RADIX_TREE_EXCEPTIONAL_SHIFT;

	switch (ops) {
	case XB_SET:
		ret = __radix_tree_create(root, index, 0, &node, &slot);
		if (ret)
			return ret;
		bitmap = rcu_dereference_raw(*slot);
		if (radix_tree_exception(bitmap)) {
			tmp = (unsigned long)bitmap;
			if (ebit < BITS_PER_LONG) {
				tmp |= 1UL << ebit;
				rcu_assign_pointer(*slot, (void *)tmp);
				return 0;
			}
			bitmap = this_cpu_xchg(ida_bitmap, NULL);
			if (!bitmap)
				return -EAGAIN;
			memset(bitmap, 0, sizeof(*bitmap));
			bitmap->bitmap[0] =
					tmp >> RADIX_TREE_EXCEPTIONAL_SHIFT;
			rcu_assign_pointer(*slot, bitmap);
		}
		if (!bitmap) {
			if (ebit < BITS_PER_LONG) {
				bitmap = (void *)((1UL << ebit) |
					RADIX_TREE_EXCEPTIONAL_ENTRY);
				__radix_tree_replace(root, node, slot, bitmap,
						     NULL, NULL);
				return 0;
			}
			bitmap = this_cpu_xchg(ida_bitmap, NULL);
			if (!bitmap)
				return -EAGAIN;
			memset(bitmap, 0, sizeof(*bitmap));
			__radix_tree_replace(root, node, slot, bitmap, NULL,
					     NULL);
		}
		__set_bit(bit, bitmap->bitmap);
		break;
	case XB_CLEAR:
		bitmap = __radix_tree_lookup(root, index, &node, &slot);
		if (radix_tree_exception(bitmap)) {
			tmp = (unsigned long)bitmap;
			if (ebit >= BITS_PER_LONG)
				return 0;
			tmp &= ~(1UL << ebit);
			if (tmp == RADIX_TREE_EXCEPTIONAL_ENTRY)
				__radix_tree_delete(root, node, slot);
			else
				rcu_assign_pointer(*slot, (void *)tmp);
			return 0;
		}
		if (!bitmap)
			return 0;
		__clear_bit(bit, bitmap->bitmap);
		if (bitmap_empty(bitmap->bitmap, IDA_BITMAP_BITS)) {
			kfree(bitmap);
			__radix_tree_delete(root, node, slot);
		}
		break;
	case XB_TEST:
		bitmap = radix_tree_lookup(root, index);
		if (!bitmap)
			return 0;
		if (radix_tree_exception(bitmap)) {
			if (ebit > BITS_PER_LONG)
				return 0;
			return (unsigned long)bitmap & (1UL << bit);
		}
		ret = test_bit(bit, bitmap->bitmap);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

/**
 *  xb_set_bit - set a bit in the xbitmap
 *  @xb: the xbitmap tree used to record the bit
 *  @bit: index of the bit to set
 *
 * This function is used to set a bit in the xbitmap. If the bitmap that @bit
 * resides in is not there, it will be allocated.
 *
 * Returns: 0 on success. %-EAGAIN indicates that @bit was not set. The caller
 * may want to call the function again.
 */
int xb_set_bit(struct xb *xb, unsigned long bit)
{
	return xb_bit_ops(xb, bit, XB_SET);
}
EXPORT_SYMBOL(xb_set_bit);

/**
 * xb_clear_bit - clear a bit in the xbitmap
 * @xb: the xbitmap tree used to record the bit
 * @bit: index of the bit to set
 *
 * This function is used to clear a bit in the xbitmap. If all the bits of the
 * bitmap are 0, the bitmap will be freed.
 */
void xb_clear_bit(struct xb *xb, unsigned long bit)
{
	xb_bit_ops(xb, bit, XB_CLEAR);
}
EXPORT_SYMBOL(xb_clear_bit);

/**
 * xb_test_bit - test a bit in the xbitmap
 * @xb: the xbitmap tree used to record the bit
 * @bit: index of the bit to set
 *
 * This function is used to test a bit in the xbitmap.
 * Returns: 1 if the bit is set, or 0 otherwise.
 */
bool xb_test_bit(const struct xb *xb, unsigned long bit)
{
	return (bool)xb_bit_ops(xb, bit, XB_TEST);
}
EXPORT_SYMBOL(xb_test_bit);

/**
 *  xb_preload - preload for xb_set_bit()
 *  @gfp_mask: allocation mask to use for preloading
 *
 * Preallocate memory to use for the next call to xb_set_bit(). This function
 * returns with preemption disabled. It will be enabled by xb_preload_end().
 */
void xb_preload(gfp_t gfp)
{
	__radix_tree_preload(gfp, XB_PRELOAD_SIZE);
	if (!this_cpu_read(ida_bitmap)) {
		struct ida_bitmap *bitmap = kmalloc(sizeof(*bitmap), gfp);

		if (!bitmap)
			return;
		bitmap = this_cpu_cmpxchg(ida_bitmap, NULL, bitmap);
		kfree(bitmap);
	}
}
EXPORT_SYMBOL(xb_preload);

/**
 *  xb_zero - zero a range of bits in the xbitmap
 *  @xb: the xbitmap that the bits reside in
 *  @start: the start of the range, inclusive
 *  @end: the end of the range, inclusive
 */
void xb_zero(struct xb *xb, unsigned long start, unsigned long end)
{
	unsigned long i;

	for (i = start; i <= end; i++)
		xb_clear_bit(xb, i);
}
EXPORT_SYMBOL(xb_zero);

/**
 * xb_find_next_bit - find next 1 or 0 in the give range of bits
 * @xb: the xbitmap that the bits reside in
 * @start: the start of the range, inclusive
 * @end: the end of the range, inclusive
 * @set: the polarity (1 or 0) of the next bit to find
 *
 * Return the index of the found bit in the xbitmap. If the returned index
 * exceeds @end, it indicates that no such bit is found in the given range.
 */
unsigned long xb_find_next_bit(struct xb *xb, unsigned long start,
			       unsigned long end, bool set)
{
	unsigned long i;

	for (i = start; i <= end; i++) {
		if (xb_test_bit(xb, i) == set)
			break;
	}

	return i;
}
EXPORT_SYMBOL(xb_find_next_bit);
