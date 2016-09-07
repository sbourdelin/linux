/*
 * Copyright (C) 2016 Facebook
 * Copyright (C) 2013-2014 Jens Axboe
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <linux/scale_bitmap.h>

int scale_bitmap_init_node(struct scale_bitmap *bitmap, unsigned int depth,
			   int shift, gfp_t flags, int node)
{
	unsigned int bits_per_word;
	unsigned int i;

	if (shift < 0) {
		shift = ilog2(BITS_PER_LONG);
		/*
		 * If the bitmap is small, shrink the number of bits per word so
		 * we spread over a few cachelines, at least. If less than 4
		 * bits, just forget about it, it's not going to work optimally
		 * anyway.
		 */
		if (depth >= 4) {
			while ((4U << shift) > depth)
				shift--;
		}
	}
	bits_per_word = 1U << shift;
	if (bits_per_word > BITS_PER_LONG)
		return -EINVAL;

	bitmap->shift = shift;
	bitmap->depth = depth;
	bitmap->map_nr = DIV_ROUND_UP(bitmap->depth, bits_per_word);

	if (depth == 0) {
		bitmap->map = NULL;
		return 0;
	}

	bitmap->map = kzalloc_node(bitmap->map_nr * sizeof(*bitmap->map), flags,
				   node);
	if (!bitmap->map)
		return -ENOMEM;

	for (i = 0; i < bitmap->map_nr; i++) {
		bitmap->map[i].depth = min(depth, bits_per_word);
		depth -= bitmap->map[i].depth;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(scale_bitmap_init_node);

void scale_bitmap_resize(struct scale_bitmap *bitmap, unsigned int depth)
{
	unsigned int bits_per_word = 1U << bitmap->shift;
	unsigned int i;

	bitmap->depth = depth;
	bitmap->map_nr = DIV_ROUND_UP(bitmap->depth, bits_per_word);

	for (i = 0; i < bitmap->map_nr; i++) {
		bitmap->map[i].depth = min(depth, bits_per_word);
		depth -= bitmap->map[i].depth;
	}
}
EXPORT_SYMBOL_GPL(scale_bitmap_resize);

static int __scale_bitmap_get_word(struct scale_bitmap_word *word,
				   unsigned int hint, bool wrap)
{
	unsigned int orig_hint = hint;
	int nr;

	while (1) {
		nr = find_next_zero_bit(&word->word, word->depth, hint);
		if (unlikely(nr >= word->depth)) {
			/*
			 * We started with an offset, and we didn't reset the
			 * offset to 0 in a failure case, so start from 0 to
			 * exhaust the map.
			 */
			if (orig_hint && hint && wrap) {
				hint = orig_hint = 0;
				continue;
			}
			return -1;
		}

		if (!test_and_set_bit(nr, &word->word))
			break;

		hint = nr + 1;
		if (hint >= word->depth - 1)
			hint = 0;
	}

	return nr;
}

int scale_bitmap_get(struct scale_bitmap *bitmap, unsigned int *alloc_hint,
		     bool round_robin)
{
	unsigned int hint, orig_hint;
	unsigned int i, index;
	int nr;

	hint = orig_hint = *alloc_hint;
	index = SB_NR_TO_INDEX(bitmap, hint);

	for (i = 0; i < bitmap->map_nr; i++) {
		nr = __scale_bitmap_get_word(&bitmap->map[index],
					     SB_NR_TO_BIT(bitmap, hint),
					     !round_robin);
		if (nr != -1) {
			nr += index << bitmap->shift;
			goto done;
		}

		/* Jump to next index. */
		index++;
		hint = index << bitmap->shift;

		if (index >= bitmap->map_nr) {
			index = 0;
			hint = 0;
		}
	}

	*alloc_hint = 0;
	return -1;

done:
	/* Only update the cache if we used the cached value. */
	if (nr == orig_hint || unlikely(round_robin)) {
		hint = nr + 1;
		if (hint >= bitmap->depth - 1)
			hint = 0;
		*alloc_hint = hint;
	}

	return nr;
}
EXPORT_SYMBOL_GPL(scale_bitmap_get);

bool scale_bitmap_any_bit_set(const struct scale_bitmap *bitmap)
{
	unsigned int i;

	for (i = 0; i < bitmap->map_nr; i++) {
		if (bitmap->map[i].word)
			return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(scale_bitmap_any_bit_set);

bool scale_bitmap_any_bit_clear(const struct scale_bitmap *bitmap)
{
	unsigned int i;

	for (i = 0; i < bitmap->map_nr; i++) {
		const struct scale_bitmap_word *word = &bitmap->map[i];
		unsigned long ret;

		ret = find_first_zero_bit(&word->word, word->depth);
		if (ret < word->depth)
			return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(scale_bitmap_any_bit_clear);

unsigned int scale_bitmap_weight(const struct scale_bitmap *bitmap)
{
	unsigned int i, weight;

	for (i = 0; i < bitmap->map_nr; i++) {
		const struct scale_bitmap_word *word = &bitmap->map[i];

		weight += bitmap_weight(&word->word, word->depth);
	}
	return weight;
}
EXPORT_SYMBOL_GPL(scale_bitmap_weight);

int scale_bitmap_queue_init_node(struct scale_bitmap_queue *sbq,
				 unsigned int depth, int shift, gfp_t flags,
				 int node)
{
	int ret;
	int i;

	ret = scale_bitmap_init_node(&sbq->map, depth, shift, flags, node);
	if (ret)
		return ret;

	sbq->wake_batch = SBQ_WAKE_BATCH;
	if (sbq->wake_batch > depth / SBQ_WAIT_QUEUES)
		sbq->wake_batch = max(1U, depth / SBQ_WAIT_QUEUES);

	atomic_set(&sbq->wake_index, 0);

	sbq->ws = kzalloc(SBQ_WAIT_QUEUES * sizeof(*sbq->ws), flags);
	if (!sbq->ws) {
		scale_bitmap_free(&sbq->map);
		return -ENOMEM;
	}

	for (i = 0; i < SBQ_WAIT_QUEUES; i++) {
		init_waitqueue_head(&sbq->ws[i].wait);
		atomic_set(&sbq->ws[i].wait_cnt, sbq->wake_batch);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(scale_bitmap_queue_init_node);

void scale_bitmap_queue_resize(struct scale_bitmap_queue *sbq,
			       unsigned int depth)
{
	scale_bitmap_resize(&sbq->map, depth);

	sbq->wake_batch = SBQ_WAKE_BATCH;
	if (sbq->wake_batch > depth / SBQ_WAIT_QUEUES)
		sbq->wake_batch = max(1U, depth / SBQ_WAIT_QUEUES);
}
EXPORT_SYMBOL_GPL(scale_bitmap_queue_resize);

static struct sbq_wait_state *sbq_wake_ptr(struct scale_bitmap_queue *sbq)
{
	int i, wake_index;

	wake_index = atomic_read(&sbq->wake_index);
	for (i = 0; i < SBQ_WAIT_QUEUES; i++) {
		struct sbq_wait_state *ws = &sbq->ws[wake_index];

		if (waitqueue_active(&ws->wait)) {
			int o = atomic_read(&sbq->wake_index);

			if (wake_index != o)
				atomic_cmpxchg(&sbq->wake_index, o, wake_index);
			return ws;
		}

		wake_index = sbq_index_inc(wake_index);
	}

	return NULL;
}

void scale_bitmap_queue_clear(struct scale_bitmap_queue *sbq, unsigned int nr)
{
	struct sbq_wait_state *ws;
	int wait_cnt;

	scale_bitmap_clear_bit(&sbq->map, nr);

	/* Ensure that the wait list checks occur after clear_bit(). */
	smp_mb();

	ws = sbq_wake_ptr(sbq);
	if (!ws)
		return;

	wait_cnt = atomic_dec_return(&ws->wait_cnt);
	if (unlikely(wait_cnt < 0))
		wait_cnt = atomic_inc_return(&ws->wait_cnt);
	if (wait_cnt == 0) {
		atomic_add(sbq->wake_batch, &ws->wait_cnt);
		sbq_index_atomic_inc(&sbq->wake_index);
		wake_up(&ws->wait);
	}
}
EXPORT_SYMBOL_GPL(scale_bitmap_queue_clear);

void scale_bitmap_queue_wake_all(struct scale_bitmap_queue *sbq)
{
	int i, wake_index;

	/*
	 * Make sure all changes prior to this are visible from other CPUs.
	 */
	smp_mb();
	wake_index = atomic_read(&sbq->wake_index);
	for (i = 0; i < SBQ_WAIT_QUEUES; i++) {
		struct sbq_wait_state *ws = &sbq->ws[wake_index];

		if (waitqueue_active(&ws->wait))
			wake_up(&ws->wait);

		wake_index = sbq_index_inc(wake_index);
	}
}
EXPORT_SYMBOL_GPL(scale_bitmap_queue_wake_all);
