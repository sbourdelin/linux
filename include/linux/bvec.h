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
 * What is multipage bvecs(segment)?
 *
 * - bvec stored in bio->bi_io_vec is always multipage(mp) style
 *
 * - bvec(struct bio_vec) represents one physically contiguous I/O
 *   buffer, now the buffer may include more than one pages since
 *   multipage(mp) bvec is supported, and all these pages represented
 *   by one bvec is physically contiguous. Before mp support, at most
 *   one page can be included in one bvec, we call it singlepage(sp)
 *   bvec.
 *
 * - .bv_page of th bvec represents the 1st page in the mp segment
 *
 * - .bv_offset of the bvec represents offset of the buffer in the bvec
 *
 * The effect on the current drivers/filesystem/dm/bcache/...:
 *
 * - almost everyone supposes that one bvec only includes one single
 *   page, so we keep the sp interface not changed, for example,
 *   bio_for_each_page() still returns bvec with single page
 *
 * - bio_for_each_page_all() will be changed to return singlepage
 *   bvec too
 *
 * - during iterating, iterator variable(struct bvec_iter) is always
 *   updated in multipage bvec style and that means bvec_iter_advance()
 *   is kept not changed
 *
 * - returned(copied) singlepage bvec is generated in flight by bvec
 *   helpers from the stored multipage bvec(segment)
 *
 * - In case that some components(such as iov_iter) need to support
 *   multipage segment, we introduce new helpers(bvec_iter_segment_*) for
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

	unsigned int            bi_done;	/* number of bytes completed */

	unsigned int            bi_bvec_done;	/* number of bytes completed in
						   current bvec */
};

/*
 * various member access, note that bio_data should of course not be used
 * on highmem page vectors
 */
#define __bvec_iter_bvec(bvec, iter)	(&(bvec)[(iter).bi_idx])

#define bvec_iter_segment_page(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_page)

#define bvec_iter_segment_len(bvec, iter)				\
	min((iter).bi_size,					\
	    __bvec_iter_bvec((bvec), (iter))->bv_len - (iter).bi_bvec_done)

#define bvec_iter_segment_offset(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_offset + (iter).bi_bvec_done)

#define bvec_iter_page_idx_in_seg(bvec, iter)			\
	(bvec_iter_segment_offset((bvec), (iter)) / PAGE_SIZE)

/*
 * <page, offset,length> of singlepage(sp) segment.
 *
 * This helpers will be implemented for building sp bvec in flight.
 */
#define bvec_iter_offset(bvec, iter)					\
	(bvec_iter_segment_offset((bvec), (iter)) % PAGE_SIZE)

#define bvec_iter_len(bvec, iter)					\
	min_t(unsigned, bvec_iter_segment_len((bvec), (iter)),		\
	    (PAGE_SIZE - (bvec_iter_offset((bvec), (iter)))))

#define bvec_iter_page(bvec, iter)					\
	nth_page(bvec_iter_segment_page((bvec), (iter)),		\
		 bvec_iter_page_idx_in_seg((bvec), (iter)))

#define bvec_iter_bvec(bvec, iter)				\
((struct bio_vec) {						\
	.bv_page	= bvec_iter_page((bvec), (iter)),	\
	.bv_len		= bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= bvec_iter_offset((bvec), (iter)),	\
})

static inline bool bvec_iter_advance(const struct bio_vec *bv,
		struct bvec_iter *iter, unsigned bytes)
{
	if (WARN_ONCE(bytes > iter->bi_size,
		     "Attempted to advance past end of bvec iter\n")) {
		iter->bi_size = 0;
		return false;
	}

	while (bytes) {
		unsigned iter_len = bvec_iter_len(bv, *iter);
		unsigned len = min(bytes, iter_len);

		bytes -= len;
		iter->bi_size -= len;
		iter->bi_bvec_done += len;
		iter->bi_done += len;

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

#endif /* __LINUX_BVEC_ITER_H */
