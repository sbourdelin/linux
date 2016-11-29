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
#include <linux/init.h>
#include <linux/version.h>

#include "dm-zoned.h"

static void dmz_bio_work(struct work_struct *work);

/*
 * Allocate a zone work.
 */
static struct dm_zone_work *dmz_alloc_zwork(struct dm_zoned_target *dzt)
{
	struct dm_zone_work *zwork;

	zwork = kmalloc(sizeof(struct dm_zone_work), GFP_NOWAIT);
	if (!zwork)
		return NULL;

	INIT_WORK(&zwork->work, dmz_bio_work);
	kref_init(&zwork->kref);
	zwork->target = dzt;
	zwork->zone = NULL;
	bio_list_init(&zwork->bio_list);

	return zwork;
}

/*
 * Free a zone work.
 */
static inline void dmz_free_zwork(struct kref *kref)
{
	struct dm_zone_work *zwork =
		container_of(kref, struct dm_zone_work, kref);
	struct dm_zone *zone = zwork->zone;

	if (zone) {
		zone->work = NULL;
		atomic_dec(&zwork->target->nr_active_zones);
	}

	kfree(zwork);
}

/*
 * Decrement a zone work reference count.
 */
static void dmz_put_zwork(struct dm_zone_work *zwork)
{
	struct dm_zoned_target *dzt;
	unsigned long flags;

	if (!zwork)
		return;

	dzt = zwork->target;
	spin_lock_irqsave(&dzt->zwork_lock, flags);
	kref_put(&zwork->kref, dmz_free_zwork);
	spin_unlock_irqrestore(&dzt->zwork_lock, flags);
}

/*
 * Target BIO completion.
 */
static inline void dmz_bio_end(struct bio *bio, int err)
{
	struct dm_zone_bioctx *bioctx
		= dm_per_bio_data(bio, sizeof(struct dm_zone_bioctx));

	if (atomic_dec_and_test(&bioctx->ref)) {
		/* User BIO Completed */
		dmz_put_zwork(bioctx->zwork);
		atomic_dec(&bioctx->target->bio_count);
		bio->bi_error = bioctx->error;
		bio_endio(bio);
	}
}

/*
 * Partial/internal BIO completion callback.
 * This terminates the user target BIO when there
 * are no more references to its context.
 */
static void dmz_bio_end_io(struct bio *bio)
{
	struct dm_zone_bioctx *bioctx = bio->bi_private;
	int err = bio->bi_error;

	if (err)
		bioctx->error = err;

	dmz_bio_end(bioctx->bio, err);

	bio_put(bio);

}

/*
 * Issue a BIO to a zone.
 * This BIO may only partially process the
 * issued target BIO.
 */
static int dmz_submit_bio(struct dm_zoned_target *dzt,
			  struct dm_zone *zone, struct bio *dzt_bio,
			  sector_t chunk_block, unsigned int nr_blocks)
{
	struct dm_zone_bioctx *bioctx
		= dm_per_bio_data(dzt_bio, sizeof(struct dm_zone_bioctx));
	unsigned int nr_sectors = dmz_blk2sect(nr_blocks);
	unsigned int size = nr_sectors << SECTOR_SHIFT;
	struct bio *clone;

	clone = bio_clone_fast(dzt_bio, GFP_NOIO, dzt->bio_set);
	if (!clone)
		return -ENOMEM;

	/* Setup the clone */
	clone->bi_bdev = dzt->zbd;
	clone->bi_opf = dzt_bio->bi_opf;
	clone->bi_iter.bi_sector = zone->sector + dmz_blk2sect(chunk_block);
	clone->bi_iter.bi_size = size;
	clone->bi_end_io = dmz_bio_end_io;
	clone->bi_private = bioctx;

	bio_advance(dzt_bio, size);

	/* Submit the clone */
	atomic_inc(&bioctx->ref);
	generic_make_request(clone);

	return 0;
}

/*
 * Zero out pages of discarded blocks accessed by a read BIO.
 */
static void dmz_handle_read_zero(struct dm_zoned_target *dzt,
				 struct bio *bio,
				 sector_t chunk_block, unsigned int nr_blocks)
{
	unsigned int size = nr_blocks << DMZ_BLOCK_SHIFT;

	dmz_dev_debug(dzt,
		      "=> ZERO READ chunk %llu -> block %llu, %u blocks\n",
		      (unsigned long long)dmz_bio_chunk(dzt, bio),
		      (unsigned long long)chunk_block,
		      nr_blocks);

	/* Clear nr_blocks */
	swap(bio->bi_iter.bi_size, size);
	zero_fill_bio(bio);
	swap(bio->bi_iter.bi_size, size);

	bio_advance(bio, size);
}

/*
 * Process a read BIO.
 */
static int dmz_handle_read(struct dm_zoned_target *dzt,
			   struct dm_zone *dzone, struct bio *bio)
{
	sector_t block = dmz_bio_block(bio);
	unsigned int nr_blocks = dmz_bio_blocks(bio);
	sector_t chunk_block = dmz_chunk_block(dzt, block);
	sector_t end_block = chunk_block + nr_blocks;
	struct dm_zone *rzone, *bzone;
	int ret;

	/* Read into unmapped chunks need only zeroing the BIO buffer */
	if (!dzone) {
		dmz_handle_read_zero(dzt, bio, chunk_block, nr_blocks);
		return 0;
	}

	dmz_dev_debug(dzt,
		      "READ %s zone %u, block %llu, %u blocks\n",
		      (dmz_is_rnd(dzone) ? "RND" : "SEQ"),
		      dmz_id(dzt, dzone),
		      (unsigned long long)chunk_block,
		      nr_blocks);

	/* Check block validity to determine the read location */
	bzone = dzone->bzone;
	while (chunk_block < end_block) {

		nr_blocks = 0;
		if (dmz_is_rnd(dzone)
		    || chunk_block < dzone->wp_block) {
			/* Test block validity in the data zone */
			ret = dmz_block_valid(dzt, dzone, chunk_block);
			if (ret < 0)
				return ret;
			if (ret > 0) {
				/* Read data zone blocks */
				nr_blocks = ret;
				rzone = dzone;
			}
		}

		/*
		 * No valid blocks found in the data zone.
		 * Check the buffer zone, if there is one.
		 */
		if (!nr_blocks && bzone) {
			ret = dmz_block_valid(dzt, bzone, chunk_block);
			if (ret < 0)
				return ret;
			if (ret > 0) {
				/* Read buffer zone blocks */
				nr_blocks = ret;
				rzone = bzone;
			}
		}

		if (nr_blocks) {

			/* Valid blocks found: read them */
			nr_blocks = min_t(unsigned int, nr_blocks,
					  end_block - chunk_block);

			dmz_dev_debug(dzt,
				"=> %s READ zone %u, block %llu, %u blocks\n",
				(dmz_is_buf(rzone) ? "BUF" : "DATA"),
				dmz_id(dzt, rzone),
				(unsigned long long)chunk_block,
				nr_blocks);

			ret = dmz_submit_bio(dzt, rzone, bio,
					     chunk_block, nr_blocks);
			if (ret)
				return ret;
			chunk_block += nr_blocks;

		} else {

			/* No valid block: zeroout the current BIO block */
			dmz_handle_read_zero(dzt, bio, chunk_block, 1);
			chunk_block++;

		}

	}

	return 0;
}

/*
 * Write blocks directly in a data zone, at the write pointer.
 * If a buffer zone is assigned, invalidate the blocks written
 * in place.
 */
static int dmz_handle_direct_write(struct dm_zoned_target *dzt,
				   struct dm_zone *dzone, struct bio *bio,
				   sector_t chunk_block,
				   unsigned int nr_blocks)
{
	struct dm_zone *bzone = dzone->bzone;
	int ret;

	dmz_dev_debug(dzt,
		      "WRITE %s zone %u, block %llu, %u blocks\n",
		      (dmz_is_rnd(dzone) ? "RND" : "SEQ"),
		      dmz_id(dzt, dzone),
		      (unsigned long long)chunk_block,
		      nr_blocks);

	if (dmz_is_readonly(dzone))
		return -EROFS;

	/* Submit write */
	ret = dmz_submit_bio(dzt, dzone, bio,
			     chunk_block, nr_blocks);
	if (ret)
		return -EIO;

	if (dmz_is_seq(dzone))
		dzone->wp_block += nr_blocks;

	/*
	 * Validate the blocks in the data zone and invalidate
	 * in the buffer zone, if there is one.
	 */
	ret = dmz_validate_blocks(dzt, dzone,
				  chunk_block, nr_blocks);
	if (ret == 0 && bzone)
		ret = dmz_invalidate_blocks(dzt, bzone,
					    chunk_block, nr_blocks);

	return ret;
}

/*
 * Write blocks in the buffer zone of @zone.
 * If no buffer zone is assigned yet, get one.
 * Called with @zone write locked.
 */
static int dmz_handle_buffered_write(struct dm_zoned_target *dzt,
				     struct dm_zone *dzone, struct bio *bio,
				     sector_t chunk_block,
				     unsigned int nr_blocks)
{
	struct dm_zone *bzone = dzone->bzone;
	int ret;

	if (!bzone) {
		/* Get a buffer zone */
		bzone = dmz_get_chunk_buffer(dzt, dzone);
		if (!bzone)
			return -ENOSPC;
	}

	dmz_dev_debug(dzt,
		      "WRITE BUF zone %u, block %llu, %u blocks\n",
		      dmz_id(dzt, bzone),
		      (unsigned long long)chunk_block,
		      nr_blocks);

	if (dmz_is_readonly(bzone))
		return -EROFS;

	/* Submit write */
	ret = dmz_submit_bio(dzt, bzone, bio,
			     chunk_block, nr_blocks);
	if (ret)
		return -EIO;

	/*
	 * Validate the blocks in the buffer zone
	 * and invalidate in the data zone.
	 */
	ret = dmz_validate_blocks(dzt, bzone,
				  chunk_block, nr_blocks);
	if (ret == 0 && chunk_block < dzone->wp_block)
		ret = dmz_invalidate_blocks(dzt, dzone,
					    chunk_block, nr_blocks);

	return ret;
}

/*
 * Process a write BIO.
 */
static int dmz_handle_write(struct dm_zoned_target *dzt,
			    struct dm_zone *dzone, struct bio *bio)
{
	sector_t block = dmz_bio_block(bio);
	unsigned int nr_blocks = dmz_bio_blocks(bio);
	sector_t chunk_block = dmz_chunk_block(dzt, block);
	int ret;

	if (!dzone)
		return -ENOSPC;

	if (dmz_is_rnd(dzone) ||
	    chunk_block == dzone->wp_block)
		/*
		 * dzone is a random zone, or it is a sequential zone
		 * and the BIO is aligned to the zone write pointer:
		 * direct write the zone.
		 */
		ret = dmz_handle_direct_write(dzt, dzone, bio,
					      chunk_block, nr_blocks);
	else
		/*
		 * This is an unaligned write in a sequential zone:
		 * use buffered write.
		 */
		ret = dmz_handle_buffered_write(dzt, dzone, bio,
						chunk_block, nr_blocks);

	dmz_validate_zone(dzt, dzone);

	return ret;
}

/*
 * Process a discard BIO.
 */
static int dmz_handle_discard(struct dm_zoned_target *dzt,
			      struct dm_zone *dzone, struct bio *bio)
{
	sector_t block = dmz_bio_block(bio);
	unsigned int nr_blocks = dmz_bio_blocks(bio);
	sector_t chunk_block = dmz_chunk_block(dzt, block);
	int ret;

	/* For unmapped chunks, there is nothing to do */
	if (!dzone)
		return 0;

	if (dmz_is_readonly(dzone))
		return -EROFS;

	dmz_dev_debug(dzt,
		"DISCARD chunk %llu -> zone %u, block %llu, %u blocks\n",
		(unsigned long long)dmz_bio_chunk(dzt, bio),
		dmz_id(dzt, dzone),
		(unsigned long long)chunk_block,
		nr_blocks);

	/*
	 * Invalidate blocks in the data zone and its
	 * buffer zone if one is mapped.
	 */
	ret = dmz_invalidate_blocks(dzt, dzone,
				    chunk_block, nr_blocks);
	if (ret == 0 && dzone->bzone)
		ret = dmz_invalidate_blocks(dzt, dzone->bzone,
					    chunk_block, nr_blocks);

	dmz_validate_zone(dzt, dzone);

	return ret;
}

/*
 * Process a BIO.
 */
static void dmz_handle_bio(struct dm_zoned_target *dzt,
			   struct dm_zone *zone, struct bio *bio)
{
	int is_sync;
	int ret;

	if (zone)
		down_read(&dzt->mblk_sem);

	is_sync = (bio_op(bio) != REQ_OP_READ) &&
		op_is_sync(bio->bi_opf);

	/* Process the BIO */
	switch (bio_op(bio)) {
	case REQ_OP_READ:
		ret = dmz_handle_read(dzt, zone, bio);
		break;
	case REQ_OP_WRITE:
		ret = dmz_handle_write(dzt, zone, bio);
		break;
	case REQ_OP_DISCARD:
		ret = dmz_handle_discard(dzt, zone, bio);
		break;
	default:
		dmz_dev_err(dzt,
			    "Unknown BIO type 0x%x\n",
			    bio_op(bio));
		ret = -EIO;
		break;
	}

	if (zone)
		up_read(&dzt->mblk_sem);

	dmz_bio_end(bio, ret);
}

/*
 * Zone BIO work function.
 */
static void dmz_bio_work(struct work_struct *work)
{
	struct dm_zone_work *zwork =
		container_of(work, struct dm_zone_work, work);
	struct dm_zoned_target *dzt = zwork->target;
	struct dm_zone *zone = zwork->zone;
	unsigned long flags;
	struct bio *bio;

	/* Process BIOs */
	while (1) {

		spin_lock_irqsave(&dzt->zwork_lock, flags);
		bio = bio_list_pop(&zwork->bio_list);
		spin_unlock_irqrestore(&dzt->zwork_lock, flags);

		if (!bio)
			break;

		dmz_handle_bio(dzt, zone, bio);

	}

	dmz_put_zwork(zwork);
}

/*
 * Flush work.
 */
static void dmz_flush_work(struct work_struct *work)
{
	struct dm_zoned_target *dzt =
		container_of(work, struct dm_zoned_target, flush_work.work);
	struct bio *bio;
	int ret;

	/* Do flush */
	ret = dmz_flush_mblocks(dzt);

	/* Process queued flush requests */
	while (1) {

		spin_lock(&dzt->flush_lock);
		bio = bio_list_pop(&dzt->flush_list);
		spin_unlock(&dzt->flush_lock);

		if (!bio)
			break;

		dmz_bio_end(bio, ret);

	}

	mod_delayed_work(dzt->flush_wq, &dzt->flush_work,
			 DMZ_FLUSH_PERIOD);
}

/*
 * Find out the zone mapping of a new BIO and process it.
 * For read and discard BIOs, no mapping may exist. For write BIOs, a mapping
 * is created (i.e. a zone allocated) is none already existed.
 */
static void dmz_map_bio(struct dm_zoned_target *dzt, struct bio *bio)
{
	struct dm_zone_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct dm_zone_bioctx));
	struct dm_zone_work *zwork;
	struct dm_zone *zone;
	unsigned long flags;

	/*
	 * Get the data zone mapping the chunk that the BIO
	 * is targeting. If there is no mapping, directly
	 * process the BIO.
	 */
	zone = dmz_get_chunk_mapping(dzt, dmz_bio_chunk(dzt, bio),
				     bio_op(bio));
	if (IS_ERR_OR_NULL(zone)) {
		if (IS_ERR(zone))
			dmz_bio_end(bio, PTR_ERR(zone));
		else
			dmz_handle_bio(dzt, NULL, bio);
		return;
	}

	/* Setup the zone work */
	spin_lock_irqsave(&dzt->zwork_lock, flags);

	WARN_ON(dmz_in_reclaim(zone));
	zwork = zone->work;
	if (zwork) {
		/* Keep current work */
		kref_get(&zwork->kref);
	} else {
		/* Get a new work */
		zwork = dmz_alloc_zwork(dzt);
		if (unlikely(!zwork)) {
			dmz_bio_end(bio, -ENOMEM);
			goto out;
		}
		zwork->zone = zone;
		zone->work = zwork;
		atomic_inc(&dzt->nr_active_zones);
	}

	/* Queue the BIO and the zone work */
	bioctx->zwork = zwork;
	bio_list_add(&zwork->bio_list, bio);
	if (queue_work(dzt->zone_wq, &zwork->work))
		kref_get(&zwork->kref);
out:
	spin_unlock_irqrestore(&dzt->zwork_lock, flags);
}

/*
 * Process a new BIO.
 */
static int dmz_map(struct dm_target *ti, struct bio *bio)
{
	struct dm_zoned_target *dzt = ti->private;
	struct dm_zone_bioctx *bioctx
		= dm_per_bio_data(bio, sizeof(struct dm_zone_bioctx));
	sector_t sector = bio->bi_iter.bi_sector;
	unsigned int nr_sectors = bio_sectors(bio);
	sector_t chunk_sector;

	dmz_dev_debug(dzt,
		"BIO sector %llu + %u => chunk %llu, block %llu, %u blocks\n",
		(u64)sector, nr_sectors,
		(u64)dmz_bio_chunk(dzt, bio),
		(u64)dmz_chunk_block(dzt, dmz_bio_block(bio)),
		(unsigned int)dmz_bio_blocks(bio));

	bio->bi_bdev = dzt->zbd;

	if (!nr_sectors &&
	    (bio_op(bio) != REQ_OP_FLUSH) &&
	    (bio_op(bio) != REQ_OP_WRITE)) {
		bio->bi_bdev = dzt->zbd;
		return DM_MAPIO_REMAPPED;
	}

	/* The BIO should be block aligned */
	if ((nr_sectors & DMZ_BLOCK_SECTORS_MASK) ||
	    (sector & DMZ_BLOCK_SECTORS_MASK)) {
		dmz_dev_err(dzt,
			    "Unaligned BIO sector %llu, len %u\n",
			    (u64)sector,
			    nr_sectors);
		return -EIO;
	}

	/* Initialize the BIO context */
	bioctx->target = dzt;
	bioctx->zwork = NULL;
	bioctx->bio = bio;
	atomic_set(&bioctx->ref, 1);
	bioctx->error = 0;

	atomic_inc(&dzt->bio_count);
	dzt->last_bio_time = jiffies;

	/* Set the BIO pending in the flush list */
	if (bio_op(bio) == REQ_OP_FLUSH ||
	    (!nr_sectors && bio_op(bio) == REQ_OP_WRITE)) {
		spin_lock(&dzt->flush_lock);
		bio_list_add(&dzt->flush_list, bio);
		spin_unlock(&dzt->flush_lock);
		dmz_trigger_flush(dzt);
		return DM_MAPIO_SUBMITTED;
	}

	/* Split zone BIOs to fit entirely into a zone */
	chunk_sector = dmz_chunk_sector(dzt, sector);
	if (chunk_sector + nr_sectors > dzt->zone_nr_sectors)
		dm_accept_partial_bio(bio,
				      dzt->zone_nr_sectors - chunk_sector);

	/* Now ready to handle this BIO */
	dmz_map_bio(dzt, bio);

	return DM_MAPIO_SUBMITTED;
}

/**
 * Parse dmsetup arguments.
 */
static int dmz_parse_args(struct dm_target *ti,
			  struct dm_arg_set *as,
			  struct dm_zoned_target_config *conf)
{
	const char *arg;

	/* Check arguments */
	if (as->argc < 1) {
		ti->error = "No target device specified";
		return -EINVAL;
	}

	/* Set defaults */
	conf->dev_path = (char *) dm_shift_arg(as);
	conf->flags = 0;
	conf->reclaim_low = DMZ_RECLAIM_LOW;
	conf->reclaim_idle_low = DMZ_RECLAIM_IDLE_LOW;

	while (as->argc) {

		arg = dm_shift_arg(as);

		if (strncmp(arg, "idle_rlow=", 9) == 0) {
			if (kstrtoul(arg + 9, 0, &conf->reclaim_idle_low) < 0 ||
			    conf->reclaim_idle_low > 100) {
				ti->error = "Invalid idle_rlow value";
				return -EINVAL;
			}
		} else if (strncmp(arg, "rlow=", 9) == 0) {
			if (kstrtoul(arg + 9, 0, &conf->reclaim_low) < 0 ||
			    conf->reclaim_low > 100) {
				ti->error = "Invalid rlow value";
				return -EINVAL;
			}
		} else {
			ti->error = "Unknown argument";
			return -EINVAL;
		}

	}

	return 0;
}

/*
 * Setup target.
 */
static int dmz_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct dm_zoned_target_config conf;
	struct dm_zoned_target *dzt;
	struct dm_arg_set as;
	int ret;

	/* Parse arguments */
	as.argc = argc;
	as.argv = argv;
	ret = dmz_parse_args(ti, &as, &conf);
	if (ret)
		return ret;

	/* Allocate and initialize the target descriptor */
	dzt = kzalloc(sizeof(struct dm_zoned_target), GFP_KERNEL);
	if (!dzt) {
		ti->error = "Allocate target descriptor failed";
		return -ENOMEM;
	}

	/* Get the target device */
	ret = dm_get_device(ti, conf.dev_path,
			    dm_table_get_mode(ti->table), &dzt->ddev);
	if (ret != 0) {
		ti->error = "Get target device failed";
		goto err;
	}

	dzt->zbd = dzt->ddev->bdev;
	if (!bdev_is_zoned(dzt->zbd)) {
		ti->error = "Not a zoned block device";
		ret = -EINVAL;
		goto err;
	}

	dzt->zbd_capacity = i_size_read(dzt->zbd->bd_inode) >> SECTOR_SHIFT;
	if (ti->begin || (ti->len != dzt->zbd_capacity)) {
		ti->error = "Partial mapping not supported";
		ret = -EINVAL;
		goto err;
	}

	(void)bdevname(dzt->zbd, dzt->zbd_name);
	dzt->zbdq = bdev_get_queue(dzt->zbd);
	dzt->flags = conf.flags;

	dzt->zones = RB_ROOT;

	dzt->mblk_rbtree = RB_ROOT;
	init_rwsem(&dzt->mblk_sem);
	spin_lock_init(&dzt->mblk_lock);
	INIT_LIST_HEAD(&dzt->mblk_lru_list);
	INIT_LIST_HEAD(&dzt->mblk_dirty_list);

	mutex_init(&dzt->map_lock);
	atomic_set(&dzt->dz_unmap_nr_rnd, 0);
	INIT_LIST_HEAD(&dzt->dz_unmap_rnd_list);
	INIT_LIST_HEAD(&dzt->dz_map_rnd_list);

	atomic_set(&dzt->dz_unmap_nr_seq, 0);
	INIT_LIST_HEAD(&dzt->dz_unmap_seq_list);
	INIT_LIST_HEAD(&dzt->dz_map_seq_list);

	init_waitqueue_head(&dzt->dz_free_wq);

	atomic_set(&dzt->nr_active_zones, 0);

	atomic_set(&dzt->nr_reclaim_seq_zones, 0);
	INIT_LIST_HEAD(&dzt->reclaim_seq_zones_list);

	dmz_dev_info(dzt,
		     "Target device: host-%s zoned block device %s\n",
		     bdev_zoned_model(dzt->zbd) == BLK_ZONED_HA ?
		     "aware" : "managed",
		     dzt->zbd_name);

	ret = dmz_init_meta(dzt, &conf);
	if (ret != 0) {
		ti->error = "Metadata initialization failed";
		goto err;
	}

	/* Set target (no write same support) */
	ti->private = dzt;
	ti->max_io_len = dzt->zone_nr_sectors << 9;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->num_write_same_bios = 0;
	ti->per_io_data_size = sizeof(struct dm_zone_bioctx);
	ti->flush_supported = true;
	ti->discards_supported = true;
	ti->split_discard_bios = true;
	ti->discard_zeroes_data_unsupported = false;

	/* The target capacity is the number of chunks that can be mapped */
	ti->len = dzt->nr_chunks * dzt->zone_nr_sectors;

	/* zone BIO work */
	atomic_set(&dzt->bio_count, 0);
	spin_lock_init(&dzt->zwork_lock);
	dzt->bio_set = bioset_create(DMZ_MIN_BIOS, 0);
	if (!dzt->bio_set) {
		ti->error = "Create BIO set failed";
		ret = -ENOMEM;
		goto err;
	}

	dzt->zone_wq = alloc_workqueue("dm_zoned_zwq_%s",
				       WQ_MEM_RECLAIM | WQ_UNBOUND,
				       0,
				       dzt->zbd_name);
	if (!dzt->zone_wq) {
		ti->error = "Create zone BIO workqueue failed";
		ret = -ENOMEM;
		goto err;
	}

	/* Flush work */
	spin_lock_init(&dzt->flush_lock);
	bio_list_init(&dzt->flush_list);
	INIT_DELAYED_WORK(&dzt->flush_work, dmz_flush_work);
	dzt->flush_wq = alloc_ordered_workqueue("dm_zoned_fwq_%s",
						WQ_MEM_RECLAIM | WQ_UNBOUND,
						dzt->zbd_name);
	if (!dzt->flush_wq) {
		ti->error = "Create flush workqueue failed";
		ret = -ENOMEM;
		goto err;
	}
	mod_delayed_work(dzt->flush_wq, &dzt->flush_work, DMZ_FLUSH_PERIOD);

	/* Conventional zone reclaim work */
	INIT_DELAYED_WORK(&dzt->reclaim_work, dmz_reclaim_work);
	dzt->reclaim_wq = alloc_ordered_workqueue("dm_zoned_rwq_%s",
						  WQ_MEM_RECLAIM | WQ_UNBOUND,
						  dzt->zbd_name);
	if (!dzt->reclaim_wq) {
		ti->error = "Create reclaim workqueue failed";
		ret = -ENOMEM;
		goto err;
	}
	dzt->reclaim_low = conf.reclaim_low;
	dzt->reclaim_idle_low = conf.reclaim_idle_low;
	if (dzt->reclaim_low > DMZ_RECLAIM_MAX)
		dzt->reclaim_low = DMZ_RECLAIM_MAX;
	if (dzt->reclaim_low < DMZ_RECLAIM_MIN)
		dzt->reclaim_low = DMZ_RECLAIM_MIN;
	if (dzt->reclaim_idle_low > DMZ_RECLAIM_IDLE_MAX)
		dzt->reclaim_idle_low = DMZ_RECLAIM_IDLE_MAX;
	if (dzt->reclaim_idle_low < dzt->reclaim_low)
		dzt->reclaim_idle_low = dzt->reclaim_low;

	dmz_dev_info(dzt,
		"Target device: %llu 512-byte logical sectors (%llu blocks)\n",
		(unsigned long long)ti->len,
		(unsigned long long)dmz_sect2blk(ti->len));

	dzt->last_bio_time = jiffies;
	dmz_trigger_reclaim(dzt);

	return 0;

err:
	if (dzt->ddev) {
		if (dzt->reclaim_wq)
			destroy_workqueue(dzt->reclaim_wq);
		if (dzt->flush_wq)
			destroy_workqueue(dzt->flush_wq);
		if (dzt->zone_wq)
			destroy_workqueue(dzt->zone_wq);
		if (dzt->bio_set)
			bioset_free(dzt->bio_set);
		dmz_cleanup_meta(dzt);
		dm_put_device(ti, dzt->ddev);
	}

	kfree(dzt);

	return ret;

}

/*
 * Cleanup target.
 */
static void dmz_dtr(struct dm_target *ti)
{
	struct dm_zoned_target *dzt = ti->private;

	dmz_dev_info(dzt, "Removing target device\n");

	flush_workqueue(dzt->zone_wq);
	destroy_workqueue(dzt->zone_wq);

	cancel_delayed_work_sync(&dzt->reclaim_work);
	destroy_workqueue(dzt->reclaim_wq);

	cancel_delayed_work_sync(&dzt->flush_work);
	destroy_workqueue(dzt->flush_wq);

	dmz_flush_mblocks(dzt);

	bioset_free(dzt->bio_set);

	dmz_cleanup_meta(dzt);

	dm_put_device(ti, dzt->ddev);

	kfree(dzt);
}

/*
 * Setup target request queue limits.
 */
static void dmz_io_hints(struct dm_target *ti,
			 struct queue_limits *limits)
{
	struct dm_zoned_target *dzt = ti->private;
	unsigned int chunk_sectors = dzt->zone_nr_sectors;

	/* Align to zone size */
	limits->chunk_sectors = chunk_sectors;
	limits->max_sectors = chunk_sectors;

	blk_limits_io_min(limits, DMZ_BLOCK_SIZE);
	blk_limits_io_opt(limits, DMZ_BLOCK_SIZE);

	limits->logical_block_size = DMZ_BLOCK_SIZE;
	limits->physical_block_size = DMZ_BLOCK_SIZE;

	limits->discard_alignment = DMZ_BLOCK_SIZE;
	limits->discard_granularity = DMZ_BLOCK_SIZE;
	limits->max_discard_sectors = chunk_sectors;
	limits->max_hw_discard_sectors = chunk_sectors;
	limits->discard_zeroes_data = true;

}

/*
 * Pass on ioctl to the backend device.
 */
static int dmz_prepare_ioctl(struct dm_target *ti,
			     struct block_device **bdev, fmode_t *mode)
{
	struct dm_zoned_target *dzt = ti->private;

	*bdev = dzt->zbd;

	return 0;
}

/*
 * Stop reclaim before suspend.
 */
static void dmz_presuspend(struct dm_target *ti)
{
	struct dm_zoned_target *dzt = ti->private;

	dmz_dev_debug(dzt, "Pre-suspend\n");

	/* Enter suspend state */
	set_bit(DMZ_SUSPENDED, &dzt->flags);
	smp_mb__after_atomic();

	/* Stop reclaim */
	cancel_delayed_work_sync(&dzt->reclaim_work);
}

/*
 * Restart reclaim if suspend failed.
 */
static void dmz_presuspend_undo(struct dm_target *ti)
{
	struct dm_zoned_target *dzt = ti->private;

	dmz_dev_debug(dzt, "Pre-suspend undo\n");

	/* Clear suspend state */
	clear_bit_unlock(DMZ_SUSPENDED, &dzt->flags);
	smp_mb__after_atomic();

	/* Restart reclaim */
	mod_delayed_work(dzt->reclaim_wq, &dzt->reclaim_work, 0);
}

/*
 * Stop works and flush on suspend.
 */
static void dmz_postsuspend(struct dm_target *ti)
{
	struct dm_zoned_target *dzt = ti->private;

	dmz_dev_debug(dzt, "Post-suspend\n");

	/* Stop works */
	flush_workqueue(dzt->zone_wq);
	flush_workqueue(dzt->flush_wq);
}

/*
 * Refresh zone information before resuming.
 */
static int dmz_preresume(struct dm_target *ti)
{
	struct dm_zoned_target *dzt = ti->private;

	if (!test_bit(DMZ_SUSPENDED, &dzt->flags))
		return 0;

	dmz_dev_debug(dzt, "Pre-resume\n");

	/* Refresh zone information */
	return dmz_resume_meta(dzt);
}

/*
 * Resume.
 */
static void dmz_resume(struct dm_target *ti)
{
	struct dm_zoned_target *dzt = ti->private;

	if (!test_bit(DMZ_SUSPENDED, &dzt->flags))
		return;

	dmz_dev_debug(dzt, "Resume\n");

	/* Clear suspend state */
	clear_bit_unlock(DMZ_SUSPENDED, &dzt->flags);
	smp_mb__after_atomic();

	/* Restart reclaim */
	mod_delayed_work(dzt->reclaim_wq, &dzt->reclaim_work, 0);

}

static int
dmz_iterate_devices(struct dm_target *ti,
		    iterate_devices_callout_fn fn,
		    void *data)
{
	struct dm_zoned_target *dzt = ti->private;
	sector_t offset = dzt->zbd_capacity -
		((sector_t)dzt->nr_chunks * dzt->zone_nr_sectors);

	return fn(ti, dzt->ddev, offset, ti->len, data);
}

static struct target_type dm_zoned_type = {
	.name		 = "dm-zoned",
	.version	 = {1, 0, 0},
	.module	 = THIS_MODULE,
	.ctr		 = dmz_ctr,
	.dtr		 = dmz_dtr,
	.map		 = dmz_map,
	.io_hints	 = dmz_io_hints,
	.prepare_ioctl	 = dmz_prepare_ioctl,
	.presuspend	 = dmz_presuspend,
	.presuspend_undo = dmz_presuspend_undo,
	.postsuspend	 = dmz_postsuspend,
	.preresume	 = dmz_preresume,
	.resume		 = dmz_resume,
	.iterate_devices = dmz_iterate_devices,
};

struct kmem_cache *dmz_zone_cache;

static int __init dmz_init(void)
{
	int ret;

	dmz_info("Version %d.%d, (C) Western Digital\n",
		 DMZ_VER_MAJ,
		 DMZ_VER_MIN);

	dmz_zone_cache = KMEM_CACHE(dm_zone, 0);
	if (!dmz_zone_cache)
		return -ENOMEM;

	ret = dm_register_target(&dm_zoned_type);
	if (ret != 0) {
		kmem_cache_destroy(dmz_zone_cache);
		return ret;
	}

	return 0;
}

static void __exit dmz_exit(void)
{
	dm_unregister_target(&dm_zoned_type);
	kmem_cache_destroy(dmz_zone_cache);
}

module_init(dmz_init);
module_exit(dmz_exit);

MODULE_DESCRIPTION(DM_NAME " target for zoned block devices");
MODULE_AUTHOR("Damien Le Moal <damien.lemoal@wdc.com>");
MODULE_LICENSE("GPL");
