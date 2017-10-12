/*
 * virtio-pmem driver
 */

#include <linux/virtio.h>
#include <linux/swap.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/oom.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/magic.h>
#include <linux/virtio_pmem.h>

void devm_vpmem_disable(struct device *dev, struct resource *res, void *addr)
{
	devm_memunmap(dev, addr);
	devm_release_mem_region(dev, res->start, resource_size(res));
}

static void pmem_flush_done(struct virtqueue *vq)
{
	return;
};

static void virtio_pmem_release_queue(void *q)
{
	blk_cleanup_queue(q);
}

static void virtio_pmem_freeze_queue(void *q)
{
	blk_freeze_queue_start(q);
}

static void virtio_pmem_release_disk(void *__pmem)
{
	struct virtio_pmem *pmem = __pmem;

	del_gendisk(pmem->disk);
	put_disk(pmem->disk);
}

static int init_vq(struct virtio_pmem *vpmem)
{
	struct virtqueue *vq;

	/* single vq */
	vq = virtio_find_single_vq(vpmem->vdev, pmem_flush_done, "flush_queue");

	if (IS_ERR(vq))
		return PTR_ERR(vq);

	return 0;
}

static struct vmem_altmap *setup_pmem_pfn(struct virtio_pmem *vpmem,
			struct resource *res, struct vmem_altmap *altmap)
{
	u32 start_pad = 0, end_trunc = 0;
	resource_size_t start, size;
	unsigned long npfns;
	phys_addr_t offset;

	size = resource_size(res);
	start = PHYS_SECTION_ALIGN_DOWN(res->start);

	if (region_intersects(start, size, IORESOURCE_SYSTEM_RAM,
		IORES_DESC_NONE) == REGION_MIXED) {

		start = res->start;
		start_pad = PHYS_SECTION_ALIGN_UP(start) - start;
	}
	start = res->start;
	size = PHYS_SECTION_ALIGN_UP(start + size) - start;

	if (region_intersects(start, size, IORESOURCE_SYSTEM_RAM,
		IORES_DESC_NONE) == REGION_MIXED) {

		size = resource_size(res);
		end_trunc = start + size -
				PHYS_SECTION_ALIGN_DOWN(start + size);
	}

	start += start_pad;
	size = resource_size(res);
	npfns = PFN_SECTION_ALIGN_UP((size - start_pad - end_trunc - SZ_8K)
						/ PAGE_SIZE);

      /*
       * vmemmap_populate_hugepages() allocates the memmap array in
       * HPAGE_SIZE chunks.
       */
	offset = ALIGN(start + SZ_8K + 64 * npfns, HPAGE_SIZE) - start;
	vpmem->data_offset = offset;

	struct vmem_altmap __altmap = {
		.base_pfn = init_altmap_base(start+start_pad),
		.reserve = init_altmap_reserve(start+start_pad),
	};

	res->start += start_pad;
	res->end -= end_trunc;
	memcpy(altmap, &__altmap, sizeof(*altmap));
	altmap->free = PHYS_PFN(offset - SZ_8K);
	altmap->alloc = 0;

	return altmap;
}

static blk_status_t pmem_do_bvec(struct virtio_pmem *pmem, struct page *page,
			unsigned int len, unsigned int off, bool is_write,
			sector_t sector)
{
	blk_status_t rc = BLK_STS_OK;
	phys_addr_t pmem_off = sector * 512 + pmem->data_offset;
	void *pmem_addr = pmem->virt_addr + pmem_off;

	if (!is_write) {
		rc = read_pmem(page, off, pmem_addr, len);
			flush_dcache_page(page);
	} else {
		flush_dcache_page(page);
		write_pmem(pmem_addr, page, off, len);
	}

	return rc;
}

static int vpmem_rw_page(struct block_device *bdev, sector_t sector,
		       struct page *page, bool is_write)
{
	struct virtio_pmem  *pmem = bdev->bd_queue->queuedata;
	blk_status_t rc;

	rc = pmem_do_bvec(pmem, page, hpage_nr_pages(page) * PAGE_SIZE,
			  0, is_write, sector);

	if (rc == 0)
		page_endio(page, is_write, 0);

	return blk_status_to_errno(rc);
}

#ifndef REQ_FLUSH
#define REQ_FLUSH REQ_PREFLUSH
#endif

static blk_qc_t virtio_pmem_make_request(struct request_queue *q,
			struct bio *bio)
{
	blk_status_t rc = 0;
	struct bio_vec bvec;
	struct bvec_iter iter;
	struct virtio_pmem *pmem = q->queuedata;

	if (bio->bi_opf & REQ_FLUSH)
		//todo host flush command

	bio_for_each_segment(bvec, bio, iter) {
		rc = pmem_do_bvec(pmem, bvec.bv_page, bvec.bv_len,
				bvec.bv_offset, op_is_write(bio_op(bio)),
				iter.bi_sector);
		if (rc) {
			bio->bi_status = rc;
			break;
		}
	}

	bio_endio(bio);
	return BLK_QC_T_NONE;
}

static const struct block_device_operations pmem_fops = {
	.owner =		THIS_MODULE,
	.rw_page =		vpmem_rw_page,
	//.revalidate_disk =	nvdimm_revalidate_disk,
};

static int virtio_pmem_probe(struct virtio_device *vdev)
{
	struct virtio_pmem *vpmem;
	int err = 0;
	void *addr;
	struct resource *res, res_pfn;
	struct request_queue *q;
	struct vmem_altmap __altmap, *altmap = NULL;
	struct gendisk *disk;
	struct device *gendev;
	int nid = dev_to_node(&vdev->dev);

	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config disabled\n",
			__func__);
		return -EINVAL;
	}

	vdev->priv = vpmem = devm_kzalloc(&vdev->dev, sizeof(*vpmem),
			GFP_KERNEL);

	if (!vpmem) {
		err = -ENOMEM;
		goto out;
	}

	dev_set_drvdata(&vdev->dev, vpmem);

	vpmem->vdev = vdev;
	err = init_vq(vpmem);
	if (err)
		goto out;

	if (!virtio_has_feature(vdev, VIRTIO_PMEM_PLUG)) {
		dev_err(&vdev->dev, "%s : pmem not supported\n",
			__func__);
		goto out;
	}

	virtio_cread(vpmem->vdev, struct virtio_pmem_config,
			start, &vpmem->start);
	virtio_cread(vpmem->vdev, struct virtio_pmem_config,
			size, &vpmem->size);

	res_pfn.start = vpmem->start;
	res_pfn.end   = vpmem->start + vpmem->size-1;

	/* used for allocating memmap in the pmem device */
	altmap	      = setup_pmem_pfn(vpmem, &res_pfn, &__altmap);

	res = devm_request_mem_region(&vdev->dev,
			res_pfn.start, resource_size(&res_pfn), "virtio-pmem");

	if (!res) {
		dev_warn(&vdev->dev, "could not reserve region ");
		return -EBUSY;
	}

	q = blk_alloc_queue_node(GFP_KERNEL, dev_to_node(&vdev->dev));

	if (!q)
		return -ENOMEM;

	if (devm_add_action_or_reset(&vdev->dev,
				virtio_pmem_release_queue, q))
		return -ENOMEM;

	vpmem->pfn_flags = PFN_DEV;

	/* allocate memap in pmem device itself */
	if (IS_ENABLED(CONFIG_ZONE_DEVICE)) {

		addr = devm_memremap_pages(&vdev->dev, res,
				&q->q_usage_counter, altmap);
		vpmem->pfn_flags |= PFN_MAP;
	} else
		addr = devm_memremap(&vdev->dev, vpmem->start,
				vpmem->size, ARCH_MEMREMAP_PMEM);

        /*
         * At release time the queue must be frozen before
         * devm_memremap_pages is unwound
         */
	if (devm_add_action_or_reset(&vdev->dev,
				virtio_pmem_freeze_queue, q))
		return -ENOMEM;

	if (IS_ERR(addr))
		return PTR_ERR(addr);

	vpmem->virt_addr = addr;
	blk_queue_write_cache(q, 0, 0);
	blk_queue_make_request(q, virtio_pmem_make_request);
	blk_queue_physical_block_size(q, PAGE_SIZE);
	blk_queue_logical_block_size(q, 512);
	blk_queue_max_hw_sectors(q, UINT_MAX);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, q);
	queue_flag_set_unlocked(QUEUE_FLAG_DAX, q);
	q->queuedata = vpmem;

	disk = alloc_disk_node(0, nid);
	if (!disk)
		return -ENOMEM;
	vpmem->disk = disk;

	disk->fops                = &pmem_fops;
	disk->queue               = q;
	disk->flags               = GENHD_FL_EXT_DEVT;
	strcpy(disk->disk_name, "vpmem");
	set_capacity(disk, vpmem->size/512);
	gendev = disk_to_dev(disk);

	virtio_device_ready(vdev);
	device_add_disk(&vdev->dev, disk);

	if (devm_add_action_or_reset(&vdev->dev,
			virtio_pmem_release_disk, vpmem))
		return -ENOMEM;

	revalidate_disk(disk);
	return 0;
out:
	vdev->config->del_vqs(vdev);
	return err;
}

static struct virtio_driver virtio_pmem_driver = {
	.feature_table		= features,
	.feature_table_size	= ARRAY_SIZE(features),
	.driver.name		= KBUILD_MODNAME,
	.driver.owner		= THIS_MODULE,
	.id_table		= id_table,
	.probe			= virtio_pmem_probe,
	//.remove		= virtio_pmem_remove,
};

module_virtio_driver(virtio_pmem_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio pmem driver");
MODULE_LICENSE("GPL");
