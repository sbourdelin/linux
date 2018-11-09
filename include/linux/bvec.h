/*
 * bvec iterator
 *
 * Copyright (C) 2001 Ming Lei <ming.lei@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public Licens
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-
 */
#ifndef __LINUX_BVEC_ITER_H
#define __LINUX_BVEC_ITER_H

#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/mm.h>

/*
 * What is multi-page bvecs?
 *
 * - bvecs stored in bio->bi_io_vec is always multi-page(mp) style
 *
 * - bvec(struct bio_vec) represents one physically contiguous I/O
 *   buffer, now the buffer may include more than one pages after
 *   multi-page(mp) bvec is supported, and all these pages represented
 *   by one bvec is physically contiguous. Before mp support, at most
 *   one page is included in one bvec, we call it single-page(sp)
 *   bvec.
 *
 * - .bv_page of the bvec represents the 1st page in the mp bvec
 *
 * - .bv_offset of the bvec represents offset of the buffer in the bvec
 *
 * The effect on the current drivers/filesystem/dm/bcache/...:
 *
 * - almost everyone supposes that one bvec only includes one single
 *   page, so we keep the sp interface not changed, for example,
 *   bio_for_each_segment() still returns bvec with single page
 *
 * - bio_for_each_segment*() will be changed to return single-page
 *   bvec too
 *
 * - during iterating, iterator variable(struct bvec_iter) is always
 *   updated in multipage bvec style and that means bvec_iter_advance()
 *   is kept not changed
 *
 * - returned(copied) single-page bvec is built in flight by bvec
 *   helpers from the stored multipage bvec
 *
 * - In case that some components(such as iov_iter) need to support
 *   multi-page bvec, we introduce new helpers(mp_bvec_iter_*) for
 *   them.
 */

/*
 * was unsigned short, but we might as well be ready for > 64kB I/O pages
 */
struct bio_vec {
	struct page	*bv_page;
	unsigned int	bv_len;
	unsigned int	bv_offset;
};

struct bvec_iter {
	sector_t		bi_sector;	/* device address in 512 byte
						   sectors */
	unsigned int		bi_size;	/* residual I/O count */

	unsigned int		bi_idx;		/* current index into bvl_vec */

	unsigned int            bi_bvec_done;	/* number of bytes completed in
						   current bvec */
};

struct bvec_iter_all {
	struct bio_vec	bv;
	int		idx;
	unsigned	done;
};

/*
 * various member access, note that bio_data should of course not be used
 * on highmem page vectors
 */
#define __bvec_iter_bvec(bvec, iter)	(&(bvec)[(iter).bi_idx])

#define mp_bvec_iter_page(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_page)

#define mp_bvec_iter_len(bvec, iter)				\
	min((iter).bi_size,					\
	    __bvec_iter_bvec((bvec), (iter))->bv_len - (iter).bi_bvec_done)

#define mp_bvec_iter_offset(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_offset + (iter).bi_bvec_done)

#define mp_bvec_iter_page_idx(bvec, iter)			\
	(mp_bvec_iter_offset((bvec), (iter)) / PAGE_SIZE)

/*
 * <page, offset,length> of single-page(sp) segment.
 *
 * This helpers are for building sp bvec in flight.
 */
#define bvec_iter_offset(bvec, iter)					\
	(mp_bvec_iter_offset((bvec), (iter)) % PAGE_SIZE)

#define bvec_iter_len(bvec, iter)					\
	min_t(unsigned, mp_bvec_iter_len((bvec), (iter)),		\
	    (PAGE_SIZE - (bvec_iter_offset((bvec), (iter)))))

#define bvec_iter_page(bvec, iter)					\
	nth_page(mp_bvec_iter_page((bvec), (iter)),		\
		 mp_bvec_iter_page_idx((bvec), (iter)))

#define bvec_iter_bvec(bvec, iter)				\
((struct bio_vec) {						\
	.bv_page	= bvec_iter_page((bvec), (iter)),	\
	.bv_len		= bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= bvec_iter_offset((bvec), (iter)),	\
})

#define mp_bvec_iter_bvec(bvec, iter)				\
((struct bio_vec) {							\
	.bv_page	= mp_bvec_iter_page((bvec), (iter)),	\
	.bv_len		= mp_bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= mp_bvec_iter_offset((bvec), (iter)),	\
})

static inline bool __bvec_iter_advance(const struct bio_vec *bv,
				       struct bvec_iter *iter,
				       unsigned bytes, bool mp)
{
	if (WARN_ONCE(bytes > iter->bi_size,
		     "Attempted to advance past end of bvec iter\n")) {
		iter->bi_size = 0;
		return false;
	}

	while (bytes) {
		unsigned len;

		if (mp)
			len = mp_bvec_iter_len(bv, *iter);
		else
			len = bvec_iter_len(bv, *iter);

		len = min(bytes, len);

		bytes -= len;
		iter->bi_size -= len;
		iter->bi_bvec_done += len;

		if (iter->bi_bvec_done == __bvec_iter_bvec(bv, *iter)->bv_len) {
			iter->bi_bvec_done = 0;
			iter->bi_idx++;
		}
	}
	return true;
}

static inline bool bvec_iter_rewind(const struct bio_vec *bv,
				     struct bvec_iter *iter,
				     unsigned int bytes)
{
	while (bytes) {
		unsigned len = min(bytes, iter->bi_bvec_done);

		if (iter->bi_bvec_done == 0) {
			if (WARN_ONCE(iter->bi_idx == 0,
				      "Attempted to rewind iter beyond "
				      "bvec's boundaries\n")) {
				return false;
			}
			iter->bi_idx--;
			iter->bi_bvec_done = __bvec_iter_bvec(bv, *iter)->bv_len;
			continue;
		}
		bytes -= len;
		iter->bi_size += len;
		iter->bi_bvec_done -= len;
	}
	return true;
}

static inline bool bvec_iter_advance(const struct bio_vec *bv,
				     struct bvec_iter *iter,
				     unsigned bytes)
{
	return __bvec_iter_advance(bv, iter, bytes, false);
}

static inline bool mp_bvec_iter_advance(const struct bio_vec *bv,
					struct bvec_iter *iter,
					unsigned bytes)
{
	return __bvec_iter_advance(bv, iter, bytes, true);
}

#define for_each_bvec(bvl, bio_vec, iter, start)			\
	for (iter = (start);						\
	     (iter).bi_size &&						\
		((bvl = bvec_iter_bvec((bio_vec), (iter))), 1);	\
	     bvec_iter_advance((bio_vec), &(iter), (bvl).bv_len))

/* for iterating one bio from start to end */
#define BVEC_ITER_ALL_INIT (struct bvec_iter)				\
{									\
	.bi_sector	= 0,						\
	.bi_size	= UINT_MAX,					\
	.bi_idx		= 0,						\
	.bi_bvec_done	= 0,						\
}

static inline struct bio_vec *bvec_init_iter_all(struct bvec_iter_all *iter_all)
{
	iter_all->bv.bv_page = NULL;
	iter_all->done = 0;

	return &iter_all->bv;
}

/* used for chunk_for_each_segment */
static inline void bvec_next_segment(const struct bio_vec *bvec,
		struct bvec_iter_all *iter_all)
{
	struct bio_vec *bv = &iter_all->bv;

	if (bv->bv_page) {
		bv->bv_page += 1;
		bv->bv_offset = 0;
	} else {
		bv->bv_page = bvec->bv_page;
		bv->bv_offset = bvec->bv_offset;
	}
	bv->bv_len = min_t(unsigned int, PAGE_SIZE - bv->bv_offset,
			bvec->bv_len - iter_all->done);
}

/*
 * Get the last singlepage segment from the multipage bvec and store it
 * in @seg
 */
static inline void bvec_last_segment(const struct bio_vec *bvec,
		struct bio_vec *seg)
{
	unsigned total = bvec->bv_offset + bvec->bv_len;
	unsigned last_page = total / PAGE_SIZE;

	if (last_page * PAGE_SIZE == total)
		last_page--;

	seg->bv_page = nth_page(bvec->bv_page, last_page);

	/* the whole segment is inside the last page */
	if (bvec->bv_offset >= last_page * PAGE_SIZE) {
		seg->bv_offset = bvec->bv_offset % PAGE_SIZE;
		seg->bv_len = bvec->bv_len;
	} else {
		seg->bv_offset = 0;
		seg->bv_len = total - last_page * PAGE_SIZE;
	}
}

#endif /* __LINUX_BVEC_ITER_H */
