/*
 * Virtio pmem Device
 *
 *
 */

#ifndef _LINUX_VIRTIO_PMEM_H
#define _LINUX_VIRTIO_PMEM_H

#include <linux/types.h>
#include <linux/virtio_types.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <linux/pfn_t.h>
#include <linux/fs.h>
#include <linux/blk-mq.h>
#include <linux/pmem_common.h>

bool pmem_should_map_pages(struct device *dev);

#define VIRTIO_PMEM_PLUG 0

struct virtio_pmem_config {

uint64_t start;
uint64_t size;
uint64_t align;
uint64_t data_offset;
};

struct virtio_pmem {

	struct virtio_device *vdev;
	struct virtqueue *req_vq;

	uint64_t start;
	uint64_t size;
	uint64_t align;
	uint64_t data_offset;
	u64 pfn_flags;
	void *virt_addr;
	struct gendisk *disk;
} __packed;

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_PMEM, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_PMEM_PLUG,
};

#endif

