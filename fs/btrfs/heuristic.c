#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/math64.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/sort.h>
#include <linux/slab.h>

#include "heuristic.h"
/* Precalculated log2 realization */
#include "log2_lshift16.h"

/* For shannon full integer entropy calculation */
#define BUCKET_SIZE (1 << 8)

struct _backet_item {
	u8  padding;
	u8  symbol;
	u16 count;
};


/* For sorting */
static int compare(const void *lhs, const void *rhs)
{
	struct _backet_item *l = (struct _backet_item *)(lhs);
	struct _backet_item *r = (struct _backet_item *)(rhs);

	return r->count - l->count;
}

/*
 * For good compressible data
 * symbol set size over sample
 * will be small <= 64
 */
static u32 _symbset_calc(const struct _backet_item *bucket)
{
	u32 a = 0;
	u32 symbset_size = 0;

	for (; a < BUCKET_SIZE && symbset_size <= 64; a++) {
		if (bucket[a].count)
			symbset_size++;
	}
	return symbset_size;
}


/*
 * Try calculate coreset size
 * i.e. how many symbols use 90% of input data
 * < 50 - good compressible data
 * > 200 - bad compressible data
 * For right & fast calculation bucket must be reverse sorted
 */
static u32 _coreset_calc(const struct _backet_item *bucket,
	const u32 sum_threshold)
{
	u32 a = 0;
	u32 coreset_sum = 0;

	for (a = 0; a < 201 && bucket[a].count; a++) {
		coreset_sum += bucket[a].count;
		if (coreset_sum > sum_threshold)
			break;
	}
	return a;
}

static u64 _entropy_perc(const struct _backet_item *bucket,
	const u32 sample_size)
{
	u64 a, p;
	u64 entropy_sum = 0;
	u64 entropy_max = LOG2_RET_SHIFT*8;

	for (a = 0; a < BUCKET_SIZE && bucket[a].count > 0; a++) {
		p = bucket[a].count;
		p = div64_u64(p*LOG2_ARG_SHIFT, sample_size);
		entropy_sum += -p*log2_lshift16(p);
	}

	entropy_sum = div64_u64(entropy_sum, LOG2_ARG_SHIFT);
	return div64_u64(entropy_sum*100, entropy_max);
}

/* Pair distance from random distribution */
static u64 _random_pairs_distribution(const struct _backet_item *bucket,
	const u32 coreset_size, const u8 *sample, u32 sample_size)
{
	u32 a, b;
	u8 pair_a[2], pair_b[2];
	u32 pairs_count;
	u64 sum = 0;
	u64 buf1, buf2;

	for (a = 0; a < coreset_size-1; a++) {
		pairs_count = 0;
		pair_a[0] = bucket[a].symbol;
		pair_a[1] = bucket[a+1].symbol;
		pair_b[1] = bucket[a].symbol;
		pair_b[0] = bucket[a+1].symbol;
		for (b = 0; b < sample_size-1; b++) {
			u16 *pair_c = (u16 *) &sample[b];

			if (pair_c == (u16 *) pair_a)
				pairs_count++;
			else if (pair_c == (u16 *) pair_b)
				pairs_count++;
		}
		buf1 = bucket[a].count*bucket[a+1].count;
		buf1 = div64_u64(buf1*100000, (sample_size*sample_size));
		buf2 = pairs_count*2*100000;
		buf2 = div64_u64(pairs_count, sample_size);
		sum += (buf1 - buf2)*(buf1 - buf2);
	}

	return div64_u64(sum, 2048);
}

/*
 * Algorithm description
 * 1. Get subset of data for fast computation
 * 2. Scan bucket for symbol set
 *    - symbol set < 64 - data will be easy compressible, return
 * 3. Try compute coreset size (symbols count that use 90% of input data)
 *    - reverse sort bucket
 *    - sum cells until we reach 90% threshold,
 *      incriment coreset size each time
 *    - coreset_size < 50  - data will be easy compressible, return
 *                   > 200 - data will be bad compressible, return
 *      in general this looks like data compression ratio 0.2 - 0.8
 * 4. Compute shannon entropy
 *    - shannon entropy count of bytes and can't count pairs & entropy_calc
 *      so assume:
 *        - usage of entropy can lead to false negative
 *          so for prevent that (in bad) case it's useful to "count" pairs
 *        - entropy are not to high < 70% easy compressible, return
 *        - entropy are high < 90%, try count pairs,
 *          if there is any noticeable amount, compression are possible, return
 *        - entropy are high > 90%, try count pairs,
 *          if there is noticeable amount, compression are possible, return
 */

#define READ_SIZE 16

enum compression_advice btrfs_compress_heuristic(struct inode *inode,
	u64 start, u64 end)
{
	enum compression_advice ret = COMPRESS_NONE;
	u64 input_size = end - start;
	u64 index = start >> PAGE_SHIFT;
	u64 end_index = end >> PAGE_SHIFT;
	struct page *page;
	u64 a, b, c;
	u64 offset_count, shift, sample_size;
	u64 coreset_size, entropy_lvl;
	u8 *sample;
	u8 *input_data;
	struct _backet_item *bucket = NULL;


	/*
	 * In data: 128K  64K   32K   4K
	 * Sample:  4096b 3072b 2048b 1024b
	 * Avoid allocating array bigger then 4kb
	 */
	if (input_size >= 96*1024)
		offset_count = 256;
	else
		offset_count = 64 + input_size/512;

	shift = input_size/offset_count;
	sample_size = offset_count*READ_SIZE;

	/*
	 * speedup by copy data to sample array +30%
	 * I think it's because of memcpy optimizations and
	 * cpu cache hits
	 */
	sample = kmalloc(sample_size, GFP_NOFS);
	if (!sample)
		goto out;

	bucket = kcalloc(BUCKET_SIZE, sizeof(struct _backet_item), GFP_NOFS);
	if (!bucket)
		goto out;

	/* Read small subset of data 1024b-4096b */
	a = 0; b = 0;
	while (index <= end_index) {
		page = find_get_page(inode->i_mapping, index);
		BUG_ON(!page); /* Pages should be in the extent_io_tree */
		input_data = kmap(page);
		c = 0;
		while (c < PAGE_SIZE - READ_SIZE) {
			if (a >= input_size  - READ_SIZE)
			 	break;
			if (b >= sample_size - READ_SIZE)
				break;
			memcpy(&sample[b], &input_data[c], READ_SIZE);
			c += shift;
			a += shift;
			b += READ_SIZE;
		}
		kunmap(page);
		put_page(page);
		index++;
	}

	for (a = 0; a < sample_size; a++)
		bucket[sample[a]].count++;

	a = _symbset_calc(bucket);
	if (a < 64) {
		ret = COMPRESS_COST_EASY;
		goto out;
	}

	/*
	 * Preset symbols
	 * For computation _random_pairs_distribution() of symbols pairs
	 * code must understand which symbols in array and where
	 */
	for (a = 0; a < BUCKET_SIZE; a++)
		bucket[a].symbol = a;

	/* Sort in reverse order */
	sort(bucket, BUCKET_SIZE, sizeof(struct _backet_item), &compare, NULL);

	coreset_size = _coreset_calc(bucket, sample_size*90/100);

	if (coreset_size < 50) {
		ret = COMPRESS_COST_EASY;
		goto out;
	}

	if (coreset_size > 200) {
		ret = COMPRESS_NONE;
		goto out;
	}

	/*
	 * Okay, code fail to fast detect data type
	 * Let's calculate entropy
	 */
	entropy_lvl = _entropy_perc(bucket, sample_size);
	if (entropy_lvl < 70) {
		ret = COMPRESS_COST_MEDIUM;
		goto out;
	}

	a = _random_pairs_distribution(bucket, coreset_size, sample, sample_size);
	if (entropy_lvl < 90) {
		if (a > 0)
			ret = COMPRESS_COST_MEDIUM;
		else
			ret = COMPRESS_NONE;
		goto out;
	} else {
		if (a > 10)
			ret = COMPRESS_COST_HARD;
		else
			ret = COMPRESS_NONE;
		goto out;
	}

out:
	kfree(sample);
	kfree(bucket);
	return ret;
}
