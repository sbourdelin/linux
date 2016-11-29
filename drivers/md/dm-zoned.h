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
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/rwsem.h>
#include <linux/rbtree.h>
#include <linux/kref.h>

#ifndef DM_ZONED_H
#define DM_ZONED_H

/*
 * Module version.
 */
#define DMZ_VER_MAJ	0
#define DMZ_VER_MIN	1

/*
 * Metadata version.
 */
#define DMZ_META_VER	1

/*
 * On-disk super block magic.
 */
#define DMZ_MAGIC	((((unsigned int)('D')) << 24) | \
			 (((unsigned int)('Z')) << 16) | \
			 (((unsigned int)('B')) <<  8) | \
			 ((unsigned int)('D')))

/*
 * On disk super block.
 * This uses a full 4KB block. This block is followed on disk
 * by the chunk mapping table to zones and the bitmap blocks
 * indicating block validity.
 * The overall resulting metadat format is:
 *    (1) Super block (1 block)
 *    (2) Chunk mapping table (nr_map_blocks)
 *    (3) Bitmap blocks (nr_bitmap_blocks)
 * with all blocks stored in consecutive random zones starting
 * from the first random zone found on disk.
 */
struct dm_zoned_super {

	/* Magic number */
	__le32		magic;			/*   4 */

	/* Metadata version number */
	__le32		version;		/*   8 */

	/* Generation number */
	__le64		gen;			/*  16 */

	/* This block number */
	__le64		sb_block;		/*  24 */

	/* The number of metadata blocks, including this super block */
	__le64		nr_meta_blocks;		/*  32 */

	/* The number of sequential zones reserved for reclaim */
	__le32		nr_reserved_seq;	/*  36 */

	/* The number of entries in the mapping table */
	__le32		nr_chunks;		/*  40 */

	/* The number of blocks used for the chunk mapping table */
	__le32		nr_map_blocks;		/*  44 */

	/* The number of blocks used for the block bitmaps */
	__le32		nr_bitmap_blocks;	/*  48 */

	/* Checksum */
	__le32		crc;			/*  52 */

	/* Padding to full 512B sector */
	u8		reserved[464];		/* 512 */

} __packed;

/*
 * Chunk mapping entry: entries are indexed by chunk number
 * and give the zone ID (dzone_id) mapping the chunk. This zone
 * may be sequential or random. If it is a sequential zone,
 * a second zone (bzone_id) used as a write buffer may also be
 * specified. This second zone will always be a random zone.
 */
struct dm_zoned_map {
	__le32			dzone_id;
	__le32			bzone_id;
};

/*
 * dm-zoned creates 4KB block size devices, always.
 */
#define DMZ_BLOCK_SHIFT		12
#define DMZ_BLOCK_SIZE		(1 << DMZ_BLOCK_SHIFT)
#define DMZ_BLOCK_MASK		(DMZ_BLOCK_SIZE - 1)

#define DMZ_BLOCK_SHIFT_BITS	(DMZ_BLOCK_SHIFT + 3)
#define DMZ_BLOCK_SIZE_BITS	(1 << DMZ_BLOCK_SHIFT_BITS)
#define DMZ_BLOCK_MASK_BITS	(DMZ_BLOCK_SIZE_BITS - 1)

#define DMZ_BLOCK_SECTORS_SHIFT	(DMZ_BLOCK_SHIFT - SECTOR_SHIFT)
#define DMZ_BLOCK_SECTORS	(DMZ_BLOCK_SIZE >> SECTOR_SHIFT)
#define DMZ_BLOCK_SECTORS_MASK	(DMZ_BLOCK_SECTORS - 1)

/*
 * Chunk mapping table metadata: 512 8-bytes entries per 4KB block.
 */
#define DMZ_MAP_ENTRIES		(DMZ_BLOCK_SIZE \
				 / sizeof(struct dm_zoned_map))
#define DMZ_MAP_ENTRIES_SHIFT	(ilog2(DMZ_MAP_ENTRIES))
#define DMZ_MAP_ENTRIES_MASK	(DMZ_MAP_ENTRIES - 1)
#define DMZ_MAP_UNMAPPED	UINT_MAX

/*
 * Block <-> sector conversion.
 */
#define dmz_blk2sect(b)		((b) << DMZ_BLOCK_SECTORS_SHIFT)
#define dmz_sect2blk(s)		((s) >> DMZ_BLOCK_SECTORS_SHIFT)

#define DMZ_MIN_BIOS		4096

#define DMZ_REPORT_NR_ZONES	4096

struct dm_zone_work;

/*
 * Zone flags.
 */
enum {

	/* Zone actual type */
	DMZ_CONV = 0,
	DMZ_SEQ_REQ,
	DMZ_SEQ_PREF,

	/* Zone critical condition */
	DMZ_OFFLINE,
	DMZ_READ_ONLY,

	/* Zone use */
	DMZ_META,
	DMZ_DATA,
	DMZ_BUF,
	DMZ_RND,
	DMZ_SEQ,

	/* Zone internal state */
	DMZ_RECLAIM,

};

/*
 * Zone descriptor.
 */
struct dm_zone {

	struct rb_node		node;
	struct list_head	link;

	unsigned long		flags;

	sector_t		sector;
	unsigned int		wp_block;
	unsigned int		weight;

	/* The chunk number that the zone maps */
	unsigned int		chunk;

	/* The work processing this zone BIOs */
	struct dm_zone_work	*work;

	/*
	 * For a sequential data zone, pointer to the random
	 * zone used as a buffer for processing unaligned write
	 * requests. For a buffer zone, this points back to the
	 * data zone.
	 */
	struct dm_zone		*bzone;

};

extern struct kmem_cache *dmz_zone_cache;

#define dmz_id(dzt, z)		((unsigned int)((z)->sector >> \
						(dzt)->zone_nr_sectors_shift))
#define dmz_is_conv(z)		test_bit(DMZ_CONV, &(z)->flags)
#define dmz_is_seqreq(z)	test_bit(DMZ_SEQ_REQ, &(z)->flags)
#define dmz_is_seqpref(z)	test_bit(DMZ_SEQ_PREF, &(z)->flags)
#define dmz_is_seq(z)		test_bit(DMZ_SEQ, &(z)->flags)
#define dmz_is_rnd(z)		test_bit(DMZ_RND, &(z)->flags)
#define dmz_is_empty(z)		((z)->wp_block == 0)
#define dmz_is_offline(z)	test_bit(DMZ_OFFLINE, &(z)->flags)
#define dmz_is_readonly(z)	test_bit(DMZ_READ_ONLY, &(z)->flags)
#define dmz_is_active(z)	((z)->work != NULL)
#define dmz_in_reclaim(z)	test_bit(DMZ_RECLAIM, &(z)->flags)

#define dmz_is_meta(z)		test_bit(DMZ_META, &(z)->flags)
#define dmz_is_buf(z)		test_bit(DMZ_BUF, &(z)->flags)
#define dmz_is_data(z)		test_bit(DMZ_DATA, &(z)->flags)

#define dmz_weight(z)		((z)->weight)

#define dmz_chunk_sector(dzt, s) ((s) & ((dzt)->zone_nr_sectors - 1))
#define dmz_chunk_block(dzt, b)	((b) & ((dzt)->zone_nr_blocks - 1))

#define dmz_bio_block(bio)	dmz_sect2blk((bio)->bi_iter.bi_sector)
#define dmz_bio_blocks(bio)	dmz_sect2blk(bio_sectors(bio))
#define dmz_bio_chunk(dzt, bio)	((bio)->bi_iter.bi_sector >> \
				 (dzt)->zone_nr_sectors_shift)
/*
 * Meta data block descriptor (for cached blocks).
 */
struct dm_zoned_mblock {

	struct rb_node		node;
	struct list_head	link;
	sector_t		no;
	atomic_t		ref;
	unsigned long		state;
	struct page		*page;
	void			*data;

};

struct dm_zoned_sb {
	sector_t		block;
	struct dm_zoned_mblock	*mblk;
	struct dm_zoned_super	*sb;
};

/*
 * Metadata block flags.
 */
enum {
	DMZ_META_DIRTY,
	DMZ_META_READING,
	DMZ_META_WRITING,
	DMZ_META_ERROR,
};

/*
 * Target flags.
 */
enum {
	DMZ_SUSPENDED,
};

/*
 * Target descriptor.
 */
struct dm_zoned_target {

	struct dm_dev		*ddev;

	/* Target zoned device information */
	char			zbd_name[BDEVNAME_SIZE];
	struct block_device	*zbd;
	sector_t		zbd_capacity;
	struct request_queue	*zbdq;
	unsigned long		flags;

	unsigned int		nr_zones;
	unsigned int		nr_useable_zones;
	unsigned int		nr_meta_blocks;
	unsigned int		nr_meta_zones;
	unsigned int		nr_data_zones;
	unsigned int		nr_rnd_zones;
	unsigned int		nr_reserved_seq;
	unsigned int		nr_chunks;

	sector_t		zone_nr_sectors;
	unsigned int		zone_nr_sectors_shift;

	sector_t		zone_nr_blocks;
	sector_t		zone_nr_blocks_shift;

	sector_t		zone_bitmap_size;
	unsigned int		zone_nr_bitmap_blocks;

	unsigned int		nr_bitmap_blocks;
	unsigned int		nr_map_blocks;

	/* Zone information tree */
	struct rb_root		zones;

	/* For metadata handling */
	struct dm_zone		*sb_zone;
	struct dm_zoned_sb	sb[2];
	unsigned int		mblk_primary;
	u64			sb_gen;
	unsigned int		max_nr_mblks;
	atomic_t		nr_mblks;
	struct rw_semaphore	mblk_sem;
	spinlock_t		mblk_lock;
	struct rb_root		mblk_rbtree;
	struct list_head	mblk_lru_list;
	struct list_head	mblk_dirty_list;

	/* Zone mapping management lock */
	struct mutex		map_lock;

	/* Data zones */
	struct dm_zoned_mblock	**dz_map_mblk;

	unsigned int		dz_nr_rnd;
	atomic_t		dz_unmap_nr_rnd;
	struct list_head	dz_unmap_rnd_list;
	struct list_head	dz_map_rnd_list;

	unsigned int		dz_nr_seq;
	atomic_t		dz_unmap_nr_seq;
	struct list_head	dz_unmap_seq_list;
	struct list_head	dz_map_seq_list;

	wait_queue_head_t	dz_free_wq;

	/* For zone BIOs */
	struct bio_set		*bio_set;
	atomic_t		nr_active_zones;
	atomic_t		bio_count;
	spinlock_t		zwork_lock;
	struct workqueue_struct *zone_wq;
	unsigned long		last_bio_time;

	/* For flush */
	spinlock_t		flush_lock;
	struct bio_list		flush_list;
	struct delayed_work	flush_work;
	struct workqueue_struct *flush_wq;

	/* For reclaim */
	unsigned int		reclaim_idle_low;
	unsigned int		reclaim_low;
	struct delayed_work	reclaim_work;
	struct workqueue_struct *reclaim_wq;
	atomic_t		nr_reclaim_seq_zones;
	struct list_head	reclaim_seq_zones_list;

};

/*
 * Zone BIO work descriptor.
 */
struct dm_zone_work {
	struct work_struct	work;
	struct kref		kref;
	struct dm_zoned_target	*target;
	struct dm_zone		*zone;
	struct bio_list		bio_list;
};

#define dmz_lock_map(dzt)	mutex_lock(&(dzt)->map_lock)
#define dmz_unlock_map(dzt)	mutex_unlock(&(dzt)->map_lock)

/*
 * Flush period (seconds).
 */
#define DMZ_FLUSH_PERIOD	(10 * HZ)

/*
 * Trigger flush.
 */
static inline void dmz_trigger_flush(struct dm_zoned_target *dzt)
{
	mod_delayed_work(dzt->flush_wq, &dzt->flush_work, 0);
}

/*
 * Number of seconds without BIO to consider
 * the target device idle.
 */
#define DMZ_IDLE_SECS		1UL

/*
 * Zone reclaim check period.
 */
#define DMZ_RECLAIM_PERIOD_SECS	DMZ_IDLE_SECS
#define DMZ_RECLAIM_PERIOD	(DMZ_RECLAIM_PERIOD_SECS * HZ)

/*
 * Low percentage of unmapped random zones that forces
 * reclaim to start.
 */
#define DMZ_RECLAIM_LOW		50
#define DMZ_RECLAIM_MIN		10
#define DMZ_RECLAIM_MAX		90

/*
 * Low percentage of unmapped randm zones that forces
 * reclaim to start when the target is idle. The minimum
 * allowed is set by reclaim_low.
 */
#define DMZ_RECLAIM_IDLE_LOW	75
#define DMZ_RECLAIM_IDLE_MAX	90

/*
 * Block I/O region for reclaim.
 */
struct dm_zoned_ioreg {
	sector_t		chunk_block;
	unsigned int		nr_blocks;
	unsigned int		nr_bvecs;
	struct bio_vec		*bvec;
	struct bio		bio;
	struct completion	wait;
	int			err;
};

/*
 * Maximum number of regions to read in a zones
 * during reclaim in one run. If more regions need
 * to be read, reclaim will loop.
 */
#define DMZ_RECLAIM_MAX_IOREGS	16

/*
 * Test if the target device is idle.
 */
static inline int dmz_idle(struct dm_zoned_target *dzt)
{
	return atomic_read(&(dzt)->bio_count) == 0 &&
		time_is_before_jiffies(dzt->last_bio_time
				       + DMZ_IDLE_SECS * HZ);
}

/*
 * Test if triggerring reclaim is necessary.
 */
static inline bool dmz_should_reclaim(struct dm_zoned_target *dzt)
{
	unsigned int ucp;

	/* Percentage of unmappped (free) random zones */
	ucp = (atomic_read(&dzt->dz_unmap_nr_rnd) * 100)
		/ dzt->dz_nr_rnd;

	if ((dmz_idle(dzt) && ucp <= dzt->reclaim_idle_low) ||
	    (!dmz_idle(dzt) && ucp <= dzt->reclaim_low))
		return true;

	return false;
}

/*
 * Schedule reclaim (delay in jiffies).
 */
static inline void dmz_schedule_reclaim(struct dm_zoned_target *dzt,
					unsigned long delay)
{
	mod_delayed_work(dzt->reclaim_wq, &dzt->reclaim_work, delay);
}

/*
 * Trigger reclaim.
 */
static inline void dmz_trigger_reclaim(struct dm_zoned_target *dzt)
{
	dmz_schedule_reclaim(dzt, 0);
}

extern void dmz_reclaim_work(struct work_struct *work);

/*
 * Target config passed as dmsetup arguments.
 */
struct dm_zoned_target_config {
	char			*dev_path;
	unsigned long		flags;
	unsigned long		reclaim_idle_low;
	unsigned long		reclaim_low;
};

/*
 * Zone BIO context.
 */
struct dm_zone_bioctx {
	struct dm_zoned_target	*target;
	struct dm_zone_work	*zwork;
	struct bio		*bio;
	atomic_t		ref;
	int			error;
};

#define dmz_info(format, args...)		\
	pr_info("dm-zoned: " format,		\
	## args)

#define dmz_dev_info(target, format, args...)	\
	pr_info("dm-zoned (%s): " format,	\
	       (dzt)->zbd_name, ## args)

#define dmz_dev_err(dzt, format, args...)	\
	pr_err("dm-zoned (%s): " format,	\
	       (dzt)->zbd_name, ## args)

#define dmz_dev_warn(dzt, format, args...)	\
	pr_warn("dm-zoned (%s): " format,	\
		(dzt)->zbd_name, ## args)

#define dmz_dev_debug(dzt, format, args...)	\
	pr_debug("dm-zoned (%s): " format,	\
		 (dzt)->zbd_name, ## args)

int dmz_init_meta(struct dm_zoned_target *dzt,
			 struct dm_zoned_target_config *conf);
int dmz_resume_meta(struct dm_zoned_target *dzt);
void dmz_cleanup_meta(struct dm_zoned_target *dzt);

int dmz_reset_zone(struct dm_zoned_target *dzt,
		   struct dm_zone *zone);

int dmz_flush_mblocks(struct dm_zoned_target *dzt);

#define DMZ_ALLOC_RND		0x01
#define DMZ_ALLOC_RECLAIM	0x02

struct dm_zone *dmz_alloc_zone(struct dm_zoned_target *dzt,
			       unsigned long flags);
void dmz_free_zone(struct dm_zoned_target *dzt,
		   struct dm_zone *zone);

void dmz_map_zone(struct dm_zoned_target *dzt,
		  struct dm_zone *zone,
			 unsigned int chunk);
void dmz_unmap_zone(struct dm_zoned_target *dzt,
		    struct dm_zone *zone);

void dmz_validate_zone(struct dm_zoned_target *dzt,
		       struct dm_zone *zone);

struct dm_zone *dmz_get_chunk_mapping(struct dm_zoned_target *dzt,
				      unsigned int chunk,
				      int op);

struct dm_zone *dmz_get_chunk_buffer(struct dm_zoned_target *dzt,
				      struct dm_zone *dzone);

int dmz_validate_blocks(struct dm_zoned_target *dzt, struct dm_zone *zone,
			sector_t chunk_block, unsigned int nr_blocks);
int dmz_invalidate_blocks(struct dm_zoned_target *dzt, struct dm_zone *zone,
			  sector_t chunk_block, unsigned int nr_blocks);
static inline int dmz_invalidate_zone(struct dm_zoned_target *dzt,
				      struct dm_zone *zone)
{
	return dmz_invalidate_blocks(dzt, zone, 0, dzt->zone_nr_blocks);
}

int dmz_block_valid(struct dm_zoned_target *dzt, struct dm_zone *zone,
		    sector_t chunk_block);

int dmz_first_valid_block(struct dm_zoned_target *dzt, struct dm_zone *zone,
			  sector_t *chunk_block);

#endif /* DM_ZONED_H */
