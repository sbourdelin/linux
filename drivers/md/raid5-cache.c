/*
 * Copyright (C) 2015 Shaohua Li <shli@fb.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/raid/md_p.h>
#include <linux/crc32c.h>
#include <linux/random.h>
#include "md.h"
#include "raid5.h"

/*
 * metadata/data stored in disk with 4k size unit (a block) regardless
 * underneath hardware sector size. only works with PAGE_SIZE == 4096
 */
#define BLOCK_SECTORS (8)

/*
 * reclaim runs every 1/4 disk size or 10G reclaimable space. This can prevent
 * recovery scans a very long log
 */
#define RECLAIM_MAX_FREE_SPACE (10 * 1024 * 1024 * 2) /* sector */
#define RECLAIM_MAX_FREE_SPACE_SHIFT (2)

/* wake up reclaim thread periodically */
#define RECLAIM_WAKEUP_INTERVAL (5 * HZ)

/*
 * We only need 2 bios per I/O unit to make progress, but ensure we
 * have a few more available to not get too tight.
 */
#define R5L_POOL_SIZE	4

enum r5c_cache_mode {
	R5C_MODE_NO_CACHE = 0,
	R5C_MODE_WRITE_THROUGH = 1,
	R5C_MODE_WRITE_BACK = 2,
};

static char *r5c_cache_mode_str[] = {"no-cache", "write-through", "write-back"};

struct r5c_cache {
	int flush_threshold;		/* flush the stripe when flush_threshold buffers are dirty  */
	int mode;			/* enum r5c_cache_mode */

	struct list_head stripe_in_cache; /* all stripes in the cache, with sh->journal_start in order */
	spinlock_t stripe_in_cache_lock;  /* lock for stripe_in_cache */

	sector_t first_sector;		/* first useful data on journal */

	/* read stats */
	atomic64_t read_full_hits;	/* the whole chunk in cache */
	atomic64_t read_partial_hits;	/* some pages of the chunk in cache */
	atomic64_t read_misses;		/* the whold chunk is not in cache */
};

struct r5l_log {
	struct md_rdev *rdev;
	struct r5c_cache cache;

	u32 uuid_checksum;

	sector_t device_size;		/* log device size, round to
					 * BLOCK_SECTORS */
	sector_t max_free_space;	/* reclaim run if free space is at
					 * this size */

	sector_t last_checkpoint;	/* log tail. where recovery scan
					 * starts from */
	u64 last_cp_seq;		/* log tail sequence */

	sector_t log_start;		/* log head. where new data appends */
	u64 seq;			/* log head sequence */

	sector_t next_checkpoint;
	u64 next_cp_seq;

	struct mutex io_mutex;
	struct r5l_io_unit *current_io;	/* current io_unit accepting new data */

	spinlock_t io_list_lock;
	struct list_head running_ios;	/* io_units which are still running,
					 * and have not yet been completely
					 * written to the log */
	struct list_head io_end_ios;	/* io_units which have been completely
					 * written to the log but not yet written
					 * to the RAID */
	struct list_head flushing_ios;	/* io_units which are waiting for log
					 * cache flush */
	struct list_head finished_ios;	/* io_units which settle down in log disk */
	struct bio flush_bio;

	struct list_head no_mem_stripes;   /* pending stripes, -ENOMEM */

	struct kmem_cache *io_kc;
	mempool_t *io_pool;
	struct bio_set *bs;
	mempool_t *meta_pool;

	struct md_thread *reclaim_thread;
	unsigned long reclaim_target;	/* number of space that need to be
					 * reclaimed.  if it's 0, reclaim spaces
					 * used by io_units which are in
					 * IO_UNIT_STRIPE_END state (eg, reclaim
					 * dones't wait for specific io_unit
					 * switching to IO_UNIT_STRIPE_END
					 * state) */
	wait_queue_head_t iounit_wait;

	struct list_head no_space_stripes; /* pending stripes, log has no space */
	spinlock_t no_space_stripes_lock;

	bool need_cache_flush;
	bool in_teardown;
};

/*
 * an IO range starts from a meta data block and end at the next meta data
 * block. The io unit's the meta data block tracks data/parity followed it. io
 * unit is written to log disk with normal write, as we always flush log disk
 * first and then start move data to raid disks, there is no requirement to
 * write io unit with FLUSH/FUA
 */
struct r5l_io_unit {
	struct r5l_log *log;

	struct page *meta_page;	/* store meta block */
	int meta_offset;	/* current offset in meta_page */

	struct bio *current_bio;/* current_bio accepting new data */

	atomic_t pending_stripe;/* how many stripes not flushed to raid */
	u64 seq;		/* seq number of the metablock */
	sector_t log_start;	/* where the io_unit starts */
	sector_t log_end;	/* where the io_unit ends */
	struct list_head log_sibling; /* log->running_ios */
	struct list_head stripe_list; /* stripes added to the io_unit */

	int state;
	bool need_split_bio;
};

/* r5l_io_unit state */
enum r5l_io_unit_state {
	IO_UNIT_RUNNING = 0,	/* accepting new IO */
	IO_UNIT_IO_START = 1,	/* io_unit bio start writing to log,
				 * don't accepting new bio */
	IO_UNIT_IO_END = 2,	/* io_unit bio finish writing to log */
	IO_UNIT_STRIPE_END = 3,	/* stripes data finished writing to raid */
};

struct r5c_chunk_map {
	int sh_count;
	struct r5conf *conf;
	struct bio *parent_bi;
	int dd_idx;
	struct stripe_head *sh_array[0];
};

static void init_r5c_cache(struct r5conf *conf, struct r5c_cache *cache)
{
	cache->flush_threshold = conf->raid_disks - conf->max_degraded;  /* full stripe */
	cache->mode = R5C_MODE_WRITE_BACK;
	INIT_LIST_HEAD(&cache->stripe_in_cache);
	spin_lock_init(&cache->stripe_in_cache_lock);

	atomic64_set(&cache->read_full_hits, 0);
	atomic64_set(&cache->read_partial_hits, 0);
	atomic64_set(&cache->read_misses, 0);
}

void r5c_set_state(struct stripe_head *sh, enum r5c_states new_state)
{
	unsigned long flags;

	spin_lock_irqsave(&sh->stripe_lock, flags);
	sh->r5c_state = new_state;
	spin_unlock_irqrestore(&sh->stripe_lock, flags);
}

static sector_t r5l_ring_add(struct r5l_log *log, sector_t start, sector_t inc)
{
	start += inc;
	if (start >= log->device_size)
		start = start - log->device_size;
	return start;
}

static sector_t r5l_ring_distance(struct r5l_log *log, sector_t start,
				  sector_t end)
{
	if (end >= start)
		return end - start;
	else
		return end + log->device_size - start;
}

static bool r5l_has_free_space(struct r5l_log *log, sector_t size)
{
	sector_t used_size;

	used_size = r5l_ring_distance(log, log->last_checkpoint,
					log->log_start);

	return log->device_size > used_size + size;
}

static void __r5l_set_io_unit_state(struct r5l_io_unit *io,
				    enum r5l_io_unit_state state)
{
	if (WARN_ON(io->state >= state))
		return;
	io->state = state;
}

void r5c_freeze_stripe_for_reclaim(struct stripe_head *sh)
{
	struct r5conf *conf = sh->raid_conf;

	if (!conf->log)
		return;

	WARN_ON(sh->r5c_state >= R5C_STATE_FROZEN);
	r5c_set_state(sh, R5C_STATE_FROZEN);
	if (!test_and_set_bit(STRIPE_PREREAD_ACTIVE, &sh->state))
		atomic_inc(&conf->preread_active_stripes);
	if (test_and_clear_bit(STRIPE_IN_R5C_CACHE, &sh->state)) {
		BUG_ON(atomic_read(&conf->r5c_cached_stripes) == 0);
		atomic_dec(&conf->r5c_cached_stripes);
	}
}

static void r5c_handle_data_cached(struct stripe_head *sh)
{
	int i;

	for (i = sh->disks; i--; )
		if (test_and_clear_bit(R5_Wantcache, &sh->dev[i].flags)) {
			set_bit(R5_InCache, &sh->dev[i].flags);
			clear_bit(R5_LOCKED, &sh->dev[i].flags);
			atomic_inc(&sh->dev_in_cache);
		}
}

/*
 * this journal write must contain full parity,
 * it may also contain data of none-overwrites
 */
static void r5c_handle_parity_cached(struct stripe_head *sh)
{
	int i;

	for (i = sh->disks; i--; )
		if (test_bit(R5_InCache, &sh->dev[i].flags))
			set_bit(R5_Wantwrite, &sh->dev[i].flags);
	r5c_set_state(sh, R5C_STATE_PARITY_DONE);
}

static void r5c_finish_cache_stripe(struct stripe_head *sh)
{
	switch (sh->r5c_state) {
	case R5C_STATE_PARITY_RUN:
		r5c_handle_parity_cached(sh);
		break;
	case R5C_STATE_CLEAN:
		r5c_set_state(sh, R5C_STATE_RUNNING);
	case R5C_STATE_RUNNING:
		r5c_handle_data_cached(sh);
		break;
	default:
		BUG();
	}
}

static void r5l_io_run_stripes(struct r5l_io_unit *io)
{
	struct stripe_head *sh, *next;

	list_for_each_entry_safe(sh, next, &io->stripe_list, log_list) {
		list_del_init(&sh->log_list);

		r5c_finish_cache_stripe(sh);

		set_bit(STRIPE_HANDLE, &sh->state);
		raid5_release_stripe(sh);
	}
}

static void r5l_log_run_stripes(struct r5l_log *log)
{
	struct r5l_io_unit *io, *next;

	assert_spin_locked(&log->io_list_lock);

	list_for_each_entry_safe(io, next, &log->running_ios, log_sibling) {
		/* don't change list order */
		if (io->state < IO_UNIT_IO_END)
			break;

		list_move_tail(&io->log_sibling, &log->finished_ios);
		r5l_io_run_stripes(io);
	}
}

static void r5l_move_to_end_ios(struct r5l_log *log)
{
	struct r5l_io_unit *io, *next;

	assert_spin_locked(&log->io_list_lock);

	list_for_each_entry_safe(io, next, &log->running_ios, log_sibling) {
		/* don't change list order */
		if (io->state < IO_UNIT_IO_END)
			break;
		list_move_tail(&io->log_sibling, &log->io_end_ios);
	}
}

static void r5l_log_endio(struct bio *bio)
{
	struct r5l_io_unit *io = bio->bi_private;
	struct r5l_log *log = io->log;
	unsigned long flags;

	if (bio->bi_error)
		md_error(log->rdev->mddev, log->rdev);

	bio_put(bio);
	mempool_free(io->meta_page, log->meta_pool);

	spin_lock_irqsave(&log->io_list_lock, flags);
	__r5l_set_io_unit_state(io, IO_UNIT_IO_END);
	if (log->need_cache_flush)
		r5l_move_to_end_ios(log);
	else
		r5l_log_run_stripes(log);
	spin_unlock_irqrestore(&log->io_list_lock, flags);

	if (log->need_cache_flush)
		md_wakeup_thread(log->rdev->mddev->thread);
}

static void r5l_submit_current_io(struct r5l_log *log)
{
	struct r5l_io_unit *io = log->current_io;
	struct r5l_meta_block *block;
	unsigned long flags;
	u32 crc;

	if (!io)
		return;

	block = page_address(io->meta_page);
	block->meta_size = cpu_to_le32(io->meta_offset);
	crc = crc32c_le(log->uuid_checksum, block, PAGE_SIZE);
	block->checksum = cpu_to_le32(crc);

	log->current_io = NULL;
	spin_lock_irqsave(&log->io_list_lock, flags);
	__r5l_set_io_unit_state(io, IO_UNIT_IO_START);
	spin_unlock_irqrestore(&log->io_list_lock, flags);

	submit_bio(WRITE, io->current_bio);
}

static struct bio *r5l_bio_alloc(struct r5l_log *log)
{
	struct bio *bio = bio_alloc_bioset(GFP_NOIO, BIO_MAX_PAGES, log->bs);

	bio->bi_rw = WRITE;
	bio->bi_bdev = log->rdev->bdev;
	bio->bi_iter.bi_sector = log->rdev->data_offset + log->log_start;

	return bio;
}

static void r5_reserve_log_entry(struct r5l_log *log, struct r5l_io_unit *io)
{
	log->log_start = r5l_ring_add(log, log->log_start, BLOCK_SECTORS);

	/*
	 * If we filled up the log device start from the beginning again,
	 * which will require a new bio.
	 *
	 * Note: for this to work properly the log size needs to me a multiple
	 * of BLOCK_SECTORS.
	 */
	if (log->log_start == 0)
		io->need_split_bio = true;

	io->log_end = log->log_start;
}

static struct r5l_io_unit *r5l_new_meta(struct r5l_log *log)
{
	struct r5l_io_unit *io;
	struct r5l_meta_block *block;

	io = mempool_alloc(log->io_pool, GFP_ATOMIC);
	if (!io)
		return NULL;
	memset(io, 0, sizeof(*io));

	io->log = log;
	INIT_LIST_HEAD(&io->log_sibling);
	INIT_LIST_HEAD(&io->stripe_list);
	io->state = IO_UNIT_RUNNING;

	io->meta_page = mempool_alloc(log->meta_pool, GFP_NOIO);
	block = page_address(io->meta_page);
	clear_page(block);
	block->magic = cpu_to_le32(R5LOG_MAGIC);
	block->version = R5LOG_VERSION;
	block->seq = cpu_to_le64(log->seq);
	block->position = cpu_to_le64(log->log_start);

	io->log_start = log->log_start;
	io->meta_offset = sizeof(struct r5l_meta_block);
	io->seq = log->seq++;

	io->current_bio = r5l_bio_alloc(log);
	io->current_bio->bi_end_io = r5l_log_endio;
	io->current_bio->bi_private = io;
	bio_add_page(io->current_bio, io->meta_page, PAGE_SIZE, 0);

	r5_reserve_log_entry(log, io);

	spin_lock_irq(&log->io_list_lock);
	list_add_tail(&io->log_sibling, &log->running_ios);
	spin_unlock_irq(&log->io_list_lock);

	return io;
}

static int r5l_get_meta(struct r5l_log *log, unsigned int payload_size)
{
	if (log->current_io &&
	    log->current_io->meta_offset + payload_size > PAGE_SIZE)
		r5l_submit_current_io(log);

	if (!log->current_io) {
		log->current_io = r5l_new_meta(log);
		if (!log->current_io)
			return -ENOMEM;
	}

	return 0;
}

static void r5l_append_payload_meta(struct r5l_log *log, u16 type,
				    sector_t location,
				    u32 checksum1, u32 checksum2,
				    bool checksum2_valid)
{
	struct r5l_io_unit *io = log->current_io;
	struct r5l_payload_data_parity *payload;

	payload = page_address(io->meta_page) + io->meta_offset;
	payload->header.type = cpu_to_le16(type);
	payload->header.flags = cpu_to_le16(0);
	payload->size = cpu_to_le32((1 + !!checksum2_valid) <<
				    (PAGE_SHIFT - 9));
	payload->location = cpu_to_le64(location);
	payload->checksum[0] = cpu_to_le32(checksum1);
	if (checksum2_valid)
		payload->checksum[1] = cpu_to_le32(checksum2);

	io->meta_offset += sizeof(struct r5l_payload_data_parity) +
		sizeof(__le32) * (1 + !!checksum2_valid);
}

static void r5l_append_payload_page(struct r5l_log *log, struct page *page)
{
	struct r5l_io_unit *io = log->current_io;

	if (io->need_split_bio) {
		struct bio *prev = io->current_bio;

		io->current_bio = r5l_bio_alloc(log);
		bio_chain(io->current_bio, prev);

		submit_bio(WRITE, prev);
	}

	if (!bio_add_page(io->current_bio, page, PAGE_SIZE, 0))
		BUG();

	r5_reserve_log_entry(log, io);
}

static int r5l_log_stripe(struct r5l_log *log, struct stripe_head *sh,
			   int data_pages, int parity_pages)
{
	int i;
	int meta_size;
	int ret;
	struct r5l_io_unit *io;
	unsigned long flags;

	meta_size =
		((sizeof(struct r5l_payload_data_parity) + sizeof(__le32))
		 * data_pages) +
		sizeof(struct r5l_payload_data_parity) +
		sizeof(__le32) * parity_pages;

	ret = r5l_get_meta(log, meta_size);
	if (ret)
		return ret;

	io = log->current_io;

	for (i = 0; i < sh->disks; i++) {
		if (!test_bit(R5_Wantwrite, &sh->dev[i].flags) &&
		    !test_bit(R5_Wantcache, &sh->dev[i].flags))
			continue;
		if (test_bit(R5_InCache, &sh->dev[i].flags))
			continue;
		if (i == sh->pd_idx || i == sh->qd_idx)
			continue;
		r5l_append_payload_meta(log, R5LOG_PAYLOAD_DATA,
					raid5_compute_blocknr(sh, i, 0),
					sh->dev[i].log_checksum, 0, false);
		r5l_append_payload_page(log, sh->dev[i].page);
	}

	if (parity_pages == 2) {
		r5l_append_payload_meta(log, R5LOG_PAYLOAD_PARITY,
					sh->sector, sh->dev[sh->pd_idx].log_checksum,
					sh->dev[sh->qd_idx].log_checksum, true);
		r5l_append_payload_page(log, sh->dev[sh->pd_idx].page);
		r5l_append_payload_page(log, sh->dev[sh->qd_idx].page);
	} else if (parity_pages == 1) {
		r5l_append_payload_meta(log, R5LOG_PAYLOAD_PARITY,
					sh->sector, sh->dev[sh->pd_idx].log_checksum,
					0, false);
		r5l_append_payload_page(log, sh->dev[sh->pd_idx].page);
	} else
		BUG_ON(parity_pages != 0);

	list_add_tail(&sh->log_list, &io->stripe_list);
	atomic_inc(&io->pending_stripe);
	sh->log_io = io;

	spin_lock_irqsave(&log->cache.stripe_in_cache_lock, flags);
	spin_lock(&sh->stripe_lock);
	if (sh->journal_start == -1L) {
		BUG_ON(!list_empty(&sh->r5c));
		sh->journal_start = log->next_checkpoint;
		list_add_tail(&sh->r5c,
			      &log->cache.stripe_in_cache);
	}
	spin_unlock(&sh->stripe_lock);
	spin_unlock_irqrestore(&log->cache.stripe_in_cache_lock, flags);
	return 0;
}

/*
 * running in raid5d, where reclaim could wait for raid5d too (when it flushes
 * data from log to raid disks), so we shouldn't wait for reclaim here
 */
int r5l_write_stripe(struct r5l_log *log, struct stripe_head *sh)
{
	int write_disks = 0;
	int data_pages, parity_pages;
	int meta_size;
	int reserve;
	int i;
	int ret = 0;

	if (!log)
		return -EAGAIN;

	/* Don't support stripe batch */
	if (sh->log_io || !test_bit(R5_Wantwrite, &sh->dev[sh->pd_idx].flags) ||
	    test_bit(STRIPE_SYNCING, &sh->state)) {
		/* the stripe is written to log, we start writing it to raid */
		clear_bit(STRIPE_LOG_TRAPPED, &sh->state);
		return -EAGAIN;
	}

	WARN_ON(sh->r5c_state < R5C_STATE_FROZEN);

	for (i = 0; i < sh->disks; i++) {
		void *addr;

		if (!test_bit(R5_Wantwrite, &sh->dev[i].flags))
			continue;

		if (test_bit(R5_InCache, &sh->dev[i].flags))
			continue;

		write_disks++;
		/* checksum is already calculated in last run */
		if (test_bit(STRIPE_LOG_TRAPPED, &sh->state))
			continue;
		addr = kmap_atomic(sh->dev[i].page);
		sh->dev[i].log_checksum = crc32c_le(log->uuid_checksum,
						    addr, PAGE_SIZE);
		kunmap_atomic(addr);
	}
	parity_pages = 1 + !!(sh->qd_idx >= 0);
	data_pages = write_disks - parity_pages;

	pr_debug("%s: write %d data_pages and %d parity_pages\n",
		 __func__, data_pages, parity_pages);

	meta_size =
		((sizeof(struct r5l_payload_data_parity) + sizeof(__le32))
		 * data_pages) +
		sizeof(struct r5l_payload_data_parity) +
		sizeof(__le32) * parity_pages;
	/* Doesn't work with very big raid array */
	if (meta_size + sizeof(struct r5l_meta_block) > PAGE_SIZE)
		return -EINVAL;

	set_bit(STRIPE_LOG_TRAPPED, &sh->state);
	/*
	 * The stripe must enter state machine again to finish the write, so
	 * don't delay.
	 */
	clear_bit(STRIPE_DELAYED, &sh->state);
	atomic_inc(&sh->count);

	mutex_lock(&log->io_mutex);
	/* meta + data */
	reserve = (1 + write_disks) << (PAGE_SHIFT - 9);
	if (!r5l_has_free_space(log, reserve)) {
		spin_lock(&log->no_space_stripes_lock);
		list_add_tail(&sh->log_list, &log->no_space_stripes);
		spin_unlock(&log->no_space_stripes_lock);

		r5l_wake_reclaim(log, reserve);
	} else {
		ret = r5l_log_stripe(log, sh, data_pages, parity_pages);
		if (ret) {
			spin_lock_irq(&log->io_list_lock);
			list_add_tail(&sh->log_list, &log->no_mem_stripes);
			spin_unlock_irq(&log->io_list_lock);
		}
	}

	mutex_unlock(&log->io_mutex);
	return 0;
}

void r5l_write_stripe_run(struct r5l_log *log)
{
	if (!log)
		return;
	mutex_lock(&log->io_mutex);
	r5l_submit_current_io(log);
	mutex_unlock(&log->io_mutex);
}

int r5l_handle_flush_request(struct r5l_log *log, struct bio *bio)
{
	if (!log)
		return -ENODEV;
	/*
	 * we flush log disk cache first, then write stripe data to raid disks.
	 * So if bio is finished, the log disk cache is flushed already. The
	 * recovery guarantees we can recovery the bio from log disk, so we
	 * don't need to flush again
	 */
	if (bio->bi_iter.bi_size == 0) {
		bio_endio(bio);
		return 0;
	}
	bio->bi_rw &= ~REQ_FLUSH;
	return -EAGAIN;
}

/* This will run after log space is reclaimed */
static void r5l_run_no_space_stripes(struct r5l_log *log)
{
	struct stripe_head *sh;

	spin_lock(&log->no_space_stripes_lock);
	while (!list_empty(&log->no_space_stripes)) {
		sh = list_first_entry(&log->no_space_stripes,
				      struct stripe_head, log_list);
		list_del_init(&sh->log_list);
		set_bit(STRIPE_HANDLE, &sh->state);
		raid5_release_stripe(sh);
	}
	spin_unlock(&log->no_space_stripes_lock);
}

static sector_t r5l_reclaimable_space(struct r5l_log *log)
{
	return r5l_ring_distance(log, log->last_checkpoint,
				 log->next_checkpoint);
}

static void r5l_run_no_mem_stripe(struct r5l_log *log)
{
	struct stripe_head *sh;

	assert_spin_locked(&log->io_list_lock);

	if (!list_empty(&log->no_mem_stripes)) {
		sh = list_first_entry(&log->no_mem_stripes,
				      struct stripe_head, log_list);
		list_del_init(&sh->log_list);
		set_bit(STRIPE_HANDLE, &sh->state);
		raid5_release_stripe(sh);
	}
}

static bool r5l_complete_finished_ios(struct r5l_log *log)
{
	struct r5l_io_unit *io, *next;
	bool found = false;

	assert_spin_locked(&log->io_list_lock);

	list_for_each_entry_safe(io, next, &log->finished_ios, log_sibling) {
		/* don't change list order */
		if (io->state < IO_UNIT_STRIPE_END)
			break;

		log->next_checkpoint = io->log_start;
		log->next_cp_seq = io->seq;

		list_del(&io->log_sibling);
		mempool_free(io, log->io_pool);
		r5l_run_no_mem_stripe(log);

		found = true;
	}

	return found;
}

static void __r5l_stripe_write_finished(struct r5l_io_unit *io)
{
	struct r5l_log *log = io->log;
	unsigned long flags;

	spin_lock_irqsave(&log->io_list_lock, flags);
	__r5l_set_io_unit_state(io, IO_UNIT_STRIPE_END);

	if (!r5l_complete_finished_ios(log)) {
		spin_unlock_irqrestore(&log->io_list_lock, flags);
		return;
	}

	if (r5l_reclaimable_space(log) > log->max_free_space)
		r5l_wake_reclaim(log, 0);

	spin_unlock_irqrestore(&log->io_list_lock, flags);
	wake_up(&log->iounit_wait);
}

void r5l_stripe_write_finished(struct stripe_head *sh)
{
	struct r5l_io_unit *io;

	io = sh->log_io;
	sh->log_io = NULL;

	if (io && atomic_dec_and_test(&io->pending_stripe))
		__r5l_stripe_write_finished(io);
}

static void r5l_log_flush_endio(struct bio *bio)
{
	struct r5l_log *log = container_of(bio, struct r5l_log,
		flush_bio);
	unsigned long flags;
	struct r5l_io_unit *io;

	if (bio->bi_error)
		md_error(log->rdev->mddev, log->rdev);

	spin_lock_irqsave(&log->io_list_lock, flags);
	list_for_each_entry(io, &log->flushing_ios, log_sibling)
		r5l_io_run_stripes(io);
	list_splice_tail_init(&log->flushing_ios, &log->finished_ios);
	spin_unlock_irqrestore(&log->io_list_lock, flags);
}

/*
 * Starting dispatch IO to raid.
 * io_unit(meta) consists of a log. There is one situation we want to avoid. A
 * broken meta in the middle of a log causes recovery can't find meta at the
 * head of log. If operations require meta at the head persistent in log, we
 * must make sure meta before it persistent in log too. A case is:
 *
 * stripe data/parity is in log, we start write stripe to raid disks. stripe
 * data/parity must be persistent in log before we do the write to raid disks.
 *
 * The solution is we restrictly maintain io_unit list order. In this case, we
 * only write stripes of an io_unit to raid disks till the io_unit is the first
 * one whose data/parity is in log.
 */
void r5l_flush_stripe_to_raid(struct r5l_log *log)
{
	bool do_flush;

	if (!log || !log->need_cache_flush)
		return;

	spin_lock_irq(&log->io_list_lock);
	/* flush bio is running */
	if (!list_empty(&log->flushing_ios)) {
		spin_unlock_irq(&log->io_list_lock);
		return;
	}
	list_splice_tail_init(&log->io_end_ios, &log->flushing_ios);
	do_flush = !list_empty(&log->flushing_ios);
	spin_unlock_irq(&log->io_list_lock);

	if (!do_flush)
		return;
	bio_reset(&log->flush_bio);
	log->flush_bio.bi_bdev = log->rdev->bdev;
	log->flush_bio.bi_end_io = r5l_log_flush_endio;
	submit_bio(WRITE_FLUSH, &log->flush_bio);
}

static void r5l_write_super(struct r5l_log *log, sector_t cp);
static void r5l_write_super_and_discard_space(struct r5l_log *log,
	sector_t end)
{
	struct block_device *bdev = log->rdev->bdev;
	struct mddev *mddev;

	r5l_write_super(log, end);

	if (!blk_queue_discard(bdev_get_queue(bdev)))
		return;

	mddev = log->rdev->mddev;
	/*
	 * This is to avoid a deadlock. r5l_quiesce holds reconfig_mutex and
	 * wait for this thread to finish. This thread waits for
	 * MD_CHANGE_PENDING clear, which is supposed to be done in
	 * md_check_recovery(). md_check_recovery() tries to get
	 * reconfig_mutex. Since r5l_quiesce already holds the mutex,
	 * md_check_recovery() fails, so the PENDING never get cleared. The
	 * in_teardown check workaround this issue.
	 */
	if (!log->in_teardown) {
		set_bit(MD_CHANGE_DEVS, &mddev->flags);
		set_bit(MD_CHANGE_PENDING, &mddev->flags);
		md_wakeup_thread(mddev->thread);
		wait_event(mddev->sb_wait,
			!test_bit(MD_CHANGE_PENDING, &mddev->flags) ||
			log->in_teardown);
		/*
		 * r5l_quiesce could run after in_teardown check and hold
		 * mutex first. Superblock might get updated twice.
		 */
		if (log->in_teardown)
			md_update_sb(mddev, 1);
	} else {
		WARN_ON(!mddev_is_locked(mddev));
		md_update_sb(mddev, 1);
	}

	/* discard IO error really doesn't matter, ignore it */
	if (log->last_checkpoint < end) {
		blkdev_issue_discard(bdev,
				log->last_checkpoint + log->rdev->data_offset,
				end - log->last_checkpoint, GFP_NOIO, 0);
	} else {
		blkdev_issue_discard(bdev,
				log->last_checkpoint + log->rdev->data_offset,
				log->device_size - log->last_checkpoint,
				GFP_NOIO, 0);
		blkdev_issue_discard(bdev, log->rdev->data_offset, end,
				GFP_NOIO, 0);
	}
	mutex_lock(&log->io_mutex);
	log->last_checkpoint = end;
	log->last_cp_seq = log->next_cp_seq;
	mutex_unlock(&log->io_mutex);
}

static void r5l_do_reclaim(struct r5l_log *log)
{
	sector_t reclaim_target = xchg(&log->reclaim_target, 0);
	sector_t reclaimable;
	sector_t next_checkpoint;
	u64 next_cp_seq;

	spin_lock_irq(&log->io_list_lock);
	/*
	 * move proper io_unit to reclaim list. We should not change the order.
	 * reclaimable/unreclaimable io_unit can be mixed in the list, we
	 * shouldn't reuse space of an unreclaimable io_unit
	 */
	while (1) {
		reclaimable = r5l_reclaimable_space(log);
		if (reclaimable >= reclaim_target ||
		    (list_empty(&log->running_ios) &&
		     list_empty(&log->io_end_ios) &&
		     list_empty(&log->flushing_ios) &&
		     list_empty(&log->finished_ios)))
			break;

		md_wakeup_thread(log->rdev->mddev->thread);
		wait_event_lock_irq(log->iounit_wait,
				    r5l_reclaimable_space(log) > reclaimable,
				    log->io_list_lock);
	}

	next_checkpoint = log->next_checkpoint;
	next_cp_seq = log->next_cp_seq;
	spin_unlock_irq(&log->io_list_lock);

	BUG_ON(reclaimable < 0);
	if (reclaimable == 0)
		return;

	r5l_run_no_space_stripes(log);
}

static void r5c_update_super(struct r5conf *conf)
{
	struct list_head *l;
	struct stripe_head *sh;
	struct r5l_log *log = conf->log;
	sector_t end = -1L;
	unsigned long flags;

	if (list_empty(&conf->log->cache.stripe_in_cache)) {
		/* all stripes flushed */
		r5l_write_super_and_discard_space(log, log->next_checkpoint);
		return;
	}
	spin_lock_irqsave(&log->cache.stripe_in_cache_lock, flags);
	l = conf->log->cache.stripe_in_cache.next;
	sh = list_entry(l, struct stripe_head, r5c);
	spin_lock(&sh->stripe_lock);
	end = sh->journal_start;
	spin_unlock(&sh->stripe_lock);
	spin_unlock_irqrestore(&log->cache.stripe_in_cache_lock, flags);

	if (end != log->last_checkpoint && end != -1L)
		r5l_write_super_and_discard_space(log, sh->journal_start);
}

static void r5l_reclaim_thread(struct md_thread *thread)
{
	struct mddev *mddev = thread->mddev;
	struct r5conf *conf = mddev->private;
	struct r5l_log *log = conf->log;

	if (!log)
		return;

	r5c_do_reclaim(conf);
	r5l_do_reclaim(log);
	r5c_update_super(conf);
	md_wakeup_thread(mddev->thread);
}

void r5l_wake_reclaim(struct r5l_log *log, sector_t space)
{
	unsigned long target;
	unsigned long new = (unsigned long)space; /* overflow in theory */

	do {
		target = log->reclaim_target;
		if (new < target)
			return;
	} while (cmpxchg(&log->reclaim_target, target, new) != target);
	md_wakeup_thread(log->reclaim_thread);
}

void r5l_quiesce(struct r5l_log *log, int state)
{
	struct mddev *mddev;
	if (!log || state == 2)
		return;
	if (state == 0) {
		log->in_teardown = 0;
		/*
		 * This is a special case for hotadd. In suspend, the array has
		 * no journal. In resume, journal is initialized as well as the
		 * reclaim thread.
		 */
		if (log->reclaim_thread)
			return;
		log->reclaim_thread = md_register_thread(r5l_reclaim_thread,
					log->rdev->mddev, "reclaim");
	} else if (state == 1) {
		/*
		 * at this point all stripes are finished, so io_unit is at
		 * least in STRIPE_END state
		 */
		log->in_teardown = 1;
		/* make sure r5l_write_super_and_discard_space exits */
		mddev = log->rdev->mddev;
		wake_up(&mddev->sb_wait);
		r5l_wake_reclaim(log, -1L);
		md_unregister_thread(&log->reclaim_thread);
		r5l_do_reclaim(log);
		r5c_update_super(log->rdev->mddev->private);
	}
}

bool r5l_log_disk_error(struct r5conf *conf)
{
	struct r5l_log *log;
	bool ret;
	/* don't allow write if journal disk is missing */
	rcu_read_lock();
	log = rcu_dereference(conf->log);

	if (!log)
		ret = test_bit(MD_HAS_JOURNAL, &conf->mddev->flags);
	else
		ret = test_bit(Faulty, &log->rdev->flags);
	rcu_read_unlock();
	return ret;
}

struct r5l_recovery_ctx {
	struct page *meta_page;		/* current meta */
	sector_t meta_total_blocks;	/* total size of current meta and data */
	sector_t pos;			/* recovery position */
	u64 seq;			/* recovery position seq */
};

static int r5l_read_meta_block(struct r5l_log *log,
			       struct r5l_recovery_ctx *ctx)
{
	struct page *page = ctx->meta_page;
	struct r5l_meta_block *mb;
	u32 crc, stored_crc;

	if (!sync_page_io(log->rdev, ctx->pos, PAGE_SIZE, page, READ, false))
		return -EIO;

	mb = page_address(page);
	stored_crc = le32_to_cpu(mb->checksum);
	mb->checksum = 0;

	if (le32_to_cpu(mb->magic) != R5LOG_MAGIC ||
	    le64_to_cpu(mb->seq) != ctx->seq ||
	    mb->version != R5LOG_VERSION ||
	    le64_to_cpu(mb->position) != ctx->pos)
		return -EINVAL;

	crc = crc32c_le(log->uuid_checksum, mb, PAGE_SIZE);
	if (stored_crc != crc)
		return -EINVAL;

	if (le32_to_cpu(mb->meta_size) > PAGE_SIZE)
		return -EINVAL;

	ctx->meta_total_blocks = BLOCK_SECTORS;

	return 0;
}

static int r5l_recovery_flush_one_stripe(struct r5l_log *log,
					 struct r5l_recovery_ctx *ctx,
					 sector_t stripe_sect,
					 int *offset, sector_t *log_offset)
{
	struct r5conf *conf = log->rdev->mddev->private;
	struct stripe_head *sh;
	struct r5l_payload_data_parity *payload;
	int disk_index;

	sh = raid5_get_active_stripe(conf, stripe_sect, 0, 0, 0);
	while (1) {
		payload = page_address(ctx->meta_page) + *offset;

		if (le16_to_cpu(payload->header.type) == R5LOG_PAYLOAD_DATA) {
			raid5_compute_sector(conf,
					     le64_to_cpu(payload->location), 0,
					     &disk_index, sh);

			sync_page_io(log->rdev, *log_offset, PAGE_SIZE,
				     sh->dev[disk_index].page, READ, false);
			sh->dev[disk_index].log_checksum =
				le32_to_cpu(payload->checksum[0]);
			set_bit(R5_Wantwrite, &sh->dev[disk_index].flags);
			ctx->meta_total_blocks += BLOCK_SECTORS;
		} else {
			disk_index = sh->pd_idx;
			sync_page_io(log->rdev, *log_offset, PAGE_SIZE,
				     sh->dev[disk_index].page, READ, false);
			sh->dev[disk_index].log_checksum =
				le32_to_cpu(payload->checksum[0]);
			set_bit(R5_Wantwrite, &sh->dev[disk_index].flags);

			if (sh->qd_idx >= 0) {
				disk_index = sh->qd_idx;
				sync_page_io(log->rdev,
					     r5l_ring_add(log, *log_offset, BLOCK_SECTORS),
					     PAGE_SIZE, sh->dev[disk_index].page,
					     READ, false);
				sh->dev[disk_index].log_checksum =
					le32_to_cpu(payload->checksum[1]);
				set_bit(R5_Wantwrite,
					&sh->dev[disk_index].flags);
			}
			ctx->meta_total_blocks += BLOCK_SECTORS * conf->max_degraded;
		}

		*log_offset = r5l_ring_add(log, *log_offset,
					   le32_to_cpu(payload->size));
		*offset += sizeof(struct r5l_payload_data_parity) +
			sizeof(__le32) *
			(le32_to_cpu(payload->size) >> (PAGE_SHIFT - 9));
		if (le16_to_cpu(payload->header.type) == R5LOG_PAYLOAD_PARITY)
			break;
	}

	for (disk_index = 0; disk_index < sh->disks; disk_index++) {
		void *addr;
		u32 checksum;

		if (!test_bit(R5_Wantwrite, &sh->dev[disk_index].flags))
			continue;
		addr = kmap_atomic(sh->dev[disk_index].page);
		checksum = crc32c_le(log->uuid_checksum, addr, PAGE_SIZE);
		kunmap_atomic(addr);
		if (checksum != sh->dev[disk_index].log_checksum)
			goto error;
	}

	for (disk_index = 0; disk_index < sh->disks; disk_index++) {
		struct md_rdev *rdev, *rrdev;

		if (!test_and_clear_bit(R5_Wantwrite,
					&sh->dev[disk_index].flags))
			continue;

		/* in case device is broken */
		rdev = rcu_dereference(conf->disks[disk_index].rdev);
		if (rdev)
			sync_page_io(rdev, stripe_sect, PAGE_SIZE,
				     sh->dev[disk_index].page, WRITE, false);
		rrdev = rcu_dereference(conf->disks[disk_index].replacement);
		if (rrdev)
			sync_page_io(rrdev, stripe_sect, PAGE_SIZE,
				     sh->dev[disk_index].page, WRITE, false);
	}
	raid5_release_stripe(sh);
	return 0;

error:
	for (disk_index = 0; disk_index < sh->disks; disk_index++)
		sh->dev[disk_index].flags = 0;
	raid5_release_stripe(sh);
	return -EINVAL;
}

static int r5l_recovery_flush_one_meta(struct r5l_log *log,
				       struct r5l_recovery_ctx *ctx)
{
	struct r5conf *conf = log->rdev->mddev->private;
	struct r5l_payload_data_parity *payload;
	struct r5l_meta_block *mb;
	int offset;
	sector_t log_offset;
	sector_t stripe_sector;

	mb = page_address(ctx->meta_page);
	offset = sizeof(struct r5l_meta_block);
	log_offset = r5l_ring_add(log, ctx->pos, BLOCK_SECTORS);

	while (offset < le32_to_cpu(mb->meta_size)) {
		int dd;

		payload = (void *)mb + offset;
		stripe_sector = raid5_compute_sector(conf,
						     le64_to_cpu(payload->location), 0, &dd, NULL);
		if (r5l_recovery_flush_one_stripe(log, ctx, stripe_sector,
						  &offset, &log_offset))
			return -EINVAL;
	}
	return 0;
}

/* copy data/parity from log to raid disks */
static void r5l_recovery_flush_log(struct r5l_log *log,
				   struct r5l_recovery_ctx *ctx)
{
	while (1) {
		if (r5l_read_meta_block(log, ctx))
			return;
		if (r5l_recovery_flush_one_meta(log, ctx))
			return;
		ctx->seq++;
		ctx->pos = r5l_ring_add(log, ctx->pos, ctx->meta_total_blocks);
	}
}

static int r5l_log_write_empty_meta_block(struct r5l_log *log, sector_t pos,
					  u64 seq)
{
	struct page *page;
	struct r5l_meta_block *mb;
	u32 crc;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return -ENOMEM;
	mb = page_address(page);
	mb->magic = cpu_to_le32(R5LOG_MAGIC);
	mb->version = R5LOG_VERSION;
	mb->meta_size = cpu_to_le32(sizeof(struct r5l_meta_block));
	mb->seq = cpu_to_le64(seq);
	mb->position = cpu_to_le64(pos);
	crc = crc32c_le(log->uuid_checksum, mb, PAGE_SIZE);
	mb->checksum = cpu_to_le32(crc);

	if (!sync_page_io(log->rdev, pos, PAGE_SIZE, page, WRITE_FUA, false)) {
		__free_page(page);
		return -EIO;
	}
	__free_page(page);
	return 0;
}

static int r5l_recovery_log(struct r5l_log *log)
{
	struct r5l_recovery_ctx ctx;

	ctx.pos = log->last_checkpoint;
	ctx.seq = log->last_cp_seq;
	ctx.meta_page = alloc_page(GFP_KERNEL);
	if (!ctx.meta_page)
		return -ENOMEM;

	r5l_recovery_flush_log(log, &ctx);
	__free_page(ctx.meta_page);

	/*
	 * we did a recovery. Now ctx.pos points to an invalid meta block. New
	 * log will start here. but we can't let superblock point to last valid
	 * meta block. The log might looks like:
	 * | meta 1| meta 2| meta 3|
	 * meta 1 is valid, meta 2 is invalid. meta 3 could be valid. If
	 * superblock points to meta 1, we write a new valid meta 2n.  if crash
	 * happens again, new recovery will start from meta 1. Since meta 2n is
	 * valid now, recovery will think meta 3 is valid, which is wrong.
	 * The solution is we create a new meta in meta2 with its seq == meta
	 * 1's seq + 10 and let superblock points to meta2. The same recovery will
	 * not think meta 3 is a valid meta, because its seq doesn't match
	 */
	if (ctx.seq > log->last_cp_seq + 1) {
		int ret;

		ret = r5l_log_write_empty_meta_block(log, ctx.pos, ctx.seq + 10);
		if (ret)
			return ret;
		log->seq = ctx.seq + 11;
		log->log_start = r5l_ring_add(log, ctx.pos, BLOCK_SECTORS);
		r5l_write_super(log, ctx.pos);
	} else {
		log->log_start = ctx.pos;
		log->seq = ctx.seq;
	}
	return 0;
}

static void r5l_write_super(struct r5l_log *log, sector_t cp)
{
	struct mddev *mddev = log->rdev->mddev;

	log->rdev->journal_tail = cp;
	set_bit(MD_CHANGE_DEVS, &mddev->flags);
}

/* TODO: use async copy */
static void r5c_copy_data_to_bvec(struct r5dev *rdev, int sh_offset,
				  struct bio_vec *bvec, int bvec_offset, int len)
{
	/* We always copy data from orig_page. This is because in R-M-W, we use
	 * page to do prexor of parity */
	void *src_p = kmap_atomic(rdev->orig_page);
	void *dst_p = kmap_atomic(bvec->bv_page);
	memcpy(dst_p + bvec_offset, src_p + sh_offset, len);
	kunmap_atomic(dst_p);
	kunmap_atomic(src_p);
}

/*
 * copy data from a chunk_map to a bio
 */
static void r5c_copy_chunk_map_to_bio(struct r5c_chunk_map *chunk_map,
			 struct bio *bio)
{
	struct bvec_iter iter;
	struct bio_vec bvec;
	int sh_idx;
	unsigned sh_offset;

	sh_idx = 0;
	sh_offset = (bio->bi_iter.bi_sector & ((sector_t)STRIPE_SECTORS-1)) << 9;

	/*
	 * If bio is not page aligned, the chunk_map will have 1 more sh than bvecs
	 * in the bio. Chunk_map may also have NULL-sh. To copy the right data, we
	 * need to walk through the chunk_map carefully. In this implementation,
	 * bvec/bvec_offset always matches with sh_array[sh_idx]/sh_offset.
	 *
	 * In the following example, the nested loop will run 4 times; and
	 * r5c_copy_data_to_bvec will be called for the first and last iteration.
	 *
	 *             --------------------------------
	 * chunk_map   | valid sh |  NULL  | valid sh |
	 *             --------------------------------
	 *                   ---------------------
	 * bio               |         |         |
	 *                   ---------------------
	 *
	 *                   |    |    |   |     |
	 * copy_data         | Y  | N  | N |  Y  |
	 */
	bio_for_each_segment(bvec, bio, iter) {
		int len;
		unsigned bvec_offset = bvec.bv_offset;
		while (bvec_offset < PAGE_SIZE) {
			len = min_t(unsigned, PAGE_SIZE - bvec_offset, PAGE_SIZE - sh_offset);
			if (chunk_map->sh_array[sh_idx])
				r5c_copy_data_to_bvec(&chunk_map->sh_array[sh_idx]->dev[chunk_map->dd_idx], sh_offset,
						      &bvec, bvec_offset, len);
			bvec_offset += len;
			sh_offset += len;
			if (sh_offset == PAGE_SIZE) {
				sh_idx += 1;
				sh_offset = 0;
			}
		}
	}
	return;
}

/*
 * release stripes in chunk_map and free the chunk_map
 */
static void free_r5c_chunk_map(struct r5c_chunk_map *chunk_map)
{
	unsigned sh_idx;
	struct stripe_head *sh;

	for (sh_idx = 0; sh_idx < chunk_map->sh_count; ++sh_idx) {
		sh = chunk_map->sh_array[sh_idx];
		if (sh) {
			set_bit(STRIPE_HANDLE, &sh->state);
			raid5_release_stripe(sh);
		}
	}
	kfree(chunk_map);
}

static void r5c_chunk_aligned_read_endio(struct bio *bio)
{
	struct r5c_chunk_map *chunk_map = (struct r5c_chunk_map *) bio->bi_private;
	struct bio *parent_bi = chunk_map->parent_bi;

	r5c_copy_chunk_map_to_bio(chunk_map, bio);
	free_r5c_chunk_map(chunk_map);
	bio_put(bio);
	bio_endio(parent_bi);
}

/*
 * look up bio in stripe cache
 * return raid_bio	-> no data in cache, read the chunk from disk
 * return new r5c_bio	-> partial data in cache, read from disk, and amend in r5c_align_endio
 * return NULL		-> all data in cache, no need to read disk
 */
struct bio *r5c_lookup_chunk(struct r5l_log *log, struct bio *raid_bio)
{
	struct r5conf *conf;
	sector_t logical_sector;
	sector_t first_stripe, last_stripe;  /* first (inclusive) stripe and last (exclusive) */
	int dd_idx;
	struct stripe_head *sh;
	unsigned sh_count, sh_idx, sh_cached;
	struct r5c_chunk_map *chunk_map;
	struct bio *r5c_bio;
	int hash;
	unsigned long flags;

	if (!log)
		return raid_bio;

	conf = log->rdev->mddev->private;

	logical_sector = raid_bio->bi_iter.bi_sector &
		~((sector_t)STRIPE_SECTORS-1);
	sh_count = DIV_ROUND_UP_SECTOR_T(bio_end_sector(raid_bio) - logical_sector, STRIPE_SECTORS);

	first_stripe = raid5_compute_sector(conf, logical_sector, 0, &dd_idx, NULL);
	last_stripe = first_stripe + STRIPE_SECTORS * sh_count;

	chunk_map = kzalloc(sizeof(struct r5c_chunk_map) + sh_count * sizeof(struct stripe_head*), GFP_NOIO);
	sh_cached = 0;

	for (sh_idx = 0; sh_idx < sh_count; ++sh_idx) {
		hash = stripe_hash_locks_hash(first_stripe + sh_idx * STRIPE_SECTORS);
		spin_lock_irqsave(conf->hash_locks + hash, flags);
		sh = __find_stripe(conf, first_stripe + sh_idx * STRIPE_SECTORS, conf->generation);
		if (sh && test_bit(R5_UPTODATE, &sh->dev[dd_idx].flags)) {
			if (!atomic_inc_not_zero(&sh->count)) {
				spin_lock(&conf->device_lock);
				if (!atomic_read(&sh->count)) {
					if (!test_bit(STRIPE_HANDLE, &sh->state))
						atomic_inc(&conf->active_stripes);
					BUG_ON(list_empty(&sh->lru) &&
					       !test_bit(STRIPE_EXPANDING, &sh->state));
					list_del_init(&sh->lru);
					if (sh->group) {
						sh->group->stripes_cnt--;
						sh->group = NULL;
					}
				}
				atomic_inc(&sh->count);
				spin_unlock(&conf->device_lock);
			}
			chunk_map->sh_array[sh_idx] = sh;
			++sh_cached;
		}
		spin_unlock_irqrestore(conf->hash_locks + hash, flags);
	}

	if (sh_cached == 0) {
		atomic64_inc(&log->cache.read_misses);
		kfree(chunk_map);
		return raid_bio;
	}

	chunk_map->sh_count = sh_count;
	chunk_map->dd_idx = dd_idx;

	if (sh_cached == sh_count) {
		atomic64_inc(&log->cache.read_full_hits);
		r5c_copy_chunk_map_to_bio(chunk_map, raid_bio);
		free_r5c_chunk_map(chunk_map);
		bio_endio(raid_bio);
		return NULL;
	}

	chunk_map->parent_bi = raid_bio;
	chunk_map->conf = conf;

	atomic64_inc(&log->cache.read_partial_hits);

	/* TODO: handle bio_clone failure? */
	r5c_bio = bio_clone_mddev(raid_bio, GFP_NOIO, log->rdev->mddev);

	r5c_bio->bi_private = chunk_map;
	r5c_bio->bi_end_io = r5c_chunk_aligned_read_endio;

	return r5c_bio;
}

ssize_t
r5c_stat_show(struct mddev *mddev, char* page)
{
	struct r5conf *conf = mddev->private;
	struct r5l_log *log;
	int ret = 0;

	if (!conf)
		return 0;

	log = conf->log;

	if (!log)
		return 0;

	ret += snprintf(page + ret, PAGE_SIZE - ret, "r5c_read_full_hits: %llu\n",
			(unsigned long long) atomic64_read(&log->cache.read_full_hits));

	ret += snprintf(page + ret, PAGE_SIZE - ret, "r5c_read_partial_hits: %llu\n",
			(unsigned long long) atomic64_read(&log->cache.read_partial_hits));

	ret += snprintf(page + ret, PAGE_SIZE - ret, "r5c_read_misses: %llu\n",
			(unsigned long long) atomic64_read(&log->cache.read_misses));

	return ret;
}

static void r5c_flush_stripe(struct r5conf *conf, struct stripe_head *sh)
{
	list_del_init(&sh->lru);
	r5c_freeze_stripe_for_reclaim(sh);
	atomic_inc(&conf->active_stripes);
	atomic_inc(&sh->count);
	set_bit(STRIPE_HANDLE, &sh->state);
	raid5_release_stripe(sh);
}

int r5c_flush_cache(struct r5conf *conf)
{
	int count = 0;

	if (!conf->log)
		return 0;
	while(!list_empty(&conf->r5c_cached_list)) {
		struct list_head *l = conf->r5c_cached_list.next;
		struct stripe_head *sh;
		sh = list_entry(l, struct stripe_head, lru);
		r5c_flush_stripe(conf, sh);
		++count;
	}
	return count;
}

ssize_t
r5c_cached_stripes_show(struct mddev *mddev, char* page)
{
	struct r5conf *conf = mddev->private;
	int ret = 0;

	if (!conf)
		return 0;

	ret += snprintf(page + ret, PAGE_SIZE - ret, "r5c_cached_stripes: %llu\n",
			(unsigned long long) atomic_read(&conf->r5c_cached_stripes));
	return ret;
}

ssize_t r5l_show_need_cache_flush(struct mddev *mddev, char *page)
{
	struct r5conf *conf = mddev->private;
	struct r5l_log *log = conf->log;
	int ret = 0;
	int val;

	if (!log)
		val = 0;
	else
		val = log->need_cache_flush;

	ret += snprintf(page + ret, PAGE_SIZE - ret, "%d\n", val);
	return ret;
}

ssize_t r5l_store_need_cache_flush(struct mddev *mddev, const char *page, size_t len)
{
	struct r5conf *conf = mddev->private;
	struct r5l_log *log = conf->log;
	int val;

	if (!log)
		return -EINVAL;

	if (kstrtoint(page, 10, &val))
		return -EINVAL;

	if (val > 1 || val < 0)
		return -EINVAL;

	log->need_cache_flush = val;
	return len;
}

ssize_t
r5c_cached_stripes_store(struct mddev *mddev, const char *page, size_t len)
{
	struct r5conf *conf = mddev->private;

	spin_lock_irq(&conf->device_lock);
	r5c_flush_cache(conf);  /* flush cache regardless of any input, TODO: change this*/
	spin_unlock_irq(&conf->device_lock);

	md_wakeup_thread(mddev->thread);
	return len;
}

ssize_t r5c_show_cache_mode(struct mddev *mddev, char *page)
{
	struct r5conf *conf = mddev->private;
	int val = 0;
	int ret = 0;

	if (conf->log)
		val = conf->log->cache.mode;
	ret += snprintf(page, PAGE_SIZE - ret, "%d: %s\n", val, r5c_cache_mode_str[val]);
	return ret;
}

ssize_t r5c_store_cache_mode(struct mddev *mddev, const char *page, size_t len)
{
	struct r5conf *conf = mddev->private;
	int val;

	if (!conf->log)
		return -EINVAL;
	if (kstrtoint(page, 10, &val))
		return -EINVAL;
	if (val < R5C_MODE_WRITE_THROUGH || val > R5C_MODE_WRITE_BACK)
		return -EINVAL;
	spin_lock_irq(&conf->device_lock);
	conf->log->cache.mode = val;
	spin_unlock_irq(&conf->device_lock);
	printk(KERN_INFO "%s: setting r5c cache mode to %d: %s\n", mdname(mddev), val, r5c_cache_mode_str[val]);
	return len;
}

int r5c_handle_stripe_dirtying(struct r5conf *conf,
			       struct stripe_head *sh,
			       struct stripe_head_state *s,
			       int disks) {
	struct r5l_log *log = conf->log;
	int i;
	struct r5dev *dev;

	if (!log || sh->r5c_state >= R5C_STATE_FROZEN)
		return -EAGAIN;

	if (conf->log->cache.mode == R5C_MODE_WRITE_THROUGH || conf->quiesce != 0 || conf->mddev->degraded != 0) {
		/* write through mode */
		r5c_freeze_stripe_for_reclaim(sh);
		return -EAGAIN;
	}

	s->to_cache = 0;

	for (i = disks; i--; ) {
		dev = &sh->dev[i];
		/* if none-overwrite, use the reclaim path (write through) */
		if (dev->towrite && !test_bit(R5_OVERWRITE, &dev->flags) &&
		    !test_bit(R5_InCache, &dev->flags)) {
			r5c_freeze_stripe_for_reclaim(sh);
			return -EAGAIN;
		}
	}

	for (i = disks; i--; ) {
		dev = &sh->dev[i];
		if (dev->towrite) {
			set_bit(R5_Wantcache, &dev->flags);
			set_bit(R5_Wantdrain, &dev->flags);
			set_bit(R5_LOCKED, &dev->flags);
			s->to_cache++;
		}
	}

	if (s->to_cache)
		set_bit(STRIPE_OP_BIODRAIN, &s->ops_request);

	return 0;
}

void r5c_handle_stripe_flush(struct r5conf *conf,
			     struct stripe_head *sh,
			     struct stripe_head_state *s,
			     int disks) {
	int i;
	int do_wakeup = 0;
	unsigned long flags;

	if (sh->r5c_state == R5C_STATE_PARITY_DONE) {
		r5c_set_state(sh, R5C_STATE_INRAID);
		for (i = disks; i--; ) {
			clear_bit(R5_InCache, &sh->dev[i].flags);
			clear_bit(R5_UPTODATE, &sh->dev[i].flags);
			if (test_and_clear_bit(R5_Overlap, &sh->dev[i].flags))
				do_wakeup = 1;
		}
		spin_lock_irqsave(&conf->log->cache.stripe_in_cache_lock, flags);
		list_del_init(&sh->r5c);
		spin_unlock_irqrestore(&conf->log->cache.stripe_in_cache_lock, flags);
		spin_lock_irqsave(&sh->stripe_lock, flags);
		sh->journal_start = -1L;
		spin_unlock_irqrestore(&sh->stripe_lock, flags);
	}
	if (do_wakeup)
		wake_up(&conf->wait_for_overlap);
}

int
r5c_cache_data(struct r5l_log *log, struct stripe_head *sh,
	       struct stripe_head_state *s)
{
	int pages;
	int meta_size;
	int reserve;
	int i;
	int ret = 0;
	int page_count = 0;

	BUG_ON(!log);
	BUG_ON(s->to_cache == 0);

	for (i = 0; i < sh->disks; i++) {
		void *addr;
		if (!test_bit(R5_Wantcache, &sh->dev[i].flags))
			continue;
		addr = kmap_atomic(sh->dev[i].page);
		sh->dev[i].log_checksum = crc32c_le(log->uuid_checksum,
						    addr, PAGE_SIZE);
		kunmap_atomic(addr);
		page_count++;
	}
	WARN_ON(page_count != s->to_cache);

	pages = s->to_cache;

	meta_size =
		((sizeof(struct r5l_payload_data_parity) + sizeof(__le32))
		 * pages);
	/* Doesn't work with very big raid array */
	if (meta_size + sizeof(struct r5l_meta_block) > PAGE_SIZE)
		return -EINVAL;

	/*
	 * The stripe must enter state machine again to call endio, so
	 * don't delay.
	 */
	clear_bit(STRIPE_DELAYED, &sh->state);
	atomic_inc(&sh->count);

	mutex_lock(&log->io_mutex);
	/* meta + data */
	reserve = (1 + pages) << (PAGE_SHIFT - 9);
	if (!r5l_has_free_space(log, reserve)) {
		spin_lock(&log->no_space_stripes_lock);
		list_add_tail(&sh->log_list, &log->no_space_stripes);
		spin_unlock(&log->no_space_stripes_lock);

		r5l_wake_reclaim(log, reserve);
	} else {
		ret = r5l_log_stripe(log, sh, pages, 0);
		if (ret) {
			spin_lock_irq(&log->io_list_lock);
			list_add_tail(&sh->log_list, &log->no_mem_stripes);
			spin_unlock_irq(&log->io_list_lock);
		}
	}

	mutex_unlock(&log->io_mutex);
	return 0;
}

static void r5c_adjust_flush_threshold(struct r5conf *conf)
{
	struct r5l_log *log = conf->log;
	int new_thres = conf->raid_disks - conf->max_degraded;

	if (atomic_read(&conf->r5c_cached_stripes) * 2 > conf->max_nr_stripes)
		new_thres = 1;
	else if (atomic_read(&conf->r5c_cached_stripes) * 4 > conf->max_nr_stripes)
		new_thres /= 2;
	else if (atomic_read(&conf->r5c_cached_stripes) * 8 > conf->max_nr_stripes)
		new_thres -= 1;

	if (test_bit(R5_INACTIVE_BLOCKED, &conf->cache_state))
		new_thres = 1;

	if (new_thres >= 1)
		log->cache.flush_threshold = new_thres;
}

void r5c_do_reclaim(struct r5conf *conf)
{
	struct stripe_head *sh, *next;
	struct r5l_log *log = conf->log;
	int count = 0;
	unsigned long flags;

	if (!log)
		return;

	spin_lock_irqsave(&conf->device_lock, flags);
	r5c_adjust_flush_threshold(conf);
	list_for_each_entry_safe(sh, next, &conf->r5c_cached_list, lru) {
		if (atomic_read(&sh->dev_in_cache) >= log->cache.flush_threshold) {
			count ++;
			r5c_flush_stripe(conf, sh);
		}
	}
	spin_unlock_irqrestore(&conf->device_lock, flags);
	if (test_bit(R5_INACTIVE_BLOCKED, &conf->cache_state))
		wake_up(&conf->wait_for_overlap);
}

static int r5l_load_log(struct r5l_log *log)
{
	struct md_rdev *rdev = log->rdev;
	struct page *page;
	struct r5l_meta_block *mb;
	sector_t cp = log->rdev->journal_tail;
	u32 stored_crc, expected_crc;
	bool create_super = false;
	int ret;

	/* Make sure it's valid */
	if (cp >= rdev->sectors || round_down(cp, BLOCK_SECTORS) != cp)
		cp = 0;
	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	if (!sync_page_io(rdev, cp, PAGE_SIZE, page, READ, false)) {
		ret = -EIO;
		goto ioerr;
	}
	mb = page_address(page);

	if (le32_to_cpu(mb->magic) != R5LOG_MAGIC ||
	    mb->version != R5LOG_VERSION) {
		create_super = true;
		goto create;
	}
	stored_crc = le32_to_cpu(mb->checksum);
	mb->checksum = 0;
	expected_crc = crc32c_le(log->uuid_checksum, mb, PAGE_SIZE);
	if (stored_crc != expected_crc) {
		create_super = true;
		goto create;
	}
	if (le64_to_cpu(mb->position) != cp) {
		create_super = true;
		goto create;
	}
create:
	if (create_super) {
		log->last_cp_seq = prandom_u32();
		cp = 0;
		/*
		 * Make sure super points to correct address. Log might have
		 * data very soon. If super hasn't correct log tail address,
		 * recovery can't find the log
		 */
		r5l_write_super(log, cp);
	} else
		log->last_cp_seq = le64_to_cpu(mb->seq);

	log->device_size = round_down(rdev->sectors, BLOCK_SECTORS);
	log->max_free_space = log->device_size >> RECLAIM_MAX_FREE_SPACE_SHIFT;
	if (log->max_free_space > RECLAIM_MAX_FREE_SPACE)
		log->max_free_space = RECLAIM_MAX_FREE_SPACE;
	log->last_checkpoint = cp;

	__free_page(page);

	return r5l_recovery_log(log);
ioerr:
	__free_page(page);
	return ret;
}

int r5l_init_log(struct r5conf *conf, struct md_rdev *rdev)
{
	struct r5l_log *log;

	if (PAGE_SIZE != 4096)
		return -EINVAL;
	log = kzalloc(sizeof(*log), GFP_KERNEL);
	if (!log)
		return -ENOMEM;
	log->rdev = rdev;

	log->need_cache_flush = (rdev->bdev->bd_disk->queue->flush_flags != 0);

	log->uuid_checksum = crc32c_le(~0, rdev->mddev->uuid,
				       sizeof(rdev->mddev->uuid));

	mutex_init(&log->io_mutex);

	spin_lock_init(&log->io_list_lock);
	INIT_LIST_HEAD(&log->running_ios);
	INIT_LIST_HEAD(&log->io_end_ios);
	INIT_LIST_HEAD(&log->flushing_ios);
	INIT_LIST_HEAD(&log->finished_ios);
	bio_init(&log->flush_bio);

	log->io_kc = KMEM_CACHE(r5l_io_unit, 0);
	if (!log->io_kc)
		goto io_kc;

	log->io_pool = mempool_create_slab_pool(R5L_POOL_SIZE, log->io_kc);
	if (!log->io_pool)
		goto io_pool;

	log->bs = bioset_create(R5L_POOL_SIZE, 0);
	if (!log->bs)
		goto io_bs;

	log->meta_pool = mempool_create_page_pool(R5L_POOL_SIZE, 0);
	if (!log->meta_pool)
		goto out_mempool;

	log->reclaim_thread = md_register_thread(r5l_reclaim_thread,
						 log->rdev->mddev, "reclaim");
	if (!log->reclaim_thread)
		goto reclaim_thread;
	log->reclaim_thread->timeout = RECLAIM_WAKEUP_INTERVAL;

	init_waitqueue_head(&log->iounit_wait);

	INIT_LIST_HEAD(&log->no_mem_stripes);

	INIT_LIST_HEAD(&log->no_space_stripes);
	spin_lock_init(&log->no_space_stripes_lock);

	init_r5c_cache(conf, &log->cache);
	if (r5l_load_log(log))
		goto error;

	rcu_assign_pointer(conf->log, log);
	set_bit(MD_HAS_JOURNAL, &conf->mddev->flags);
	return 0;

error:
	md_unregister_thread(&log->reclaim_thread);
reclaim_thread:
	mempool_destroy(log->meta_pool);
out_mempool:
	bioset_free(log->bs);
io_bs:
	mempool_destroy(log->io_pool);
io_pool:
	kmem_cache_destroy(log->io_kc);
io_kc:
	kfree(log);
	return -EINVAL;
}

void r5l_exit_log(struct r5l_log *log)
{
	md_unregister_thread(&log->reclaim_thread);
	mempool_destroy(log->meta_pool);
	bioset_free(log->bs);
	mempool_destroy(log->io_pool);
	kmem_cache_destroy(log->io_kc);
	kfree(log);
}
