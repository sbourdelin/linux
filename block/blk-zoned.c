/*
 * Zoned block device handling
 *
 * Copyright (c) 2015, Hannes Reinecke
 * Copyright (c) 2015, SUSE Linux GmbH
 *
 * Copyright (c) 2016, Damien Le Moal
 * Copyright (c) 2016, Western Digital
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rbtree.h>
#include <linux/blkdev.h>

void blk_init_zones(struct request_queue *q)
{
	spin_lock_init(&q->zones_lock);
	q->zones = RB_ROOT;
}

/**
 * blk_drop_zones - Empty a zoned device zone tree.
 * @q: queue of the zoned device to operate on
 *
 * Free all zone descriptors added to the queue zone tree.
 */
void blk_drop_zones(struct request_queue *q)
{
	struct rb_root *root = &q->zones;
	struct blk_zone *zone, *next;

	rbtree_postorder_for_each_entry_safe(zone, next, root, node)
		kfree(zone);
	q->zones = RB_ROOT;
}
EXPORT_SYMBOL_GPL(blk_drop_zones);

/**
 * blk_insert_zone - Add a new zone struct to the queue RB-tree.
 * @q: queue of the zoned device to operate on
 * @new_zone: The zone struct to add
 *
 * If @new_zone is not already added to the zone tree, add it.
 * Otherwise, return the existing entry.
 */
struct blk_zone *blk_insert_zone(struct request_queue *q,
				 struct blk_zone *new_zone)
{
	struct rb_root *root = &q->zones;
	struct rb_node **new = &(root->rb_node), *parent = NULL;
	struct blk_zone *zone = NULL;
	unsigned long flags;

	spin_lock_irqsave(&q->zones_lock, flags);

	/* Figure out where to put new node */
	while (*new) {
		zone = container_of(*new, struct blk_zone, node);
		parent = *new;
		if (new_zone->start + new_zone->len <= zone->start)
			new = &((*new)->rb_left);
		else if (new_zone->start >= zone->start + zone->len)
			new = &((*new)->rb_right);
		else
			/* Return existing zone */
			break;
		zone = NULL;
	}

	if (!zone) {
		/* No existing zone: add new node and rebalance tree */
		rb_link_node(&new_zone->node, parent, new);
		rb_insert_color(&new_zone->node, root);
	}

	spin_unlock_irqrestore(&q->zones_lock, flags);

	return zone;
}
EXPORT_SYMBOL_GPL(blk_insert_zone);

/**
 * blk_lookup_zone - Search a zone in a zoned device zone tree.
 * @q: queue of the zoned device tree to search
 * @sector: A sector within the zone to search for
 *
 * Search the zone containing @sector in the zone tree owned
 * by @q. NULL is returned if the zone is not found. Since this
 * can be called concurrently with blk_insert_zone during device
 * initialization, the tree traversal is protected using the
 * zones_lock of the queue.
 */
struct blk_zone *blk_lookup_zone(struct request_queue *q, sector_t sector)
{
	struct rb_root *root = &q->zones;
	struct rb_node *node = root->rb_node;
	struct blk_zone *zone = NULL;
	unsigned long flags;

	spin_lock_irqsave(&q->zones_lock, flags);

	while (node) {
		zone = container_of(node, struct blk_zone, node);
		if (sector < zone->start)
			node = node->rb_left;
		else if (sector >= zone->start + zone->len)
			node = node->rb_right;
		else
			break;
		zone = NULL;
	}

	spin_unlock_irqrestore(&q->zones_lock, flags);

	return zone;
}
EXPORT_SYMBOL_GPL(blk_lookup_zone);

/**
 * Execute a zone operation (REQ_OP_ZONE*)
 */
static int blkdev_issue_zone_operation(struct block_device *bdev,
				       unsigned int op,
				       sector_t sector, sector_t nr_sects,
				       gfp_t gfp_mask)
{
	struct bio *bio;
	int ret;

	if (!bdev_zoned(bdev))
		return -EOPNOTSUPP;

	/*
	 * Make sure bi_size does not overflow because
	 * of some weird very large zone size.
	 */
	if (nr_sects && (unsigned long long)nr_sects << 9 > UINT_MAX)
		return -EINVAL;

	bio = bio_alloc(gfp_mask, 1);
	if (!bio)
		return -ENOMEM;

	bio->bi_iter.bi_sector = sector;
	bio->bi_iter.bi_size = nr_sects << 9;
	bio->bi_vcnt = 0;
	bio->bi_bdev = bdev;
	bio_set_op_attrs(bio, op, 0);

	ret = submit_bio_wait(bio);

	bio_put(bio);

	return ret;
}

/**
 * blkdev_update_zones - Force an update of a device zone information
 * @bdev:	Target block device
 *
 * Force an update of all zones information of @bdev. This call does not
 * block waiting for the update to complete. On return, all zones are only
 * marked as "in-update". Waiting on the zone update to complete can be done
 * on a per zone basis using the function blk_wait_for_zone_update.
 */
int blkdev_update_zones(struct block_device *bdev,
			gfp_t gfp_mask)
{
	return blkdev_issue_zone_operation(bdev, REQ_OP_ZONE_REPORT,
					   0, 0, gfp_mask);
}

/*
 * Wait for a zone update to complete.
 */
static void __blk_wait_for_zone_update(struct blk_zone *zone)
{
	might_sleep();
	if (test_bit(BLK_ZONE_IN_UPDATE, &zone->flags))
		wait_on_bit_io(&zone->flags, BLK_ZONE_IN_UPDATE,
			       TASK_UNINTERRUPTIBLE);
}

/**
 * blk_wait_for_zone_update - Wait for a zone information update
 * @zone: The zone to wait for
 *
 * This must be called with the zone lock held. If @zone is not
 * under update, returns immediately. Otherwise, wait for the
 * update flag to be cleared on completion of the zone information
 * update by the device driver.
 */
void blk_wait_for_zone_update(struct blk_zone *zone)
{
	WARN_ON_ONCE(!test_bit(BLK_ZONE_LOCKED, &zone->flags));
	while (test_bit(BLK_ZONE_IN_UPDATE, &zone->flags)) {
		blk_unlock_zone(zone);
		__blk_wait_for_zone_update(zone);
		blk_lock_zone(zone);
	}
}

/**
 * blkdev_report_zone - Get a zone information
 * @bdev:	Target block device
 * @sector:	A sector of the zone to report
 * @update:	Force an update of the zone information
 * @gfp_mask:	Memory allocation flags (for bio_alloc)
 *
 * Get a zone from the zone cache. And return it.
 * If update is requested, issue a report zone operation
 * and wait for the zone information to be updated.
 */
struct blk_zone *blkdev_report_zone(struct block_device *bdev,
				    sector_t sector,
				    bool update,
				    gfp_t gfp_mask)
{
	struct request_queue *q = bdev_get_queue(bdev);
	struct blk_zone *zone;
	int ret;

	zone = blk_lookup_zone(q, sector);
	if (!zone)
		return ERR_PTR(-ENXIO);

	if (update) {
		ret = blkdev_issue_zone_operation(bdev, REQ_OP_ZONE_REPORT,
						  zone->start, zone->len,
						  gfp_mask);
		if (ret)
			return ERR_PTR(ret);
		__blk_wait_for_zone_update(zone);
	}

	return zone;
}

/**
 * Execute a zone action (open, close, reset or finish).
 */
static int blkdev_issue_zone_action(struct block_device *bdev,
				    sector_t sector, unsigned int op,
				    gfp_t gfp_mask)
{
	struct request_queue *q = bdev_get_queue(bdev);
	struct blk_zone *zone;
	sector_t nr_sects;
	int ret;

	if (!blk_queue_zoned(q))
		return -EOPNOTSUPP;

	if (sector == ~0ULL) {
		/* All zones */
		sector = 0;
		nr_sects = 0;
	} else {
		/* This zone */
		zone = blk_lookup_zone(q, sector);
		if (!zone)
			return -ENXIO;
		sector = zone->start;
		nr_sects = zone->len;
	}

	ret = blkdev_issue_zone_operation(bdev, op, sector,
					  nr_sects, gfp_mask);
	if (ret == 0 && !nr_sects)
		blkdev_update_zones(bdev, gfp_mask);

	return ret;
}

/**
 * blkdev_reset_zone - Reset a zone write pointer
 * @bdev:	target block device
 * @sector:	A sector of the zone to reset or ~0ul for all zones.
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 *
 * Description:
 *    Reset a zone or all zones write pointer.
 */
int blkdev_reset_zone(struct block_device *bdev,
		      sector_t sector, gfp_t gfp_mask)
{
	return blkdev_issue_zone_action(bdev, sector, REQ_OP_ZONE_RESET,
					gfp_mask);
}

/**
 * blkdev_open_zone - Explicitely open a zone
 * @bdev:	target block device
 * @sector:	A sector of the zone to open or ~0ul for all zones.
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 *
 * Description:
 *    Open a zone or all possible zones.
 */
int blkdev_open_zone(struct block_device *bdev,
		     sector_t sector, gfp_t gfp_mask)
{
	return blkdev_issue_zone_action(bdev, sector, REQ_OP_ZONE_OPEN,
					gfp_mask);
}

/**
 * blkdev_close_zone - Close an open zone
 * @bdev:	target block device
 * @sector:	A sector of the zone to close or ~0ul for all zones.
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 *
 * Description:
 *    Close a zone or all open zones.
 */
int blkdev_close_zone(struct block_device *bdev,
		      sector_t sector, gfp_t gfp_mask)
{
	return blkdev_issue_zone_action(bdev, sector, REQ_OP_ZONE_CLOSE,
					gfp_mask);
}

/**
 * blkdev_finish_zone - Finish a zone (make it full)
 * @bdev:	target block device
 * @sector:	A sector of the zone to close or ~0ul for all zones.
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 *
 * Description:
 *    Finish one zone or all possible zones.
 */
int blkdev_finish_zone(struct block_device *bdev,
		       sector_t sector, gfp_t gfp_mask)
{
	return blkdev_issue_zone_action(bdev, sector, REQ_OP_ZONE_FINISH,
					gfp_mask);
}
