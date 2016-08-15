#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/device-mapper.h>
#include <linux/dm-io.h>
#include <linux/crypto.h>
#include <linux/lzo.h>
#include <linux/kthread.h>
#include <linux/page-flags.h>
#include <linux/completion.h>
#include <linux/vmalloc.h>
#include "dm-inplace-compress.h"

#define DM_MSG_PREFIX "dm-inplace-compress"

static struct dm_icomp_compressor_data compressors[] = {
	[DMCP_COMP_ALG_LZO] = {
		.name = "lzo",
		.comp_len = lzo_comp_len,
		.max_comp_len = lzo_max_comp_len,
	},
	[DMCP_COMP_ALG_842] = {
		.name = "842",
		.comp_len = nx842_comp_len,
		.max_comp_len = nx842_max_comp_len,
	},
};
static int default_compressor = -1;

#define DMCP_ALGO_LENGTH 9
static char dm_icomp_algorithm[DMCP_ALGO_LENGTH] = "lzo";
static struct kparam_string dm_icomp_compressor_kparam = {
	.string =	dm_icomp_algorithm,
	.maxlen =	sizeof(dm_icomp_algorithm),
};
static int dm_icomp_compressor_param_set(const char *,
		const struct kernel_param *);
static struct kernel_param_ops dm_icomp_compressor_param_ops = {
	.set =	dm_icomp_compressor_param_set,
	.get =	param_get_string,
};
module_param_cb(compress_algorithm, &dm_icomp_compressor_param_ops,
		&dm_icomp_compressor_kparam, 0644);

static int dm_icomp_get_compressor(const char *s)
{
	int r, val_len;

	if (crypto_has_comp(s, 0, 0)) {
		for (r = 0; r < ARRAY_SIZE(compressors); r++) {
			val_len = strlen(compressors[r].name);
			if (strncmp(s, compressors[r].name, val_len) == 0)
				return r;
		}
	}
	return -1;
}

static int dm_icomp_compressor_param_set(const char *val,
		const struct kernel_param *kp)
{
	int ret;
	char str[kp->str->maxlen], *s;
	int val_len = strlen(val)+1;

	strlcpy(str, val, val_len);
	s = strim(str);
	ret = dm_icomp_get_compressor(s);
	if (ret < 0) {
		DMWARN("Compressor %s not supported", s);
		return -1;
	}
	DMWARN("compressor  is %s", s);
	default_compressor = ret;
	strlcpy(dm_icomp_algorithm, compressors[ret].name,
		sizeof(dm_icomp_algorithm));
	return 0;
}

static struct kmem_cache *dm_icomp_req_cachep;
static struct kmem_cache *dm_icomp_io_range_cachep;
static struct kmem_cache *dm_icomp_meta_io_cachep;

static struct dm_icomp_io_worker dm_icomp_io_workers[NR_CPUS];
static struct workqueue_struct *dm_icomp_wq;

static u8 dm_icomp_get_meta(struct dm_icomp_info *info, u64 block_index)
{
	u64 first_bit = block_index * DMCP_META_BITS;
	int bits, offset;
	u8 data, ret = 0;

	offset = first_bit & 7;
	bits = min_t(u8, DMCP_META_BITS, 8 - offset);

	data = info->meta_bitmap[first_bit >> 3];
	ret = (data >> offset) & ((1 << bits) - 1);

	if (bits < DMCP_META_BITS) {
		data = info->meta_bitmap[(first_bit >> 3) + 1];
		bits = DMCP_META_BITS - bits;
		ret |= (data & ((1 << bits) - 1)) << (DMCP_META_BITS - bits);
	}
	return ret;
}

static void dm_icomp_set_meta(struct dm_icomp_info *info, u64 block_index,
		u8 meta, bool dirty_meta)
{
	u64 first_bit = block_index * DMCP_META_BITS;
	int bits, offset;
	u8 data;
	struct page *page;

	offset = first_bit & 7;
	bits = min_t(u8, DMCP_META_BITS, 8 - offset);

	data = info->meta_bitmap[first_bit >> 3];
	data &= ~(((1 << bits) - 1) << offset);
	data |= (meta & ((1 << bits) - 1)) << offset;
	info->meta_bitmap[first_bit >> 3] = data;

	if (info->write_mode == DMCP_WRITE_BACK) {
		page = vmalloc_to_page(&info->meta_bitmap[first_bit >> 3]);
		if (dirty_meta)
			SetPageDirty(page);
		else
			ClearPageDirty(page);
	}

	if (bits < DMCP_META_BITS) {
		meta >>= bits;
		data = info->meta_bitmap[(first_bit >> 3) + 1];
		bits = DMCP_META_BITS - bits;
		data = (data >> bits) << bits;
		data |= meta & ((1 << bits) - 1);
		info->meta_bitmap[(first_bit >> 3) + 1] = data;

		if (info->write_mode == DMCP_WRITE_BACK) {
			page = vmalloc_to_page(&info->meta_bitmap[
						(first_bit >> 3) + 1]);
			if (dirty_meta)
				SetPageDirty(page);
			else
				ClearPageDirty(page);
		}
	}
}

static void dm_icomp_set_extent(struct dm_icomp_req *req, u64 block,
	u16 logical_blocks, sector_t data_sectors)
{
	int i;
	u8 data;

	for (i = 0; i < logical_blocks; i++) {
		data = min_t(sector_t, data_sectors, 8);
		data_sectors -= data;
		if (i != 0)
			data |= DMCP_TAIL_MASK;
		/* For FUA, we write out meta data directly */
		dm_icomp_set_meta(req->info, block + i, data,
					!(req->bio->bi_rw & REQ_FUA));
	}
}

static void dm_icomp_get_extent(struct dm_icomp_info *info, u64 block_index,
	u64 *first_block_index, u16 *logical_sectors, u16 *data_sectors)
{
	u8 data;

	data = dm_icomp_get_meta(info, block_index);
	while (data & DMCP_TAIL_MASK) {
		block_index--;
		data = dm_icomp_get_meta(info, block_index);
	}
	*first_block_index = block_index;
	*logical_sectors = DMCP_BLOCK_SIZE >> 9;
	*data_sectors = data & DMCP_LENGTH_MASK;
	block_index++;
	while (block_index < info->data_blocks) {
		data = dm_icomp_get_meta(info, block_index);
		if (!(data & DMCP_TAIL_MASK))
			break;
		*logical_sectors += DMCP_BLOCK_SIZE >> 9;
		*data_sectors += data & DMCP_LENGTH_MASK;
		block_index++;
	}
}

static int dm_icomp_access_super(struct dm_icomp_info *info, void *addr, int rw)
{
	struct dm_io_region region;
	struct dm_io_request req;
	unsigned long io_error = 0;
	int ret;

	region.bdev = info->dev->bdev;
	region.sector = 0;
	region.count = DMCP_BLOCK_SIZE >> 9;

	req.bi_rw = rw;
	req.mem.type = DM_IO_KMEM;
	req.mem.offset = 0;
	req.mem.ptr.addr = addr;
	req.notify.fn = NULL;
	req.client = info->io_client;

	ret = dm_io(&req, 1, &region, &io_error);
	if (ret || io_error)
		return -EIO;
	return 0;
}

static void dm_icomp_meta_io_done(unsigned long error, void *context)
{
	struct dm_icomp_meta_io *meta_io = context;

	meta_io->fn(meta_io->data, error);
	kmem_cache_free(dm_icomp_meta_io_cachep, meta_io);
}

static int dm_icomp_write_meta(struct dm_icomp_info *info, u64 start_page,
	u64 end_page, void *data,
	void (*fn)(void *data, unsigned long error), int rw)
{
	struct dm_icomp_meta_io *meta_io;

	WARN_ON(end_page > info->meta_bitmap_pages);

	meta_io = kmem_cache_alloc(dm_icomp_meta_io_cachep, GFP_NOIO);
	if (!meta_io) {
		fn(data, -ENOMEM);
		return -ENOMEM;
	}
	meta_io->data = data;
	meta_io->fn = fn;

	meta_io->io_region.bdev = info->dev->bdev;
	meta_io->io_region.sector = DMCP_META_START_SECTOR +
					(start_page << (PAGE_SHIFT - 9));
	meta_io->io_region.count = (end_page - start_page) << (PAGE_SHIFT - 9);

	atomic64_add(meta_io->io_region.count << 9, &info->meta_write_size);

	meta_io->io_req.bi_rw = rw;
	meta_io->io_req.mem.type = DM_IO_VMA;
	meta_io->io_req.mem.offset = 0;
	meta_io->io_req.mem.ptr.addr = info->meta_bitmap +
						(start_page << PAGE_SHIFT);
	meta_io->io_req.notify.fn = dm_icomp_meta_io_done;
	meta_io->io_req.notify.context = meta_io;
	meta_io->io_req.client = info->io_client;

	dm_io(&meta_io->io_req, 1, &meta_io->io_region, NULL);
	return 0;
}

struct writeback_flush_data {
	struct completion complete;
	atomic_t cnt;
};

static void writeback_flush_io_done(void *data, unsigned long error)
{
	struct writeback_flush_data *wb = data;

	if (atomic_dec_return(&wb->cnt))
		return;
	complete(&wb->complete);
}

static void dm_icomp_flush_dirty_meta(struct dm_icomp_info *info,
			struct writeback_flush_data *data)
{
	struct page *page;
	u64 start = 0, index;
	u32 pending = 0, cnt = 0;
	bool dirty;
	struct blk_plug plug;

	blk_start_plug(&plug);
	for (index = 0; index < info->meta_bitmap_pages; index++, cnt++) {
		if (cnt == 256) {
			cnt = 0;
			cond_resched();
		}

		page = vmalloc_to_page(info->meta_bitmap +
					(index << PAGE_SHIFT));
		dirty = TestClearPageDirty(page);

		if (pending == 0 && dirty) {
			start = index;
			pending++;
			continue;
		} else if (pending == 0)
			continue;
		else if (pending > 0 && dirty) {
			pending++;
			continue;
		}

		/* pending > 0 && !dirty */
		atomic_inc(&data->cnt);
		dm_icomp_write_meta(info, start, start + pending, data,
			writeback_flush_io_done, WRITE);
		pending = 0;
	}

	if (pending > 0) {
		atomic_inc(&data->cnt);
		dm_icomp_write_meta(info, start, start + pending, data,
			writeback_flush_io_done, WRITE);
	}
	blkdev_issue_flush(info->dev->bdev, GFP_NOIO, NULL);
	blk_finish_plug(&plug);
}

static int dm_icomp_meta_writeback_thread(void *data)
{
	struct dm_icomp_info *info = data;
	struct writeback_flush_data wb;

	atomic_set(&wb.cnt, 1);
	init_completion(&wb.complete);

	while (!kthread_should_stop()) {
		schedule_timeout_interruptible(
			msecs_to_jiffies(info->writeback_delay * 1000));
		dm_icomp_flush_dirty_meta(info, &wb);
	}

	dm_icomp_flush_dirty_meta(info, &wb);

	writeback_flush_io_done(&wb, 0);
	wait_for_completion(&wb.complete);
	return 0;
}

static int dm_icomp_init_meta(struct dm_icomp_info *info, bool new)
{
	struct dm_io_region region;
	struct dm_io_request req;
	unsigned long io_error = 0;
	struct blk_plug plug;
	int ret;
	ssize_t len = DIV_ROUND_UP_ULL(info->meta_bitmap_bits, BITS_PER_LONG);

	len *= sizeof(unsigned long);

	region.bdev = info->dev->bdev;
	region.sector = DMCP_META_START_SECTOR;
	region.count = (len + 511) >> 9;

	req.mem.type = DM_IO_VMA;
	req.mem.offset = 0;
	req.mem.ptr.addr = info->meta_bitmap;
	req.notify.fn = NULL;
	req.client = info->io_client;

	blk_start_plug(&plug);
	if (new) {
		memset(info->meta_bitmap, 0, len);
		req.bi_rw = WRITE_FLUSH;
		ret = dm_io(&req, 1, &region, &io_error);
	} else {
		req.bi_rw = READ;
		ret = dm_io(&req, 1, &region, &io_error);
	}
	blk_finish_plug(&plug);

	if (ret || io_error) {
		info->ti->error = "Access metadata error";
		return -EIO;
	}

	if (info->write_mode == DMCP_WRITE_BACK) {
		info->writeback_tsk = kthread_run(
			dm_icomp_meta_writeback_thread,
			info, "dm_icomp_writeback");
		if (!info->writeback_tsk) {
			info->ti->error = "Create writeback thread error";
			return -EINVAL;
		}
	}

	return 0;
}

static int dm_icomp_alloc_compressor(struct dm_icomp_info *info)
{
	int i;

	for_each_possible_cpu(i) {
		info->tfm[i] = crypto_alloc_comp(
			compressors[info->comp_alg].name, 0, 0);
		if (IS_ERR(info->tfm[i])) {
			info->tfm[i] = NULL;
			goto err;
		}
	}
	return 0;
err:
	for_each_possible_cpu(i) {
		if (info->tfm[i]) {
			crypto_free_comp(info->tfm[i]);
			info->tfm[i] = NULL;
		}
	}
	return -ENOMEM;
}

static void dm_icomp_free_compressor(struct dm_icomp_info *info)
{
	int i;

	for_each_possible_cpu(i) {
		if (info->tfm[i]) {
			crypto_free_comp(info->tfm[i]);
			info->tfm[i] = NULL;
		}
	}
}

static int dm_icomp_read_or_create_super(struct dm_icomp_info *info)
{
	void *addr;
	struct dm_icomp_super_block *super;
	u64 total_blocks;
	u64 data_blocks, meta_blocks;
	u32 rem, cnt;
	bool new_super = false;
	int ret;
	ssize_t len;

	total_blocks = i_size_read(info->dev->bdev->bd_inode) >>
					DMCP_BLOCK_SHIFT;
	data_blocks = total_blocks - 1;
	rem = do_div(data_blocks, DMCP_BLOCK_SIZE * 8 + DMCP_META_BITS);
	meta_blocks = data_blocks * DMCP_META_BITS;
	data_blocks *= DMCP_BLOCK_SIZE * 8;

	cnt = rem;
	rem /= (DMCP_BLOCK_SIZE * 8 / DMCP_META_BITS + 1);
	data_blocks += rem * (DMCP_BLOCK_SIZE * 8 / DMCP_META_BITS);
	meta_blocks += rem;

	cnt %= (DMCP_BLOCK_SIZE * 8 / DMCP_META_BITS + 1);
	meta_blocks += 1;
	data_blocks += cnt - 1;

	info->data_blocks = data_blocks;
	info->data_start = (1 + meta_blocks) << DMCP_BLOCK_SECTOR_SHIFT;

	if ((data_blocks << DMCP_BLOCK_SECTOR_SHIFT) < info->ti->len) {
		info->ti->error =
			"Insufficient sectors to satisfy requested size";
		return -ENOMEM;
	}

	addr = kzalloc(DMCP_BLOCK_SIZE, GFP_KERNEL);
	if (!addr) {
		info->ti->error = "Cannot allocate super";
		return -ENOMEM;
	}

	super = addr;
	ret = dm_icomp_access_super(info, addr, READ);
	if (ret)
		goto out;

	if (le64_to_cpu(super->magic) == DMCP_SUPER_MAGIC) {
		if (le64_to_cpu(super->meta_blocks) != meta_blocks ||
		    le64_to_cpu(super->data_blocks) != data_blocks) {
			info->ti->error = "Super is invalid";
			ret = -EINVAL;
			goto out;
		}
		if (!crypto_has_comp(compressors[info->comp_alg].name, 0, 0)) {
			info->ti->error =
					"Compressor algorithm doesn't support";
			ret = -EINVAL;
			goto out;
		}
	} else {
		super->magic = cpu_to_le64(DMCP_SUPER_MAGIC);
		super->meta_blocks = cpu_to_le64(meta_blocks);
		super->data_blocks = cpu_to_le64(data_blocks);
		super->comp_alg = default_compressor;
		ret = dm_icomp_access_super(info, addr, WRITE_FUA);
		if (ret) {
			info->ti->error = "Access super fails";
			goto out;
		}
		new_super = true;
	}

	if (dm_icomp_alloc_compressor(info)) {
		ret = -ENOMEM;
		goto out;
	}

	info->meta_bitmap_bits = data_blocks * DMCP_META_BITS;
	len = DIV_ROUND_UP_ULL(info->meta_bitmap_bits, BITS_PER_LONG);
	len *= sizeof(unsigned long);
	info->meta_bitmap_pages = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	info->meta_bitmap = vmalloc(info->meta_bitmap_pages * PAGE_SIZE);
	if (!info->meta_bitmap) {
		ret = -ENOMEM;
		goto bitmap_err;
	}

	ret = dm_icomp_init_meta(info, new_super);
	if (ret)
		goto meta_err;

	return 0;
meta_err:
	vfree(info->meta_bitmap);
bitmap_err:
	dm_icomp_free_compressor(info);
out:
	kfree(addr);
	return ret;
}

/*
 * <dev> [ <writethough>/<writeback> <meta_commit_delay> ]
 *	 [ <compressor> <type> ]
 */
static int dm_icomp_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct dm_icomp_info *info;
	char mode[15];
	int par = 0;
	int ret, i;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		ti->error = "dm-inplace-compress: Cannot allocate context";
		return -ENOMEM;
	}
	info->ti = ti;
	info->comp_alg = default_compressor;
	while (++par < argc) {
		if (sscanf(argv[par], "%s", mode) != 1) {
			ti->error = "Invalid argument";
			ret = -EINVAL;
			goto err_para;
		}

		if (strcmp(mode, "writeback") == 0) {
			info->write_mode = DMCP_WRITE_BACK;
			if (sscanf(argv[++par], "%u",
				 &info->writeback_delay) != 1) {
				ti->error = "Invalid argument";
				ret = -EINVAL;
				goto err_para;
			}
		} else if (strcmp(mode, "writethrough") == 0) {
			info->write_mode = DMCP_WRITE_THROUGH;
		} else if (strcmp(mode, "compressor") == 0) {
			if (sscanf(argv[++par], "%s", mode) != 1) {
				ti->error = "Invalid argument";
				ret = -EINVAL;
				goto err_para;
			}
			ret = dm_icomp_get_compressor(mode);
			if (ret >= 0) {
				DMWARN("compressor  is %s", mode);
				info->comp_alg = ret;
			} else {
				ti->error = "Unsupported compressor";
				ret = -EINVAL;
				goto err_para;
			}
		}
	}

	if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table),
							&info->dev)) {
		ti->error = "Can't get device";
		ret = -EINVAL;
		goto err_para;
	}

	info->io_client = dm_io_client_create();
	if (!info->io_client) {
		ti->error = "Can't create io client";
		ret = -EINVAL;
		goto err_ioclient;
	}

	if (bdev_logical_block_size(info->dev->bdev) != 512) {
		ti->error = "Can't logical block size too big";
		ret = -EINVAL;
		goto err_blocksize;
	}

	ret = dm_icomp_read_or_create_super(info);
	if (ret)
		goto err_blocksize;

	for (i = 0; i < BITMAP_HASH_LEN; i++) {
		info->bitmap_locks[i].io_running = 0;
		spin_lock_init(&info->bitmap_locks[i].wait_lock);
		INIT_LIST_HEAD(&info->bitmap_locks[i].wait_list);
	}

	atomic64_set(&info->compressed_write_size, 0);
	atomic64_set(&info->uncompressed_write_size, 0);
	atomic64_set(&info->meta_write_size, 0);
	ti->num_flush_bios = 1;
	/* ti->num_discard_bios = 1; */
	ti->private = info;
	return 0;
err_blocksize:
	dm_io_client_destroy(info->io_client);
err_ioclient:
	dm_put_device(ti, info->dev);
err_para:
	kfree(info);
	return ret;
}

static void dm_icomp_dtr(struct dm_target *ti)
{
	struct dm_icomp_info *info = ti->private;

	if (info->write_mode == DMCP_WRITE_BACK)
		kthread_stop(info->writeback_tsk);
	dm_icomp_free_compressor(info);
	vfree(info->meta_bitmap);
	dm_io_client_destroy(info->io_client);
	dm_put_device(ti, info->dev);
	kfree(info);
}

static u64 dm_icomp_sector_to_block(sector_t sect)
{
	return sect >> DMCP_BLOCK_SECTOR_SHIFT;
}

static struct dm_icomp_hash_lock *dm_icomp_block_hash_lock(
		struct dm_icomp_info *info, u64 block_index)
{
	return &info->bitmap_locks[(block_index >> BITMAP_HASH_SHIFT) &
			BITMAP_HASH_MASK];
}

static struct dm_icomp_hash_lock *dm_icomp_trylock_block(
		struct dm_icomp_info *info,
		struct dm_icomp_req *req, u64 block_index)
{
	struct dm_icomp_hash_lock *hash_lock;

	hash_lock = dm_icomp_block_hash_lock(req->info, block_index);

	spin_lock_irq(&hash_lock->wait_lock);
	if (!hash_lock->io_running) {
		hash_lock->io_running = 1;
		spin_unlock_irq(&hash_lock->wait_lock);
		return hash_lock;
	}
	list_add_tail(&req->sibling, &hash_lock->wait_list);
	spin_unlock_irq(&hash_lock->wait_lock);
	return NULL;
}

static void dm_icomp_queue_req_list(struct dm_icomp_info *info,
	 struct list_head *list);

static void dm_icomp_unlock_block(struct dm_icomp_info *info,
	struct dm_icomp_req *req, struct dm_icomp_hash_lock *hash_lock)
{
	LIST_HEAD(pending_list);
	unsigned long flags;

	spin_lock_irqsave(&hash_lock->wait_lock, flags);
	/* wakeup all pending reqs to avoid live lock */
	list_splice_init(&hash_lock->wait_list, &pending_list);
	hash_lock->io_running = 0;
	spin_unlock_irqrestore(&hash_lock->wait_lock, flags);

	dm_icomp_queue_req_list(info, &pending_list);
}

static int dm_icomp_lock_req_range(struct dm_icomp_req *req)
{
	u64 block_index, first_block_index;
	u64 first_lock_block, second_lock_block;
	u16 logical_sectors, data_sectors;

	block_index = dm_icomp_sector_to_block(req->bio->bi_iter.bi_sector);
	req->locks[0] = dm_icomp_trylock_block(req->info, req, block_index);
	if (!req->locks[0])
		return 0;
	dm_icomp_get_extent(req->info, block_index, &first_block_index,
				&logical_sectors, &data_sectors);
	if (dm_icomp_block_hash_lock(req->info, first_block_index) !=
						req->locks[0]) {
		dm_icomp_unlock_block(req->info, req, req->locks[0]);

		first_lock_block = first_block_index;
		second_lock_block = block_index;
		goto two_locks;
	}

	block_index = dm_icomp_sector_to_block(bio_end_sector(req->bio) - 1);
	dm_icomp_get_extent(req->info, block_index, &first_block_index,
				&logical_sectors, &data_sectors);
	first_block_index += dm_icomp_sector_to_block(logical_sectors) - 1;
	if (dm_icomp_block_hash_lock(req->info, first_block_index) !=
						req->locks[0]) {
		second_lock_block = first_block_index;
		goto second_lock;
	}
	req->locked_locks = 1;
	return 1;

two_locks:
	req->locks[0] = dm_icomp_trylock_block(req->info, req,
		first_lock_block);
	if (!req->locks[0])
		return 0;
second_lock:
	req->locks[1] = dm_icomp_trylock_block(req->info, req,
				second_lock_block);
	if (!req->locks[1]) {
		dm_icomp_unlock_block(req->info, req, req->locks[0]);
		return 0;
	}
	/* Don't need check if meta is changed */
	req->locked_locks = 2;
	return 1;
}

static void dm_icomp_unlock_req_range(struct dm_icomp_req *req)
{
	int i;

	for (i = req->locked_locks - 1; i >= 0; i--)
		dm_icomp_unlock_block(req->info, req, req->locks[i]);
}

static void dm_icomp_queue_req(struct dm_icomp_info *info,
		struct dm_icomp_req *req)
{
	unsigned long flags;
	struct dm_icomp_io_worker *worker = &dm_icomp_io_workers[req->cpu];

	spin_lock_irqsave(&worker->lock, flags);
	list_add_tail(&req->sibling, &worker->pending);
	spin_unlock_irqrestore(&worker->lock, flags);

	queue_work_on(req->cpu, dm_icomp_wq, &worker->work);
}

static void dm_icomp_queue_req_list(struct dm_icomp_info *info,
		struct list_head *list)
{
	struct dm_icomp_req *req;

	while (!list_empty(list)) {
		req = list_first_entry(list, struct dm_icomp_req, sibling);
		list_del_init(&req->sibling);
		dm_icomp_queue_req(info, req);
	}
}

static void dm_icomp_get_req(struct dm_icomp_req *req)
{
	atomic_inc(&req->io_pending);
}

static void *dm_icomp_kmalloc(size_t size, gfp_t flags)
{
	return  kmalloc(size, flags);
}

static void *dm_icomp_krealloc(void *addr, size_t size,
		 size_t orig_size, gfp_t flags)
{
	return krealloc(addr, size, flags);
}

static void dm_icomp_kfree(void *addr, unsigned int size)
{
	kfree(addr);
}


static void dm_icomp_free_io_range(struct dm_icomp_io_range *io)
{
	dm_icomp_kfree(io->decomp_data, io->decomp_len);
	dm_icomp_kfree(io->comp_data, io->comp_len);
	kmem_cache_free(dm_icomp_io_range_cachep, io);
}

static void dm_icomp_put_req(struct dm_icomp_req *req)
{
	struct dm_icomp_io_range *io;

	if (atomic_dec_return(&req->io_pending))
		return;

	if (req->stage == STAGE_INIT) /* waiting for locking */
		return;

	if (req->stage == STAGE_READ_DECOMP ||
	    req->stage == STAGE_WRITE_COMP ||
	    req->result)
		req->stage = STAGE_DONE;

	if (req->stage != STAGE_DONE) {
		dm_icomp_queue_req(req->info, req);
		return;
	}

	while (!list_empty(&req->all_io)) {
		io = list_entry(req->all_io.next,
			struct dm_icomp_io_range, next);
		list_del(&io->next);
		dm_icomp_free_io_range(io);
	}

	dm_icomp_unlock_req_range(req);

	req->bio->bi_error = req->result;
	bio_endio(req->bio);
	kmem_cache_free(dm_icomp_req_cachep, req);
}

static void dm_icomp_io_range_done(unsigned long error, void *context)
{
	struct dm_icomp_io_range *io = context;

	if (error)
		io->req->result = error;
	dm_icomp_put_req(io->req);
}

static inline int dm_icomp_compressor_len(struct dm_icomp_info *info, int len)
{
	if (compressors[info->comp_alg].comp_len)
		return compressors[info->comp_alg].comp_len(len);
	return len;
}

static inline int dm_icomp_compressor_maxlen(struct dm_icomp_info *info,
		int len)
{
	if (compressors[info->comp_alg].max_comp_len)
		return compressors[info->comp_alg].max_comp_len(len);
	return len;
}

/*
 * caller should set region.sector, region.count. bi_rw. IO always to/from
 * comp_data
 */
static struct dm_icomp_io_range *dm_icomp_create_io_range(
		struct dm_icomp_req *req, int comp_len)
{
	struct dm_icomp_io_range *io;

	io = kmem_cache_alloc(dm_icomp_io_range_cachep, GFP_NOIO);
	if (!io)
		return NULL;

	io->comp_data = dm_icomp_kmalloc(comp_len, GFP_NOIO);
	if (!io->comp_data) {
		kmem_cache_free(dm_icomp_io_range_cachep, io);
		return NULL;
	}

	io->io_req.notify.fn = dm_icomp_io_range_done;
	io->io_req.notify.context = io;
	io->io_req.client = req->info->io_client;
	io->io_req.mem.type = DM_IO_KMEM;
	io->io_req.mem.ptr.addr = io->comp_data;
	io->io_req.mem.offset = 0;

	io->io_region.bdev = req->info->dev->bdev;

	io->comp_len = comp_len;
	io->req = req;

	io->decomp_data = NULL;
	io->decomp_len = 0;
	io->decomp_req_len = 0;
	return io;
}

static struct dm_icomp_io_range *dm_icomp_create_io_read_range(
		struct dm_icomp_req *req, int comp_len, int decomp_len)
{
	struct dm_icomp_io_range *io = dm_icomp_create_io_range(req, comp_len);

	if (io) {
		/* note down the requested length for decompress buffer.
		 * but dont allocate it yet.
		 */
		io->decomp_req_len = decomp_len;
	}
	return io;
}

static int dm_icomp_update_io_read_range(struct dm_icomp_io_range *io)
{
	if (io->decomp_len)
		return 0;

	io->decomp_data = dm_icomp_kmalloc(io->decomp_req_len, GFP_NOIO);
	if (!io->decomp_data)
		return 1;
	io->decomp_len = io->decomp_req_len;

	return 0;
}

static void dm_icomp_bio_copy(struct bio *bio, off_t bio_off, void *buf,
		ssize_t len, bool to_buf)
{
	struct bio_vec bv;
	struct bvec_iter iter;
	off_t buf_off = 0;
	ssize_t size;
	void *addr;

	WARN_ON(bio_off + len > (bio_sectors(bio) << 9));

	bio_for_each_segment(bv, bio, iter) {
		int length = bv.bv_len;

		if (bio_off >= length) {
			bio_off -= length;
			continue;
		}
		addr = kmap_atomic(bv.bv_page);
		size = min_t(ssize_t, len, length - bio_off);
		if (to_buf)
			memcpy(buf + buf_off, addr + bio_off + bv.bv_offset,
				size);
		else
			memcpy(addr + bio_off + bv.bv_offset, buf + buf_off,
				size);
		kunmap_atomic(addr);
		bio_off = 0;
		buf_off += size;
		len -= size;
	}
}

static int dm_icomp_mod_to_max_io_range(struct dm_icomp_info *info,
			 struct dm_icomp_io_range *io)
{
	unsigned int maxlen = dm_icomp_compressor_maxlen(info, io->decomp_len);

	if (maxlen <= io->comp_len)
		return -ENOSPC;
	io->io_req.mem.ptr.addr = io->comp_data =
		dm_icomp_krealloc(io->comp_data, maxlen,
			io->comp_len, GFP_NOIO);
	if (!io->comp_data) {
		DMWARN("UNFORTUNE allocation failure ");
		io->comp_len = 0;
		return -ENOSPC;
	}
	io->comp_len = maxlen;
	return 0;
}

static struct dm_icomp_io_range *dm_icomp_create_io_write_range(
		struct dm_icomp_req *req)
{
	struct dm_icomp_io_range *io;
	sector_t size  = bio_sectors(req->bio)<<9;
	int comp_len = dm_icomp_compressor_len(req->info, size);
	void *addr;

	addr  = dm_icomp_kmalloc(size, GFP_NOIO);
	if (!addr)
		return NULL;

	io = dm_icomp_create_io_range(req, comp_len);
	if (!io) {
		dm_icomp_kfree(addr, size);
		return NULL;
	}

	io->decomp_data = addr;
	io->decomp_len = size;

	dm_icomp_bio_copy(req->bio, 0, io->decomp_data, size, true);
	return io;
}

/*
 * return value:
 * < 0 : error
 * == 0 : ok
 * == 1 : ok, but comp/decomp is skipped
 * Compressed data size is roundup of 512, which makes the payload.
 * We store the actual compressed len in the last u32 of the payload.
 * If there is no free space, we add 512 to the payload size.
 */
static int dm_icomp_io_range_compress(struct dm_icomp_info *info,
		struct dm_icomp_io_range *io, unsigned int *comp_len,
		void *decomp_data, unsigned int decomp_len)
{
	unsigned int actual_comp_len = io->comp_len;
	u32 *addr;
	struct crypto_comp *tfm =  info->tfm[get_cpu()];
	int ret;

	ret = crypto_comp_compress(tfm, decomp_data, decomp_len,
		io->comp_data, &actual_comp_len);

	if (ret || actual_comp_len > io->comp_len) {
		ret = dm_icomp_mod_to_max_io_range(info, io);
		if (!ret) {
			actual_comp_len = io->comp_len;
			ret = crypto_comp_compress(tfm, decomp_data, decomp_len,
				io->comp_data, &actual_comp_len);
		}
	}

	put_cpu();

	if (ret < 0)
		DMWARN("CO Error %d ", ret);

	atomic64_add(decomp_len, &info->uncompressed_write_size);
	if (ret || decomp_len < actual_comp_len + 2*sizeof(u32) + 512) {
		*comp_len = decomp_len;
		atomic64_add(*comp_len, &info->compressed_write_size);
		return 1;
	}

	*comp_len = round_up(actual_comp_len, 512);
	if (*comp_len - actual_comp_len < 2*sizeof(u32))
		*comp_len += 512;
	atomic64_add(*comp_len, &info->compressed_write_size);
	addr = io->comp_data + *comp_len;
	addr--;
	*addr = cpu_to_le32(actual_comp_len);
	addr--;
	*addr = cpu_to_le32(DMCP_COMPRESS_MAGIC);
	return 0;
}

/*
 * return value:
 * < 0 : error
 * == 0 : ok
 * == 1 : ok, but comp/decomp is skipped
 */
static int dm_icomp_io_range_decompress(struct dm_icomp_info *info,
	void *comp_data, unsigned int comp_len, void *decomp_data,
	unsigned int decomp_len)
{
	struct crypto_comp *tfm;
	u32 *addr;
	int ret;

	if (comp_len == decomp_len)
		return 1;

	addr = comp_data + comp_len;
	addr--;
	comp_len = le32_to_cpu(*addr);
	addr--;

	if (comp_len == decomp_len)
		return 1;
	if (le32_to_cpu(*addr) == DMCP_COMPRESS_MAGIC) {
		tfm = info->tfm[get_cpu()];
		ret = crypto_comp_decompress(tfm, comp_data, comp_len,
			decomp_data, &decomp_len);
		put_cpu();
		if (ret)
			return -EINVAL;
	} else
		memset(decomp_data, 0, decomp_len);
	return 0;
}

static void dm_icomp_handle_read_decomp(struct dm_icomp_req *req)
{
	struct dm_icomp_io_range *io;
	off_t bio_off = 0;
	int ret;

	req->stage = STAGE_READ_DECOMP;

	if (req->result)
		return;

	list_for_each_entry(io, &req->all_io, next) {
		ssize_t dst_off = 0, src_off = 0, len;

		io->io_region.sector -= req->info->data_start;

		if (dm_icomp_update_io_read_range(io)) {
			req->result = -EIO;
			return;
		}

		/* Do decomp here */
		ret = dm_icomp_io_range_decompress(req->info, io->comp_data,
			io->comp_len, io->decomp_data, io->decomp_len);
		if (ret < 0) {
			req->result = -EIO;
			return;
		}

		if (io->io_region.sector >= req->bio->bi_iter.bi_sector)
			dst_off = (io->io_region.sector -
				 req->bio->bi_iter.bi_sector) << 9;
		else
			src_off = (req->bio->bi_iter.bi_sector -
				 io->io_region.sector) << 9;

		len = min_t(ssize_t, io->decomp_len - src_off,
			(bio_sectors(req->bio) << 9) - dst_off);

		/* io range in all_io list is ordered for read IO */
		while (bio_off != dst_off) {
			ssize_t size = min_t(ssize_t, PAGE_SIZE,
					dst_off - bio_off);
			dm_icomp_bio_copy(req->bio, bio_off, empty_zero_page,
					size, false);
			bio_off += size;
		}

		if (ret == 1)
			dm_icomp_bio_copy(req->bio, dst_off,
					io->comp_data + src_off, len, false);
		else
			dm_icomp_bio_copy(req->bio, dst_off,
					io->decomp_data + src_off, len, false);
		bio_off = dst_off + len;
	}

	while (bio_off != (bio_sectors(req->bio) << 9)) {
		ssize_t size = min_t(ssize_t, PAGE_SIZE,
			(bio_sectors(req->bio) << 9) - bio_off);
		dm_icomp_bio_copy(req->bio, bio_off, empty_zero_page,
			size, false);
		bio_off += size;
	}
}

static void dm_icomp_read_one_extent(struct dm_icomp_req *req, u64 block,
	u16 logical_sectors, u16 data_sectors)
{
	struct dm_icomp_io_range *io;

	if (block+(data_sectors>>DMCP_BLOCK_SECTOR_SHIFT) >=
			req->info->data_blocks) {
		req->result = -EIO;
		return;
	}

	io = dm_icomp_create_io_read_range(req, data_sectors << 9,
		logical_sectors << 9);
	if (!io) {
		req->result = -EIO;
		return;
	}

	dm_icomp_get_req(req);
	list_add_tail(&io->next, &req->all_io);

	io->io_region.sector = (block << DMCP_BLOCK_SECTOR_SHIFT) +
				req->info->data_start;
	io->io_region.count = data_sectors;

	io->io_req.bi_rw = READ;
	dm_io(&io->io_req, 1, &io->io_region, NULL);
}

static void dm_icomp_handle_read_read_existing(struct dm_icomp_req *req)
{
	u64 block_index, first_block_index;
	u16 logical_sectors, data_sectors;

	req->stage = STAGE_READ_EXISTING;

	block_index = dm_icomp_sector_to_block(req->bio->bi_iter.bi_sector);
again:
	dm_icomp_get_extent(req->info, block_index, &first_block_index,
		&logical_sectors, &data_sectors);
	if (data_sectors > 0)
		dm_icomp_read_one_extent(req, first_block_index,
			logical_sectors, data_sectors);

	if (req->result)
		return;

	block_index = first_block_index + (logical_sectors >>
				DMCP_BLOCK_SECTOR_SHIFT);
	if (((block_index << DMCP_BLOCK_SECTOR_SHIFT) <
			 bio_end_sector(req->bio)) &&
			((block_index) < req->info->data_blocks))
		goto again;

	/* A shortcut if all data is in already */
	if (list_empty(&req->all_io))
		dm_icomp_handle_read_decomp(req);
}

static void dm_icomp_handle_read_request(struct dm_icomp_req *req)
{
	dm_icomp_get_req(req);

	if (req->stage == STAGE_INIT) {
		if (!dm_icomp_lock_req_range(req)) {
			dm_icomp_put_req(req);
			return;
		}

		dm_icomp_handle_read_read_existing(req);
	} else if (req->stage == STAGE_READ_EXISTING)
		dm_icomp_handle_read_decomp(req);

	dm_icomp_put_req(req);
}

static void dm_icomp_write_meta_done(void *context, unsigned long error)
{
	struct dm_icomp_req *req = context;

	dm_icomp_put_req(req);
}

static u64 dm_icomp_block_meta_page_index(u64 block, bool end)
{
	u64 bits = block * DMCP_META_BITS - !!end;
	/* (1 << 3) bits per byte */
	return bits >> (3 + PAGE_SHIFT);
}

static int dm_icomp_handle_write_modify(struct dm_icomp_io_range *io,
	u64 *meta_start, u64 *meta_end, bool *handle_bio)
{
	struct dm_icomp_req *req = io->req;
	sector_t start, count;
	unsigned int comp_len;
	off_t offset;
	u64 page_index;
	int ret;

	io->io_region.sector -= req->info->data_start;

	/* decompress original data */
	ret = dm_icomp_io_range_decompress(req->info, io->comp_data,
		io->comp_len, io->decomp_data, io->decomp_len);
	if (ret < 0) {
		req->result = -EINVAL;
		return -EIO;
	}

	start = io->io_region.sector;
	count = io->decomp_len >> 9;
	if (start < req->bio->bi_iter.bi_sector && start + count >
					bio_end_sector(req->bio)) {
		/* we don't split an extent */
		if (ret == 1) {
			memcpy(io->decomp_data, io->comp_data, io->decomp_len);
			dm_icomp_bio_copy(req->bio, 0,
			   io->decomp_data +
			   ((req->bio->bi_iter.bi_sector - start) << 9),
			   bio_sectors(req->bio) << 9, true);
		} else {
			dm_icomp_bio_copy(req->bio, 0,
			   io->decomp_data +
			   ((req->bio->bi_iter.bi_sector - start) << 9),
			   bio_sectors(req->bio) << 9, true);

			dm_icomp_kfree(io->comp_data, io->comp_len);
			/* New compressed len might be bigger */
			io->comp_data = dm_icomp_kmalloc(
				dm_icomp_compressor_len(
				req->info, io->decomp_len), GFP_NOIO);
			io->comp_len = io->decomp_len;
			if (!io->comp_data) {
				req->result = -ENOMEM;
				return -EIO;
			}
			io->io_req.mem.ptr.addr = io->comp_data;
		}
		/* need compress data */
		ret = 0;
		offset = 0;
		*handle_bio = false;
	} else if (start < req->bio->bi_iter.bi_sector) {
		count = req->bio->bi_iter.bi_sector - start;
		offset = 0;
	} else {
		offset = bio_end_sector(req->bio) - start;
		start = bio_end_sector(req->bio);
		count = count - offset;
	}

	/* Original data is uncompressed, we don't need writeback */
	if (ret == 1) {
		comp_len = count << 9;
		goto handle_meta;
	}

	/* assume compress less data uses less space (at least 4k lsess data) */
	comp_len = io->comp_len;
	ret = dm_icomp_io_range_compress(req->info, io, &comp_len,
		io->decomp_data + (offset << 9), count << 9);
	if (ret < 0) {
		req->result = -EIO;
		return -EIO;
	}

	dm_icomp_get_req(req);
	if (ret == 1)
		io->io_req.mem.ptr.addr = io->decomp_data + (offset << 9);
	io->io_region.count = comp_len >> 9;
	io->io_region.sector = start + req->info->data_start;

	io->io_req.bi_rw = req->bio->bi_rw;
	dm_io(&io->io_req, 1, &io->io_region, NULL);
handle_meta:
	dm_icomp_set_extent(req, start >> DMCP_BLOCK_SECTOR_SHIFT,
		count >> DMCP_BLOCK_SECTOR_SHIFT, comp_len >> 9);

	page_index = dm_icomp_block_meta_page_index(start >>
					DMCP_BLOCK_SECTOR_SHIFT, false);
	if (*meta_start > page_index)
		*meta_start = page_index;
	page_index = dm_icomp_block_meta_page_index(
			(start + count) >> DMCP_BLOCK_SECTOR_SHIFT, true);
	if (*meta_end < page_index)
		*meta_end = page_index;
	return 0;
}

static void dm_icomp_handle_write_comp(struct dm_icomp_req *req)
{
	struct dm_icomp_io_range *io;
	sector_t count;
	unsigned int comp_len;
	u64 meta_start = -1L, meta_end = 0, page_index;
	int ret;
	bool handle_bio = true;

	req->stage = STAGE_WRITE_COMP;

	if (req->result)
		return;

	list_for_each_entry(io, &req->all_io, next) {
		if (dm_icomp_handle_write_modify(io, &meta_start, &meta_end,
						&handle_bio))
			return;
	}

	if (!handle_bio)
		goto update_meta;

	count = bio_sectors(req->bio);
	io = dm_icomp_create_io_write_range(req);
	if (!io) {
		req->result = -EIO;
		return;
	}
	dm_icomp_bio_copy(req->bio, 0, io->decomp_data, count << 9, true);

	/* compress data */
	comp_len = io->comp_len;
	ret = dm_icomp_io_range_compress(req->info, io, &comp_len,
			io->decomp_data, count << 9);
	if (ret < 0) {
		dm_icomp_free_io_range(io);
		req->result = -EIO;
		return;
	}

	dm_icomp_get_req(req);
	list_add_tail(&io->next, &req->all_io);
	io->io_region.sector = req->bio->bi_iter.bi_sector +
			 req->info->data_start;

	if (ret == 1)
		io->io_req.mem.ptr.addr = io->decomp_data;

	io->io_region.count = comp_len >> 9;
	io->io_req.bi_rw = req->bio->bi_rw;
	dm_io(&io->io_req, 1, &io->io_region, NULL);
	dm_icomp_set_extent(req,
		req->bio->bi_iter.bi_sector >> DMCP_BLOCK_SECTOR_SHIFT,
		count >> DMCP_BLOCK_SECTOR_SHIFT, comp_len >> 9);

	page_index = dm_icomp_block_meta_page_index(
		req->bio->bi_iter.bi_sector >> DMCP_BLOCK_SECTOR_SHIFT, false);
	if (meta_start > page_index)
		meta_start = page_index;

	page_index = dm_icomp_block_meta_page_index(
	   (req->bio->bi_iter.bi_sector + count) >> DMCP_BLOCK_SECTOR_SHIFT,
	     true);

	if (meta_end < page_index)
		meta_end = page_index;
update_meta:
	if (req->info->write_mode == DMCP_WRITE_THROUGH ||
						(req->bio->bi_rw & REQ_FUA)) {
		dm_icomp_get_req(req);
		dm_icomp_write_meta(req->info, meta_start, meta_end + 1, req,
			dm_icomp_write_meta_done, req->bio->bi_rw);
	}
}

static void dm_icomp_handle_write_read_existing(struct dm_icomp_req *req)
{
	u64 block_index, first_block_index;
	u16 logical_sectors, data_sectors;

	req->stage = STAGE_READ_EXISTING;

	block_index = dm_icomp_sector_to_block(req->bio->bi_iter.bi_sector);
	dm_icomp_get_extent(req->info, block_index, &first_block_index,
		&logical_sectors, &data_sectors);
	if (data_sectors > 0 && (first_block_index < block_index ||
	    first_block_index + dm_icomp_sector_to_block(logical_sectors) >
	    dm_icomp_sector_to_block(bio_end_sector(req->bio))))
		dm_icomp_read_one_extent(req, first_block_index,
			logical_sectors, data_sectors);

	if (req->result)
		return;

	if (first_block_index + dm_icomp_sector_to_block(logical_sectors) >=
	    dm_icomp_sector_to_block(bio_end_sector(req->bio)))
		goto out;

	block_index = dm_icomp_sector_to_block(bio_end_sector(req->bio)) - 1;
	dm_icomp_get_extent(req->info, block_index, &first_block_index,
		&logical_sectors, &data_sectors);
	if (data_sectors > 0 &&
	    first_block_index + dm_icomp_sector_to_block(logical_sectors) >
	    block_index + 1)
		dm_icomp_read_one_extent(req, first_block_index,
			logical_sectors, data_sectors);

	if (req->result)
		return;
out:
	if (list_empty(&req->all_io))
		dm_icomp_handle_write_comp(req);
}

static void dm_icomp_handle_write_request(struct dm_icomp_req *req)
{
	dm_icomp_get_req(req);

	if (req->stage == STAGE_INIT) {
		if (!dm_icomp_lock_req_range(req)) {
			dm_icomp_put_req(req);
			return;
		}

		dm_icomp_handle_write_read_existing(req);
	} else if (req->stage == STAGE_READ_EXISTING)
		dm_icomp_handle_write_comp(req);

	dm_icomp_put_req(req);
}

/* For writeback mode */
static void dm_icomp_handle_flush_request(struct dm_icomp_req *req)
{
	struct writeback_flush_data wb;

	atomic_set(&wb.cnt, 1);
	init_completion(&wb.complete);

	dm_icomp_flush_dirty_meta(req->info, &wb);

	writeback_flush_io_done(&wb, 0);
	wait_for_completion(&wb.complete);

	req->bio->bi_error = 0;
	bio_endio(req->bio);
	kmem_cache_free(dm_icomp_req_cachep, req);
}

static void dm_icomp_handle_request(struct dm_icomp_req *req)
{
	if (req->bio->bi_rw & REQ_FLUSH)
		dm_icomp_handle_flush_request(req);
	else if (req->bio->bi_rw & REQ_WRITE)
		dm_icomp_handle_write_request(req);
	else
		dm_icomp_handle_read_request(req);
}

static void dm_icomp_do_request_work(struct work_struct *work)
{
	struct dm_icomp_io_worker *worker = container_of(work,
				struct dm_icomp_io_worker, work);
	LIST_HEAD(list);
	struct dm_icomp_req *req;
	struct blk_plug plug;
	bool repeat;

	blk_start_plug(&plug);
again:
	spin_lock_irq(&worker->lock);
	list_splice_init(&worker->pending, &list);
	spin_unlock_irq(&worker->lock);

	repeat = !list_empty(&list);
	while (!list_empty(&list)) {
		req = list_first_entry(&list, struct dm_icomp_req, sibling);
		list_del(&req->sibling);

		dm_icomp_handle_request(req);
	}
	if (repeat)
		goto again;
	blk_finish_plug(&plug);
}

static int dm_icomp_map(struct dm_target *ti, struct bio *bio)
{
	struct dm_icomp_info *info = ti->private;
	struct dm_icomp_req *req;

	if ((bio->bi_rw & REQ_FLUSH) &&
			info->write_mode == DMCP_WRITE_THROUGH) {
		bio->bi_bdev = info->dev->bdev;
		return DM_MAPIO_REMAPPED;
	}
	req = kmem_cache_alloc(dm_icomp_req_cachep, GFP_NOIO);
	if (!req)
		return -EIO;

	req->bio = bio;
	req->info = info;
	atomic_set(&req->io_pending, 0);
	INIT_LIST_HEAD(&req->all_io);
	req->result = 0;
	req->stage = STAGE_INIT;
	req->locked_locks = 0;

	req->cpu = raw_smp_processor_id();
	dm_icomp_queue_req(info, req);

	return DM_MAPIO_SUBMITTED;
}

static void dm_icomp_status(struct dm_target *ti, status_type_t type,
	  unsigned int status_flags, char *result, unsigned int maxlen)
{
	struct dm_icomp_info *info = ti->private;
	unsigned int sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%lu %lu %lu",
			atomic64_read(&info->uncompressed_write_size),
			atomic64_read(&info->compressed_write_size),
			atomic64_read(&info->meta_write_size));
		break;
	case STATUSTYPE_TABLE:
		if (info->write_mode == DMCP_WRITE_BACK)
			DMEMIT("%s %s %d", info->dev->name, "writeback",
				info->writeback_delay);
		else
			DMEMIT("%s %s", info->dev->name, "writethrough");
		break;
	}
}

static int dm_icomp_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	struct dm_icomp_info *info = ti->private;

	return fn(ti, info->dev, info->data_start,
		info->data_blocks << DMCP_BLOCK_SECTOR_SHIFT, data);
}

static void dm_icomp_io_hints(struct dm_target *ti,
			    struct queue_limits *limits)
{
	/* No blk_limits_logical_block_size */
	limits->logical_block_size = limits->physical_block_size =
		limits->io_min = DMCP_BLOCK_SIZE;
}

static struct target_type dm_icomp_target = {
	.name   = "inplacecompress",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr    = dm_icomp_ctr,
	.dtr    = dm_icomp_dtr,
	.map    = dm_icomp_map,
	.status = dm_icomp_status,
	.iterate_devices = dm_icomp_iterate_devices,
	.io_hints = dm_icomp_io_hints,
};

static int __init dm_icomp_init(void)
{
	int r;
	int arr_size = ARRAY_SIZE(compressors);

	for (r = 0; r < arr_size; r++)
		if (crypto_has_comp(compressors[r].name, 0, 0))
			break;
	if (r >= arr_size) {
		DMWARN("No crypto compressors are supported");
		return -EINVAL;
	}
	default_compressor = r;
	strlcpy(dm_icomp_algorithm, compressors[r].name,
			sizeof(dm_icomp_algorithm));
	DMWARN(" %s crypto compressor used ",
			compressors[default_compressor].name);

	r = -ENOMEM;
	dm_icomp_req_cachep = kmem_cache_create("dm_icomp_requests",
		sizeof(struct dm_icomp_req), 0, 0, NULL);
	if (!dm_icomp_req_cachep) {
		DMWARN("Can't create request cache");
		goto err;
	}

	dm_icomp_io_range_cachep = kmem_cache_create("dm_icomp_io_range",
		sizeof(struct dm_icomp_io_range), 0, 0, NULL);
	if (!dm_icomp_io_range_cachep) {
		DMWARN("Can't create io_range cache");
		goto err;
	}

	dm_icomp_meta_io_cachep = kmem_cache_create("dm_icomp_meta_io",
		sizeof(struct dm_icomp_meta_io), 0, 0, NULL);
	if (!dm_icomp_meta_io_cachep) {
		DMWARN("Can't create meta_io cache");
		goto err;
	}

	dm_icomp_wq = alloc_workqueue("dm_icomp_io",
		WQ_UNBOUND|WQ_MEM_RECLAIM|WQ_CPU_INTENSIVE, 0);
	if (!dm_icomp_wq) {
		DMWARN("Can't create io workqueue");
		goto err;
	}

	r = dm_register_target(&dm_icomp_target);
	if (r < 0) {
		DMWARN("target registration failed");
		goto err;
	}

	for_each_possible_cpu(r) {
		INIT_LIST_HEAD(&dm_icomp_io_workers[r].pending);
		spin_lock_init(&dm_icomp_io_workers[r].lock);
		INIT_WORK(&dm_icomp_io_workers[r].work,
				dm_icomp_do_request_work);
	}
	return 0;
err:
	kmem_cache_destroy(dm_icomp_req_cachep);
	kmem_cache_destroy(dm_icomp_io_range_cachep);
	kmem_cache_destroy(dm_icomp_meta_io_cachep);
	if (dm_icomp_wq)
		destroy_workqueue(dm_icomp_wq);

	return r;
}

static void __exit dm_icomp_exit(void)
{
	dm_unregister_target(&dm_icomp_target);
	kmem_cache_destroy(dm_icomp_req_cachep);
	kmem_cache_destroy(dm_icomp_io_range_cachep);
	kmem_cache_destroy(dm_icomp_meta_io_cachep);
	destroy_workqueue(dm_icomp_wq);
}

module_init(dm_icomp_init);
module_exit(dm_icomp_exit);

MODULE_AUTHOR("Shaohua Li <shli@kernel.org>");
MODULE_DESCRIPTION(DM_NAME " target with data inplace-compression");
MODULE_LICENSE("GPL");
