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
 * I/O region BIO completion callback.
 */
static void dmz_reclaim_endio(struct bio *bio)
{
	struct dm_zoned_ioreg *ioreg = bio->bi_private;

	ioreg->err = bio->bi_error;
	complete(&ioreg->wait);
}

/*
 * Free an I/O region.
 */
static void dmz_reclaim_free_ioreg(struct dm_zoned_ioreg *ioreg)
{
	int i;

	if (ioreg->bvec) {
		for (i = 0; i < ioreg->nr_bvecs; i++)
			__free_page(ioreg->bvec[i].bv_page);
		kfree(ioreg->bvec);
	}
	kfree(ioreg);
}

/*
 * Allocate and initialize an I/O region and its BIO.
 */
static struct dm_zoned_ioreg *dmz_reclaim_alloc_ioreg(sector_t chunk_block,
						unsigned int nr_blocks)
{
	struct dm_zoned_ioreg *ioreg;
	unsigned int nr_bvecs;
	struct bio_vec *bvec, *bv;
	int i;

	ioreg = kmalloc(sizeof(struct dm_zoned_ioreg), GFP_NOIO | __GFP_ZERO);
	if (!ioreg)
		return NULL;

	nr_bvecs = min_t(unsigned int, BIO_MAX_PAGES,
			 ((nr_blocks << DMZ_BLOCK_SHIFT) + PAGE_SIZE - 1)
			 >> PAGE_SHIFT);
	nr_blocks = min_t(unsigned int, nr_blocks,
			  nr_bvecs << (PAGE_SHIFT - DMZ_BLOCK_SHIFT));

	bvec = kcalloc(nr_bvecs, sizeof(struct bio_vec), GFP_NOIO);
	if (!bvec)
		goto err;

	ioreg->chunk_block = chunk_block;
	ioreg->nr_blocks = nr_blocks;
	ioreg->nr_bvecs = nr_bvecs;
	ioreg->bvec = bvec;

	for (i = 0; i < nr_bvecs; i++) {

		bv = &bvec[i];

		bv->bv_offset = 0;
		bv->bv_len = min_t(unsigned int, PAGE_SIZE,
				   nr_blocks << DMZ_BLOCK_SHIFT);

		bv->bv_page = alloc_page(GFP_NOIO);
		if (!bv->bv_page)
			goto err;

		nr_blocks -= bv->bv_len >> DMZ_BLOCK_SHIFT;
	}

	return ioreg;

err:
	dmz_reclaim_free_ioreg(ioreg);

	return NULL;
}

/*
 * Submit an I/O region for reading or writing in @zone.
 */
static void dmz_reclaim_submit_ioreg(struct dm_zoned_target *dzt,
				     struct dm_zone *zone,
				     struct dm_zoned_ioreg *ioreg,
				     unsigned int op)
{
	struct bio *bio = &ioreg->bio;

	init_completion(&ioreg->wait);
	ioreg->err = 0;

	bio_init(bio, ioreg->bvec, ioreg->nr_bvecs);
	bio->bi_vcnt = ioreg->nr_bvecs;
	bio->bi_bdev = dzt->zbd;
	bio->bi_end_io = dmz_reclaim_endio;
	bio->bi_private = ioreg;
	bio->bi_iter.bi_sector = dmz_blk2sect(dmz_sect2blk(zone->sector)
					      + ioreg->chunk_block);
	bio->bi_iter.bi_size = ioreg->nr_blocks << DMZ_BLOCK_SHIFT;
	bio_set_op_attrs(bio, op, 0);

	submit_bio(bio);
}

/*
 * Read the next region of valid blocks after @chunk_block
 * in @zone.
 */
static struct dm_zoned_ioreg *dmz_reclaim_read(struct dm_zoned_target *dzt,
					       struct dm_zone *zone,
					       sector_t chunk_block)
{
	struct dm_zoned_ioreg *ioreg;
	int ret;

	if (chunk_block >= dzt->zone_nr_blocks)
		return NULL;

	/* Get valid block range */
	ret = dmz_first_valid_block(dzt, zone, &chunk_block);
	if (ret < 0)
		return ERR_PTR(ret);
	if (!ret)
		return NULL;

	/* Build I/O region */
	ioreg = dmz_reclaim_alloc_ioreg(chunk_block, ret);
	if (!ioreg)
		return ERR_PTR(-ENOMEM);

	dmz_dev_debug(dzt,
		      "Reclaim: Read %s zone %u, block %llu+%u\n",
		      dmz_is_rnd(zone) ? "RND" : "SEQ",
		      dmz_id(dzt, zone),
		      (unsigned long long)chunk_block,
		      ioreg->nr_blocks);

	dmz_reclaim_submit_ioreg(dzt, zone, ioreg, REQ_OP_READ);

	return ioreg;

}

/*
 * Align a sequential zone write pointer to chunk_block.
 */
static int dmz_reclaim_align_wp(struct dm_zoned_target *dzt,
				struct dm_zone *zone, sector_t chunk_block)
{
	sector_t wp_block = zone->wp_block;
	unsigned int nr_blocks;
	int ret;

	if (wp_block > chunk_block)
		return -EIO;

	/*
	 * Zeroout the space between the write
	 * pointer and the requested position.
	 */
	nr_blocks = chunk_block - zone->wp_block;
	if (!nr_blocks)
		return 0;

	ret = blkdev_issue_zeroout(dzt->zbd,
				   zone->sector + dmz_blk2sect(wp_block),
				   dmz_blk2sect(nr_blocks),
				   GFP_NOIO, false);
	if (ret) {
		dmz_dev_err(dzt,
			    "Align zone %u wp %llu to +%u blocks failed %d\n",
			    dmz_id(dzt, zone),
			    (unsigned long long)wp_block,
			    nr_blocks,
			    ret);
		return ret;
	}

	zone->wp_block += nr_blocks;

	return 0;
}

/*
 * Write blocks.
 */
static int dmz_reclaim_write(struct dm_zoned_target *dzt,
			     struct dm_zone *zone,
			     struct dm_zoned_ioreg **ioregs,
			     unsigned int nr_ioregs)
{
	struct dm_zoned_ioreg *ioreg;
	sector_t chunk_block;
	int i, ret = 0;

	for (i = 0; i < nr_ioregs; i++) {

		ioreg = ioregs[i];

		/* Wait for the read I/O to complete */
		wait_for_completion_io(&ioreg->wait);

		if (ret || ioreg->err) {
			if (ret == 0)
				ret = ioreg->err;
			dmz_reclaim_free_ioreg(ioreg);
			ioregs[i] = NULL;
			continue;
		}

		chunk_block = ioreg->chunk_block;

		dmz_dev_debug(dzt,
			      "Reclaim: Write %s zone %u, block %llu+%u\n",
			      dmz_is_rnd(zone) ? "RND" : "SEQ",
			      dmz_id(dzt, zone),
			      (unsigned long long)chunk_block,
			      ioreg->nr_blocks);

		/*
		 * If we are writing in a sequential zones,
		 * we must make sure that writes are sequential. So
		 * fill up any eventual hole between writes.
		 */
		if (dmz_is_seq(zone)) {
			ret = dmz_reclaim_align_wp(dzt, zone, chunk_block);
			if (ret)
				break;
		}

		/* Do write */
		dmz_reclaim_submit_ioreg(dzt, zone, ioreg, REQ_OP_WRITE);
		wait_for_completion_io(&ioreg->wait);

		ret = ioreg->err;
		if (ret) {
			dmz_dev_err(dzt, "Reclaim: Write failed\n");
		} else {
			ret = dmz_validate_blocks(dzt, zone, chunk_block,
						  ioreg->nr_blocks);
			if (ret == 0 && dmz_is_seq(zone))
				zone->wp_block += ioreg->nr_blocks;
		}

		ioregs[i] = NULL;
		dmz_reclaim_free_ioreg(ioreg);

	}

	return ret;
}

/*
 * Move valid blocks of src_zone into dst_zone.
 */
static int dmz_reclaim_copy_zone(struct dm_zoned_target *dzt,
				 struct dm_zone *src_zone,
				 struct dm_zone *dst_zone)
{
	struct dm_zoned_ioreg *ioregs[DMZ_RECLAIM_MAX_IOREGS];
	struct dm_zoned_ioreg *ioreg;
	sector_t chunk_block = 0, end_block;
	int nr_ioregs = 0, i, ret;

	if (dmz_is_seq(src_zone))
		end_block = src_zone->wp_block;
	else
		end_block = dzt->zone_nr_blocks;

	while (chunk_block < end_block) {

		/* Read valid regions from source zone */
		nr_ioregs = 0;
		while (nr_ioregs < DMZ_RECLAIM_MAX_IOREGS &&
		       chunk_block < end_block) {

			ioreg = dmz_reclaim_read(dzt, src_zone, chunk_block);
			if (IS_ERR(ioreg)) {
				ret = PTR_ERR(ioreg);
				goto err;
			}
			if (!ioreg)
				break;

			chunk_block = ioreg->chunk_block + ioreg->nr_blocks;
			ioregs[nr_ioregs] = ioreg;
			nr_ioregs++;

		}

		/* Are we done ? */
		if (!nr_ioregs)
			break;

		/* Write in destination zone */
		ret = dmz_reclaim_write(dzt, dst_zone, ioregs, nr_ioregs);
		if (ret != 0)
			goto err;

	}

	return 0;

err:
	for (i = 0; i < nr_ioregs; i++) {
		ioreg = ioregs[i];
		if (ioreg) {
			wait_for_completion_io(&ioreg->wait);
			dmz_reclaim_free_ioreg(ioreg);
		}
	}

	return ret;
}

/*
 * Allocate a sequential zone.
 */
static struct dm_zone *dmz_reclaim_alloc_seq_zone(struct dm_zoned_target *dzt)
{
	struct dm_zone *zone;
	int ret;

	dmz_lock_map(dzt);
	zone = dmz_alloc_zone(dzt, DMZ_ALLOC_RECLAIM);
	dmz_unlock_map(dzt);

	if (!zone)
		return NULL;

	ret = dmz_reset_zone(dzt, zone);
	if (ret != 0) {
		dmz_lock_map(dzt);
		dmz_free_zone(dzt, zone);
		dmz_unlock_map(dzt);
		return NULL;
	}

	return zone;
}

/*
 * Clear a zone reclaim flag.
 */
static inline void dmz_reclaim_put_zone(struct dm_zoned_target *dzt,
					struct dm_zone *zone)
{
	WARN_ON(dmz_is_active(zone));
	WARN_ON(!dmz_in_reclaim(zone));

	clear_bit_unlock(DMZ_RECLAIM, &zone->flags);
	smp_mb__after_atomic();
	wake_up_bit(&zone->flags, DMZ_RECLAIM);
}

/*
 * Move valid blocks of dzone buffer zone into dzone
 * and free the buffer zone.
 */
static int dmz_reclaim_buf(struct dm_zoned_target *dzt,
			   struct dm_zone *dzone)
{
	struct dm_zone *bzone = dzone->bzone;
	int ret;

	dmz_dev_debug(dzt,
		"Chunk %u, move buf zone %u (weight %u) "
		"to data zone %u (weight %u)\n",
		dzone->chunk,
		dmz_id(dzt, bzone),
		dmz_weight(bzone),
		dmz_id(dzt, dzone),
		dmz_weight(dzone));

	/* Flush data zone into the buffer zone */
	ret = dmz_reclaim_copy_zone(dzt, bzone, dzone);
	if (ret < 0)
		return ret;

	/* Free the buffer zone */
	dmz_invalidate_zone(dzt, bzone);
	dmz_lock_map(dzt);
	dmz_unmap_zone(dzt, bzone);
	dmz_reclaim_put_zone(dzt, dzone);
	dmz_free_zone(dzt, bzone);
	dmz_unlock_map(dzt);

	return 0;
}

/*
 * Move valid blocks of dzone into its buffer zone and free dzone.
 */
static int dmz_reclaim_seq_data(struct dm_zoned_target *dzt,
				struct dm_zone *dzone)
{
	unsigned int chunk = dzone->chunk;
	struct dm_zone *bzone = dzone->bzone;
	int ret = 0;

	dmz_dev_debug(dzt,
		"Chunk %u, move data zone %u (weight %u) "
		"to buf zone %u (weight %u)\n",
		chunk,
		dmz_id(dzt, dzone),
		dmz_weight(dzone),
		dmz_id(dzt, bzone),
		dmz_weight(bzone));

	/* Flush data zone into the buffer zone */
	ret = dmz_reclaim_copy_zone(dzt, dzone, bzone);
	if (ret < 0)
		return ret;

	/*
	 * Free the data zone and remap the chunk to
	 * the buffer zone.
	 */
	dmz_invalidate_zone(dzt, dzone);
	dmz_lock_map(dzt);
	dmz_unmap_zone(dzt, bzone);
	dmz_unmap_zone(dzt, dzone);
	dmz_reclaim_put_zone(dzt, dzone);
	dmz_free_zone(dzt, dzone);
	dmz_map_zone(dzt, bzone, chunk);
	dmz_unlock_map(dzt);

	return 0;
}

/*
 * Move valid blocks of the random data zone dzone into a free sequential data
 * zone. Once blocks are moved, remap the zone chunk to sequential zone.
 */
static int dmz_reclaim_rnd_data(struct dm_zoned_target *dzt,
				struct dm_zone *dzone)
{
	unsigned int chunk = dzone->chunk;
	struct dm_zone *szone = NULL;
	int ret;

	if (!dmz_weight(dzone))
		/* Empty zone: just free it */
		goto out;

	/* Get a free sequential zone */
	szone = dmz_reclaim_alloc_seq_zone(dzt);
	if (!szone)
		return -ENOSPC;

	dmz_dev_debug(dzt,
		"Chunk %u, move rnd zone %u (weight %u) to seq zone %u\n",
		chunk,
		dmz_id(dzt, dzone),
		dmz_weight(dzone),
		dmz_id(dzt, szone));

	/* Flush the random data zone into the sequential zone */
	ret = dmz_reclaim_copy_zone(dzt, dzone, szone);
	if (ret) {
		/* Invalidate the sequential zone and free it */
		dmz_invalidate_zone(dzt, szone);
		dmz_lock_map(dzt);
		dmz_free_zone(dzt, szone);
		dmz_unlock_map(dzt);
		return ret;
	}

	/* Invalidate all blocks in the data zone */
	dmz_invalidate_zone(dzt, dzone);

out:
	/* Free the data zone and remap the chunk */
	dmz_lock_map(dzt);
	dmz_unmap_zone(dzt, dzone);
	dmz_reclaim_put_zone(dzt, dzone);
	dmz_free_zone(dzt, dzone);
	if (szone)
		dmz_map_zone(dzt, szone, chunk);
	dmz_unlock_map(dzt);

	return 0;
}

/*
 * Lock a zone for reclaim. Returns 0 if the zone cannot be locked or if it is
 * already locked and 1 otherwise.
 */
static inline int dmz_reclaim_lock_zone(struct dm_zoned_target *dzt,
					struct dm_zone *zone)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&dzt->zwork_lock, flags);

	/* Active zones cannot be reclaimed */
	if (!dmz_is_active(zone))
		ret = !test_and_set_bit(DMZ_RECLAIM, &zone->flags);

	spin_unlock_irqrestore(&dzt->zwork_lock, flags);

	return ret;
}

/*
 * Select a random zone for reclaim.
 */
static struct dm_zone *dmz_reclaim_get_rnd_zone(struct dm_zoned_target *dzt)
{
	struct dm_zone *dzone = NULL;
	struct dm_zone *zone;

	if (list_empty(&dzt->dz_map_rnd_list))
		return NULL;

	list_for_each_entry(zone, &dzt->dz_map_rnd_list, link) {
		if (dmz_is_buf(zone))
			dzone = zone->bzone;
		else
			dzone = zone;
		if (dmz_reclaim_lock_zone(dzt, dzone))
			return dzone;
	}

	return NULL;
}

/*
 * Select a buffered sequential zone for reclaim.
 */
static struct dm_zone *dmz_reclaim_get_seq_zone(struct dm_zoned_target *dzt)
{
	struct dm_zone *zone;

	if (list_empty(&dzt->dz_map_seq_list))
		return NULL;

	list_for_each_entry(zone, &dzt->dz_map_seq_list, link) {
		if (!zone->bzone)
			continue;
		if (dmz_reclaim_lock_zone(dzt, zone))
			return zone;
	}

	return NULL;
}

/*
 * Select a zone for reclaim.
 */
static struct dm_zone *dmz_reclaim_get_zone(struct dm_zoned_target *dzt)
{
	struct dm_zone *zone = NULL;

	/*
	 * Search for a zone candidate to reclaim: 2 cases are possible.
	 * (1) There is no free sequential zones. Then a random data zone
	 *     cannot be reclaimed. So choose a sequential zone to reclaim so
	 *     that afterward a random zone can be reclaimed.
	 * (2) At least one free sequential zone is available, then choose
	 *     the oldest random zone (data or buffer) that can be locked.
	 */
	dmz_lock_map(dzt);
	if (list_empty(&dzt->reclaim_seq_zones_list))
		zone = dmz_reclaim_get_seq_zone(dzt);
	else
		zone = dmz_reclaim_get_rnd_zone(dzt);
	dmz_unlock_map(dzt);

	return zone;
}

/*
 * Find a reclaim candidate zone and reclaim it.
 */
static int dmz_reclaim(struct dm_zoned_target *dzt)
{
	struct dm_zone *dzone;
	struct dm_zone *rzone;
	unsigned long start;
	int ret;

	dzone = dmz_reclaim_get_zone(dzt);
	if (!dzone)
		return 0;

	/*
	 * Do not run concurrently with flush so that the entire reclaim
	 * process is treated as a "transaction" similarly to BIO processing.
	 */
	down_read(&dzt->mblk_sem);

	start = jiffies;

	if (dmz_is_rnd(dzone)) {

		/*
		 * Reclaim the random data zone by moving its
		 * valid data blocks to a free sequential zone.
		 */
		ret = dmz_reclaim_rnd_data(dzt, dzone);
		rzone = dzone;

	} else {

		struct dm_zone *bzone = dzone->bzone;
		sector_t chunk_block = 0;

		ret = dmz_first_valid_block(dzt, bzone, &chunk_block);
		if (ret < 0)
			goto out;

		if (chunk_block >= dzone->wp_block) {
			/*
			 * Valid blocks in the buffer zone are after
			 * the data zone write pointer: copy them there.
			 */
			ret = dmz_reclaim_buf(dzt, dzone);
			rzone = bzone;
		} else {
			/*
			 * Reclaim the data zone by merging it into the
			 * buffer zone so that the buffer zone itself can
			 * be later reclaimed.
			 */
			ret = dmz_reclaim_seq_data(dzt, dzone);
			rzone = dzone;
		}

	}

out:
	up_read(&dzt->mblk_sem);

	if (ret) {
		dmz_reclaim_put_zone(dzt, dzone);
		ret = 0;
	} else {
		dmz_dev_debug(dzt,
			      "Reclaimed zoned %u in %u ms\n",
			      dmz_id(dzt, rzone),
			      jiffies_to_msecs(jiffies - start));
		ret = 1;
	}

	dmz_trigger_flush(dzt);

	return ret;
}

/**
 * Zone reclaim work.
 */
void dmz_reclaim_work(struct work_struct *work)
{
	struct dm_zoned_target *dzt =
		container_of(work, struct dm_zoned_target, reclaim_work.work);
	unsigned long delay = DMZ_RECLAIM_PERIOD;
	int reclaimed = 0;

	dmz_dev_debug(dzt,
		      "%s, %u BIOs, %u %% free rzones, %d active zones\n",
		      (dmz_idle(dzt) ? "idle" : "busy"),
		      atomic_read(&dzt->bio_count),
		      atomic_read(&dzt->dz_unmap_nr_rnd) * 100 /
		      dzt->dz_nr_rnd,
		      atomic_read(&dzt->nr_active_zones));

	if (dmz_should_reclaim(dzt))
		reclaimed = dmz_reclaim(dzt);

	if (reclaimed ||
	    (dmz_should_reclaim(dzt)
	     && atomic_read(&dzt->nr_reclaim_seq_zones)))
		/* Some progress and more to expect: run again right away */
		delay = 0;

	dmz_schedule_reclaim(dzt, delay);
}

