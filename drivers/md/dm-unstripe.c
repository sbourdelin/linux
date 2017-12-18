/*
 * Copyright © 2017 Intel Corporation
 *
 * Authors:
 *    Scott  Bauer      <scott.bauer@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include "dm.h"
#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/bitops.h>
#include <linux/device-mapper.h>


struct unstripe {
	struct dm_dev *ddisk;
	sector_t chunk_sectors;
	sector_t stripe_sectors;
	u8 chunk_shift;
	u8 cur_drive;
};


#define DM_MSG_PREFIX "dm-unstripe"
static const char *parse_err = "Please provide the necessary information:"
	"<drive> <device (0 indexed)> <total_devices>"
	" <chunk size in 512B sectors || 0 to use max hw sector size>";

/*
 * Argument layout:
 * <drive> <stripe/drive to extract (0 indexed)>
 *         <total_devices> <chunk size in 512B sect>
 */
static int set_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct block_device *bbdev;
	struct unstripe *target;
	unsigned int chunk_size;
	u64 tot_sec, mod;
	u8 cur_drive, tot_drives;
	char dummy;
	int ret;

	if (argc != 4) {
		DMERR("%s", parse_err);
		return -EINVAL;
	}

	if (sscanf(argv[1], "%hhu%c", &cur_drive, &dummy) != 1 ||
	    sscanf(argv[2], "%hhu%c", &tot_drives, &dummy) != 1 ||
	    sscanf(argv[3], "%u%c", &chunk_size, &dummy) != 1) {
		DMERR("%s", parse_err);
		return -EINVAL;
	}

	if (tot_drives == 0 || (cur_drive >= tot_drives && tot_drives > 1)) {
		DMERR("Please provide a drive between [0,%hhu)", tot_drives);
		return -EINVAL;
	}

	target = kzalloc(sizeof(*target), GFP_KERNEL);

	if (!target) {
		DMERR("Failed to allocate space for DM unstripe!");
		return -ENOMEM;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
			    &target->ddisk);
	if (ret) {
		kfree(target);
		DMERR("dm-unstripe dev lookup failure! for drive %s", argv[0]);
		return ret;
	}

	bbdev = target->ddisk->bdev;

	target->cur_drive = cur_drive;
	if (chunk_size)
		target->chunk_sectors = chunk_size;
	else
		target->chunk_sectors =
			queue_max_hw_sectors(bdev_get_queue(bbdev));

	target->stripe_sectors = (tot_drives - 1) * target->chunk_sectors;
	target->chunk_shift = fls(target->chunk_sectors) - 1;

	ret = dm_set_target_max_io_len(ti, target->chunk_sectors);
	if (ret) {
		dm_put_device(ti, target->ddisk);
		kfree(target);
		DMERR("Failed to set max io len!");
		return ret;
	}
	ti->private = target;

	tot_sec = i_size_read(bbdev->bd_inode) >> SECTOR_SHIFT;
	mod = tot_sec % target->chunk_sectors;

	if (ti->len == 1)
		ti->len = (tot_sec / tot_drives) - mod;
	ti->begin = 0;
	return 0;
}

static void set_dtr(struct dm_target *ti)
{
	struct unstripe *target = ti->private;

	dm_put_device(ti, target->ddisk);
	kfree(target);
}


static sector_t map_to_core(struct dm_target *ti, struct bio *bio)
{
	struct unstripe *target = ti->private;
	unsigned long long sec = bio->bi_iter.bi_sector;
	unsigned long long group;

	group = (sec >> target->chunk_shift);
	/* Account for what drive we're operating on */
	sec += (target->cur_drive * target->chunk_sectors);
	/* Shift us up to the right "row" on the drive*/
	sec += target->stripe_sectors * group;
	return sec;
}

static int set_map_bio(struct dm_target *ti, struct bio *bio)
{
	struct unstripe *target = ti->private;

	if (bio_sectors(bio))
		bio->bi_iter.bi_sector = map_to_core(ti, bio);

	bio_set_dev(bio, target->ddisk->bdev);
	submit_bio(bio);
	return DM_MAPIO_SUBMITTED;
}

static void set_iohints(struct dm_target *ti,
			struct queue_limits *limits)
{
	struct unstripe *target = ti->private;
	struct queue_limits *lim = &bdev_get_queue(target->ddisk->bdev)->limits;

	blk_limits_io_min(limits, lim->io_min);
	blk_limits_io_opt(limits, lim->io_opt);
	limits->chunk_sectors = target->chunk_sectors;
}

static int set_iterate(struct dm_target *ti, iterate_devices_callout_fn fn,
		       void *data)
{
	struct unstripe *target = ti->private;

	return fn(ti, target->ddisk, 0, ti->len, data);
}

static struct target_type iset_target = {
	.name = "unstripe",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr = set_ctr,
	.dtr = set_dtr,
	.map = set_map_bio,
	.iterate_devices = set_iterate,
	.io_hints = set_iohints,
};

static int __init dm_unstripe_init(void)
{
	int r = dm_register_target(&iset_target);

	if (r < 0)
		DMERR("register failed %d", r);

	return r;
}

static void __exit dm_unstripe_exit(void)
{
	dm_unregister_target(&iset_target);
}

module_init(dm_unstripe_init);
module_exit(dm_unstripe_exit);

MODULE_DESCRIPTION(DM_NAME " DM unstripe");
MODULE_ALIAS("dm-unstripe");
MODULE_AUTHOR("Scott Bauer <scott.bauer@intel.com>");
MODULE_LICENSE("GPL");
