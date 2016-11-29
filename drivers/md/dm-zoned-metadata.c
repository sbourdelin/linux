/*
 * (C) Copyright 2016 Western Digital.
 *
 * This software is distributed under the terms of the GNU Lesser General
 * Public License version 2, or any later version, "as is," without technical
 * support, and WITHOUT ANY WARRANTY, without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Author: Damien Le Moal <damien.lemoal@wdc.com>
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/slab.h>

#include "dm-zoned.h"

/*
 * Allocate a metadata block.
 */
static struct dm_zoned_mblock *dmz_alloc_mblock(struct dm_zoned_target *dzt,
						sector_t mblk_no)
{
	struct dm_zoned_mblock *mblk = NULL;
	unsigned long flags;

	/* See if we can reuse allocated blocks */
	if (dzt->max_nr_mblks &&
	    atomic_read(&dzt->nr_mblks) >= dzt->max_nr_mblks) {

		spin_lock_irqsave(&dzt->mblk_lock, flags);
		if (list_empty(&dzt->mblk_lru_list)) {
			/* Cleanup dirty blocks */
			dmz_trigger_flush(dzt);
		} else {
			mblk = list_first_entry(&dzt->mblk_lru_list,
						struct dm_zoned_mblock, link);
			list_del_init(&mblk->link);
			rb_erase(&mblk->node, &dzt->mblk_rbtree);
			mblk->no = mblk_no;
		}
		spin_unlock_irqrestore(&dzt->mblk_lock, flags);

		if (mblk)
			return mblk;
	}

	/* Allocate a new block */
	mblk = kmalloc(sizeof(struct dm_zoned_mblock), GFP_NOIO);
	if (!mblk)
		return NULL;

	mblk->page = alloc_page(GFP_NOIO);
	if (!mblk->page) {
		kfree(mblk);
		return NULL;
	}

	RB_CLEAR_NODE(&mblk->node);
	INIT_LIST_HEAD(&mblk->link);
	atomic_set(&mblk->ref, 0);
	mblk->state = 0;
	mblk->no = mblk_no;
	mblk->data = page_address(mblk->page);

	atomic_inc(&dzt->nr_mblks);

	return mblk;
}

/*
 * Free a metadata block.
 */
static void dmz_free_mblock(struct dm_zoned_target *dzt,
			    struct dm_zoned_mblock *mblk)
{
	__free_pages(mblk->page, 0);
	kfree(mblk);

	atomic_dec(&dzt->nr_mblks);
}

/*
 * Insert a metadata block in the rbtree.
 */
static void dmz_insert_mblock(struct dm_zoned_target *dzt,
			      struct dm_zoned_mblock *mblk)
{
	struct rb_root *root = &dzt->mblk_rbtree;
	struct rb_node **new = &(root->rb_node), *parent = NULL;
	struct dm_zoned_mblock *b;

	/* Figure out where to put the new node */
	while (*new) {
		b = container_of(*new, struct dm_zoned_mblock, node);
		parent = *new;
		new = (b->no < mblk->no) ?
			&((*new)->rb_left) : &((*new)->rb_right);
	}

	/* Add new node and rebalance tree */
	rb_link_node(&mblk->node, parent, new);
	rb_insert_color(&mblk->node, root);
}

/*
 * Insert a metadata block in the rbtree.
 */
static struct dm_zoned_mblock *dmz_lookup_mblock(struct dm_zoned_target *dzt,
						 sector_t mblk_no)
{
	struct rb_root *root = &dzt->mblk_rbtree;
	struct rb_node *node = root->rb_node;
	struct dm_zoned_mblock *mblk;

	while (node) {
		mblk = container_of(node, struct dm_zoned_mblock, node);
		if (mblk->no == mblk_no)
			return mblk;
		node = (mblk->no < mblk_no) ? node->rb_left : node->rb_right;
	}

	return NULL;
}

/*
 * Metadata block BIO end callback.
 */
static void dmz_mblock_bio_end_io(struct bio *bio)
{
	struct dm_zoned_mblock *mblk = bio->bi_private;
	int flag;

	if (bio->bi_error)
		set_bit(DMZ_META_ERROR, &mblk->state);

	if (bio_op(bio) == REQ_OP_WRITE)
		flag = DMZ_META_WRITING;
	else
		flag = DMZ_META_READING;

	clear_bit_unlock(flag, &mblk->state);
	smp_mb__after_atomic();
	wake_up_bit(&mblk->state, flag);

	bio_put(bio);
}

/*
 * Read a metadata block from disk.
 */
static struct dm_zoned_mblock *dmz_fetch_mblock(struct dm_zoned_target *dzt,
						sector_t mblk_no)
{
	struct dm_zoned_mblock *mblk;
	sector_t block = dzt->sb[dzt->mblk_primary].block + mblk_no;
	unsigned long flags;
	struct bio *bio;

	/* Get block and insert it */
	mblk = dmz_alloc_mblock(dzt, mblk_no);
	if (!mblk)
		return NULL;

	spin_lock_irqsave(&dzt->mblk_lock, flags);
	atomic_inc(&mblk->ref);
	set_bit(DMZ_META_READING, &mblk->state);
	dmz_insert_mblock(dzt, mblk);
	spin_unlock_irqrestore(&dzt->mblk_lock, flags);

	bio = bio_alloc(GFP_NOIO, 1);
	bio->bi_iter.bi_sector = dmz_blk2sect(block);
	bio->bi_bdev = dzt->zbd;
	bio->bi_private = mblk;
	bio->bi_end_io = dmz_mblock_bio_end_io;
	bio_set_op_attrs(bio, REQ_OP_READ, REQ_META | REQ_PRIO);
	bio_add_page(bio, mblk->page, DMZ_BLOCK_SIZE, 0);
	submit_bio(bio);

	return mblk;
}

/*
 * Free metadata blocks.
 */
static void dmz_shrink_mblock_cache(struct dm_zoned_target *dzt)
{
	struct dm_zoned_mblock *mblk;

	if (!dzt->max_nr_mblks)
		return;

	while (atomic_read(&dzt->nr_mblks) > dzt->max_nr_mblks &&
	       !list_empty(&dzt->mblk_lru_list)) {

		mblk = list_first_entry(&dzt->mblk_lru_list,
					struct dm_zoned_mblock, link);
		list_del_init(&mblk->link);
		rb_erase(&mblk->node, &dzt->mblk_rbtree);
		dmz_free_mblock(dzt, mblk);
	}
}

/*
 * Release a metadata block.
 */
static void dmz_release_mblock(struct dm_zoned_target *dzt,
			       struct dm_zoned_mblock *mblk)
{
	unsigned long flags;

	if (!mblk)
		return;

	spin_lock_irqsave(&dzt->mblk_lock, flags);

	if (atomic_dec_and_test(&mblk->ref)) {
		if (test_bit(DMZ_META_ERROR, &mblk->state)) {
			rb_erase(&mblk->node, &dzt->mblk_rbtree);
			dmz_free_mblock(dzt, mblk);
		} else if (!test_bit(DMZ_META_DIRTY, &mblk->state)) {
			list_add_tail(&mblk->link, &dzt->mblk_lru_list);
		}
	}

	dmz_shrink_mblock_cache(dzt);

	spin_unlock_irqrestore(&dzt->mblk_lock, flags);
}

/*
 * Get a metadata block from the rbtree. If the block
 * is not present, read it from disk.
 */
static struct dm_zoned_mblock *dmz_get_mblock(struct dm_zoned_target *dzt,
					      sector_t mblk_no)
{
	struct dm_zoned_mblock *mblk;
	unsigned long flags;

	/* Check rbtree */
	spin_lock_irqsave(&dzt->mblk_lock, flags);
	mblk = dmz_lookup_mblock(dzt, mblk_no);
	if (mblk) {
		/* Cache hit: remove block from LRU list */
		if (atomic_inc_return(&mblk->ref) == 1 &&
		    !test_bit(DMZ_META_DIRTY, &mblk->state))
			list_del_init(&mblk->link);
	}
	spin_unlock_irqrestore(&dzt->mblk_lock, flags);

	if (!mblk) {
		/* Cache miss: read the block from disk */
		mblk = dmz_fetch_mblock(dzt, mblk_no);
		if (!mblk)
			return ERR_PTR(-ENOMEM);
	}

	/* Wait for on-going read I/O and check for error */
	wait_on_bit_io(&mblk->state, DMZ_META_READING,
		       TASK_UNINTERRUPTIBLE);
	if (test_bit(DMZ_META_ERROR, &mblk->state)) {
		dmz_release_mblock(dzt, mblk);
		return ERR_PTR(-EIO);
	}

	return mblk;
}

/*
 * Mark a metadata block dirty.
 */
static void dmz_dirty_mblock(struct dm_zoned_target *dzt,
			     struct dm_zoned_mblock *mblk)
{
	unsigned long flags;

	spin_lock_irqsave(&dzt->mblk_lock, flags);

	if (!test_and_set_bit(DMZ_META_DIRTY, &mblk->state))
		list_add_tail(&mblk->link, &dzt->mblk_dirty_list);

	spin_unlock_irqrestore(&dzt->mblk_lock, flags);
}

/*
 * Issue a metadata block write BIO.
 */
static void dmz_write_mblock(struct dm_zoned_target *dzt,
			     struct dm_zoned_mblock *mblk,
			     unsigned int set)
{
	sector_t block = dzt->sb[set].block + mblk->no;
	struct bio *bio;

	set_bit(DMZ_META_WRITING, &mblk->state);

	bio = bio_alloc(GFP_NOIO, 1);
	bio->bi_iter.bi_sector = dmz_blk2sect(block);
	bio->bi_bdev = dzt->zbd;
	bio->bi_private = mblk;
	bio->bi_end_io = dmz_mblock_bio_end_io;
	bio_set_op_attrs(bio, REQ_OP_WRITE, REQ_META | REQ_PRIO);
	bio_add_page(bio, mblk->page, DMZ_BLOCK_SIZE, 0);
	submit_bio(bio);
}

/*
 * CRC32
 */
static u32 dmz_sb_crc32(u32 crc, const void *buf, size_t length)
{
	unsigned char *p = (unsigned char *)buf;
	int i;

#define CRCPOLY_LE 0xedb88320

	while (length--) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((crc & 1) ? CRCPOLY_LE : 0);
	}

	return crc;
}

/*
 * Sync read/write a block.
 */
static int dmz_rdwr_block_sync(struct dm_zoned_target *dzt,
			       int op,
			       sector_t block,
			       struct page *page)
{
	struct bio *bio;
	int ret;

	bio = bio_alloc(GFP_NOIO, 1);
	bio->bi_iter.bi_sector = dmz_blk2sect(block);
	bio->bi_bdev = dzt->zbd;
	bio_set_op_attrs(bio, op, REQ_SYNC | REQ_META | REQ_PRIO);
	bio_add_page(bio, page, DMZ_BLOCK_SIZE, 0);
	ret = submit_bio_wait(bio);
	bio_put(bio);

	return ret;
}

/*
 * Write super block of the specified metadata set.
 */
static int dmz_write_sb(struct dm_zoned_target *dzt,
			unsigned int set)
{
	sector_t block = dzt->sb[set].block;
	struct dm_zoned_mblock *mblk = dzt->sb[set].mblk;
	struct dm_zoned_super *sb = dzt->sb[set].sb;
	u64 sb_gen = dzt->sb_gen + 1;
	u32 crc;
	int ret;

	sb->magic = cpu_to_le32(DMZ_MAGIC);
	sb->version = cpu_to_le32(DMZ_META_VER);

	sb->gen = cpu_to_le64(sb_gen);

	sb->sb_block = cpu_to_le64(block);
	sb->nr_meta_blocks = cpu_to_le32(dzt->nr_meta_blocks);
	sb->nr_reserved_seq = cpu_to_le32(dzt->nr_reserved_seq);
	sb->nr_chunks = cpu_to_le32(dzt->nr_chunks);

	sb->nr_map_blocks = cpu_to_le32(dzt->nr_map_blocks);
	sb->nr_bitmap_blocks = cpu_to_le32(dzt->nr_bitmap_blocks);

	sb->crc = 0;
	crc = dmz_sb_crc32(sb_gen, sb, DMZ_BLOCK_SIZE);
	sb->crc = cpu_to_le32(crc);

	ret = dmz_rdwr_block_sync(dzt, REQ_OP_WRITE, block, mblk->page);
	if (ret == 0)
		ret = blkdev_issue_flush(dzt->zbd, GFP_KERNEL, NULL);

	return ret;
}

/*
 * Write dirty metadata blocks to the specified set.
 */
static int dmz_write_dirty_mblocks(struct dm_zoned_target *dzt,
				   struct list_head *write_list,
				   unsigned int set)
{
	struct dm_zoned_mblock *mblk;
	struct blk_plug plug;
	int ret = 0;

	/* Issue writes */
	blk_start_plug(&plug);
	list_for_each_entry(mblk, write_list, link)
		dmz_write_mblock(dzt, mblk, set);
	blk_finish_plug(&plug);

	/* Wait for completion */
	list_for_each_entry(mblk, write_list, link) {
		wait_on_bit_io(&mblk->state, DMZ_META_WRITING,
			       TASK_UNINTERRUPTIBLE);
		if (test_bit(DMZ_META_ERROR, &mblk->state)) {
			dmz_dev_err(dzt,
				    "Write metablock %u/%llu failed\n",
				    set,
				    (u64)mblk->no);
			clear_bit(DMZ_META_ERROR, &mblk->state);
			ret = -EIO;
		}
	}

	return ret;
}
/*
 * Log dirty metadata blocks.
 */
static int dmz_log_dirty_mblocks(struct dm_zoned_target *dzt,
				 struct list_head *write_list)
{
	unsigned int log_set = dzt->mblk_primary ^ 0x1;
	int ret;

	/* Write dirty blocks to the log */
	ret = dmz_write_dirty_mblocks(dzt, write_list, log_set);
	if (ret)
		return ret;

	/* Flush drive cache (this will also sync data) */
	ret = blkdev_issue_flush(dzt->zbd, GFP_KERNEL, NULL);
	if (ret)
		return ret;

	/*
	 * No error so far: now validate the log by updating the
	 * log index super block generation.
	 */
	ret = dmz_write_sb(dzt, log_set);
	if (ret)
		return ret;

	return 0;
}

/*
 * Flush dirty metadata blocks.
 */
int dmz_flush_mblocks(struct dm_zoned_target *dzt)
{
	struct dm_zoned_mblock *mblk;
	struct list_head write_list;
	int ret;

	INIT_LIST_HEAD(&write_list);

	/*
	 * Prevent all zone works from running. This ensure exclusive access
	 * to all zones bitmaps. However, the mapping table may still be
	 * modified by incoming write requests. So also take the map lock.
	 */
	down_write(&dzt->mblk_sem);
	dmz_lock_map(dzt);

	if (list_empty(&dzt->mblk_dirty_list)) {
		/* Nothing to do */
		ret = blkdev_issue_flush(dzt->zbd, GFP_KERNEL, NULL);
		goto out;
	}

	dmz_dev_debug(dzt, "FLUSH mblock set %u, gen %llu\n",
		      dzt->mblk_primary ^ 0x1,
		      dzt->sb_gen + 1);

	/*
	 * The primary metadata set is still clean. Keep it this way until
	 * all updates are successful in the secondary set. That is, use
	 * the secondary set as a log.
	 */
	list_splice_init(&dzt->mblk_dirty_list, &write_list);

	ret = dmz_log_dirty_mblocks(dzt, &write_list);
	if (ret)
		goto out;

	/*
	 * The log is on disk. It is now safe to update in place
	 * in the current set.
	 */
	ret = dmz_write_dirty_mblocks(dzt, &write_list, dzt->mblk_primary);
	if (ret)
		goto out;

	ret = dmz_write_sb(dzt, dzt->mblk_primary);
	if (ret)
		goto out;

	while (!list_empty(&write_list)) {
		mblk = list_first_entry(&write_list,
					struct dm_zoned_mblock, link);
		list_del_init(&mblk->link);

		clear_bit(DMZ_META_DIRTY, &mblk->state);
		if (atomic_read(&mblk->ref) == 0)
			list_add_tail(&mblk->link, &dzt->mblk_lru_list);

	}

	dzt->sb_gen++;

out:
	if (ret && !list_empty(&write_list))
		list_splice(&write_list, &dzt->mblk_dirty_list);

	dmz_unlock_map(dzt);
	up_write(&dzt->mblk_sem);

	return ret;
}

/*
 * Check super block.
 */
static int dmz_check_sb(struct dm_zoned_target *dzt,
			struct dm_zoned_super *sb)
{
	unsigned int nr_meta_zones, nr_data_zones;
	u32 crc, stored_crc;
	u64 gen;

	gen = le64_to_cpu(sb->gen);
	stored_crc = le32_to_cpu(sb->crc);
	sb->crc = 0;
	crc = dmz_sb_crc32(gen, sb, DMZ_BLOCK_SIZE);
	if (crc != stored_crc) {
		dmz_dev_err(dzt,
			    "Invalid checksum (needed 0x%08x %08x, got 0x%08x)\n",
			    crc,
			    le32_to_cpu(crc),
			    stored_crc);
		return -ENXIO;
	}

	if (le32_to_cpu(sb->magic) != DMZ_MAGIC) {
		dmz_dev_err(dzt,
			    "Invalid meta magic (need 0x%08x, got 0x%08x)\n",
			    DMZ_MAGIC,
			    le32_to_cpu(sb->magic));
		return -ENXIO;
	}

	if (le32_to_cpu(sb->version) != DMZ_META_VER) {
		dmz_dev_err(dzt,
			    "Invalid meta version (need %d, got %d)\n",
			    DMZ_META_VER,
			    le32_to_cpu(sb->version));
		return -ENXIO;
	}

	nr_meta_zones =
		(le32_to_cpu(sb->nr_meta_blocks) + dzt->zone_nr_blocks - 1)
		>> dzt->zone_nr_blocks_shift;
	if (!nr_meta_zones ||
	    nr_meta_zones >= dzt->nr_rnd_zones) {
		dmz_dev_err(dzt,
			    "Invalid number of metadata blocks\n");
		return -ENXIO;
	}

	if (!le32_to_cpu(sb->nr_reserved_seq) ||
	    le32_to_cpu(sb->nr_reserved_seq) >=
	    (dzt->nr_useable_zones - nr_meta_zones)) {
		dmz_dev_err(dzt,
			    "Invalid number of reserved sequential zones\n");
		return -ENXIO;
	}

	nr_data_zones = dzt->nr_useable_zones -
		(nr_meta_zones * 2 + le32_to_cpu(sb->nr_reserved_seq));
	if (le32_to_cpu(sb->nr_chunks) > nr_data_zones) {
		dmz_dev_err(dzt,
			    "Invalid number of chunks %u / %u\n",
			    le32_to_cpu(sb->nr_chunks),
			    nr_data_zones);
		return -ENXIO;
	}

	/* OK */
	dzt->nr_meta_blocks = le32_to_cpu(sb->nr_meta_blocks);
	dzt->nr_reserved_seq = le32_to_cpu(sb->nr_reserved_seq);
	dzt->nr_chunks = le32_to_cpu(sb->nr_chunks);
	dzt->nr_map_blocks = le32_to_cpu(sb->nr_map_blocks);
	dzt->nr_bitmap_blocks = le32_to_cpu(sb->nr_bitmap_blocks);
	dzt->nr_meta_zones = nr_meta_zones;
	dzt->nr_data_zones = nr_data_zones;

	return 0;
}

/*
 * Read the first or second super block from disk.
 */
static int dmz_read_sb(struct dm_zoned_target *dzt, unsigned int set)
{
	return dmz_rdwr_block_sync(dzt, REQ_OP_READ,
				   dzt->sb[set].block,
				   dzt->sb[set].mblk->page);
}

/*
 * Determine the position of the secondary super blocks on disk.
 * This is used only if a corruption of the primary super block
 * is detected.
 */
static int dmz_lookup_secondary_sb(struct dm_zoned_target *dzt)
{
	struct dm_zoned_mblock *mblk;
	int i;

	/* Allocate a block */
	mblk = dmz_alloc_mblock(dzt, 0);
	if (!mblk)
		return -ENOMEM;

	dzt->sb[1].mblk = mblk;
	dzt->sb[1].sb = mblk->data;

	/* Bad first super block: search for the second one */
	dzt->sb[1].block = dzt->sb[0].block + dzt->zone_nr_blocks;
	for (i = 0; i < dzt->nr_rnd_zones - 1; i++) {
		if (dmz_read_sb(dzt, 1) != 0)
			break;
		if (le32_to_cpu(dzt->sb[1].sb->magic) == DMZ_MAGIC)
			return 0;
		dzt->sb[1].block += dzt->zone_nr_blocks;
	}

	dmz_free_mblock(dzt, mblk);
	dzt->sb[1].mblk = NULL;

	return -EIO;
}

/*
 * Read the first or second super block from disk.
 */
static int dmz_get_sb(struct dm_zoned_target *dzt, unsigned int set)
{
	struct dm_zoned_mblock *mblk;
	int ret;

	/* Allocate a block */
	mblk = dmz_alloc_mblock(dzt, 0);
	if (!mblk)
		return -ENOMEM;

	dzt->sb[set].mblk = mblk;
	dzt->sb[set].sb = mblk->data;

	/* Read super block */
	ret = dmz_read_sb(dzt, set);
	if (ret) {
		dmz_free_mblock(dzt, mblk);
		dzt->sb[set].mblk = NULL;
		return ret;
	}

	return 0;
}

/*
 * Recover a metadata set.
 */
static int dmz_recover_mblocks(struct dm_zoned_target *dzt,
			       unsigned int dst_set)
{
	unsigned int src_set = dst_set ^ 0x1;
	struct page *page;
	int i, ret;

	dmz_dev_warn(dzt,
		     "Metadata set %u invalid: recovering\n",
		     dst_set);

	if (dst_set == 0)
		dzt->sb[0].block = dmz_sect2blk(dzt->sb_zone->sector);
	else
		dzt->sb[1].block = dzt->sb[0].block +
			(dzt->nr_meta_zones * dzt->zone_nr_blocks);

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	/* Copy metadata blocks */
	for (i = 1; i < dzt->nr_meta_blocks; i++) {
		ret = dmz_rdwr_block_sync(dzt, REQ_OP_READ,
					  dzt->sb[src_set].block + i,
					  page);
		if (ret)
			goto out;
		ret = dmz_rdwr_block_sync(dzt, REQ_OP_WRITE,
					  dzt->sb[dst_set].block + i,
					  page);
		if (ret)
			goto out;
	}

	/* Finalize with the super block */
	if (!dzt->sb[dst_set].mblk) {
		dzt->sb[dst_set].mblk = dmz_alloc_mblock(dzt, 0);
		if (!dzt->sb[dst_set].mblk) {
			ret = -ENOMEM;
			goto out;
		}
		dzt->sb[dst_set].sb = dzt->sb[dst_set].mblk->data;
	}

	ret = dmz_write_sb(dzt, dst_set);

out:
	__free_pages(page, 0);

	return ret;
}

/*
 * Get super block from disk.
 */
static int dmz_load_sb(struct dm_zoned_target *dzt)
{
	bool sb_good[2] = {false, false};
	u64 sb_gen[2] = {0, 0};
	int ret;

	/* Read and check the primary super block */
	dzt->sb[0].block = dmz_sect2blk(dzt->sb_zone->sector);
	ret = dmz_get_sb(dzt, 0);
	if (ret) {
		dmz_dev_err(dzt,
			    "Read primary super block failed\n");
		return ret;
	}

	ret = dmz_check_sb(dzt, dzt->sb[0].sb);

	/* Read and check secondary super block */
	if (ret == 0) {
		sb_good[0] = true;
		dzt->sb[1].block = dzt->sb[0].block +
			(dzt->nr_meta_zones * dzt->zone_nr_blocks);
		ret = dmz_get_sb(dzt, 1);
	} else {
		ret = dmz_lookup_secondary_sb(dzt);
	}
	if (ret) {
		dmz_dev_err(dzt,
			    "Read secondary super block\n");
		return ret;
	}

	ret = dmz_check_sb(dzt, dzt->sb[1].sb);
	if (ret == 0)
		sb_good[1] = true;

	/* Use highest generation sb first */
	if (!sb_good[0] && !sb_good[1]) {
		dmz_dev_err(dzt,
			    "No valid super block found\n");
		return -EIO;
	}

	if (sb_good[0])
		sb_gen[0] = le64_to_cpu(dzt->sb[0].sb->gen);
	else
		ret = dmz_recover_mblocks(dzt, 0);

	if (sb_good[1])
		sb_gen[1] = le64_to_cpu(dzt->sb[1].sb->gen);
	else
		ret = dmz_recover_mblocks(dzt, 1);

	if (ret) {
		dmz_dev_err(dzt,
			    "Recovery failed\n");
		return -EIO;
	}

	if (sb_gen[0] >= sb_gen[1]) {
		dzt->sb_gen = sb_gen[0];
		dzt->mblk_primary = 0;
	} else {
		dzt->sb_gen = sb_gen[1];
		dzt->mblk_primary = 1;
	}

	dmz_dev_info(dzt,
		     "Using super block %u (gen %llu)\n",
		     dzt->mblk_primary,
		     dzt->sb_gen);

	return 0;
}

/*
 * Allocate, initialize and add a zone descriptor
 * to the device zone tree.
 */
static int dmz_insert_zone(struct dm_zoned_target *dzt,
			   struct blk_zone *blkz)
{
	struct rb_root *root = &dzt->zones;
	struct rb_node **new = &(root->rb_node), *parent = NULL;
	struct dm_zone *zone;
	int ret = 0;

	/* Runt zone ? If yes, ignore it */
	if (blkz->len != dzt->zone_nr_sectors) {
		if (blkz->start + blkz->len == dzt->zbd_capacity)
			return 0;
		return -ENXIO;
	}

	/* Allocate and initialize a zone descriptor */
	zone = kmem_cache_zalloc(dmz_zone_cache, GFP_KERNEL);
	if (!zone)
		return -ENOMEM;

	RB_CLEAR_NODE(&zone->node);
	INIT_LIST_HEAD(&zone->link);
	zone->chunk = DMZ_MAP_UNMAPPED;

	if (blkz->type == BLK_ZONE_TYPE_CONVENTIONAL) {
		set_bit(DMZ_CONV, &zone->flags);
	} else if (blkz->type == BLK_ZONE_TYPE_SEQWRITE_REQ) {
		set_bit(DMZ_SEQ_REQ, &zone->flags);
	} else if (blkz->type == BLK_ZONE_TYPE_SEQWRITE_PREF) {
		set_bit(DMZ_SEQ_PREF, &zone->flags);
	} else {
		ret = -ENXIO;
		goto out;
	}

	if (blkz->cond == BLK_ZONE_COND_OFFLINE)
		set_bit(DMZ_OFFLINE, &zone->flags);
	else if (blkz->cond == BLK_ZONE_COND_READONLY)
		set_bit(DMZ_READ_ONLY, &zone->flags);

	zone->sector = blkz->start;
	if (dmz_is_conv(zone))
		zone->wp_block = 0;
	else
		zone->wp_block = dmz_sect2blk(blkz->wp - blkz->start);

	/* Figure out where to put new node */
	while (*new) {
		struct dm_zone *z = container_of(*new, struct dm_zone, node);

		parent = *new;
		if (zone->sector + dzt->zone_nr_sectors <= z->sector) {
			new = &((*new)->rb_left);
		} else if (zone->sector >= z->sector + dzt->zone_nr_sectors) {
			new = &((*new)->rb_right);
		} else {
			dmz_dev_warn(dzt,
				     "Zone %u already inserted\n",
				     dmz_id(dzt, zone));
			ret = -ENXIO;
			goto out;
		}
	}

	/* Add new node and rebalance tree */
	rb_link_node(&zone->node, parent, new);
	rb_insert_color(&zone->node, root);

	/* Count zones */
	dzt->nr_zones++;
	if (!dmz_is_readonly(zone) &&
	    !dmz_is_offline(zone))
		dzt->nr_useable_zones++;

out:
	if (ret)
		kfree(zone);

	return ret;
}

/*
 * Lookup a zone in the zone rbtree.
 */
static struct dm_zone *dmz_lookup_zone(struct dm_zoned_target *dzt,
				       unsigned int zone_id)
{
	struct rb_root *root = &dzt->zones;
	struct rb_node *node = root->rb_node;
	struct dm_zone *zone = NULL;
	sector_t sector = (sector_t)zone_id << dzt->zone_nr_sectors_shift;

	while (node) {
		zone = container_of(node, struct dm_zone, node);
		if (sector < zone->sector)
			node = node->rb_left;
		else if (sector >= zone->sector + dzt->zone_nr_sectors)
			node = node->rb_right;
		else
			break;
		zone = NULL;
	}

	return zone;
}

/*
 * Free zones descriptors.
 */
static void dmz_drop_zones(struct dm_zoned_target *dzt)
{
	struct rb_root *root = &dzt->zones;
	struct dm_zone *zone, *next;

	/* Free the zone descriptors */
	rbtree_postorder_for_each_entry_safe(zone, next, root, node)
		kmem_cache_free(dmz_zone_cache, zone);
	dzt->zones = RB_ROOT;
}

/*
 * Allocate and initialize zone descriptors using the zone
 * information from disk.
 */
static int dmz_init_zones(struct dm_zoned_target *dzt)
{
	struct dm_zone *zone;
	struct blk_zone *blkz;
	unsigned int nr_blkz;
	sector_t sector = 0;
	int i, ret = 0;

	/* Init */
	dzt->zone_nr_sectors = dzt->zbdq->limits.chunk_sectors;
	dzt->zone_nr_sectors_shift = ilog2(dzt->zone_nr_sectors);

	dzt->zone_nr_blocks = dmz_sect2blk(dzt->zone_nr_sectors);
	dzt->zone_nr_blocks_shift = ilog2(dzt->zone_nr_blocks);

	dzt->zone_bitmap_size = dzt->zone_nr_blocks >> 3;
	dzt->zone_nr_bitmap_blocks =
		dzt->zone_bitmap_size >> DMZ_BLOCK_SHIFT;

	/* Get zone information */
	nr_blkz = DMZ_REPORT_NR_ZONES;
	blkz = kcalloc(nr_blkz, sizeof(struct blk_zone), GFP_KERNEL);
	if (!blkz) {
		dmz_dev_err(dzt,
			    "No memory for report zones\n");
		return -ENOMEM;
	}

	/*
	 * Get zone information and initialize zone descriptors.
	 * At the same time, determine where the super block
	 * should be: first block of the first randomly writable
	 * zone.
	 */
	while (sector < dzt->zbd_capacity) {

		/* Get zone information */
		nr_blkz = DMZ_REPORT_NR_ZONES;
		ret = blkdev_report_zones(dzt->zbd, sector,
					  blkz, &nr_blkz,
					  GFP_KERNEL);
		if (ret) {
			dmz_dev_err(dzt,
				    "Report zones failed %d\n",
				    ret);
			goto out;
		}

		/* Process report */
		for (i = 0; i < nr_blkz; i++) {
			ret = dmz_insert_zone(dzt, &blkz[i]);
			if (ret)
				goto out;
			sector += dzt->zone_nr_sectors;
		}

	}

	if (sector < dzt->zbd_capacity) {
		dmz_dev_err(dzt,
			    "Failed to get zone information\n");
		ret = -ENXIO;
		goto out;
	}

	/*
	 * The entire zone configuration of the disk is now known.
	 * We however need to fix it: remove the last zone if it is
	 * a smaller runt zone, and determine the actual use (random or
	 * sequential) of zones. For a host-managed drive, all conventional
	 * zones are used as random zones. The same applies for host-aware
	 * drives, but if the number of conventional zones is too low,
	 * sequential write preferred zones are marked as random zones until
	 * the total random zones represent 1% of the drive capacity. Since
	 * zones can be in any order, this is a 2 step process.
	 */

	/* Step 1: process conventional zones */
	for (i = 0; i < dzt->nr_zones; i++) {
		zone = dmz_lookup_zone(dzt, i);
		if (dmz_is_conv(zone)) {
			set_bit(DMZ_RND, &zone->flags);
			dzt->nr_rnd_zones++;
		}
	}

	/* Step 2: process sequential zones */
	for (i = 0; i < dzt->nr_zones; i++) {

		zone = dmz_lookup_zone(dzt, i);
		if (dmz_is_seqreq(zone)) {
			set_bit(DMZ_SEQ, &zone->flags);
		} else if (dmz_is_seqpref(zone)) {
			if (dzt->nr_rnd_zones < dzt->nr_zones / 100) {
				set_bit(DMZ_RND, &zone->flags);
				zone->wp_block = 0;
				dzt->nr_rnd_zones++;
			} else {
				set_bit(DMZ_SEQ, &zone->flags);
			}
		}
		if (!dzt->sb_zone && dmz_is_rnd(zone))
			/* Super block zone */
			dzt->sb_zone = zone;
	}

out:
	if (ret)
		dmz_drop_zones(dzt);

	return ret;
}

/*
 * Update a zone information.
 */
static int dmz_update_zone(struct dm_zoned_target *dzt, struct dm_zone *zone)
{
	unsigned int nr_blkz = 1;
	struct blk_zone blkz;
	int ret;

	/* Get zone information from disk */
	ret = blkdev_report_zones(dzt->zbd, zone->sector,
				  &blkz, &nr_blkz,
				  GFP_KERNEL);
	if (ret) {
		dmz_dev_err(dzt,
			    "Get zone %u report failed\n",
			    dmz_id(dzt, zone));
		return ret;
	}

	clear_bit(DMZ_OFFLINE, &zone->flags);
	clear_bit(DMZ_READ_ONLY, &zone->flags);
	if (blkz.cond == BLK_ZONE_COND_OFFLINE)
		set_bit(DMZ_OFFLINE, &zone->flags);
	else if (blkz.cond == BLK_ZONE_COND_READONLY)
		set_bit(DMZ_READ_ONLY, &zone->flags);

	if (dmz_is_seq(zone))
		zone->wp_block = dmz_sect2blk(blkz.wp - blkz.start);
	else
		zone->wp_block = 0;

	return 0;
}

/*
 * Check zone information after a resume.
 */
static int dmz_check_zones(struct dm_zoned_target *dzt)
{
	struct dm_zone *zone;
	sector_t wp_block;
	unsigned int i;
	int ret;

	/* Check zones */
	for (i = 0; i < dzt->nr_zones; i++) {

		zone = dmz_lookup_zone(dzt, i);
		if (!zone) {
			dmz_dev_err(dzt,
				    "Unable to get zone %u\n", i);
			return -EIO;
		}

		wp_block = zone->wp_block;

		ret = dmz_update_zone(dzt, zone);
		if (ret) {
			dmz_dev_err(dzt,
				    "Broken zone %u\n", i);
			return ret;
		}

		if (dmz_is_offline(zone)) {
			dmz_dev_warn(dzt,
				     "Zone %u is offline\n", i);
			continue;
		}

		/* Check write pointer */
		if (!dmz_is_seq(zone))
			zone->wp_block = 0;
		else if (zone->wp_block != wp_block) {
			dmz_dev_err(dzt,
				    "Zone %u: Invalid wp (%llu / %llu)\n",
				    i,
				    (u64)zone->wp_block,
				    (u64)wp_block);
			zone->wp_block = wp_block;
			dmz_invalidate_blocks(dzt, zone, zone->wp_block,
					dzt->zone_nr_blocks - zone->wp_block);
			dmz_validate_zone(dzt, zone);
		}

	}

	return 0;
}

/*
 * Reset a zone write pointer.
 */
int dmz_reset_zone(struct dm_zoned_target *dzt, struct dm_zone *zone)
{
	int ret;

	/*
	 * Ignore offline zones, read only zones,
	 * conventional zones and empty sequential zones.
	 */
	if (dmz_is_offline(zone) ||
	    dmz_is_readonly(zone) ||
	    dmz_is_conv(zone) ||
	    (dmz_is_seqreq(zone) && dmz_is_empty(zone)))
		return 0;

	ret = blkdev_reset_zones(dzt->zbd,
				 zone->sector,
				 dzt->zone_nr_sectors,
				 GFP_KERNEL);
	if (ret) {
		dmz_dev_err(dzt,
			    "Reset zone %u failed %d\n",
			    dmz_id(dzt, zone),
			    ret);
		return ret;
	}

	/* Rewind */
	zone->wp_block = 0;

	return 0;
}

static void dmz_get_zone_weight(struct dm_zoned_target *dzt,
				struct dm_zone *zone);

/*
 * Initialize chunk mapping.
 */
static int dmz_load_mapping(struct dm_zoned_target *dzt)
{
	struct dm_zone *dzone, *bzone;
	struct dm_zoned_mblock *dmap_mblk = NULL;
	struct dm_zoned_map *dmap;
	unsigned int i = 0, e = 0, chunk = 0;
	unsigned int dzone_id;
	unsigned int bzone_id;

	/* Metadata block array for the chunk mapping table */
	dzt->dz_map_mblk = kcalloc(dzt->nr_map_blocks,
				   sizeof(struct dm_zoned_mblk *),
				   GFP_KERNEL);
	if (!dzt->dz_map_mblk)
		return -ENOMEM;

	/* Get chunk mapping table blocks and initialize zone mapping */
	while (chunk < dzt->nr_chunks) {

		if (!dmap_mblk) {
			/* Get mapping block */
			dmap_mblk = dmz_get_mblock(dzt, i + 1);
			if (IS_ERR(dmap_mblk))
				return PTR_ERR(dmap_mblk);
			dzt->dz_map_mblk[i] = dmap_mblk;
			dmap = (struct dm_zoned_map *) dmap_mblk->data;
			i++;
			e = 0;
		}

		/* Check data zone */
		dzone_id = le32_to_cpu(dmap[e].dzone_id);
		if (dzone_id == DMZ_MAP_UNMAPPED)
			goto next;

		dzone = dmz_lookup_zone(dzt, dzone_id);
		if (!dzone)
			return -EIO;

		set_bit(DMZ_DATA, &dzone->flags);
		dzone->chunk = chunk;
		dmz_get_zone_weight(dzt, dzone);

		if (dmz_is_rnd(dzone))
			list_add_tail(&dzone->link, &dzt->dz_map_rnd_list);
		else
			list_add_tail(&dzone->link, &dzt->dz_map_seq_list);

		/* Check buffer zone */
		bzone_id = le32_to_cpu(dmap[e].bzone_id);
		if (bzone_id == DMZ_MAP_UNMAPPED)
			goto next;

		bzone = dmz_lookup_zone(dzt, bzone_id);
		if (!bzone || !dmz_is_rnd(bzone))
			return -EIO;

		set_bit(DMZ_DATA, &bzone->flags);
		set_bit(DMZ_BUF, &bzone->flags);
		bzone->chunk = chunk;
		bzone->bzone = dzone;
		dzone->bzone = bzone;
		dmz_get_zone_weight(dzt, bzone);
		list_add_tail(&bzone->link, &dzt->dz_map_rnd_list);

next:
		chunk++;
		e++;
		if (e >= DMZ_MAP_ENTRIES)
			dmap_mblk = NULL;

	}

	/*
	 * At this point, only meta zones and mapped data zones were
	 * fully initialized. All remaining zones are unmapped data
	 * zones. Finish initializing those here.
	 */
	for (i = 0; i < dzt->nr_zones; i++) {

		dzone = dmz_lookup_zone(dzt, i);
		if (!dzone)
			return -EIO;

		if (dmz_is_meta(dzone))
			continue;

		if (dmz_is_rnd(dzone))
			dzt->dz_nr_rnd++;
		else
			dzt->dz_nr_seq++;

		if (dmz_is_data(dzone))
			/* Already initialized */
			continue;

		/* Unmapped data zone */
		set_bit(DMZ_DATA, &dzone->flags);
		dzone->chunk = DMZ_MAP_UNMAPPED;
		if (dmz_is_rnd(dzone)) {
			list_add_tail(&dzone->link,
				      &dzt->dz_unmap_rnd_list);
			atomic_inc(&dzt->dz_unmap_nr_rnd);
		} else if (atomic_read(&dzt->nr_reclaim_seq_zones) <
			   dzt->nr_reserved_seq) {
			list_add_tail(&dzone->link,
				      &dzt->reclaim_seq_zones_list);
			atomic_inc(&dzt->nr_reclaim_seq_zones);
			dzt->dz_nr_seq--;
		} else {
			list_add_tail(&dzone->link,
				      &dzt->dz_unmap_seq_list);
			atomic_inc(&dzt->dz_unmap_nr_seq);
		}
	}

	return 0;
}

/*
 * Set a data chunk mapping.
 */
static void dmz_set_chunk_mapping(struct dm_zoned_target *dzt,
				  unsigned int chunk,
				  unsigned int dzone_id,
				  unsigned int bzone_id)
{
	struct dm_zoned_mblock *dmap_mblk =
		dzt->dz_map_mblk[chunk >> DMZ_MAP_ENTRIES_SHIFT];
	struct dm_zoned_map *dmap = (struct dm_zoned_map *) dmap_mblk->data;
	int map_idx = chunk & DMZ_MAP_ENTRIES_MASK;

	dmap[map_idx].dzone_id = cpu_to_le32(dzone_id);
	dmap[map_idx].bzone_id = cpu_to_le32(bzone_id);
	dmz_dirty_mblock(dzt, dmap_mblk);
}

/*
 * The list of mapped zones is maintained in LRU order.
 * This rotates a zone at the end of its map list.
 */
static void __dmz_lru_zone(struct dm_zoned_target *dzt,
			   struct dm_zone *zone)
{
	if (list_empty(&zone->link))
		return;

	list_del_init(&zone->link);
	if (dmz_is_seq(zone))
		/* LRU rotate sequential zone */
		list_add_tail(&zone->link, &dzt->dz_map_seq_list);
	else
		/* LRU rotate random zone */
		list_add_tail(&zone->link, &dzt->dz_map_rnd_list);
}

/*
 * The list of mapped random zones is maintained
 * in LRU order. This rotates a zone at the end of the list.
 */
static void dmz_lru_zone(struct dm_zoned_target *dzt,
			 struct dm_zone *zone)
{
	__dmz_lru_zone(dzt, zone);
	if (zone->bzone)
		__dmz_lru_zone(dzt, zone->bzone);
}

/*
 * Wait for any zone to be freed.
 */
static void dmz_wait_for_free_zones(struct dm_zoned_target *dzt)
{
	DEFINE_WAIT(wait);

	dmz_trigger_reclaim(dzt);

	prepare_to_wait(&dzt->dz_free_wq, &wait, TASK_UNINTERRUPTIBLE);
	dmz_unlock_map(dzt);

	io_schedule_timeout(HZ);

	dmz_lock_map(dzt);
	finish_wait(&dzt->dz_free_wq, &wait);
}

/*
 * Wait for a zone reclaim to complete.
 */
static void dmz_wait_for_reclaim(struct dm_zoned_target *dzt,
				 struct dm_zone *zone)
{
	dmz_unlock_map(dzt);
	wait_on_bit_timeout(&zone->flags, DMZ_RECLAIM,
			    TASK_UNINTERRUPTIBLE,
			    HZ);
	dmz_lock_map(dzt);
}

/*
 * Get a data chunk mapping zone.
 */
struct dm_zone *dmz_get_chunk_mapping(struct dm_zoned_target *dzt,
				      unsigned int chunk, int op)
{
	struct dm_zoned_mblock *dmap_mblk =
		dzt->dz_map_mblk[chunk >> DMZ_MAP_ENTRIES_SHIFT];
	struct dm_zoned_map *dmap = (struct dm_zoned_map *) dmap_mblk->data;
	int dmap_idx = chunk & DMZ_MAP_ENTRIES_MASK;
	unsigned int dzone_id;
	struct dm_zone *dzone = NULL;

	dmz_lock_map(dzt);

again:

	/* Get the chunk mapping */
	dzone_id = le32_to_cpu(dmap[dmap_idx].dzone_id);
	if (dzone_id == DMZ_MAP_UNMAPPED) {
		/*
		 * Read or discard in unmapped chunks are fine. But for
		 * writes, we need a mapping, so get one.
		 */
		if (op != REQ_OP_WRITE)
			goto out;

		/* Alloate a random zone */
		dzone = dmz_alloc_zone(dzt, DMZ_ALLOC_RND);
		if (!dzone) {
			dmz_wait_for_free_zones(dzt);
			goto again;
		}

		dmz_map_zone(dzt, dzone, chunk);

	} else {

		/* The chunk is already mapped: get the mapping zone */
		dzone = dmz_lookup_zone(dzt, dzone_id);
		if (!dzone || dzone->chunk != chunk) {
			dzone = ERR_PTR(-EIO);
			goto out;
		}

	}

	/*
	 * If the zone is being reclaimed, the chunk mapping may change.
	 * So wait for reclaim to complete and retry.
	 */
	if (dmz_in_reclaim(dzone)) {
		dmz_wait_for_reclaim(dzt, dzone);
		goto again;
	}

	dmz_lru_zone(dzt, dzone);

out:
	dmz_unlock_map(dzt);

	return dzone;
}

/*
 * Allocate and map a random zone to buffer a chunk
 * already mapped to a sequential zone.
 */
struct dm_zone *dmz_get_chunk_buffer(struct dm_zoned_target *dzt,
				     struct dm_zone *dzone)
{
	struct dm_zone *bzone;
	unsigned int chunk;

	dmz_lock_map(dzt);

	chunk = dzone->chunk;

	/* Alloate a random zone */
	do {
		bzone = dmz_alloc_zone(dzt, DMZ_ALLOC_RND);
		if (!bzone)
			dmz_wait_for_free_zones(dzt);
	} while (!bzone);

	if (bzone) {

		if (dmz_is_seqpref(bzone))
			dmz_reset_zone(dzt, bzone);

		/* Update the chunk mapping */
		dmz_set_chunk_mapping(dzt, chunk,
				      dmz_id(dzt, dzone),
				      dmz_id(dzt, bzone));

		set_bit(DMZ_BUF, &bzone->flags);
		bzone->chunk = chunk;
		bzone->bzone = dzone;
		dzone->bzone = bzone;
		list_add_tail(&bzone->link, &dzt->dz_map_rnd_list);

	}

	dmz_unlock_map(dzt);

	return bzone;
}

/*
 * Get an unmapped (free) zone.
 * This must be called with the mapping lock held.
 */
struct dm_zone *dmz_alloc_zone(struct dm_zoned_target *dzt,
			       unsigned long flags)
{
	struct list_head *list;
	struct dm_zone *zone;

	if (flags & DMZ_ALLOC_RND)
		list = &dzt->dz_unmap_rnd_list;
	else
		list = &dzt->dz_unmap_seq_list;

	if (list_empty(list)) {

		/*
		 * No free zone: if this is for reclaim, allow using the
		 * reserved sequential zones.
		 */
		if (!(flags & DMZ_ALLOC_RECLAIM) ||
		    list_empty(&dzt->reclaim_seq_zones_list))
			return NULL;

		zone = list_first_entry(&dzt->reclaim_seq_zones_list,
					struct dm_zone, link);
		list_del_init(&zone->link);
		atomic_dec(&dzt->nr_reclaim_seq_zones);
		return zone;

	}

	zone = list_first_entry(list, struct dm_zone, link);
	list_del_init(&zone->link);

	if (dmz_is_rnd(zone))
		atomic_dec(&dzt->dz_unmap_nr_rnd);
	else
		atomic_dec(&dzt->dz_unmap_nr_seq);

	if (dmz_is_offline(zone)) {
		dmz_dev_warn(dzt,
			     "Zone %u is offline\n",
			     dmz_id(dzt, zone));
		zone = NULL;
	}

	if (dmz_should_reclaim(dzt))
		dmz_trigger_reclaim(dzt);

	return zone;
}

/*
 * Free a zone.
 * This must be called with the mapping lock held.
 */
void dmz_free_zone(struct dm_zoned_target *dzt, struct dm_zone *zone)
{

	/* Return the zone to its type unmap list */
	if (dmz_is_rnd(zone)) {
		list_add_tail(&zone->link, &dzt->dz_unmap_rnd_list);
		atomic_inc(&dzt->dz_unmap_nr_rnd);
	} else if (atomic_read(&dzt->nr_reclaim_seq_zones) <
		   dzt->nr_reserved_seq) {
		list_add_tail(&zone->link, &dzt->reclaim_seq_zones_list);
		atomic_inc(&dzt->nr_reclaim_seq_zones);
	} else {
		list_add_tail(&zone->link, &dzt->dz_unmap_seq_list);
		atomic_inc(&dzt->dz_unmap_nr_seq);
	}

	wake_up_all(&dzt->dz_free_wq);
}

/*
 * Map a chunk to a zone.
 * This must be called with the mapping lock held.
 */
void dmz_map_zone(struct dm_zoned_target *dzt,
		  struct dm_zone *dzone, unsigned int chunk)
{

	if (dmz_is_seqpref(dzone))
		dmz_reset_zone(dzt, dzone);

	/* Set the chunk mapping */
	dmz_set_chunk_mapping(dzt, chunk,
			      dmz_id(dzt, dzone),
			      DMZ_MAP_UNMAPPED);
	dzone->chunk = chunk;
	if (dmz_is_rnd(dzone))
		list_add_tail(&dzone->link, &dzt->dz_map_rnd_list);
	else
		list_add_tail(&dzone->link, &dzt->dz_map_seq_list);
}

/*
 * Unmap a zone.
 * This must be called with the mapping lock held.
 */
void dmz_unmap_zone(struct dm_zoned_target *dzt, struct dm_zone *zone)
{
	unsigned int chunk = zone->chunk;
	unsigned int dzone_id;

	if (chunk == DMZ_MAP_UNMAPPED)
		/* Already unmapped */
		return;

	if (test_and_clear_bit(DMZ_BUF, &zone->flags)) {

		/*
		 * Unmapping buffer zone: clear only
		 * the chunk buffer mapping
		 */
		dzone_id = dmz_id(dzt, zone->bzone);
		zone->bzone->bzone = NULL;
		zone->bzone = NULL;

	} else {

		/*
		 * Unmapping data zone: the zone must
		 * not be any buffer zone.
		 */
		dzone_id = DMZ_MAP_UNMAPPED;

	}

	dmz_set_chunk_mapping(dzt, chunk, dzone_id,
			      DMZ_MAP_UNMAPPED);

	zone->chunk = DMZ_MAP_UNMAPPED;
	list_del_init(&zone->link);
}

/*
 * Write and discard change the block validity in data
 * zones and their buffer zones. Check all blocks to see
 * if those zones can be reclaimed and freed on the fly
 * (if all blocks are invalid).
 */
void dmz_validate_zone(struct dm_zoned_target *dzt, struct dm_zone *dzone)
{
	struct dm_zone *bzone;

	dmz_lock_map(dzt);

	bzone = dzone->bzone;
	if (bzone) {
		if (!dmz_weight(bzone)) {
			/* Empty buffer zone: reclaim it */
			dmz_unmap_zone(dzt, bzone);
			dmz_free_zone(dzt, bzone);
			bzone = NULL;
		} else {
			dmz_lru_zone(dzt, bzone);
		}
	}

	if (!dmz_weight(dzone) && !bzone) {
		/* Unbuffered empty data zone: reclaim it */
		dmz_unmap_zone(dzt, dzone);
		dmz_free_zone(dzt, dzone);
	} else {
		dmz_lru_zone(dzt, dzone);
	}

	dmz_unlock_map(dzt);
}

/*
 * Set @nr_bits bits in @bitmap starting from @bit.
 * Return the number of bits changed from 0 to 1.
 */
static unsigned int dmz_set_bits(unsigned long *bitmap,
				 unsigned int bit, unsigned int nr_bits)
{
	unsigned long *addr;
	unsigned int end = bit + nr_bits;
	unsigned int n = 0;

	while (bit < end) {

		if (((bit & (BITS_PER_LONG - 1)) == 0) &&
		    ((end - bit) >= BITS_PER_LONG)) {
			/* Try to set the whole word at once */
			addr = bitmap + BIT_WORD(bit);
			if (*addr == 0) {
				*addr = ULONG_MAX;
				n += BITS_PER_LONG;
				bit += BITS_PER_LONG;
				continue;
			}
		}

		if (!test_and_set_bit(bit, bitmap))
			n++;
		bit++;

	}

	return n;

}

/*
 * Get the bitmap block storing the bit for chunk_block in zone.
 */
static struct dm_zoned_mblock *dmz_get_bitmap(struct dm_zoned_target *dzt,
					      struct dm_zone *zone,
					      sector_t chunk_block)
{
	sector_t bitmap_block = 1 + dzt->nr_map_blocks
		+ (sector_t)(dmz_id(dzt, zone)
			     * dzt->zone_nr_bitmap_blocks)
		+ (chunk_block >> DMZ_BLOCK_SHIFT_BITS);

	return dmz_get_mblock(dzt, bitmap_block);
}

/*
 * Validate all the blocks in the range [block..block+nr_blocks-1].
 */
int dmz_validate_blocks(struct dm_zoned_target *dzt,
			struct dm_zone *zone,
			sector_t chunk_block, unsigned int nr_blocks)
{
	unsigned int count, bit, nr_bits;
	struct dm_zoned_mblock *mblk;
	unsigned int n = 0;

	dmz_dev_debug(dzt,
		      "=> VALIDATE zone %u, block %llu, %u blocks\n",
		      dmz_id(dzt, zone),
		      (u64)chunk_block,
		      nr_blocks);

	WARN_ON(chunk_block + nr_blocks > dzt->zone_nr_blocks);

	while (nr_blocks) {

		/* Get bitmap block */
		mblk = dmz_get_bitmap(dzt, zone, chunk_block);
		if (IS_ERR(mblk))
			return PTR_ERR(mblk);

		/* Set bits */
		bit = chunk_block & DMZ_BLOCK_MASK_BITS;
		nr_bits = min(nr_blocks, DMZ_BLOCK_SIZE_BITS - bit);

		count = dmz_set_bits((unsigned long *) mblk->data,
				     bit, nr_bits);
		if (count) {
			dmz_dirty_mblock(dzt, mblk);
			n += count;
		}
		dmz_release_mblock(dzt, mblk);

		nr_blocks -= nr_bits;
		chunk_block += nr_bits;

	}

	if (likely(zone->weight + n <= dzt->zone_nr_blocks)) {
		zone->weight += n;
	} else {
		dmz_dev_warn(dzt,
			     "Zone %u: weight %u should be <= %llu\n",
			     dmz_id(dzt, zone),
			     zone->weight,
			     (u64)dzt->zone_nr_blocks - n);
		zone->weight = dzt->zone_nr_blocks;
	}

	dmz_dev_debug(dzt,
		      "=> VALIDATE zone %u => weight %u\n",
		      dmz_id(dzt, zone),
		      zone->weight);

	return 0;
}

/*
 * Clear nr_bits bits in bitmap starting from bit.
 * Return the number of bits cleared.
 */
static int dmz_clear_bits(unsigned long *bitmap,
			  int bit, int nr_bits)
{
	unsigned long *addr;
	int end = bit + nr_bits;
	int n = 0;

	while (bit < end) {

		if (((bit & (BITS_PER_LONG - 1)) == 0) &&
		    ((end - bit) >= BITS_PER_LONG)) {
			/* Try to clear whole word at once */
			addr = bitmap + BIT_WORD(bit);
			if (*addr == ULONG_MAX) {
				*addr = 0;
				n += BITS_PER_LONG;
				bit += BITS_PER_LONG;
				continue;
			}
		}

		if (test_and_clear_bit(bit, bitmap))
			n++;
		bit++;

	}

	return n;

}

/*
 * Invalidate all the blocks in the range [block..block+nr_blocks-1].
 */
int dmz_invalidate_blocks(struct dm_zoned_target *dzt,
			  struct dm_zone *zone,
			  sector_t chunk_block, unsigned int nr_blocks)
{
	unsigned int count, bit, nr_bits;
	struct dm_zoned_mblock *mblk;
	unsigned int n = 0;

	dmz_dev_debug(dzt,
		      "=> INVALIDATE zone %u, block %llu, %u blocks\n",
		      dmz_id(dzt, zone),
		      (u64)chunk_block,
		      nr_blocks);

	WARN_ON(chunk_block + nr_blocks > dzt->zone_nr_blocks);

	while (nr_blocks) {

		/* Get bitmap block */
		mblk = dmz_get_bitmap(dzt, zone, chunk_block);
		if (IS_ERR(mblk))
			return PTR_ERR(mblk);

		/* Clear bits */
		bit = chunk_block & DMZ_BLOCK_MASK_BITS;
		nr_bits = min(nr_blocks, DMZ_BLOCK_SIZE_BITS - bit);

		count = dmz_clear_bits((unsigned long *) mblk->data,
				       bit, nr_bits);
		if (count) {
			dmz_dirty_mblock(dzt, mblk);
			n += count;
		}
		dmz_release_mblock(dzt, mblk);

		nr_blocks -= nr_bits;
		chunk_block += nr_bits;

	}

	if (zone->weight >= n) {
		zone->weight -= n;
	} else {
		dmz_dev_warn(dzt,
			     "Zone %u: weight %u should be >= %u\n",
			     dmz_id(dzt, zone),
			     zone->weight,
			     n);
		zone->weight = 0;
	}

	return 0;
}

/*
 * Get a block bit value.
 */
static int dmz_test_block(struct dm_zoned_target *dzt,
			  struct dm_zone *zone,
			  sector_t chunk_block)
{
	struct dm_zoned_mblock *mblk;
	int ret;

	WARN_ON(chunk_block >= dzt->zone_nr_blocks);

	/* Get bitmap block */
	mblk = dmz_get_bitmap(dzt, zone, chunk_block);
	if (IS_ERR(mblk))
		return PTR_ERR(mblk);

	/* Get offset */
	ret = test_bit(chunk_block & DMZ_BLOCK_MASK_BITS,
		       (unsigned long *) mblk->data) != 0;

	dmz_release_mblock(dzt, mblk);

	return ret;
}

/*
 * Return the number of blocks from chunk_block to the first block with a bit
 * value specified by set. Search at most nr_blocks blocks from chunk_block.
 */
static int dmz_to_next_set_block(struct dm_zoned_target *dzt,
				 struct dm_zone *zone,
				 sector_t chunk_block, unsigned int nr_blocks,
				 int set)
{
	struct dm_zoned_mblock *mblk;
	unsigned int bit, set_bit, nr_bits;
	unsigned long *bitmap;
	int n = 0;

	WARN_ON(chunk_block + nr_blocks > dzt->zone_nr_blocks);

	while (nr_blocks) {

		/* Get bitmap block */
		mblk = dmz_get_bitmap(dzt, zone, chunk_block);
		if (IS_ERR(mblk))
			return PTR_ERR(mblk);

		/* Get offset */
		bitmap = (unsigned long *) mblk->data;
		bit = chunk_block & DMZ_BLOCK_MASK_BITS;
		nr_bits = min(nr_blocks, DMZ_BLOCK_SIZE_BITS - bit);
		if (set)
			set_bit = find_next_bit(bitmap,
						DMZ_BLOCK_SIZE_BITS,
						bit);
		else
			set_bit = find_next_zero_bit(bitmap,
						     DMZ_BLOCK_SIZE_BITS,
						     bit);
		dmz_release_mblock(dzt, mblk);

		n += set_bit - bit;
		if (set_bit < DMZ_BLOCK_SIZE_BITS)
			break;

		nr_blocks -= nr_bits;
		chunk_block += nr_bits;

	}

	return n;
}

/*
 * Test if chunk_block is valid. If it is, the number of consecutive
 * valid blocks from chunk_block will be returned.
 */
int dmz_block_valid(struct dm_zoned_target *dzt,
		    struct dm_zone *zone,
		    sector_t chunk_block)
{
	int valid;

	/* Test block */
	valid = dmz_test_block(dzt, zone, chunk_block);
	if (valid <= 0)
		return valid;

	/* The block is valid: get the number of valid blocks from block */
	return dmz_to_next_set_block(dzt, zone, chunk_block,
				     dzt->zone_nr_blocks - chunk_block,
				     0);
}

/*
 * Find the first valid block from @chunk_block in @zone.
 * If such a block is found, its number is returned using
 * @chunk_block and the total number of valid blocks from @chunk_block
 * is returned.
 */
int dmz_first_valid_block(struct dm_zoned_target *dzt,
			  struct dm_zone *zone,
			  sector_t *chunk_block)
{
	sector_t start_block = *chunk_block;
	int ret;

	ret = dmz_to_next_set_block(dzt, zone, start_block,
				    dzt->zone_nr_blocks - start_block, 1);
	if (ret < 0)
		return ret;

	start_block += ret;
	*chunk_block = start_block;

	return dmz_to_next_set_block(dzt, zone, start_block,
				     dzt->zone_nr_blocks - start_block, 0);
}

/*
 * Count the number of bits set starting from bit up to bit + nr_bits - 1.
 */
static int dmz_count_bits(void *bitmap, int bit, int nr_bits)
{
	unsigned long *addr;
	int end = bit + nr_bits;
	int n = 0;

	while (bit < end) {

		if (((bit & (BITS_PER_LONG - 1)) == 0) &&
		    ((end - bit) >= BITS_PER_LONG)) {
			addr = (unsigned long *)bitmap + BIT_WORD(bit);
			if (*addr == ULONG_MAX) {
				n += BITS_PER_LONG;
				bit += BITS_PER_LONG;
				continue;
			}
		}

		if (test_bit(bit, bitmap))
			n++;
		bit++;

	}

	return n;

}

/*
 * Get a zone weight.
 */
static void dmz_get_zone_weight(struct dm_zoned_target *dzt,
				struct dm_zone *zone)
{
	struct dm_zoned_mblock *mblk;
	sector_t chunk_block = 0;
	unsigned int bit, nr_bits;
	unsigned int nr_blocks = dzt->zone_nr_blocks;
	void *bitmap;
	int n = 0;

	while (nr_blocks) {

		/* Get bitmap block */
		mblk = dmz_get_bitmap(dzt, zone, chunk_block);
		if (IS_ERR(mblk)) {
			n = 0;
			break;
		}

		/* Count bits in this block */
		bitmap = mblk->data;
		bit = chunk_block & DMZ_BLOCK_MASK_BITS;
		nr_bits = min(nr_blocks, DMZ_BLOCK_SIZE_BITS - bit);
		n += dmz_count_bits(bitmap, bit, nr_bits);

		dmz_release_mblock(dzt, mblk);

		nr_blocks -= nr_bits;
		chunk_block += nr_bits;

	}

	zone->weight = n;
}

/*
 * Initialize the target metadata.
 */
int dmz_init_meta(struct dm_zoned_target *dzt,
		  struct dm_zoned_target_config *conf)
{
	unsigned int i, zid;
	struct dm_zone *zone;
	int ret;

	/* Initialize zone descriptors */
	ret = dmz_init_zones(dzt);
	if (ret)
		goto out;

	/* Get super block */
	ret = dmz_load_sb(dzt);
	if (ret)
		goto out;

	/* Set metadata zones starting from sb_zone */
	zid = dmz_id(dzt, dzt->sb_zone);
	for (i = 0; i < dzt->nr_meta_zones << 1; i++) {
		zone = dmz_lookup_zone(dzt, zid + i);
		if (!zone || !dmz_is_rnd(zone))
			return -ENXIO;
		set_bit(DMZ_META, &zone->flags);
	}

	/*
	 * Maximum allowed size of the cache: we need 2 super blocks,
	 * the chunk map blocks and enough blocks to be able to cache
	 * up to 128 zones.
	 */
	dzt->max_nr_mblks = 2 + dzt->nr_map_blocks +
		dzt->zone_nr_bitmap_blocks * 64;

	/* Load mapping table */
	ret = dmz_load_mapping(dzt);
	if (ret)
		goto out;

	dmz_dev_info(dzt,
		     "Backend device:\n");
	dmz_dev_info(dzt,
		     "    %llu 512-byte logical sectors\n",
		     (u64)dzt->nr_zones
		     << dzt->zone_nr_sectors_shift);
	dmz_dev_info(dzt,
		     "    %u zones of %llu 512-byte logical sectors\n",
		     dzt->nr_zones,
		     (u64)dzt->zone_nr_sectors);
	dmz_dev_info(dzt,
		     "    %u metadata zones\n",
		     dzt->nr_meta_zones * 2);
	dmz_dev_info(dzt,
		     "    %u data zones for %u chunks\n",
		     dzt->nr_data_zones,
		     dzt->nr_chunks);
	dmz_dev_info(dzt,
		     "        %u random zones (%u unmapped)\n",
		     dzt->dz_nr_rnd,
		     atomic_read(&dzt->dz_unmap_nr_rnd));
	dmz_dev_info(dzt,
		     "        %u sequential zones (%u unmapped)\n",
		     dzt->dz_nr_seq,
		     atomic_read(&dzt->dz_unmap_nr_seq));
	dmz_dev_info(dzt,
		     "    %u reserved sequential data zones\n",
		     dzt->nr_reserved_seq);

	dmz_dev_debug(dzt,
		      "Format:\n");
	dmz_dev_debug(dzt,
		      "%u metadata blocks per set (%u max cache)\n",
		      dzt->nr_meta_blocks,
		      dzt->max_nr_mblks);
	dmz_dev_debug(dzt,
		      "    %u data zone mapping blocks\n",
		      dzt->nr_map_blocks);
	dmz_dev_debug(dzt,
		      "    %u bitmap blocks\n",
		      dzt->nr_bitmap_blocks);

out:
	if (ret)
		dmz_cleanup_meta(dzt);

	return ret;
}

/*
 * Cleanup the target metadata resources.
 */
void dmz_cleanup_meta(struct dm_zoned_target *dzt)
{
	struct rb_root *root = &dzt->mblk_rbtree;
	struct dm_zoned_mblock *mblk, *next;
	int i;

	/* Release zone mapping resources */
	if (dzt->dz_map_mblk) {
		for (i = 0; i < dzt->nr_map_blocks; i++)
			dmz_release_mblock(dzt, dzt->dz_map_mblk[i]);
		kfree(dzt->dz_map_mblk);
		dzt->dz_map_mblk = NULL;
	}

	/* Release super blocks */
	for (i = 0; i < 2; i++) {
		if (dzt->sb[i].mblk) {
			dmz_free_mblock(dzt, dzt->sb[i].mblk);
			dzt->sb[i].mblk = NULL;
		}
	}

	/* Free cached blocks */
	while (!list_empty(&dzt->mblk_dirty_list)) {
		mblk = list_first_entry(&dzt->mblk_dirty_list,
					struct dm_zoned_mblock, link);
		dmz_dev_warn(dzt, "mblock %llu still in dirty list (ref %u)\n",
			     (u64)mblk->no,
			     atomic_read(&mblk->ref));
		list_del_init(&mblk->link);
		rb_erase(&mblk->node, &dzt->mblk_rbtree);
		dmz_free_mblock(dzt, mblk);
	}

	while (!list_empty(&dzt->mblk_lru_list)) {
		mblk = list_first_entry(&dzt->mblk_lru_list,
					struct dm_zoned_mblock, link);
		list_del_init(&mblk->link);
		rb_erase(&mblk->node, &dzt->mblk_rbtree);
		dmz_free_mblock(dzt, mblk);
	}

	/* Sanity checks: the mblock rbtree should now be empty */
	rbtree_postorder_for_each_entry_safe(mblk, next, root, node) {
		dmz_dev_warn(dzt, "mblock %llu ref %u still in rbtree\n",
			     (u64)mblk->no,
			     atomic_read(&mblk->ref));
		atomic_set(&mblk->ref, 0);
		dmz_free_mblock(dzt, mblk);
	}

	/* Free the zone descriptors */
	dmz_drop_zones(dzt);
}

/*
 * Check metadata on resume.
 */
int dmz_resume_meta(struct dm_zoned_target *dzt)
{
	return dmz_check_zones(dzt);
}

