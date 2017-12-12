#include <linux/export.h>
#include <linux/xbitmap.h>
#include <linux/bitmap.h>
#include <linux/slab.h>

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
}

int __weak main(void)
{
	radix_tree_init();
	xbitmap_checks();
}
#endif
