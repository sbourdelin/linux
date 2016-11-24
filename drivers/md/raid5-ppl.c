/*
 * Partial Parity Log for closing the RAID5 write hole
 * Copyright (c) 2016, Intel Corporation.
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

#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/crc32c.h>
#include <linux/async_tx.h>
#include <linux/module.h>
#include <linux/raid/md_p.h>
#include "md.h"
#include "raid5.h"
#include "raid5-cache.h"

static bool ppl_debug;
module_param(ppl_debug, bool, 0644);
MODULE_PARM_DESC(ppl_debug, "Debug mode for md raid5 PPL");

#define dbg(format, args...)						\
do {									\
	if (ppl_debug)							\
		printk(KERN_DEBUG"[%d] %s() "format,			\
			current->pid, __func__, ##args);		\
} while (0)

struct ppl_conf {
	int count;
	struct r5l_log **child_logs;
};

struct ppl_header_entry {
	__le64 data_sector;	/* Raid sector of the new data */
	__le32 pp_size;		/* Length of partial parity */
	__le32 data_size;	/* Length of data */
	__u8 parity_disk;	/* Member disk containing parity */
	__le32 checksum;	/* Checksum of this entry */
} __packed;

#define PPL_HEADER_SIZE PAGE_SIZE
#define PPL_HDR_RESERVED 512
#define PPL_HDR_ENTRY_SPACE \
	(PPL_HEADER_SIZE - PPL_HDR_RESERVED - 3 * sizeof(u32) - sizeof(u64))
#define PPL_HDR_MAX_ENTRIES \
	(PPL_HDR_ENTRY_SPACE / sizeof(struct ppl_header_entry))
#define PPL_ENTRY_SPACE_IMSM (128 * 1024)

struct ppl_header {
	__u8 reserved[PPL_HDR_RESERVED];/* Reserved space */
	__le32 signature;		/* Signature (family number of volume) */
	__le64 generation;		/* Generation number of PP Header */
	__le32 entries_count;		/* Number of entries in entry array */
	__le32 checksum;		/* Checksum of PP Header */
	struct ppl_header_entry entries[PPL_HDR_MAX_ENTRIES];
} __packed;

static void ppl_log_endio(struct bio *bio)
{
	struct r5l_io_unit *io = bio->bi_private;
	struct r5l_log *log = io->log;
	unsigned long flags;

	dbg("io %p seq: %llu\n", io, io->seq);

	if (bio->bi_error)
		md_error(log->rdev->mddev, log->rdev);

	bio_put(bio);
	mempool_free(io->meta_page, log->meta_pool);

	spin_lock_irqsave(&log->io_list_lock, flags);
	__r5l_set_io_unit_state(io, IO_UNIT_IO_END);
	if (log->need_cache_flush) {
		list_move_tail(&io->log_sibling, &log->io_end_ios);
	} else {
		list_move_tail(&io->log_sibling, &log->finished_ios);
		r5l_io_run_stripes(io);
	}
	spin_unlock_irqrestore(&log->io_list_lock, flags);

	if (log->need_cache_flush)
		md_wakeup_thread(log->rdev->mddev->thread);
}

static struct r5l_io_unit *ppl_new_iounit(struct r5l_log *log,
					  struct stripe_head *sh)
{
	struct r5l_io_unit *io;
	struct ppl_header *pplhdr;
	struct r5conf *conf = log->rdev->mddev->private;
	struct r5l_log *parent_log = conf->log;

	io = mempool_alloc(log->io_pool, GFP_ATOMIC);
	if (!io)
		return NULL;

	memset(io, 0, sizeof(*io));
	io->log = log;
	INIT_LIST_HEAD(&io->log_sibling);
	INIT_LIST_HEAD(&io->stripe_list);
	INIT_LIST_HEAD(&io->stripe_finished_list);
	io->state = IO_UNIT_RUNNING;

	io->meta_page = mempool_alloc(log->meta_pool, GFP_NOIO);
	pplhdr = page_address(io->meta_page);
	clear_page(pplhdr);
	memset(pplhdr->reserved, 0xff, PPL_HDR_RESERVED);
	pplhdr->signature = cpu_to_le32(log->uuid_checksum);

	io->current_bio = bio_alloc_bioset(GFP_NOIO, BIO_MAX_PAGES, log->bs);
	bio_set_op_attrs(io->current_bio, REQ_OP_WRITE, 0);

	io->current_bio->bi_bdev = log->rdev->bdev;
	io->current_bio->bi_iter.bi_sector = log->rdev->ppl.sector;
	io->current_bio->bi_end_io = ppl_log_endio;
	io->current_bio->bi_private = io;
	bio_add_page(io->current_bio, io->meta_page, PAGE_SIZE, 0);

	spin_lock(&parent_log->io_list_lock);
	io->seq = parent_log->seq++;
	spin_unlock(&parent_log->io_list_lock);
	pplhdr->generation = cpu_to_le64(io->seq);

	return io;
}

static int ppl_log_stripe(struct r5l_log *log, struct stripe_head *sh)
{
	struct r5l_io_unit *io;
	struct ppl_header *pplhdr;
	struct ppl_header_entry *pplhdr_entry = NULL;
	int i;
	sector_t data_sector;
	unsigned long flags;
	int data_disks = 0;
	unsigned int entry_space = (log->rdev->ppl.size << 9) - PPL_HEADER_SIZE;

	dbg("<%llu>\n", (unsigned long long)sh->sector);

	io = log->current_io;
	if (!io) {
		io = ppl_new_iounit(log, sh);
		if (!io)
			return -ENOMEM;
		spin_lock_irqsave(&log->io_list_lock, flags);
		list_add_tail(&io->log_sibling, &log->running_ios);
		spin_unlock_irqrestore(&log->io_list_lock, flags);
	} else if (io->meta_offset >= entry_space) {
		/*
		 * this io_unit is full - set meta_offset to -1 to
		 * indicate that other units are waiting for this one
		 */
		io->meta_offset = -1;

		dbg("add blocked io_unit by %p seq: %llu\n", io, io->seq);
		io = ppl_new_iounit(log, sh);
		if (!io) {
			log->current_io->meta_offset = entry_space;
			return -ENOMEM;
		}
		/*
		 * reuse need_split_bio to mark that this io_unit is
		 * blocked by an other
		 */
		io->need_split_bio = true;

		spin_lock_irqsave(&log->io_list_lock, flags);
		list_add_tail(&io->log_sibling, &log->running_ios);
		spin_unlock_irqrestore(&log->io_list_lock, flags);
	}

	log->current_io = io;
	io->meta_offset += PAGE_SIZE;

	for (i = 0; i < sh->disks; i++) {
		struct r5dev *dev = &sh->dev[i];
		if (i != sh->pd_idx && test_bit(R5_LOCKED, &dev->flags)) {
			if (!data_disks)
				data_sector = dev->sector;
			data_disks++;
		}
	}
	BUG_ON(!data_disks);

	dbg("io: %p seq: %llu data_sector: %llu data_disks: %d\n",
	    io, io->seq, (unsigned long long)data_sector, data_disks);
	pplhdr = page_address(io->meta_page);

	if (pplhdr->entries_count > 0) {
		/* check if we can merge with the previous entry */
		struct ppl_header_entry *prev;
		prev = &pplhdr->entries[pplhdr->entries_count-1];

		if ((prev->data_sector + (prev->pp_size >> 9) == data_sector) &&
		    (prev->data_size == prev->pp_size * data_disks) &&
		    (data_sector >> ilog2(sh->raid_conf->chunk_sectors) ==
		     prev->data_sector >> ilog2(sh->raid_conf->chunk_sectors)))
			pplhdr_entry = prev;
	}

	if (pplhdr_entry) {
		pplhdr_entry->data_size += PAGE_SIZE * data_disks;
		pplhdr_entry->pp_size += PAGE_SIZE;
	} else {
		pplhdr_entry = &pplhdr->entries[pplhdr->entries_count++];
		pplhdr_entry->data_sector = data_sector;
		pplhdr_entry->data_size = PAGE_SIZE * data_disks;
		pplhdr_entry->pp_size = PAGE_SIZE;
		pplhdr_entry->parity_disk = sh->pd_idx;
	}

	BUG_ON(pplhdr->entries_count > PPL_HDR_MAX_ENTRIES);

	if (test_bit(STRIPE_FULL_WRITE, &sh->state))
		bio_add_page(io->current_bio, ZERO_PAGE(0), PAGE_SIZE, 0);
	else
		bio_add_page(io->current_bio, sh->ppl_page, PAGE_SIZE, 0);

	list_add_tail(&sh->log_list, &io->stripe_list);
	atomic_inc(&io->pending_stripe);
	sh->log_io = io;

	return 0;
}

static int ppl_write_stripe(struct r5l_log *log, struct stripe_head *sh)
{
	struct r5l_io_unit *io = sh->log_io;

	if (io || !test_bit(R5_Wantwrite, &sh->dev[sh->pd_idx].flags) ||
	    test_bit(STRIPE_SYNCING, &sh->state) || !log || !log->rdev ||
	    test_bit(Faulty, &log->rdev->flags)) {
		clear_bit(STRIPE_LOG_TRAPPED, &sh->state);
		return -EAGAIN;
	}

	set_bit(STRIPE_LOG_TRAPPED, &sh->state);
	clear_bit(STRIPE_DELAYED, &sh->state);
	atomic_inc(&sh->count);

	mutex_lock(&log->io_mutex);
	if (ppl_log_stripe(log, sh)) {
		spin_lock_irq(&log->io_list_lock);
		list_add_tail(&sh->log_list, &log->no_mem_stripes);
		spin_unlock_irq(&log->io_list_lock);
	}
	mutex_unlock(&log->io_mutex);

	return 0;
}

static void ppl_submit_iounit(struct r5l_io_unit *io)
{
	struct mddev *mddev = io->log->rdev->mddev;
	struct r5conf *conf = mddev->private;
	int chunk_pages = conf->chunk_sectors >> (PAGE_SHIFT - 9);
	int block_size = queue_logical_block_size(mddev->queue);
	struct ppl_header *pplhdr = page_address(io->meta_page);
	struct bio *bio = io->current_bio;
	int i;
	int bvi = 1;

	dbg("io %p seq: %llu\n", io, io->seq);

	for (i = 0; i < pplhdr->entries_count; i++) {
		struct ppl_header_entry *e = &pplhdr->entries[i];
		u32 crc = ~0;
		u32 pp_size;

		if (e->pp_size >> 9 == conf->chunk_sectors &&
				e->data_size == e->pp_size *
				(conf->raid_disks - conf->max_degraded)) {
			int x;

			for (x = bvi; x < bio->bi_vcnt - chunk_pages; x++)
				bio->bi_io_vec[x] = bio->bi_io_vec[x + chunk_pages];

			bio->bi_vcnt -= chunk_pages;
			bio->bi_iter.bi_size -= chunk_pages << PAGE_SHIFT;
			e->pp_size = 0;
		}

		pp_size = e->pp_size;

		while (pp_size) {
			void *addr = page_address(bio->bi_io_vec[bvi].bv_page);
			crc = crc32c_le(crc, addr, PAGE_SIZE);
			pp_size -= PAGE_SIZE;
			bvi++;
		}

		dbg("    entry: %d, data sector: %llu, PPL size: %u, data size %u\n",
		    i, e->data_sector, e->pp_size, e->data_size);

		e->data_sector = cpu_to_le64(e->data_sector >>
				ilog2(block_size >> 9));
		e->pp_size = cpu_to_le32(e->pp_size);
		e->data_size = cpu_to_le32(e->data_size);
		e->checksum = cpu_to_le32(~crc);
	}
	pplhdr->entries_count = cpu_to_le32(pplhdr->entries_count);
	pplhdr->checksum = cpu_to_le32(~crc32c_le(~0, pplhdr, PAGE_SIZE));

	dbg("submit_bio() size: %u sector: %llu dev: %s\n",
	    bio->bi_iter.bi_size, (unsigned long long)bio->bi_iter.bi_sector,
	    bio->bi_bdev->bd_disk->disk_name);
	submit_bio(bio);
}

static void ppl_submit_current_io(struct r5l_log *log)
{
	struct r5l_io_unit *io, *io_submit = NULL;
	unsigned long flags;

	spin_lock_irqsave(&log->io_list_lock, flags);
	list_for_each_entry(io, &log->running_ios, log_sibling) {
		if (io->state >= IO_UNIT_IO_START)
			break;

		if (io->state == IO_UNIT_RUNNING && !io->need_split_bio) {
			__r5l_set_io_unit_state(io, IO_UNIT_IO_START);

			if (io == log->current_io) {
				BUG_ON(io->meta_offset < 0);
				log->current_io = NULL;
			}

			io_submit = io;
			break;
		}
	}
	spin_unlock_irqrestore(&log->io_list_lock, flags);

	if (io_submit)
		ppl_submit_iounit(io_submit);
}

static void ppl_write_stripe_run(struct r5l_log *log)
{
	mutex_lock(&log->io_mutex);
	ppl_submit_current_io(log);
	mutex_unlock(&log->io_mutex);
}

static void __ppl_stripe_write_finished(struct r5l_io_unit *io)
{
	struct r5l_log *log = io->log;
	unsigned long flags;

	dbg("io %p seq: %llu\n", io, io->seq);

	spin_lock_irqsave(&log->io_list_lock, flags);

	if (io->meta_offset < 0) {
		struct r5l_io_unit *io_next = list_first_entry(&log->running_ios,
				struct r5l_io_unit, log_sibling);
		BUG_ON(!io_next->need_split_bio);
		io_next->need_split_bio = false;
	}

	list_del(&io->log_sibling);
	mempool_free(io, log->io_pool);
	r5l_run_no_mem_stripe(log);

	spin_unlock_irqrestore(&log->io_list_lock, flags);
}

static void ppl_xor(int size, struct page *page1, struct page *page2,
		    struct page *page_result)
{
	struct async_submit_ctl submit;
	struct dma_async_tx_descriptor *tx;
	struct page *xor_srcs[] = { page1, page2 };

	init_async_submit(&submit, ASYNC_TX_ACK|ASYNC_TX_XOR_DROP_DST,
			  NULL, NULL, NULL, NULL);
	tx = async_xor(page_result, xor_srcs, 0, 2, size, &submit);

	async_tx_quiesce(&tx);
}

static int ppl_recover_entry(struct r5l_log *log, struct ppl_header_entry *e,
			     sector_t ppl_sector)
{
	struct mddev *mddev = log->rdev->mddev;
	struct r5conf *conf = mddev->private;

	int block_size = queue_logical_block_size(mddev->queue);
	struct page *pages;
	struct page *page1;
	struct page *page2;
	sector_t r_sector_first = e->data_sector * (block_size >> 9);
	sector_t r_sector_last = r_sector_first + (e->data_size >> 9) - 1;
	int strip_sectors = conf->chunk_sectors;
	int i;
	int ret = 0;

	if (e->pp_size > 0 && (e->pp_size >> 9) < strip_sectors) {
		if (e->data_size > e->pp_size)
			r_sector_last = r_sector_first +
				(e->data_size / e->pp_size) * strip_sectors - 1;
		strip_sectors = e->pp_size >> 9;
	}

	pages = alloc_pages(GFP_KERNEL, 1);
	if (!pages)
		return -ENOMEM;
	page1 = pages;
	page2 = pages + 1;

	dbg("array sector first %llu, last %llu\n",
	    (unsigned long long)r_sector_first,
	    (unsigned long long)r_sector_last);

	/* if start and end is 4k aligned, use a 4k block */
	if (block_size == 512 &&
			r_sector_first % (PAGE_SIZE >> 9) == 0 &&
			(r_sector_last + 1) % (PAGE_SIZE >> 9) == 0)
		block_size = PAGE_SIZE;

	/* iterate through blocks in strip */
	for (i = 0; i < strip_sectors; i += (block_size >> 9)) {
		bool update_parity = false;
		sector_t parity_sector;
		struct md_rdev *parity_rdev;
		struct stripe_head sh;
		int disk;

		dbg("  iter %d start\n", i);
		memset(page_address(page1), 0, PAGE_SIZE);

		/* iterate through data member disks */
		for (disk = 0; disk < (conf->raid_disks - conf->max_degraded);
				disk++) {
			int dd_idx;
			struct md_rdev *rdev;
			sector_t sector;
			sector_t r_sector = r_sector_first + i +
					    (disk * conf->chunk_sectors);

			dbg("    data member disk %d start\n", disk);
			if (r_sector > r_sector_last) {
				dbg("    array sector %llu doesn't need parity update\n",
				    (unsigned long long)r_sector);
				continue;
			}

			update_parity = true;

			/* map raid sector to member disk */
			sector = raid5_compute_sector(conf, r_sector, 0, &dd_idx, NULL);
			dbg("    processing array sector %llu => data mem disk %d, sector %llu\n",
			    (unsigned long long)r_sector, dd_idx,
			    (unsigned long long)sector);

			rdev = conf->disks[dd_idx].rdev;
			if (!rdev) {
				dbg("    data member disk %d missing\n", dd_idx);
				update_parity = false;
				break;
			}

			dbg("    reading data member disk %s sector %llu\n",
			    rdev->bdev->bd_disk->disk_name,
			    (unsigned long long)sector);
			if (!sync_page_io(rdev, sector, block_size, page2,
					REQ_OP_READ, 0, false)) {
				md_error(mddev, rdev);
				dbg("    read failed!\n");
				ret = -EIO;
				goto out;
			}

			ppl_xor(block_size, page1, page2, page1);
		}

		if (!update_parity)
			continue;

		if (e->pp_size > 0) {
			dbg("  reading pp disk sector %llu\n",
			    (unsigned long long)(ppl_sector + i));
			if (!sync_page_io(log->rdev,
					ppl_sector - log->rdev->data_offset + i,
					block_size, page2, REQ_OP_READ, 0,
					false)) {
				dbg("  read failed!\n");
				md_error(mddev, log->rdev);
				ret = -EIO;
				goto out;
			}

			ppl_xor(block_size, page1, page2, page1);
		}

		/* map raid sector to parity disk */
		parity_sector = raid5_compute_sector(conf, r_sector_first + i,
				0, &disk, &sh);
		BUG_ON(sh.pd_idx != e->parity_disk);
		parity_rdev = conf->disks[sh.pd_idx].rdev;

		BUG_ON(parity_rdev->bdev->bd_dev != log->rdev->bdev->bd_dev);
		dbg("  write parity at sector %llu, parity disk %s\n",
		    (unsigned long long)parity_sector,
		    parity_rdev->bdev->bd_disk->disk_name);
		if (!sync_page_io(parity_rdev, parity_sector, block_size,
				page1, REQ_OP_WRITE, 0, false)) {
			dbg("  parity write error!\n");
			md_error(mddev, parity_rdev);
			ret = -EIO;
			goto out;
		}
	}

out:
	__free_pages(pages, 1);
	return ret;
}

static int ppl_recover(struct r5l_log *log, struct ppl_header *pplhdr)
{
	struct mddev *mddev = log->rdev->mddev;
	sector_t ppl_sector = log->rdev->ppl.sector + (PPL_HEADER_SIZE >> 9);
	struct page *page;
	int i;
	int ret = 0;

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	/* iterate through all PPL entries saved */
	for (i = 0; i < pplhdr->entries_count; i++) {
		struct ppl_header_entry *e = &pplhdr->entries[i];
		u32 size = le32_to_cpu(e->pp_size);
		sector_t sector = ppl_sector;
		int ppl_entry_sectors = size >> 9;
		u32 crc, crc_stored;

		dbg("disk: %d, entry: %d, ppl_sector: %llu ppl_size: %u\n",
		    log->rdev->raid_disk, i, (unsigned long long)ppl_sector,
		    size);

		crc = ~0;
		crc_stored = le32_to_cpu(e->checksum);

		while (size) {
			int s = size > PAGE_SIZE ? PAGE_SIZE : size;

			if (!sync_page_io(log->rdev,
					sector - log->rdev->data_offset,
					s, page, REQ_OP_READ, 0, false)) {
				md_error(mddev, log->rdev);
				ret = -EIO;
				goto out;
			}

			crc = crc32c_le(crc, page_address(page), s);

			size -= s;
			sector += s >> 9;
		}

		crc = ~crc;

		if (crc != crc_stored) {
			dbg("ppl entry crc does not match: stored: 0x%x calculated: 0x%x\n",
			    crc_stored, crc);
			ret++;
		} else {
			int ret2;
			e->data_sector = le64_to_cpu(e->data_sector);
			e->pp_size = le32_to_cpu(e->pp_size);
			e->data_size = le32_to_cpu(e->data_size);

			ret2 = ppl_recover_entry(log, e, ppl_sector);
			if (ret2) {
				ret = ret2;
				goto out;
			}
		}

		ppl_sector += ppl_entry_sectors;
	}
out:
	__free_page(page);
	return ret;
}

static int ppl_write_empty_header(struct r5l_log *log)
{
	struct page *page;
	struct ppl_header *pplhdr;
	int ret = 0;

	dbg("disk: %d ppl_sector: %llu\n",
	    log->rdev->raid_disk, (unsigned long long)log->rdev->ppl.sector);

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return -ENOMEM;

	pplhdr = page_address(page);
	memset(pplhdr->reserved, 0xff, PPL_HDR_RESERVED);
	pplhdr->signature = cpu_to_le32(log->uuid_checksum);
	pplhdr->checksum = cpu_to_le32(~crc32c_le(~0, pplhdr, PAGE_SIZE));

	if (!sync_page_io(log->rdev, log->rdev->ppl.sector -
			  log->rdev->data_offset, PPL_HEADER_SIZE, page,
			  REQ_OP_WRITE, 0, false)) {
		md_error(log->rdev->mddev, log->rdev);
		ret = -EIO;
	}

	__free_page(page);
	return ret;
}

static int ppl_load_distributed(struct r5l_log *log)
{
	struct mddev *mddev = log->rdev->mddev;
	struct page *page;
	struct ppl_header *pplhdr;
	u32 crc, crc_stored;
	int ret = 0;

	dbg("disk: %d\n", log->rdev->raid_disk);

	/* read PPL header */
	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	if (!sync_page_io(log->rdev,
			  log->rdev->ppl.sector - log->rdev->data_offset,
			  PAGE_SIZE, page, REQ_OP_READ, 0, false)) {
		md_error(mddev, log->rdev);
		ret = -EIO;
		goto out;
	}
	pplhdr = page_address(page);

	/* check header validity */
	crc_stored = le32_to_cpu(pplhdr->checksum);
	pplhdr->checksum = 0;
	crc = ~crc32c_le(~0, pplhdr, PAGE_SIZE);

	if (crc_stored != crc) {
		dbg("ppl header crc does not match: stored: 0x%x calculated: 0x%x\n",
		    crc_stored, crc);
		ret = 1;
		goto out;
	}

	pplhdr->signature = le32_to_cpu(pplhdr->signature);
	pplhdr->generation = le64_to_cpu(pplhdr->generation);
	pplhdr->entries_count = le32_to_cpu(pplhdr->entries_count);

	if (pplhdr->signature != log->uuid_checksum) {
		dbg("ppl header signature does not match: stored: 0x%x configured: 0x%x\n",
		    pplhdr->signature, log->uuid_checksum);
		ret = 1;
		goto out;
	}

	if (mddev->recovery_cp != MaxSector)
		ret = ppl_recover(log, pplhdr);
out:
	__free_page(page);

	if (ret >= 0) {
		int ret2 = ppl_write_empty_header(log);
		if (ret2)
			ret = ret2;
	}

	dbg("return: %d\n", ret);
	return ret;
}

static int ppl_load(struct r5l_log *log)
{
	struct ppl_conf *ppl_conf = log->private;
	int ret = 0;
	int i;

	for (i = 0; i < ppl_conf->count; i++) {
		struct r5l_log *log_child = ppl_conf->child_logs[i];
		int ret2;

		/* Missing drive */
		if (!log_child)
			continue;

		ret2 = ppl_load_distributed(log_child);
		if (ret2 < 0) {
			ret = ret2;
			break;
		}

		ret += ret2;
	}

	dbg("return: %d\n", ret);
	return ret;
}

#define IMSM_MPB_SIG "Intel Raid ISM Cfg Sig. "
#define IMSM_MPB_ORIG_FAMILY_NUM_OFFSET 64

static int ppl_find_signature_imsm(struct mddev *mddev, u32 *signature)
{
	struct md_rdev *rdev;
	char *buf;
	int ret = 0;
	u32 orig_family_num = 0;
	struct page *page;
	struct mddev *container;

	container = mddev_find_container(mddev);
	if (!container || strncmp(container->metadata_type, "imsm", 4)) {
		pr_err("Container metadata type is not imsm\n");
		return -EINVAL;
	}

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	buf = page_address(page);

	rdev_for_each(rdev, container) {
		u32 tmp;
		struct md_rdev *rdev2;
		bool found = false;

		/* only use rdevs that are both in container and mddev */
		rdev_for_each(rdev2, mddev)
			if (rdev2->bdev == rdev->bdev) {
				found = true;
				break;
			}

		if (!found)
			continue;

		if (!sync_page_io(rdev, 0,
				queue_logical_block_size(rdev->bdev->bd_queue),
				page, REQ_OP_READ, 0, true)) {
			ret = -EIO;
			goto out;
		}

		if (strncmp(buf, IMSM_MPB_SIG, strlen(IMSM_MPB_SIG)) != 0) {
			dbg("imsm mpb signature does not match\n");
			ret = 1;
			goto out;
		}

		tmp = le32_to_cpu(*(u32 *)(buf + IMSM_MPB_ORIG_FAMILY_NUM_OFFSET));

		if (orig_family_num && orig_family_num != tmp) {
			dbg("orig_family_num is not the same on all disks\n");
			ret = 1;
			goto out;
		}

		orig_family_num = tmp;
	}

	*signature = orig_family_num;
out:
	__free_page(page);
	return ret;
}

static void ppl_exit_log_child(struct r5l_log *log)
{
	clear_bit(JournalPpl, &log->rdev->flags);
	kfree(log);
}

static void __ppl_exit_log(struct r5l_log *log)
{
	struct ppl_conf *ppl_conf = log->private;

	if (ppl_conf->child_logs) {
		struct r5l_log *log_child;
		int i;

		for (i = 0; i < ppl_conf->count; i++) {
			log_child = ppl_conf->child_logs[i];
			if (!log_child)
				continue;

			clear_bit(MD_HAS_PPL, &log_child->rdev->mddev->flags);
			ppl_exit_log_child(log_child);
		}
		kfree(ppl_conf->child_logs);
	}
	kfree(ppl_conf);

	mempool_destroy(log->meta_pool);
	if (log->bs)
		bioset_free(log->bs);
	mempool_destroy(log->io_pool);
	kmem_cache_destroy(log->io_kc);
}

static int ppl_init_log_child(struct r5l_log *log_parent,
			      struct md_rdev *rdev, struct r5l_log **log_child)
{
	struct r5l_log *log;
	struct request_queue *q;

	log = kzalloc(sizeof(struct r5l_log), GFP_KERNEL);
	if (!log)
		return -ENOMEM;

	*log_child = log;
	log->rdev = rdev;

	mutex_init(&log->io_mutex);
	spin_lock_init(&log->io_list_lock);
	INIT_LIST_HEAD(&log->running_ios);
	INIT_LIST_HEAD(&log->io_end_ios);
	INIT_LIST_HEAD(&log->flushing_ios);
	INIT_LIST_HEAD(&log->finished_ios);
	INIT_LIST_HEAD(&log->no_mem_stripes);
	bio_init(&log->flush_bio);

	log->io_kc = log_parent->io_kc;
	log->io_pool = log_parent->io_pool;
	log->bs = log_parent->bs;
	log->meta_pool = log_parent->meta_pool;
	log->uuid_checksum = log_parent->uuid_checksum;

	if (rdev->mddev->external) {
		log->rdev->ppl.sector = log->rdev->data_offset +
					log->rdev->sectors;
		log->rdev->ppl.size = (PPL_HEADER_SIZE +
				       PPL_ENTRY_SPACE_IMSM) << 9;
	} else {
		log->rdev->ppl.sector = log->rdev->sb_start +
					log->rdev->ppl.offset;
	}
	log->policy = log_parent->policy;
	q = bdev_get_queue(log->rdev->bdev);
	log->need_cache_flush = test_bit(QUEUE_FLAG_WC, &q->queue_flags) != 0;

	set_bit(JournalPpl, &rdev->flags);

	return 0;
}

static int __ppl_init_log(struct r5l_log *log, struct r5conf *conf)
{
	struct ppl_conf *ppl_conf;
	struct mddev *mddev = conf->mddev;
	int ret;
	int i;

	if (PAGE_SIZE != 4096)
		return -EINVAL;

	ppl_conf = kzalloc(sizeof(struct ppl_conf), GFP_KERNEL);
	if (!ppl_conf)
		return -ENOMEM;
	log->private = ppl_conf;

	if (mddev->external) {
		ret = ppl_find_signature_imsm(mddev, &log->uuid_checksum);
		if (ret) {
			pr_err("Failed to read imsm signature\n");
			ret = -EINVAL;
			goto err;
		}
	} else {
		log->uuid_checksum = crc32c_le(~0, mddev->uuid,
					       sizeof(mddev->uuid));
	}

	if (mddev->bitmap) {
		pr_err("PPL is not compatible with bitmap\n");
		ret = -EINVAL;
		goto err;
	}

	spin_lock_init(&log->io_list_lock);

	log->io_kc = KMEM_CACHE(r5l_io_unit, 0);
	if (!log->io_kc) {
		ret = -EINVAL;
		goto err;
	}

	log->io_pool = mempool_create_slab_pool(conf->raid_disks, log->io_kc);
	if (!log->io_pool) {
		ret = -EINVAL;
		goto err;
	}

	log->bs = bioset_create(conf->raid_disks, 0);
	if (!log->bs) {
		ret = -EINVAL;
		goto err;
	}

	log->meta_pool = mempool_create_page_pool(conf->raid_disks, 0);
	if (!log->meta_pool) {
		ret = -EINVAL;
		goto err;
	}

	log->need_cache_flush = true;

	ppl_conf->count = conf->raid_disks;
	ppl_conf->child_logs = kzalloc(sizeof(struct r5l_log *) * ppl_conf->count,
				       GFP_KERNEL);
	if (!ppl_conf->child_logs) {
		ret = -ENOMEM;
		goto err;
	}

	for (i = 0; i < ppl_conf->count; i++) {
		struct r5l_log *log_child;
		struct md_rdev *rdev = conf->disks[i].rdev;

		if (!rdev)
			continue;

		ret = ppl_init_log_child(log, rdev, &log_child);
		if (ret)
			goto err;

		ppl_conf->child_logs[i] = log_child;
	}

	ret = ppl_load(log);
	if (!ret && mddev->recovery_cp == 0 && !mddev->degraded)
		mddev->recovery_cp = MaxSector;
	else if (ret < 0)
		goto err;

	rcu_assign_pointer(conf->log, log);
	set_bit(MD_HAS_PPL, &mddev->flags);

	return 0;
err:
	__ppl_exit_log(log);
	return ret;
}

static void ppl_log_stop(struct r5l_log *log)
{
	struct r5l_io_unit *io, *next;
	unsigned long flags;
	bool wait;

	/* wait for in flight ios to complete */
	do {
		wait = false;
		spin_lock_irqsave(&log->io_list_lock, flags);
		list_for_each_entry(io, &log->running_ios, log_sibling) {
			if (io->state == IO_UNIT_IO_START) {
				wait = true;
				break;
			}
		}
		if (!wait)
			wait = !list_empty(&log->flushing_ios);
		spin_unlock_irqrestore(&log->io_list_lock, flags);
	} while (wait);

	/* clean up iounits */
	spin_lock_irqsave(&log->io_list_lock, flags);

	list_for_each_entry_safe(io, next, &log->running_ios, log_sibling) {
		list_move_tail(&io->log_sibling, &log->finished_ios);
		bio_put(io->current_bio);
		mempool_free(io->meta_page, log->meta_pool);
	}
	list_splice_tail_init(&log->io_end_ios, &log->finished_ios);

	list_for_each_entry_safe(io, next, &log->finished_ios, log_sibling) {
		struct stripe_head *sh;
		list_for_each_entry(sh, &io->stripe_list, log_list) {
			clear_bit(STRIPE_LOG_TRAPPED, &sh->state);
			sh->log_io = NULL;
		}
		r5l_io_run_stripes(io);
		list_for_each_entry(sh, &io->stripe_finished_list, log_list) {
			sh->log_io = NULL;
		}
		list_del(&io->log_sibling);
		mempool_free(io, log->io_pool);
	}
	r5l_run_no_mem_stripe(log);

	spin_unlock_irqrestore(&log->io_list_lock, flags);
}

static int __ppl_modify_log(struct r5l_log *log, struct md_rdev *rdev, int op)
{
	struct r5l_log *log_child;
	struct ppl_conf *ppl_conf = log->private;

	if (!rdev)
		return -EINVAL;

	dbg("rdev->raid_disk: %d op: %d\n", rdev->raid_disk, op);

	if (rdev->raid_disk < 0)
		return 0;

	if (rdev->raid_disk >= ppl_conf->count)
		return -ENODEV;

	if (op == 0) {
		log_child = ppl_conf->child_logs[rdev->raid_disk];
		if (!log_child)
			return 0;
		ppl_conf->child_logs[rdev->raid_disk] = NULL;
		ppl_log_stop(log_child);
		ppl_exit_log_child(log_child);
	} else if (op == 1) {
		int ret = ppl_init_log_child(log, rdev, &log_child);
		if (ret)
			return ret;
		ret = ppl_write_empty_header(log_child);
		if (ret)
			return ret;
		ppl_conf->child_logs[rdev->raid_disk] = log_child;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int __ppl_write_stripe(struct r5l_log *log, struct stripe_head *sh)
{
	struct ppl_conf *ppl_conf = log->private;
	struct r5l_log *log_child = ppl_conf->child_logs[sh->pd_idx];

	return ppl_write_stripe(log_child, sh);
}

static void __ppl_write_stripe_run(struct r5l_log *log)
{
	struct ppl_conf *ppl_conf = log->private;
	struct r5l_log *log_child;
	int i;

	for (i = 0; i < ppl_conf->count; i++) {
		log_child = ppl_conf->child_logs[i];
		if (log_child)
			ppl_write_stripe_run(log_child);
	}
}

static void __ppl_flush_stripe_to_raid(struct r5l_log *log)
{
	struct ppl_conf *ppl_conf = log->private;
	struct r5l_log *log_child;
	int i;

	for (i = 0; i < ppl_conf->count; i++) {
		log_child = ppl_conf->child_logs[i];
		if (log_child)
			__r5l_flush_stripe_to_raid(log_child);
	}
}

struct r5l_policy r5l_ppl = {
	.init_log = __ppl_init_log,
	.exit_log = __ppl_exit_log,
	.modify_log = __ppl_modify_log,
	.write_stripe = __ppl_write_stripe,
	.write_stripe_run = __ppl_write_stripe_run,
	.flush_stripe_to_raid = __ppl_flush_stripe_to_raid,
	.stripe_write_finished = __ppl_stripe_write_finished,
	.handle_flush_request = NULL,
	.quiesce = NULL,
};
