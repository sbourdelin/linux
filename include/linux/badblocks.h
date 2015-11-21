#ifndef _LINUX_BADBLOCKS_H
#define _LINUX_BADBLOCKS_H

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/kernel.h>

#define BB_LEN_MASK	(0x00000000000001FFULL)
#define BB_OFFSET_MASK	(0x7FFFFFFFFFFFFE00ULL)
#define BB_ACK_MASK	(0x8000000000000000ULL)
#define BB_MAX_LEN	512
#define BB_OFFSET(x)	(((x) & BB_OFFSET_MASK) >> 9)
#define BB_LEN(x)	(((x) & BB_LEN_MASK) + 1)
#define BB_ACK(x)	(!!((x) & BB_ACK_MASK))
#define BB_MAKE(a, l, ack) (((a)<<9) | ((l)-1) | ((u64)(!!(ack)) << 63))

/* Bad block numbers are stored sorted in a single page.
 * 64bits is used for each block or extent.
 * 54 bits are sector number, 9 bits are extent size,
 * 1 bit is an 'acknowledged' flag.
 */
#define MAX_BADBLOCKS	(PAGE_SIZE/8)

struct badblocks {
	int count;		/* count of bad blocks */
	int unacked_exist;	/* there probably are unacknowledged
				 * bad blocks.  This is only cleared
				 * when a read discovers none
				 */
	int shift;		/* shift from sectors to block size
				 * a -ve shift means badblocks are
				 * disabled.*/
	u64 *page;		/* badblock list */
	int changed;
	seqlock_t lock;
	sector_t sector;
	sector_t size;		/* in sectors */
};

/* Bad block management.
 * We can record which blocks on each device are 'bad' and so just
 * fail those blocks, or that stripe, rather than the whole device.
 * Entries in the bad-block table are 64bits wide.  This comprises:
 * Length of bad-range, in sectors: 0-511 for lengths 1-512
 * Start of bad-range, sector offset, 54 bits (allows 8 exbibytes)
 *  A 'shift' can be set so that larger blocks are tracked and
 *  consequently larger devices can be covered.
 * 'Acknowledged' flag - 1 bit. - the most significant bit.
 *
 * Locking of the bad-block table uses a seqlock so badblocks_check
 * might need to retry if it is very unlucky.
 * We will sometimes want to check for bad blocks in a bi_end_io function,
 * so we use the write_seqlock_irq variant.
 *
 * When looking for a bad block we specify a range and want to
 * know if any block in the range is bad.  So we binary-search
 * to the last range that starts at-or-before the given endpoint,
 * (or "before the sector after the target range")
 * then see if it ends after the given start.
 * We return
 *  0 if there are no known bad blocks in the range
 *  1 if there are known bad block which are all acknowledged
 * -1 if there are bad blocks which have not yet been acknowledged in metadata.
 * plus the start/length of the first bad section we overlap.
 */
static inline int badblocks_check(struct badblocks *bb, sector_t s, int sectors,
		   sector_t *first_bad, int *bad_sectors)
{
	int hi;
	int lo;
	u64 *p = bb->page;
	int rv;
	sector_t target = s + sectors;
	unsigned seq;

	if (bb->shift > 0) {
		/* round the start down, and the end up */
		s >>= bb->shift;
		target += (1<<bb->shift) - 1;
		target >>= bb->shift;
		sectors = target - s;
	}
	/* 'target' is now the first block after the bad range */

retry:
	seq = read_seqbegin(&bb->lock);
	lo = 0;
	rv = 0;
	hi = bb->count;

	/* Binary search between lo and hi for 'target'
	 * i.e. for the last range that starts before 'target'
	 */
	/* INVARIANT: ranges before 'lo' and at-or-after 'hi'
	 * are known not to be the last range before target.
	 * VARIANT: hi-lo is the number of possible
	 * ranges, and decreases until it reaches 1
	 */
	while (hi - lo > 1) {
		int mid = (lo + hi) / 2;
		sector_t a = BB_OFFSET(p[mid]);
		if (a < target)
			/* This could still be the one, earlier ranges
			 * could not. */
			lo = mid;
		else
			/* This and later ranges are definitely out. */
			hi = mid;
	}
	/* 'lo' might be the last that started before target, but 'hi' isn't */
	if (hi > lo) {
		/* need to check all range that end after 's' to see if
		 * any are unacknowledged.
		 */
		while (lo >= 0 &&
		       BB_OFFSET(p[lo]) + BB_LEN(p[lo]) > s) {
			if (BB_OFFSET(p[lo]) < target) {
				/* starts before the end, and finishes after
				 * the start, so they must overlap
				 */
				if (rv != -1 && BB_ACK(p[lo]))
					rv = 1;
				else
					rv = -1;
				*first_bad = BB_OFFSET(p[lo]);
				*bad_sectors = BB_LEN(p[lo]);
			}
			lo--;
		}
	}

	if (read_seqretry(&bb->lock, seq))
		goto retry;

	return rv;
}

/*
 * Add a range of bad blocks to the table.
 * This might extend the table, or might contract it
 * if two adjacent ranges can be merged.
 * We binary-search to find the 'insertion' point, then
 * decide how best to handle it.
 */
static inline int badblocks_set(struct badblocks *bb, sector_t s, int sectors,
			    int acknowledged)
{
	u64 *p;
	int lo, hi;
	int rv = 1;
	unsigned long flags;

	if (bb->shift < 0)
		/* badblocks are disabled */
		return 0;

	if (bb->shift) {
		/* round the start down, and the end up */
		sector_t next = s + sectors;
		s >>= bb->shift;
		next += (1<<bb->shift) - 1;
		next >>= bb->shift;
		sectors = next - s;
	}

	write_seqlock_irqsave(&bb->lock, flags);

	p = bb->page;
	lo = 0;
	hi = bb->count;
	/* Find the last range that starts at-or-before 's' */
	while (hi - lo > 1) {
		int mid = (lo + hi) / 2;
		sector_t a = BB_OFFSET(p[mid]);
		if (a <= s)
			lo = mid;
		else
			hi = mid;
	}
	if (hi > lo && BB_OFFSET(p[lo]) > s)
		hi = lo;

	if (hi > lo) {
		/* we found a range that might merge with the start
		 * of our new range
		 */
		sector_t a = BB_OFFSET(p[lo]);
		sector_t e = a + BB_LEN(p[lo]);
		int ack = BB_ACK(p[lo]);
		if (e >= s) {
			/* Yes, we can merge with a previous range */
			if (s == a && s + sectors >= e)
				/* new range covers old */
				ack = acknowledged;
			else
				ack = ack && acknowledged;

			if (e < s + sectors)
				e = s + sectors;
			if (e - a <= BB_MAX_LEN) {
				p[lo] = BB_MAKE(a, e-a, ack);
				s = e;
			} else {
				/* does not all fit in one range,
				 * make p[lo] maximal
				 */
				if (BB_LEN(p[lo]) != BB_MAX_LEN)
					p[lo] = BB_MAKE(a, BB_MAX_LEN, ack);
				s = a + BB_MAX_LEN;
			}
			sectors = e - s;
		}
	}
	if (sectors && hi < bb->count) {
		/* 'hi' points to the first range that starts after 's'.
		 * Maybe we can merge with the start of that range */
		sector_t a = BB_OFFSET(p[hi]);
		sector_t e = a + BB_LEN(p[hi]);
		int ack = BB_ACK(p[hi]);
		if (a <= s + sectors) {
			/* merging is possible */
			if (e <= s + sectors) {
				/* full overlap */
				e = s + sectors;
				ack = acknowledged;
			} else
				ack = ack && acknowledged;

			a = s;
			if (e - a <= BB_MAX_LEN) {
				p[hi] = BB_MAKE(a, e-a, ack);
				s = e;
			} else {
				p[hi] = BB_MAKE(a, BB_MAX_LEN, ack);
				s = a + BB_MAX_LEN;
			}
			sectors = e - s;
			lo = hi;
			hi++;
		}
	}
	if (sectors == 0 && hi < bb->count) {
		/* we might be able to combine lo and hi */
		/* Note: 's' is at the end of 'lo' */
		sector_t a = BB_OFFSET(p[hi]);
		int lolen = BB_LEN(p[lo]);
		int hilen = BB_LEN(p[hi]);
		int newlen = lolen + hilen - (s - a);
		if (s >= a && newlen < BB_MAX_LEN) {
			/* yes, we can combine them */
			int ack = BB_ACK(p[lo]) && BB_ACK(p[hi]);
			p[lo] = BB_MAKE(BB_OFFSET(p[lo]), newlen, ack);
			memmove(p + hi, p + hi + 1,
				(bb->count - hi - 1) * 8);
			bb->count--;
		}
	}
	while (sectors) {
		/* didn't merge (it all).
		 * Need to add a range just before 'hi' */
		if (bb->count >= MAX_BADBLOCKS) {
			/* No room for more */
			rv = 0;
			break;
		} else {
			int this_sectors = sectors;
			memmove(p + hi + 1, p + hi,
				(bb->count - hi) * 8);
			bb->count++;

			if (this_sectors > BB_MAX_LEN)
				this_sectors = BB_MAX_LEN;
			p[hi] = BB_MAKE(s, this_sectors, acknowledged);
			sectors -= this_sectors;
			s += this_sectors;
		}
	}

	bb->changed = 1;
	if (!acknowledged)
		bb->unacked_exist = 1;
	write_sequnlock_irqrestore(&bb->lock, flags);

	return rv;
}

/*
 * Remove a range of bad blocks from the table.
 * This may involve extending the table if we spilt a region,
 * but it must not fail.  So if the table becomes full, we just
 * drop the remove request.
 */
static inline int badblocks_clear(struct badblocks *bb, sector_t s, int sectors)
{
	u64 *p;
	int lo, hi;
	sector_t target = s + sectors;
	int rv = 0;

	if (bb->shift > 0) {
		/* When clearing we round the start up and the end down.
		 * This should not matter as the shift should align with
		 * the block size and no rounding should ever be needed.
		 * However it is better the think a block is bad when it
		 * isn't than to think a block is not bad when it is.
		 */
		s += (1<<bb->shift) - 1;
		s >>= bb->shift;
		target >>= bb->shift;
		sectors = target - s;
	}

	write_seqlock_irq(&bb->lock);

	p = bb->page;
	lo = 0;
	hi = bb->count;
	/* Find the last range that starts before 'target' */
	while (hi - lo > 1) {
		int mid = (lo + hi) / 2;
		sector_t a = BB_OFFSET(p[mid]);
		if (a < target)
			lo = mid;
		else
			hi = mid;
	}
	if (hi > lo) {
		/* p[lo] is the last range that could overlap the
		 * current range.  Earlier ranges could also overlap,
		 * but only this one can overlap the end of the range.
		 */
		if (BB_OFFSET(p[lo]) + BB_LEN(p[lo]) > target) {
			/* Partial overlap, leave the tail of this range */
			int ack = BB_ACK(p[lo]);
			sector_t a = BB_OFFSET(p[lo]);
			sector_t end = a + BB_LEN(p[lo]);

			if (a < s) {
				/* we need to split this range */
				if (bb->count >= MAX_BADBLOCKS) {
					rv = -ENOSPC;
					goto out;
				}
				memmove(p+lo+1, p+lo, (bb->count - lo) * 8);
				bb->count++;
				p[lo] = BB_MAKE(a, s-a, ack);
				lo++;
			}
			p[lo] = BB_MAKE(target, end - target, ack);
			/* there is no longer an overlap */
			hi = lo;
			lo--;
		}
		while (lo >= 0 &&
		       BB_OFFSET(p[lo]) + BB_LEN(p[lo]) > s) {
			/* This range does overlap */
			if (BB_OFFSET(p[lo]) < s) {
				/* Keep the early parts of this range. */
				int ack = BB_ACK(p[lo]);
				sector_t start = BB_OFFSET(p[lo]);
				p[lo] = BB_MAKE(start, s - start, ack);
				/* now low doesn't overlap, so.. */
				break;
			}
			lo--;
		}
		/* 'lo' is strictly before, 'hi' is strictly after,
		 * anything between needs to be discarded
		 */
		if (hi - lo > 1) {
			memmove(p+lo+1, p+hi, (bb->count - hi) * 8);
			bb->count -= (hi - lo - 1);
		}
	}

	bb->changed = 1;
out:
	write_sequnlock_irq(&bb->lock);
	return rv;
}

/*
 * Acknowledge all bad blocks in a list.
 * This only succeeds if ->changed is clear.  It is used by
 * in-kernel metadata updates
 */
static inline void ack_all_badblocks(struct badblocks *bb)
{
	if (bb->page == NULL || bb->changed)
		/* no point even trying */
		return;
	write_seqlock_irq(&bb->lock);

	if (bb->changed == 0 && bb->unacked_exist) {
		u64 *p = bb->page;
		int i;
		for (i = 0; i < bb->count ; i++) {
			if (!BB_ACK(p[i])) {
				sector_t start = BB_OFFSET(p[i]);
				int len = BB_LEN(p[i]);
				p[i] = BB_MAKE(start, len, 1);
			}
		}
		bb->unacked_exist = 0;
	}
	write_sequnlock_irq(&bb->lock);
}

/* sysfs access to bad-blocks list. */
static inline ssize_t badblocks_show(struct badblocks *bb, char *page,
		int unack)
{
	size_t len;
	int i;
	u64 *p = bb->page;
	unsigned seq;

	if (bb->shift < 0)
		return 0;

retry:
	seq = read_seqbegin(&bb->lock);

	len = 0;
	i = 0;

	while (len < PAGE_SIZE && i < bb->count) {
		sector_t s = BB_OFFSET(p[i]);
		unsigned int length = BB_LEN(p[i]);
		int ack = BB_ACK(p[i]);
		i++;

		if (unack && ack)
			continue;

		len += snprintf(page+len, PAGE_SIZE-len, "%llu %u\n",
				(unsigned long long)s << bb->shift,
				length << bb->shift);
	}
	if (unack && len == 0)
		bb->unacked_exist = 0;

	if (read_seqretry(&bb->lock, seq))
		goto retry;

	return len;
}

#define DO_DEBUG 1

static inline ssize_t badblocks_store(struct badblocks *bb, const char *page,
		size_t len, int unack)
{
	unsigned long long sector;
	int length;
	char newline;
#ifdef DO_DEBUG
	/* Allow clearing via sysfs *only* for testing/debugging.
	 * Normally only a successful write may clear a badblock
	 */
	int clear = 0;
	if (page[0] == '-') {
		clear = 1;
		page++;
	}
#endif /* DO_DEBUG */

	switch (sscanf(page, "%llu %d%c", &sector, &length, &newline)) {
	case 3:
		if (newline != '\n')
			return -EINVAL;
	case 2:
		if (length <= 0)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

#ifdef DO_DEBUG
	if (clear) {
		badblocks_clear(bb, sector, length);
		return len;
	}
#endif /* DO_DEBUG */
	if (badblocks_set(bb, sector, length, !unack))
		return len;
	else
		return -ENOSPC;
}

static inline int badblocks_init(struct badblocks *bb, int enable)
{
	bb->count = 0;
	if (enable)
		bb->shift = 0;
	else
		bb->shift = -1;
	bb->page = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (bb->page == NULL)
		return -ENOMEM;
	seqlock_init(&bb->lock);

	return 0;
}

static inline void badblocks_free(struct badblocks *bb)
{
	kfree(bb->page);
}

#endif
