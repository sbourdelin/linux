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
tida_expand(struct tida *tida, gfp_t gfp, unsigned long *flags)
	__releases(tida->lock)
	__acquires(tida->lock)
{
	unsigned long newalloc, oldalloc = tida->alloc;
	unsigned long *bits;

	newalloc = oldalloc ? 2 * oldalloc : BITS_PER_LONG;

	spin_unlock_irqrestore(&tida->lock, *flags);
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
tida_get(struct tida *tida, gfp_t gfp)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&tida->lock, flags);
	while (1) {
		/* find_next_zero_bit is fine with a NULL bitmap as long as size is 0 */
		ret = find_next_zero_bit(tida->bits, tida->alloc, tida->hint);
		if (ret < tida->alloc)
			break;
		ret = tida_expand(tida, gfp, &flags);
		if (ret < 0)
			goto out;
	}

	__set_bit(ret, tida->bits);
	tida->hint = ret+1;
out:
	spin_unlock_irqrestore(&tida->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(tida_get);

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
