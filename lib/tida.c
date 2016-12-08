#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/tida.h>

/*
 * An extremely simple integer id allocator with small memory
 * footprint, mostly useful for cases where up to a few hundred ids
 * get allocated.
 *
 * Note that the backing bitmap is never shrunk.
 */

/*
 * Invariant:
 *
 *   0 <= tida->hint
 *     <= find_next_zero_bit(tida->bits, tida->alloc, 0)
 *     <= tida->alloc.
 */

static int
tida_expand(struct tida *tida, gfp_t gfp, unsigned long *flags, unsigned long minalloc)
	__releases(tida->lock)
	__acquires(tida->lock)
{
	unsigned long newalloc, oldalloc = tida->alloc;
	unsigned long *bits;

	spin_unlock_irqrestore(&tida->lock, *flags);
	newalloc = max(2*oldalloc, round_up(minalloc, BITS_PER_LONG));
	bits = kcalloc(BITS_TO_LONGS(newalloc), sizeof(*bits), gfp);
	spin_lock_irqsave(&tida->lock, *flags);

	if (!bits)
		return -ENOMEM;

	if (tida->alloc < newalloc) {
		memcpy(bits, tida->bits, tida->alloc/8);
		tida->alloc = newalloc;
		swap(tida->bits, bits);
	}
	kfree(bits);

	return 0;
}

int
tida_get_above(struct tida *tida, int start, gfp_t gfp)
{
	unsigned long flags;
	int ret, from;

	if (WARN_ON_ONCE(start < 0))
		return -EINVAL;

	spin_lock_irqsave(&tida->lock, flags);
	while (1) {
		/* find_next_zero_bit is fine with a NULL bitmap as long as size is 0 */
		from = max(start, tida->hint);
		ret = find_next_zero_bit(tida->bits, tida->alloc, from);
		if (ret < tida->alloc)
			break;
		ret = tida_expand(tida, gfp, &flags, from + 1);
		if (ret < 0)
			goto out;
	}

	__set_bit(ret, tida->bits);
	if (start <= tida->hint)
		tida->hint = ret + 1;
out:
	spin_unlock_irqrestore(&tida->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(tida_get_above);

void
tida_put(struct tida *tida, int id)
{
	unsigned long flags;

	spin_lock_irqsave(&tida->lock, flags);
	__clear_bit(id, tida->bits);
	if (id < tida->hint)
		tida->hint = id;
	spin_unlock_irqrestore(&tida->lock, flags);
}
EXPORT_SYMBOL_GPL(tida_put);

void
tida_init(struct tida *tida)
{
	memset(tida, 0, sizeof(*tida));
	spin_lock_init(&tida->lock);
}
EXPORT_SYMBOL_GPL(tida_init);

void
tida_destroy(struct tida *tida)
{
	kfree(tida->bits);
}
EXPORT_SYMBOL_GPL(tida_destroy);
