/*
 * Persistent Memory Block Multi-Queue Driver
 * - This driver is largely adapted from Ross's pmem block driver.
 * Copyright (c) 2014-2017, Intel Corporation.
 * Copyright (c) 2015, Christoph Hellwig <hch@lst.de>.
 * Copyright (c) 2015, Boaz Harrosh <boaz@plexistor.com>.
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

#include <asm/cacheflush.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/badblocks.h>
#include <linux/memremap.h>
#include <linux/vmalloc.h>
#include <linux/blk-mq.h>
#include <linux/pfn_t.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/dax.h>
#include <linux/nd.h>
#include <linux/blk-mq.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/nodemask.h>
#include "pmem.h"
#include "pfn.h"
#include "nd.h"

static int use_dma = 1;
module_param(use_dma, int, 0444);
MODULE_PARM_DESC(use_dma, "Turn on/off DMA usage");

static int queue_depth = 128;
module_param(queue_depth, int, 0444);
MODULE_PARM_DESC(queue_depth, "I/O Queue Depth for multi queue mode");

/* typically maps to number of DMA channels/devices per socket */
static int q_per_node = 8;
module_param(q_per_node, int, 0444);
MODULE_PARM_DESC(q_per_node, "Hardware queues per node");

static int num_sg = 128;
module_param(num_sg, int, 0444);
MODULE_PARM_DESC(num_sg, "Number of scatterlist entries per request");

struct pmem_cmd {
	struct request *rq;
	struct dma_chan *chan;
	int sg_nents;
	struct scatterlist sg[];
};

static struct device *to_dev(struct pmem_device *pmem)
{
	/*
	 * nvdimm bus services need a 'dev' parameter, and we record the device
	 * at init in bb.dev.
	 */
	return pmem->bb.dev;
}

static struct nd_region *to_region(struct pmem_device *pmem)
{
	return to_nd_region(to_dev(pmem)->parent);
}

static blk_status_t pmem_clear_poison(struct pmem_device *pmem,
		phys_addr_t offset, unsigned int len)
{
	struct device *dev = to_dev(pmem);
	sector_t sector;
	long cleared;
	blk_status_t rc = BLK_STS_OK;

	sector = (offset - pmem->data_offset) / 512;

	cleared = nvdimm_clear_poison(dev, pmem->phys_addr + offset, len);
	if (cleared < len)
		rc = BLK_STS_IOERR;
	if (cleared > 0 && cleared / 512) {
		cleared /= 512;
		dev_dbg(dev, "%s: %#llx clear %ld sector%s\n", __func__,
				(unsigned long long) sector, cleared,
				cleared > 1 ? "s" : "");
		badblocks_clear(&pmem->bb, sector, cleared);
		if (pmem->bb_state)
			sysfs_notify_dirent(pmem->bb_state);
	}

	arch_invalidate_pmem(pmem->virt_addr + offset, len);

	return rc;
}

static void write_pmem(void *pmem_addr, struct page *page,
		unsigned int off, unsigned int len)
{
	void *mem = kmap_atomic(page);

	memcpy_flushcache(pmem_addr, mem + off, len);
	kunmap_atomic(mem);
}

static blk_status_t read_pmem(struct page *page, unsigned int off,
		void *pmem_addr, unsigned int len)
{
	int rc;
	void *mem = kmap_atomic(page);

	rc = memcpy_mcsafe(mem + off, pmem_addr, len);
	kunmap_atomic(mem);
	if (rc)
		return BLK_STS_IOERR;
	return BLK_STS_OK;
}

static blk_status_t pmem_do_bvec(struct pmem_device *pmem, struct page *page,
			unsigned int len, unsigned int off, bool is_write,
			sector_t sector)
{
	blk_status_t rc = BLK_STS_OK;
	bool bad_pmem = false;
	phys_addr_t pmem_off = sector * 512 + pmem->data_offset;
	void *pmem_addr = pmem->virt_addr + pmem_off;

	if (unlikely(is_bad_pmem(&pmem->bb, sector, len)))
		bad_pmem = true;

	if (!is_write) {
		if (unlikely(bad_pmem))
			rc = BLK_STS_IOERR;
		else {
			rc = read_pmem(page, off, pmem_addr, len);
			flush_dcache_page(page);
		}
	} else {
		/*
		 * Note that we write the data both before and after
		 * clearing poison.  The write before clear poison
		 * handles situations where the latest written data is
		 * preserved and the clear poison operation simply marks
		 * the address range as valid without changing the data.
		 * In this case application software can assume that an
		 * interrupted write will either return the new good
		 * data or an error.
		 *
		 * However, if pmem_clear_poison() leaves the data in an
		 * indeterminate state we need to perform the write
		 * after clear poison.
		 */
		flush_dcache_page(page);
		write_pmem(pmem_addr, page, off, len);
		if (unlikely(bad_pmem)) {
			rc = pmem_clear_poison(pmem, pmem_off, len);
			write_pmem(pmem_addr, page, off, len);
		}
	}

	return rc;
}

/* account for REQ_FLUSH rename, replace with REQ_PREFLUSH after v4.8-rc1 */
#ifndef REQ_FLUSH
#define REQ_FLUSH REQ_PREFLUSH
#endif

static int pmem_rw_page(struct block_device *bdev, sector_t sector,
		       struct page *page, bool is_write)
{
	struct pmem_device *pmem = bdev->bd_queue->queuedata;
	blk_status_t rc;

	rc = pmem_do_bvec(pmem, page, PAGE_SIZE, 0, is_write, sector);

	/*
	 * The ->rw_page interface is subtle and tricky.  The core
	 * retries on any error, so we can only invoke page_endio() in
	 * the successful completion case.  Otherwise, we'll see crashes
	 * caused by double completion.
	 */
	if (rc == 0)
		page_endio(page, is_write, 0);

	return blk_status_to_errno(rc);
}

/* see "strong" declaration in tools/testing/nvdimm/pmem-dax.c */
__weak long __pmem_direct_access(struct pmem_device *pmem, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn)
{
	resource_size_t offset = PFN_PHYS(pgoff) + pmem->data_offset;

	if (unlikely(is_bad_pmem(&pmem->bb, PFN_PHYS(pgoff) / 512,
					PFN_PHYS(nr_pages))))
		return -EIO;
	*kaddr = pmem->virt_addr + offset;
	*pfn = phys_to_pfn_t(pmem->phys_addr + offset, pmem->pfn_flags);

	/*
	 * If badblocks are present, limit known good range to the
	 * requested range.
	 */
	if (unlikely(pmem->bb.count))
		return nr_pages;
	return PHYS_PFN(pmem->size - pmem->pfn_pad - offset);
}

static const struct block_device_operations pmem_fops = {
	.owner =		THIS_MODULE,
	.rw_page =		pmem_rw_page,
	.revalidate_disk =	nvdimm_revalidate_disk,
};

static long pmem_dax_direct_access(struct dax_device *dax_dev,
		pgoff_t pgoff, long nr_pages, void **kaddr, pfn_t *pfn)
{
	struct pmem_device *pmem = dax_get_private(dax_dev);

	return __pmem_direct_access(pmem, pgoff, nr_pages, kaddr, pfn);
}

static size_t pmem_copy_from_iter(struct dax_device *dax_dev, pgoff_t pgoff,
		void *addr, size_t bytes, struct iov_iter *i)
{
	return copy_from_iter_flushcache(addr, bytes, i);
}

static void pmem_dax_flush(struct dax_device *dax_dev, pgoff_t pgoff,
		void *addr, size_t size)
{
	arch_wb_cache_pmem(addr, size);
}

static const struct dax_operations pmem_dax_ops = {
	.direct_access = pmem_dax_direct_access,
	.copy_from_iter = pmem_copy_from_iter,
	.flush = pmem_dax_flush,
};

static const struct attribute_group *pmem_attribute_groups[] = {
	&dax_attribute_group,
	NULL,
};

static void pmem_release_queue(void *data)
{
	struct pmem_device *pmem = data;

	blk_cleanup_queue(pmem->q);
	blk_mq_free_tag_set(&pmem->tag_set);
}

static void pmem_freeze_queue(void *q)
{
	blk_freeze_queue_start(q);
}

static void pmem_release_disk(void *__pmem)
{
	struct pmem_device *pmem = __pmem;

	kill_dax(pmem->dax_dev);
	put_dax(pmem->dax_dev);
	del_gendisk(pmem->disk);
	put_disk(pmem->disk);
}

static void nd_pmem_dma_callback(void *data,
		const struct dmaengine_result *res)
{
	struct pmem_cmd *cmd = data;
	struct request *req = cmd->rq;
	struct request_queue *q = req->q;
	struct pmem_device *pmem = q->queuedata;
	struct nd_region *nd_region = to_region(pmem);
	struct device *dev = to_dev(pmem);
	blk_status_t blk_status = BLK_STS_OK;

	if (res) {
		switch (res->result) {
		case DMA_TRANS_READ_FAILED:
		case DMA_TRANS_WRITE_FAILED:
		case DMA_TRANS_ABORTED:
			dev_dbg(dev, "bio failed\n");
			blk_status = BLK_STS_IOERR;
			break;
		case DMA_TRANS_NOERROR:
		default:
			break;
		}
	}

	if (req_op(req) == REQ_OP_WRITE && req->cmd_flags & REQ_FUA)
		nvdimm_flush(nd_region);

	blk_mq_end_request(cmd->rq, blk_status);
}

static int pmem_check_bad_pmem(struct pmem_cmd *cmd, bool is_write)
{
	struct request *req = cmd->rq;
	struct request_queue *q = req->q;
	struct pmem_device *pmem = q->queuedata;
	struct bio_vec bvec;
	struct req_iterator iter;

	rq_for_each_segment(bvec, req, iter) {
		sector_t sector = iter.iter.bi_sector;
		unsigned int len = bvec.bv_len;
		unsigned int off = bvec.bv_offset;

		if (unlikely(is_bad_pmem(&pmem->bb, sector, len))) {
			if (is_write) {
				struct page *page = bvec.bv_page;
				phys_addr_t pmem_off = sector * 512 +
					pmem->data_offset;
				void *pmem_addr = pmem->virt_addr + pmem_off;

		/*
		 * Note that we write the data both before and after
		 * clearing poison.  The write before clear poison
		 * handles situations where the latest written data is
		 * preserved and the clear poison operation simply marks
		 * the address range as valid without changing the data.
		 * In this case application software can assume that an
		 * interrupted write will either return the new good
		 * data or an error.
		 *
		 * However, if pmem_clear_poison() leaves the data in an
		 * indeterminate state we need to perform the write
		 * after clear poison.
		 */
				flush_dcache_page(page);
				write_pmem(pmem_addr, page, off, len);
				pmem_clear_poison(pmem, pmem_off, len);
				write_pmem(pmem_addr, page, off, len);
			} else
				return -EIO;
		}
	}

	return 0;
}

static blk_status_t pmem_handle_cmd_dma(struct pmem_cmd *cmd, bool is_write)
{
	struct request *req = cmd->rq;
	struct request_queue *q = req->q;
	struct pmem_device *pmem = q->queuedata;
	struct device *dev = to_dev(pmem);
	phys_addr_t pmem_off = blk_rq_pos(req) * 512 + pmem->data_offset;
	void *pmem_addr = pmem->virt_addr + pmem_off;
	size_t len;
	struct dma_device *dma = cmd->chan->device;
	struct dmaengine_unmap_data *unmap;
	dma_cookie_t cookie;
	struct dma_async_tx_descriptor *txd;
	struct page *page;
	unsigned int off;
	int rc;
	blk_status_t blk_status = BLK_STS_OK;
	enum dma_data_direction dir;
	dma_addr_t dma_addr;

	rc = pmem_check_bad_pmem(cmd, is_write);
	if (rc < 0) {
		blk_status = BLK_STS_IOERR;
		goto err;
	}

	unmap = dmaengine_get_unmap_data(dma->dev, 2, GFP_NOWAIT);
	if (!unmap) {
		dev_dbg(dev, "failed to get dma unmap data\n");
		blk_status = BLK_STS_IOERR;
		goto err;
	}

	/*
	 * If reading from pmem, writing to scatterlist,
	 * and if writing to pmem, reading from scatterlist.
	 */
	dir = is_write ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	cmd->sg_nents = blk_rq_map_sg(req->q, req, cmd->sg);
	if (cmd->sg_nents < 1) {
		blk_status = BLK_STS_IOERR;
		goto err;
	}

	WARN_ON_ONCE(cmd->sg_nents > num_sg);

	rc = dma_map_sg(dma->dev, cmd->sg, cmd->sg_nents, dir);
	if (rc < 1) {
		dev_dbg(dma->dev, "DMA scatterlist mapping error\n");
		blk_status = BLK_STS_IOERR;
		goto err;
	}

	unmap->unmap_sg.sg = cmd->sg;
	unmap->sg_nents = cmd->sg_nents;
	if (is_write)
		unmap->from_sg = 1;
	else
		unmap->to_sg = 1;

	len = blk_rq_payload_bytes(req);
	page = virt_to_page(pmem_addr);
	off = offset_in_page(pmem_addr);
	dir = is_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	dma_addr = dma_map_page(dma->dev, page, off, len, dir);
	if (dma_mapping_error(dma->dev, unmap->addr[0])) {
		dev_dbg(dma->dev, "DMA buffer mapping error\n");
		blk_status = BLK_STS_IOERR;
		goto err_unmap_sg;
	}

	unmap->unmap_sg.buf_phys = dma_addr;
	unmap->len = len;
	if (is_write)
		unmap->to_cnt = 1;
	else
		unmap->from_cnt = 1;

	txd = dmaengine_prep_dma_memcpy_sg(cmd->chan,
				cmd->sg, cmd->sg_nents, dma_addr,
				!is_write, DMA_PREP_INTERRUPT);
	if (!txd) {
		dev_dbg(dma->dev, "dma prep failed\n");
		blk_status = BLK_STS_IOERR;
		goto err_unmap_buffer;
	}

	txd->callback_result = nd_pmem_dma_callback;
	txd->callback_param = cmd;
	dma_set_unmap(txd, unmap);
	cookie = dmaengine_submit(txd);
	if (dma_submit_error(cookie)) {
		dev_dbg(dma->dev, "dma submit error\n");
		blk_status = BLK_STS_IOERR;
		goto err_set_unmap;
	}

	dmaengine_unmap_put(unmap);
	dma_async_issue_pending(cmd->chan);
	return BLK_STS_OK;

err_set_unmap:
	dmaengine_unmap_put(unmap);
err_unmap_buffer:
	dma_unmap_page(dev, dma_addr, len, dir);
err_unmap_sg:
	if (dir == DMA_TO_DEVICE)
		dir = DMA_FROM_DEVICE;
	else
		dir = DMA_TO_DEVICE;
	dma_unmap_sg(dev, cmd->sg, cmd->sg_nents, dir);
	dmaengine_unmap_put(unmap);
err:
	blk_mq_end_request(cmd->rq, blk_status);
	return blk_status;
}

static blk_status_t pmem_handle_cmd(struct pmem_cmd *cmd, bool is_write)
{
	struct request *req = cmd->rq;
	struct request_queue *q = req->q;
	struct pmem_device *pmem = q->queuedata;
	struct nd_region *nd_region = to_region(pmem);
	struct bio_vec bvec;
	struct req_iterator iter;
	blk_status_t blk_status = BLK_STS_OK;

	rq_for_each_segment(bvec, req, iter) {
		blk_status = pmem_do_bvec(pmem, bvec.bv_page, bvec.bv_len,
				bvec.bv_offset, is_write,
				iter.iter.bi_sector);
		if (blk_status != BLK_STS_OK)
			break;
	}

	if (is_write && req->cmd_flags & REQ_FUA)
		nvdimm_flush(nd_region);

	blk_mq_end_request(cmd->rq, blk_status);

	return blk_status;
}

typedef blk_status_t (*pmem_do_io)(struct pmem_cmd *cmd, bool is_write);

static blk_status_t pmem_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
	struct pmem_cmd *cmd = blk_mq_rq_to_pdu(bd->rq);
	struct request *req = cmd->rq = bd->rq;
	struct request_queue *q = req->q;
	struct pmem_device *pmem = q->queuedata;
	struct nd_region *nd_region = to_region(pmem);
	blk_status_t blk_status = BLK_STS_OK;
	pmem_do_io do_io;

	blk_mq_start_request(req);

	if (use_dma)
		cmd->chan = dma_find_channel(DMA_MEMCPY_SG);

	if (cmd->chan)
		do_io = pmem_handle_cmd_dma;
	else
		do_io = pmem_handle_cmd;

	switch (req_op(req)) {
	case REQ_FLUSH:
		nvdimm_flush(nd_region);
		blk_mq_end_request(cmd->rq, BLK_STS_OK);
		break;
	case REQ_OP_READ:
		blk_status = do_io(cmd, false);
		break;
	case REQ_OP_WRITE:
		blk_status = do_io(cmd, true);
		break;
	default:
		blk_status = BLK_STS_NOTSUPP;
		break;
	}

	if (blk_status != BLK_STS_OK)
		blk_mq_end_request(cmd->rq, blk_status);

	return blk_status;
}

static const struct blk_mq_ops pmem_mq_ops = {
	.queue_rq	= pmem_queue_rq,
};

static int pmem_attach_disk(struct device *dev,
		struct nd_namespace_common *ndns)
{
	struct nd_namespace_io *nsio = to_nd_namespace_io(&ndns->dev);
	struct nd_region *nd_region = to_nd_region(dev->parent);
	struct vmem_altmap __altmap, *altmap = NULL;
	int nid = dev_to_node(dev), fua, wbc;
	struct resource *res = &nsio->res;
	struct nd_pfn *nd_pfn = NULL;
	struct dax_device *dax_dev;
	struct nd_pfn_sb *pfn_sb;
	struct pmem_device *pmem;
	struct resource pfn_res;
	struct device *gendev;
	struct gendisk *disk;
	void *addr;
	int rc;
	struct dma_chan *chan = NULL;

	/* while nsio_rw_bytes is active, parse a pfn info block if present */
	if (is_nd_pfn(dev)) {
		nd_pfn = to_nd_pfn(dev);
		altmap = nvdimm_setup_pfn(nd_pfn, &pfn_res, &__altmap);
		if (IS_ERR(altmap))
			return PTR_ERR(altmap);
	}

	/* we're attaching a block device, disable raw namespace access */
	devm_nsio_disable(dev, nsio);

	pmem = devm_kzalloc(dev, sizeof(*pmem), GFP_KERNEL);
	if (!pmem)
		return -ENOMEM;

	dev_set_drvdata(dev, pmem);
	pmem->phys_addr = res->start;
	pmem->size = resource_size(res);
	fua = nvdimm_has_flush(nd_region);
	if (!IS_ENABLED(CONFIG_ARCH_HAS_UACCESS_FLUSHCACHE) || fua < 0) {
		dev_warn(dev, "unable to guarantee persistence of writes\n");
		fua = 0;
	}
	wbc = nvdimm_has_cache(nd_region);

	if (!devm_request_mem_region(dev, res->start, resource_size(res),
				dev_name(&ndns->dev))) {
		dev_warn(dev, "could not reserve region %pR\n", res);
		return -EBUSY;
	}

	if (use_dma) {
		chan = dma_find_channel(DMA_MEMCPY_SG);
		if (!chan) {
			use_dma = 0;
			dev_warn(dev, "Forced back to CPU, no DMA\n");
		}
	}

	pmem->tag_set.ops = &pmem_mq_ops;
	pmem->tag_set.nr_hw_queues = num_possible_nodes() * q_per_node;
	pmem->tag_set.queue_depth = queue_depth;
	pmem->tag_set.numa_node = dev_to_node(dev);
	pmem->tag_set.cmd_size = sizeof(struct pmem_cmd) +
		sizeof(struct scatterlist) * num_sg;
	pmem->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
	pmem->tag_set.driver_data = pmem;

	rc = blk_mq_alloc_tag_set(&pmem->tag_set);
	if (rc < 0)
		return rc;

	pmem->q = blk_mq_init_queue(&pmem->tag_set);
	if (IS_ERR(pmem->q)) {
		blk_mq_free_tag_set(&pmem->tag_set);
		return -ENOMEM;
	}

	if (devm_add_action_or_reset(dev, pmem_release_queue, pmem)) {
		pmem_release_queue(pmem);
		return -ENOMEM;
	}

	pmem->pfn_flags = PFN_DEV;
	if (is_nd_pfn(dev)) {
		addr = devm_memremap_pages(dev, &pfn_res,
				&pmem->q->q_usage_counter, altmap);
		pfn_sb = nd_pfn->pfn_sb;
		pmem->data_offset = le64_to_cpu(pfn_sb->dataoff);
		pmem->pfn_pad = resource_size(res) - resource_size(&pfn_res);
		pmem->pfn_flags |= PFN_MAP;
		res = &pfn_res; /* for badblocks populate */
		res->start += pmem->data_offset;
	} else if (pmem_should_map_pages(dev)) {
		addr = devm_memremap_pages(dev, &nsio->res,
				&pmem->q->q_usage_counter, NULL);
		pmem->pfn_flags |= PFN_MAP;
	} else
		addr = devm_memremap(dev, pmem->phys_addr,
				pmem->size, ARCH_MEMREMAP_PMEM);

	/*
	 * At release time the queue must be frozen before
	 * devm_memremap_pages is unwound
	 */
	if (devm_add_action_or_reset(dev, pmem_freeze_queue, pmem->q))
		return -ENOMEM;

	if (IS_ERR(addr))
		return PTR_ERR(addr);
	pmem->virt_addr = addr;

	blk_queue_write_cache(pmem->q, wbc, fua);
	blk_queue_physical_block_size(pmem->q, PAGE_SIZE);
	blk_queue_logical_block_size(pmem->q, pmem_sector_size(ndns));
	if (use_dma) {
		u64 xfercap = dma_get_desc_xfercap(chan);

		/* set it to some sane size if DMA driver didn't export */
		if (xfercap == 0)
			xfercap = SZ_1M;

		dev_dbg(dev, "xfercap: %#llx\n", xfercap);
		/* max xfer size is per_descriptor_cap * num_of_sg */
		blk_queue_max_hw_sectors(pmem->q, num_sg * xfercap / 512);
		blk_queue_max_segments(pmem->q, num_sg);
	}
		blk_queue_max_hw_sectors(pmem->q, UINT_MAX);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, pmem->q);
	queue_flag_set_unlocked(QUEUE_FLAG_DAX, pmem->q);
	pmem->q->queuedata = pmem;

	disk = alloc_disk_node(0, nid);
	if (!disk)
		return -ENOMEM;
	pmem->disk = disk;

	disk->fops		= &pmem_fops;
	disk->queue		= pmem->q;
	disk->flags		= GENHD_FL_EXT_DEVT;
	nvdimm_namespace_disk_name(ndns, disk->disk_name);
	set_capacity(disk, (pmem->size - pmem->pfn_pad - pmem->data_offset)
			/ 512);
	if (devm_init_badblocks(dev, &pmem->bb))
		return -ENOMEM;
	nvdimm_badblocks_populate(nd_region, &pmem->bb, res);
	disk->bb = &pmem->bb;

	dax_dev = alloc_dax(pmem, disk->disk_name, &pmem_dax_ops);
	if (!dax_dev) {
		put_disk(disk);
		return -ENOMEM;
	}
	dax_write_cache(dax_dev, wbc);
	pmem->dax_dev = dax_dev;

	gendev = disk_to_dev(disk);
	gendev->groups = pmem_attribute_groups;

	device_add_disk(dev, disk);
	if (devm_add_action_or_reset(dev, pmem_release_disk, pmem))
		return -ENOMEM;

	revalidate_disk(disk);

	pmem->bb_state = sysfs_get_dirent(disk_to_dev(disk)->kobj.sd,
					  "badblocks");
	if (!pmem->bb_state)
		dev_warn(dev, "'badblocks' notification disabled\n");

	return 0;
}

static int nd_pmem_probe(struct device *dev)
{
	struct nd_namespace_common *ndns;

	ndns = nvdimm_namespace_common_probe(dev);
	if (IS_ERR(ndns))
		return PTR_ERR(ndns);

	if (devm_nsio_enable(dev, to_nd_namespace_io(&ndns->dev)))
		return -ENXIO;

	if (is_nd_btt(dev))
		return nvdimm_namespace_attach_btt(ndns);

	if (is_nd_pfn(dev))
		return pmem_attach_disk(dev, ndns);

	/* if we find a valid info-block we'll come back as that personality */
	if (nd_btt_probe(dev, ndns) == 0 || nd_pfn_probe(dev, ndns) == 0
			|| nd_dax_probe(dev, ndns) == 0)
		return -ENXIO;

	/* ...otherwise we're just a raw pmem device */
	return pmem_attach_disk(dev, ndns);
}

static int nd_pmem_remove(struct device *dev)
{
	struct pmem_device *pmem = dev_get_drvdata(dev);

	if (is_nd_btt(dev))
		nvdimm_namespace_detach_btt(to_nd_btt(dev));
	else {
		/*
		 * Note, this assumes device_lock() context to not race
		 * nd_pmem_notify()
		 */
		sysfs_put(pmem->bb_state);
		pmem->bb_state = NULL;
	}
	nvdimm_flush(to_nd_region(dev->parent));

	return 0;
}

static void nd_pmem_shutdown(struct device *dev)
{
	nvdimm_flush(to_nd_region(dev->parent));
}

static void nd_pmem_notify(struct device *dev, enum nvdimm_event event)
{
	struct nd_region *nd_region;
	resource_size_t offset = 0, end_trunc = 0;
	struct nd_namespace_common *ndns;
	struct nd_namespace_io *nsio;
	struct resource res;
	struct badblocks *bb;
	struct kernfs_node *bb_state;

	if (event != NVDIMM_REVALIDATE_POISON)
		return;

	if (is_nd_btt(dev)) {
		struct nd_btt *nd_btt = to_nd_btt(dev);

		ndns = nd_btt->ndns;
		nd_region = to_nd_region(ndns->dev.parent);
		nsio = to_nd_namespace_io(&ndns->dev);
		bb = &nsio->bb;
		bb_state = NULL;
	} else {
		struct pmem_device *pmem = dev_get_drvdata(dev);

		nd_region = to_region(pmem);
		bb = &pmem->bb;
		bb_state = pmem->bb_state;

		if (is_nd_pfn(dev)) {
			struct nd_pfn *nd_pfn = to_nd_pfn(dev);
			struct nd_pfn_sb *pfn_sb = nd_pfn->pfn_sb;

			ndns = nd_pfn->ndns;
			offset = pmem->data_offset +
					__le32_to_cpu(pfn_sb->start_pad);
			end_trunc = __le32_to_cpu(pfn_sb->end_trunc);
		} else {
			ndns = to_ndns(dev);
		}

		nsio = to_nd_namespace_io(&ndns->dev);
	}

	res.start = nsio->res.start + offset;
	res.end = nsio->res.end - end_trunc;
	nvdimm_badblocks_populate(nd_region, bb, &res);
	if (bb_state)
		sysfs_notify_dirent(bb_state);
}

static struct nd_device_driver nd_pmem_driver = {
	.probe = nd_pmem_probe,
	.remove = nd_pmem_remove,
	.notify = nd_pmem_notify,
	.shutdown = nd_pmem_shutdown,
	.drv = {
		.name = "nd_pmem",
	},
	.type = ND_DRIVER_NAMESPACE_IO | ND_DRIVER_NAMESPACE_PMEM,
};

static int __init pmem_init(void)
{
	if (use_dma)
		dmaengine_get();

	return nd_driver_register(&nd_pmem_driver);
}
module_init(pmem_init);

static void pmem_exit(void)
{
	if (use_dma)
		dmaengine_put();

	driver_unregister(&nd_pmem_driver.drv);
}
module_exit(pmem_exit);

MODULE_SOFTDEP("pre: dmaengine");
MODULE_AUTHOR("Dave Jiang <dave.jiang@intel.com>");
MODULE_LICENSE("GPL v2");
