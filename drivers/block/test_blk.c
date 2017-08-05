/*
 * test_blk.c - A memory-based test block device driver.
 *
 * Copyright (c) 2017 Facebook, Inc.
 *
 * Parts derived from drivers/block/null_blk.c and drivers/block/brd.c,
 * copyright to respective owners.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/configfs.h>
#include <linux/radix-tree.h>
#include <linux/idr.h>

#define SECTOR_SHIFT		9
#define PAGE_SECTORS_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define PAGE_SECTORS		(1 << PAGE_SECTORS_SHIFT)
#define SECTOR_SIZE		(1 << SECTOR_SHIFT)
#define SECTOR_MASK		(PAGE_SECTORS - 1)

#define FREE_BATCH		16

struct testb {
	unsigned int index;
	struct request_queue *q;
	struct gendisk *disk;

	struct testb_device *t_dev;

	struct blk_mq_tag_set tag_set;

	char disk_name[DISK_NAME_LEN];
};

/*
 * testb_page is a page in memory for testb devices.
 *
 * @page:	The page holding the data.
 * @bitmap:	The bitmap represents which sector in the page has data.
 *		Each bit represents one block size. For example, sector 8
 *		will use the 7th bit
 */
struct testb_page {
	struct page *page;
	unsigned long bitmap;
};

/*
 * Status flags for testb_device.
 *
 * CONFIGURED:	Device has been configured and turned on. Cannot reconfigure.
 * UP:		Device is currently on and visible in userspace.
 */
enum testb_device_flags {
	TESTB_DEV_FL_CONFIGURED	= 0,
	TESTB_DEV_FL_UP		= 1,
};

/*
 * testb_device represents the characteristics of a virtual device.
 *
 * @item:	The struct used by configfs to represent items in fs.
 * @lock:	Protect data of the device
 * @testb:	The device that these attributes belong to.
 * @pages:	The storage of the device.
 * @flags:	TEST_DEV_FL_ flags to indicate various status.
 *
 * @power:	1 means on; 0 means off.
 * @size:	The size of the disk (in bytes).
 * @blocksize:	The block size for the request queue.
 * @nr_queues:	The number of queues.
 * @q_depth:	The depth of each queue.
 * @discard:	If enable discard
 */
struct testb_device {
	struct config_item item;
	spinlock_t lock;
	struct testb *testb;
	struct radix_tree_root pages;
	unsigned long flags;

	uint power;
	u64 size;
	uint blocksize;
	uint nr_queues;
	uint q_depth;
	uint discard;
};

static int testb_poweron_device(struct testb_device *dev);
static void testb_poweroff_device(struct testb_device *dev);
static void testb_free_device_storage(struct testb_device *t_dev);

static inline struct testb_device *to_testb_device(struct config_item *item)
{
	return item ? container_of(item, struct testb_device, item) : NULL;
}

static inline ssize_t testb_device_uint_attr_show(uint val, char *page)
{
	return snprintf(page, PAGE_SIZE, "%u\n", val);
}

static ssize_t
testb_device_uint_attr_store(uint *val, const char *page, size_t count)
{
	uint tmp;
	int result;

	result = kstrtouint(page, 0, &tmp);
	if (result)
		return result;

	*val = tmp;
	return count;
}

static inline ssize_t testb_device_u64_attr_show(u64 val, char *page)
{
	return snprintf(page, PAGE_SIZE, "%llu\n", val);
}

static ssize_t
testb_device_u64_attr_store(u64 *val, const char *page, size_t count)
{
	int result;
	u64 tmp;

	result = kstrtoull(page, 0, &tmp);
	if (result)
		return result;

	*val = tmp;
	return count;
}

/* The following macro should only be used with TYPE = {uint, u64}. */
#define TESTB_DEVICE_ATTR(NAME, TYPE)						\
static ssize_t									\
testb_device_##NAME##_show(struct config_item *item, char *page)		\
{										\
	return testb_device_##TYPE##_attr_show(					\
				to_testb_device(item)->NAME, page);		\
}										\
static ssize_t									\
testb_device_##NAME##_store(struct config_item *item, const char *page,		\
			    size_t count)					\
{										\
	if (test_bit(TESTB_DEV_FL_CONFIGURED, &to_testb_device(item)->flags))	\
		return -EBUSY;							\
	return testb_device_##TYPE##_attr_store(				\
			&to_testb_device(item)->NAME, page, count);		\
}										\
CONFIGFS_ATTR(testb_device_, NAME);

TESTB_DEVICE_ATTR(size, u64);
TESTB_DEVICE_ATTR(blocksize, uint);
TESTB_DEVICE_ATTR(nr_queues, uint);
TESTB_DEVICE_ATTR(q_depth, uint);
TESTB_DEVICE_ATTR(discard, uint);

static ssize_t testb_device_power_show(struct config_item *item, char *page)
{
	return testb_device_uint_attr_show(to_testb_device(item)->power, page);
}

static ssize_t testb_device_power_store(struct config_item *item,
				     const char *page, size_t count)
{
	struct testb_device *t_dev = to_testb_device(item);
	uint newp = 0;
	ssize_t ret;

	ret = testb_device_uint_attr_store(&newp, page, count);
	if (ret < 0)
		return ret;

	if (!t_dev->power && newp) {
		if (test_and_set_bit(TESTB_DEV_FL_UP, &t_dev->flags))
			return count;
		ret = testb_poweron_device(t_dev);
		if (ret) {
			clear_bit(TESTB_DEV_FL_UP, &t_dev->flags);
			return -ENOMEM;
		}

		set_bit(TESTB_DEV_FL_CONFIGURED, &t_dev->flags);
		t_dev->power = newp;
	} else if (to_testb_device(item)->power && !newp) {
		t_dev->power = newp;
		testb_poweroff_device(t_dev);
		clear_bit(TESTB_DEV_FL_UP, &t_dev->flags);
	}

	return count;
}

CONFIGFS_ATTR(testb_device_, power);

static struct configfs_attribute *testb_device_attrs[] = {
	&testb_device_attr_power,
	&testb_device_attr_size,
	&testb_device_attr_blocksize,
	&testb_device_attr_nr_queues,
	&testb_device_attr_q_depth,
	&testb_device_attr_discard,
	NULL,
};

static void testb_device_release(struct config_item *item)
{
	struct testb_device *t_dev = to_testb_device(item);

	testb_free_device_storage(t_dev);
	kfree(t_dev);
}

static struct configfs_item_operations testb_device_ops = {
	.release	= testb_device_release,
};

static struct config_item_type testb_device_type = {
	.ct_item_ops	= &testb_device_ops,
	.ct_attrs	= testb_device_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct
config_item *testb_group_make_item(struct config_group *group, const char *name)
{
	struct testb_device *t_dev;

	t_dev = kzalloc(sizeof(struct testb_device), GFP_KERNEL);
	if (!t_dev)
		return ERR_PTR(-ENOMEM);
	spin_lock_init(&t_dev->lock);
	INIT_RADIX_TREE(&t_dev->pages, GFP_ATOMIC);

	config_item_init_type_name(&t_dev->item, name, &testb_device_type);

	/* Initialize attributes with default values. */
	t_dev->size = 1024 * 1024 * 1024ULL;
	t_dev->blocksize = 512;
	t_dev->nr_queues = 2;
	t_dev->q_depth = 64;
	t_dev->discard = 1;

	return &t_dev->item;
}

static void
testb_group_drop_item(struct config_group *group, struct config_item *item)
{
	struct testb_device *t_dev = to_testb_device(item);

	if (test_and_clear_bit(TESTB_DEV_FL_UP, &t_dev->flags)) {
		testb_poweroff_device(t_dev);
		t_dev->power = 0;
	}
	config_item_put(item);
}

static ssize_t memb_group_features_show(struct config_item *item, char *page)
{
	return snprintf(page, PAGE_SIZE, "\n");
}

CONFIGFS_ATTR_RO(memb_group_, features);

static struct configfs_attribute *testb_group_attrs[] = {
	&memb_group_attr_features,
	NULL,
};

static struct configfs_group_operations testb_group_ops = {
	.make_item	= testb_group_make_item,
	.drop_item	= testb_group_drop_item,
};

static struct config_item_type testb_group_type = {
	.ct_group_ops	= &testb_group_ops,
	.ct_attrs	= testb_group_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem testb_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "testb",
			.ci_type = &testb_group_type,
		},
	},
};

static DEFINE_IDA(testb_indices);
static DEFINE_MUTEX(testb_lock);
static int testb_major;

static struct testb_page *testb_alloc_page(gfp_t gfp_flags)
{
	struct testb_page *t_page;

	t_page = kmalloc(sizeof(struct testb_page), gfp_flags);
	if (!t_page)
		goto out;

	t_page->page = alloc_pages(gfp_flags, 0);
	if (!t_page->page)
		goto out_freepage;

	t_page->bitmap = 0;
	return t_page;
out_freepage:
	kfree(t_page);
out:
	return NULL;
}

static void testb_free_page(struct testb_page *t_page)
{
	WARN_ON(!t_page);

	__free_page(t_page->page);
	kfree(t_page);
}

static void testb_free_sector(struct testb *testb, sector_t sector)
{
	unsigned int sector_bit;
	u64 idx;
	struct testb_page *t_page, *ret;
	struct radix_tree_root *root;

	assert_spin_locked(&testb->t_dev->lock);

	root = &testb->t_dev->pages;
	idx = sector >> PAGE_SECTORS_SHIFT;
	sector_bit = (sector & SECTOR_MASK);

	t_page = radix_tree_lookup(root, idx);
	if (t_page) {
		__clear_bit(sector_bit, &t_page->bitmap);

		if (!t_page->bitmap) {
			ret = radix_tree_delete_item(root, idx, t_page);
			WARN_ON(ret != t_page);
			testb_free_page(ret);
		}
	}
}

static struct testb_page *testb_radix_tree_insert(struct testb *testb, u64 idx,
	struct testb_page *t_page)
{
	struct radix_tree_root *root;

	assert_spin_locked(&testb->t_dev->lock);

	root = &testb->t_dev->pages;

	if (radix_tree_insert(root, idx, t_page)) {
		testb_free_page(t_page);
		t_page = radix_tree_lookup(root, idx);
		WARN_ON(!t_page || t_page->page->index != idx);
	}

	return t_page;
}

static void testb_free_device_storage(struct testb_device *t_dev)
{
	unsigned long pos = 0;
	int nr_pages;
	struct testb_page *ret, *t_pages[FREE_BATCH];
	struct radix_tree_root *root;

	root = &t_dev->pages;

	do {
		int i;

		nr_pages = radix_tree_gang_lookup(root,
				(void **)t_pages, pos, FREE_BATCH);

		for (i = 0; i < nr_pages; i++) {
			pos = t_pages[i]->page->index;
			ret = radix_tree_delete_item(root, pos, t_pages[i]);
			WARN_ON(ret != t_pages[i]);
			testb_free_page(ret);
		}

		pos++;
	} while (nr_pages == FREE_BATCH);
}

static struct testb_page *testb_lookup_page(struct testb *testb,
	sector_t sector, bool for_write)
{
	unsigned int sector_bit;
	u64 idx;
	struct testb_page *t_page;

	assert_spin_locked(&testb->t_dev->lock);

	idx = sector >> PAGE_SECTORS_SHIFT;
	sector_bit = (sector & SECTOR_MASK);

	t_page = radix_tree_lookup(&testb->t_dev->pages, idx);
	WARN_ON(t_page && t_page->page->index != idx);

	if (t_page && (for_write || test_bit(sector_bit, &t_page->bitmap)))
		return t_page;

	return NULL;
}

static struct testb_page *testb_insert_page(struct testb *testb,
	sector_t sector, unsigned long *lock_flag)
{
	u64 idx;
	struct testb_page *t_page;

	assert_spin_locked(&testb->t_dev->lock);

	t_page = testb_lookup_page(testb, sector, true);
	if (t_page)
		return t_page;

	spin_unlock_irqrestore(&testb->t_dev->lock, *lock_flag);

	t_page = testb_alloc_page(GFP_NOIO);
	if (!t_page)
		goto out_lock;

	if (radix_tree_preload(GFP_NOIO))
		goto out_freepage;

	spin_lock_irqsave(&testb->t_dev->lock, *lock_flag);
	idx = sector >> PAGE_SECTORS_SHIFT;
	t_page->page->index = idx;
	t_page = testb_radix_tree_insert(testb, idx, t_page);
	radix_tree_preload_end();

	return t_page;
out_freepage:
	testb_free_page(t_page);
out_lock:
	spin_lock_irqsave(&testb->t_dev->lock, *lock_flag);
	return testb_lookup_page(testb, sector, true);
}

static int copy_to_testb(struct testb *testb, struct page *source,
	unsigned int off, sector_t sector, size_t n, unsigned long *lock_flag)
{
	size_t temp, count = 0;
	unsigned int offset;
	struct testb_page *t_page;
	void *dst, *src;

	while (count < n) {
		temp = min_t(size_t, testb->t_dev->blocksize, n - count);

		offset = (sector & SECTOR_MASK) << SECTOR_SHIFT;
		t_page = testb_insert_page(testb, sector, lock_flag);
		if (!t_page)
			return -ENOSPC;

		src = kmap_atomic(source);
		dst = kmap_atomic(t_page->page);
		memcpy(dst + offset, src + off + count, temp);
		kunmap_atomic(dst);
		kunmap_atomic(src);

		__set_bit(sector & SECTOR_MASK, &t_page->bitmap);

		count += temp;
		sector += temp >> SECTOR_SHIFT;
	}
	return 0;
}

static int copy_from_testb(struct testb *testb, struct page *dest,
	unsigned int off, sector_t sector, size_t n, unsigned long *lock_flag)
{
	size_t temp, count = 0;
	unsigned int offset;
	struct testb_page *t_page;
	void *dst, *src;

	while (count < n) {
		temp = min_t(size_t, testb->t_dev->blocksize, n - count);

		offset = (sector & SECTOR_MASK) << SECTOR_SHIFT;
		t_page = testb_lookup_page(testb, sector, false);

		dst = kmap_atomic(dest);
		if (!t_page) {
			memset(dst + off + count, 0, temp);
			goto next;
		}
		src = kmap_atomic(t_page->page);
		memcpy(dst + off + count, src + offset, temp);
		kunmap_atomic(src);
next:
		kunmap_atomic(dst);

		count += temp;
		sector += temp >> SECTOR_SHIFT;
	}
	return 0;
}

static void testb_handle_discard(struct testb *testb, sector_t sector, size_t n)
{
	size_t temp;
	unsigned long lock_flag;

	spin_lock_irqsave(&testb->t_dev->lock, lock_flag);
	while (n > 0) {
		temp = min_t(size_t, n, testb->t_dev->blocksize);
		testb_free_sector(testb, sector);
		sector += temp >> SECTOR_SHIFT;
		n -= temp;
	}
	spin_unlock_irqrestore(&testb->t_dev->lock, lock_flag);
}

static int testb_handle_flush(struct testb *testb)
{
	return 0;
}

static int testb_transfer(struct testb *testb, struct page *page,
	unsigned int len, unsigned int off, bool is_write, sector_t sector,
	unsigned long *lock_flags)
{
	int err = 0;

	if (!is_write) {
		err = copy_from_testb(testb, page, off, sector, len,
						lock_flags);
		flush_dcache_page(page);
	} else {
		flush_dcache_page(page);
		err = copy_to_testb(testb, page, off, sector, len,
						lock_flags);
	}

	return err;
}

static int testb_handle_rq(struct request *rq)
{
	struct testb *testb = rq->q->queuedata;
	int err;
	unsigned int len;
	sector_t sector;
	struct req_iterator iter;
	struct bio_vec bvec;
	unsigned long lock_flag;

	sector = blk_rq_pos(rq);

	if (req_op(rq) == REQ_OP_DISCARD) {
		testb_handle_discard(testb, sector, blk_rq_bytes(rq));
		return 0;
	} else if (req_op(rq) == REQ_OP_FLUSH)
		return testb_handle_flush(testb);

	spin_lock_irqsave(&testb->t_dev->lock, lock_flag);
	rq_for_each_segment(bvec, rq, iter) {
		len = bvec.bv_len;
		err = testb_transfer(testb, bvec.bv_page, len, bvec.bv_offset,
				     op_is_write(req_op(rq)), sector,
				     &lock_flag);
		if (err) {
			spin_unlock_irqrestore(&testb->t_dev->lock, lock_flag);
			return err;
		}
		sector += len >> SECTOR_SHIFT;
	}
	spin_unlock_irqrestore(&testb->t_dev->lock, lock_flag);

	return 0;
}

static blk_status_t
testb_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
	int err;

	blk_mq_start_request(bd->rq);

	err = testb_handle_rq(bd->rq);
	if (err)
		return errno_to_blk_status(err);

	blk_mq_complete_request(bd->rq);
	return BLK_STS_OK;
}

static void testb_softirq_done_fn(struct request *rq)
{
	blk_mq_end_request(rq, BLK_STS_OK);
}

static const struct blk_mq_ops testb_mq_ops = {
	.queue_rq	= testb_queue_rq,
	.complete	= testb_softirq_done_fn,
};

static const struct block_device_operations testb_fops = {
	.owner		= THIS_MODULE,
};

static void testb_free_bdev(struct testb *testb)
{
	mutex_lock(&testb_lock);
	ida_simple_remove(&testb_indices, testb->index);
	mutex_unlock(&testb_lock);

	blk_cleanup_queue(testb->q);
	blk_mq_free_tag_set(&testb->tag_set);

	kfree(testb);
}

static void testb_gendisk_unregister(struct testb *testb)
{
	del_gendisk(testb->disk);

	put_disk(testb->disk);
}

static void testb_poweroff_device(struct testb_device *dev)
{
	testb_gendisk_unregister(dev->testb);
	testb_free_bdev(dev->testb);
}

static void testb_config_discard(struct testb *testb)
{
	if (testb->t_dev->discard == 0)
		return;
	testb->q->limits.discard_granularity = testb->t_dev->blocksize;
	testb->q->limits.discard_alignment = testb->t_dev->blocksize;
	blk_queue_max_discard_sectors(testb->q, UINT_MAX >> 9);
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, testb->q);
}

static void testb_config_flush(struct testb *testb)
{
	blk_queue_write_cache(testb->q, true, true);
	blk_queue_flush_queueable(testb->q, true);
}

static int testb_gendisk_register(struct testb *testb)
{
	sector_t size;
	struct gendisk *disk;

	disk = testb->disk = alloc_disk(DISK_MAX_PARTS);
	if (!disk)
		return -ENOMEM;

	size = testb->t_dev->size;
	set_capacity(disk, size >> 9);

	disk->flags		= GENHD_FL_EXT_DEVT;
	disk->major		= testb_major;
	disk->first_minor	= testb->index * DISK_MAX_PARTS;
	disk->fops		= &testb_fops;
	disk->private_data	= testb;
	disk->queue		= testb->q;
	snprintf(disk->disk_name, DISK_NAME_LEN, "%s", testb->disk_name);

	add_disk(testb->disk);
	return 0;
}

static int testb_alloc_bdev(struct testb_device *t_dev)
{
	int ret;
	struct testb *testb;

	testb = kzalloc(sizeof(struct testb), GFP_KERNEL);
	if (!testb) {
		ret = -ENOMEM;
		goto out;
	}

	t_dev->blocksize = (t_dev->blocksize >> SECTOR_SHIFT) << SECTOR_SHIFT;
	t_dev->blocksize = clamp_t(uint, t_dev->blocksize, 512, 4096);

	if (t_dev->nr_queues > nr_cpu_ids)
		t_dev->nr_queues = nr_cpu_ids;
	else if (!t_dev->nr_queues)
		t_dev->nr_queues = 1;

	testb->t_dev = t_dev;
	t_dev->testb = testb;

	testb->tag_set.ops = &testb_mq_ops;
	testb->tag_set.nr_hw_queues = t_dev->nr_queues;
	testb->tag_set.queue_depth = t_dev->q_depth;
	testb->tag_set.numa_node = NUMA_NO_NODE;
	testb->tag_set.cmd_size = 0;
	testb->tag_set.flags = BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_SG_MERGE |
			      BLK_MQ_F_BLOCKING;
	testb->tag_set.driver_data = testb;

	ret = blk_mq_alloc_tag_set(&testb->tag_set);
	if (ret)
		goto out_cleanup_queues;

	testb->q = blk_mq_init_queue(&testb->tag_set);
	if (IS_ERR(testb->q)) {
		ret = -ENOMEM;
		goto out_cleanup_tags;
	}

	testb->q->queuedata = testb;
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, testb->q);
	queue_flag_clear_unlocked(QUEUE_FLAG_ADD_RANDOM, testb->q);

	testb_config_discard(testb);
	testb_config_flush(testb);

	blk_queue_logical_block_size(testb->q, t_dev->blocksize);
	blk_queue_physical_block_size(testb->q, t_dev->blocksize);

	snprintf(testb->disk_name, CONFIGFS_ITEM_NAME_LEN, "testb_%s",
		 t_dev->item.ci_name);

	mutex_lock(&testb_lock);
	testb->index = ida_simple_get(&testb_indices, 0, 0, GFP_KERNEL);
	mutex_unlock(&testb_lock);

	return 0;
out_cleanup_tags:
	blk_mq_free_tag_set(&testb->tag_set);
out_cleanup_queues:
	kfree(testb);
out:
	return ret;
}

static int testb_poweron_device(struct testb_device *dev)
{
	int ret;

	ret = testb_alloc_bdev(dev);
	if (ret)
		return ret;
	if (testb_gendisk_register(dev->testb)) {
		testb_free_bdev(dev->testb);
		return -EINVAL;
	}
	return 0;
}

static int __init testb_init(void)
{
	int ret = 0;
	struct configfs_subsystem *subsys = &testb_subsys;

	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);

	testb_major = register_blkdev(0, "testb");
	if (testb_major < 0)
		return testb_major;

	ret = configfs_register_subsystem(subsys);
	if (ret)
		goto out_unregister;

	return 0;
out_unregister:
	unregister_blkdev(testb_major, "testb");
	return ret;
}

static void __exit testb_exit(void)
{
	unregister_blkdev(testb_major, "testb");

	configfs_unregister_subsystem(&testb_subsys);
}

module_init(testb_init);
module_exit(testb_exit);

MODULE_AUTHOR("Will Koh <kkc6196@fb.com>, Shaohua Li <shli@fb.com>");
MODULE_LICENSE("GPL");
