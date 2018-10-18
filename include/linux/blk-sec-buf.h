/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BLK_SEC_BUF_H
#define _LINUX_BLK_SEC_BUF_H

#include <linux/slab.h>
#include <linux/blkdev.h>

#define  BLK_NR_SEC_BUF_SLAB   ((PAGE_SIZE >> 9) > 128 ? 128 : (PAGE_SIZE >> 9))
struct blk_sec_buf_slabs {
	int ref_cnt;
	struct kmem_cache *slabs[BLK_NR_SEC_BUF_SLAB];
};

int blk_create_sec_buf_slabs(char *name, struct request_queue *q);
void blk_destroy_sec_buf_slabs(struct request_queue *q);

void *blk_alloc_sec_buf(struct request_queue *q, int size, gfp_t flags);
void blk_free_sec_buf(struct request_queue *q, void *buf, int size);

static inline int bdev_create_sec_buf_slabs(struct block_device *bdev)
{
	char *name = bdev->bd_disk ? bdev->bd_disk->disk_name : "unknown";

	return blk_create_sec_buf_slabs(name, bdev->bd_queue);
}

static inline void bdev_destroy_sec_buf_slabs(struct block_device *bdev)
{
	blk_destroy_sec_buf_slabs(bdev->bd_queue);
}

static inline void *bdev_alloc_sec_buf(struct block_device *bdev, int size,
		gfp_t flags)
{
	return blk_alloc_sec_buf(bdev->bd_queue, size, flags);
}
static inline void bdev_free_sec_buf(struct block_device *bdev, void *buf,
		int size)
{
	blk_free_sec_buf(bdev->bd_queue, buf, size);
}

#endif
