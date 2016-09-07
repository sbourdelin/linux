/*
 * Fast and scalable bitmaps.
 *
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

#ifndef __LINUX_SCALE_BITMAP_H
#define __LINUX_SCALE_BITMAP_H

#include <linux/kernel.h>
#include <linux/slab.h>

/**
 * struct scale_bitmap_word - Word in a &struct scale_bitmap.
 */
struct scale_bitmap_word {
	/**
	 * @word: The bitmap word itself.
	 */
	unsigned long word;

	/**
	 * @depth: Number of bits being used in @word.
	 */
	unsigned long depth;
} ____cacheline_aligned_in_smp;

/**
 * struct scale_bitmap - Scalable bitmap.
 *
 * A &struct scale_bitmap is spread over multiple cachelines to avoid ping-pong.
 * This trades off higher memory usage for better scalability.
 */
struct scale_bitmap {
	/**
	 * @depth: Number of bits used in the whole bitmap..
	 */
	unsigned int depth;

	/**
	 * @shift: log2(number of bits used per word)
	 */
	unsigned int shift;

	/**
	 * @map_nr: Number of words (cachelines) being used for the bitmap.
	 */
	unsigned int map_nr;

	/**
	 * @map: Allocated bitmap.
	 */
	struct scale_bitmap_word *map;
};

#define SBQ_WAIT_QUEUES 8
#define SBQ_WAKE_BATCH 8

/**
 * struct sbq_wait_state - Wait queue in a &struct scale_bitmap.
 */
struct sbq_wait_state {
	/**
	 * @wait_cnt: Number of frees remaining before we wake up.
	 */
	atomic_t wait_cnt;

	/**
	 * @wait: Wait queue.
	 */
	wait_queue_head_t wait;
} ____cacheline_aligned_in_smp;

/**
 * struct scale_bitmap_queue - Scalable bitmap with the added ability to wait on
 * free bits.
 *
 * A &struct scale_bitmap_queue uses multiple wait queues and rolling wakeups to
 * avoid contention on the wait queue spinlock. This ensures that we don't hit a
 * scalability wall when we run out of free bits and have to start putting tasks
 * to sleep.
 */
struct scale_bitmap_queue {
	/**
	 * @map: Scalable bitmap.
	 */
	struct scale_bitmap map;

	/*
	 * @alloc_hint: Cache of last successfully allocated or freed bit.
	 *
	 * This is per-cpu, which allows multiple users to stick to different
	 * cachelines until the map is exhausted.
	 */
	unsigned int __percpu *alloc_hint;

	/**
	 * @wake_batch: Number of bits which must be freed before we wake up any
	 * waiters.
	 */
	unsigned int wake_batch;

	/**
	 * @wake_index: Next wait queue in @ws to wake up.
	 */
	atomic_t wake_index;

	/**
	 * @ws: Wait queues.
	 */
	struct sbq_wait_state *ws;

	/**
	 * @round_robin: Allocate bits in strict round-robin order.
	 */
	bool round_robin;
};

/**
 * scale_bitmap_init_node() - Initialize a &struct scale_bitmap on a specific
 * memory node.
 * @bitmap: Bitmap to initialize.
 * @depth: Number of bits to allocate.
 * @shift: Use 2^@shift bits per word in the bitmap; if a negative number if
 *         given, a good default is chosen.
 * @flags: Allocation flags.
 * @node: Memory node to allocate on.
 *
 * Return: Zero on success or negative errno on failure.
 */
int scale_bitmap_init_node(struct scale_bitmap *bitmap, unsigned int depth,
			   int shift, gfp_t flags, int node);

/**
 * scale_bitmap_free() - Free memory used by a &struct scale_bitmap.
 * @bitmap: Bitmap to free.
 */
static inline void scale_bitmap_free(struct scale_bitmap *bitmap)
{
	kfree(bitmap->map);
	bitmap->map = NULL;
}

/**
 * scale_bitmap_resize() - Resize a &struct scale_bitmap.
 * @bitmap: Bitmap to resize.
 * @depth: New number of bits to resize to.
 *
 * Doesn't reallocate anything. It's up to the caller to ensure that the new
 * depth doesn't exceed the depth that the bitmap was initialized with.
 */
void scale_bitmap_resize(struct scale_bitmap *bitmap, unsigned int depth);

/**
 * scale_bitmap_get() - Try to allocate a free bit from a &struct scale_bitmap.
 * @bitmap: Bitmap to allocate from.
 * @alloc_hint: Cache of last successfully allocated bit. This should be per-cpu
 *              for best results, which allows multiple users to stick to
 *              different cachelines until the map is exhausted.
 * @round_robin: If true, be stricter about allocation order; always allocate
 *               starting from the last allocated bit. This is less efficient
 *               than the default behavior (false).
 *
 * Return: Non-negative allocated bit number if successful, -1 otherwise.
 */
int scale_bitmap_get(struct scale_bitmap *bitmap, unsigned int *alloc_hint,
		     bool round_robin);

/**
 * scale_bitmap_any_bit_set() - Check for a set bit in a &struct scale_bitmap.
 * @bitmap: Bitmap to check.
 *
 * Return: true if any bit in the bitmap is set, false otherwise.
 */
bool scale_bitmap_any_bit_set(const struct scale_bitmap *bitmap);

/**
 * scale_bitmap_any_bit_clear() - Check for an unset bit in a &struct
 * scale_bitmap.
 * @bitmap: Bitmap to check.
 *
 * Return: true if any bit in the bitmap is clear, false otherwise.
 */
bool scale_bitmap_any_bit_clear(const struct scale_bitmap *bitmap);

typedef bool (*sb_for_each_fn)(struct scale_bitmap *, unsigned int, void *);

/**
 * scale_bitmap_for_each_set() - Iterate over each set bit in a &struct
 * scale_bitmap.
 * @bitmap: Bitmap to iterate over.
 * @fn: Callback. Should return true to continue or false to break early.
 * @data: Pointer to pass to callback.
 *
 * This is inline even though it's non-trivial so that the function calls to the
 * callback will hopefully get optimized away.
 */
static inline void scale_bitmap_for_each_set(struct scale_bitmap *bitmap,
					     sb_for_each_fn fn, void *data)
{
	unsigned int i;

	for (i = 0; i < bitmap->map_nr; i++) {
		struct scale_bitmap_word *word = &bitmap->map[i];
		unsigned int off, nr;

		if (!word->word)
			continue;

		nr = 0;
		off = i << bitmap->shift;
		while (1) {
			nr = find_next_bit(&word->word, word->depth, nr);
			if (nr >= word->depth)
				break;

			if (!fn(bitmap, off + nr, data))
				return;

			nr++;
		}
	}
}

#define SB_NR_TO_INDEX(bitmap, bitnr) ((bitnr) >> (bitmap)->shift)
#define SB_NR_TO_BIT(bitmap, bitnr) ((bitnr) & ((1U << (bitmap)->shift) - 1U))

static inline unsigned long *__scale_bitmap_word(struct scale_bitmap *bitmap,
						 unsigned int bitnr)
{
	return &bitmap->map[SB_NR_TO_INDEX(bitmap, bitnr)].word;
}

/* Helpers equivalent to the operations in asm/bitops.h and linux/bitmap.h */

static inline void scale_bitmap_set_bit(struct scale_bitmap *bitmap,
					unsigned int bitnr)
{
	set_bit(SB_NR_TO_BIT(bitmap, bitnr),
		__scale_bitmap_word(bitmap, bitnr));
}

static inline void scale_bitmap_clear_bit(struct scale_bitmap *bitmap,
					  unsigned int bitnr)
{
	clear_bit(SB_NR_TO_BIT(bitmap, bitnr),
		  __scale_bitmap_word(bitmap, bitnr));
}

static inline int scale_bitmap_test_bit(struct scale_bitmap *bitmap,
					unsigned int bitnr)
{
	return test_bit(SB_NR_TO_BIT(bitmap, bitnr),
			__scale_bitmap_word(bitmap, bitnr));
}

unsigned int scale_bitmap_weight(const struct scale_bitmap *bitmap);

/**
 * scale_bitmap_queue_init_node() - Initialize a &struct scale_bitmap_queue on a
 * specific memory node.
 * @sbq: Bitmap queue to initialize.
 * @depth: See scale_bitmap_init_node().
 * @shift: See scale_bitmap_init_node().
 * @round_robin: See scale_bitmap_get().
 * @flags: Allocation flags.
 * @node: Memory node to allocate on.
 *
 * Return: Zero on success or negative errno on failure.
 */
int scale_bitmap_queue_init_node(struct scale_bitmap_queue *sbq,
				 unsigned int depth, int shift,
				 bool round_robin, gfp_t flags, int node);

/**
 * scale_bitmap_queue_free() - Free memory used by a &struct scale_bitmap_queue.
 *
 * @sbq: Bitmap queue to free.
 */
static inline void scale_bitmap_queue_free(struct scale_bitmap_queue *sbq)
{
	kfree(sbq->ws);
	free_percpu(sbq->alloc_hint);
	scale_bitmap_free(&sbq->map);
}

/**
 * scale_bitmap_queue_resize() - Resize a &struct scale_bitmap_queue.
 * @sbq: Bitmap queue to resize.
 * @depth: New number of bits to resize to.
 *
 * Like scale_bitmap_resize(), this doesn't reallocate anything. It has to do
 * some extra work on the &struct scale_bitmap_queue, so it's not safe to just
 * resize the underlying &struct scale_bitmap.
 */
void scale_bitmap_queue_resize(struct scale_bitmap_queue *sbq,
			       unsigned int depth);

/**
 * __scale_bitmap_queue_get() - Try to allocate a free bit from a &struct
 * scale_bitmap_queue with preemption already disabled.
 * @sbq: Bitmap queue to allocate from.
 *
 * Return: Non-negative allocated bit number if successful, -1 otherwise.
 */
static inline int __scale_bitmap_queue_get(struct scale_bitmap_queue *sbq)
{
	return scale_bitmap_get(&sbq->map, this_cpu_ptr(sbq->alloc_hint),
				sbq->round_robin);
}

/**
 * scale_bitmap_queue_get() - Try to allocate a free bit from a &struct
 * scale_bitmap_queue.
 * @sbq: Bitmap queue to allocate from.
 * @cpu: Output parameter; will contain the CPU we ran on (e.g., to be passed to
 *       scale_bitmap_queue_clear()).
 *
 * Return: Non-negative allocated bit number if successful, -1 otherwise.
 */
static inline int scale_bitmap_queue_get(struct scale_bitmap_queue *sbq,
					 unsigned int *cpu)
{
	int ret;

	*cpu = get_cpu();
	ret = __scale_bitmap_queue_get(sbq);
	put_cpu();
	return ret;
}

/**
 * scale_bitmap_queue_clear() - Free an allocated bit and wake up waiters on a
 * &struct scale_bitmap_queue.
 * @sbq: Bitmap to free from.
 * @nr: Bit number to free.
 * @cpu: CPU the bit was allocated on.
 */
void scale_bitmap_queue_clear(struct scale_bitmap_queue *sbq, unsigned int nr,
			      unsigned int cpu);

static inline int sbq_index_inc(int index)
{
	return (index + 1) & (SBQ_WAIT_QUEUES - 1);
}

static inline void sbq_index_atomic_inc(atomic_t *index)
{
	int old = atomic_read(index);
	int new = sbq_index_inc(old);
	atomic_cmpxchg(index, old, new);
}

/**
 * sbq_wait_ptr() - Get the next wait queue to use for a &struct
 * scale_bitmap_queue.
 * @sbq: Bitmap queue to wait on.
 * @wait_index: A counter per "user" of @sbq.
 */
static inline struct sbq_wait_state *sbq_wait_ptr(struct scale_bitmap_queue *sbq,
						  atomic_t *wait_index)
{
	struct sbq_wait_state *ws;

	ws = &sbq->ws[atomic_read(wait_index)];
	sbq_index_atomic_inc(wait_index);
	return ws;
}

/**
 * scale_bitmap_queue_wake_all() - Wake up everything waiting on a &struct
 * scale_bitmap_queue.
 * @sbq: Bitmap queue to wake up.
 */
void scale_bitmap_queue_wake_all(struct scale_bitmap_queue *sbq);

#endif /* __LINUX_SCALE_BITMAP_H */
