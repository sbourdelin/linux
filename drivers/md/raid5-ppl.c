/*
 * Partial Parity Log for closing the RAID5 write hole
 * Copyright (c) 2017, Intel Corporation.
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
#include <linux/raid/md_p.h>
#include "md.h"
#include "raid5.h"
#include "raid5-cache.h"

/*
 * PPL consists of a 4KB header (struct ppl_header) and at least 128KB for
 * partial parity data. The header contains an array of entries
 * (struct ppl_header_entry) which describe the logged write requests.
 * Partial parity for the entries comes after the header, written in the same
 * sequence as the entries:
 *
 * Header
 *   entry0
 *   ...
 *   entryN
 * PP data
 *   PP for entry0
 *   ...
 *   PP for entryN
 *
 * Every entry holds a checksum of its partial parity, the header also has a
 * checksum of the header itself. Entries for full stripes writes contain no
 * partial parity, they only mark the stripes for which parity should be
 * recalculated after an unclean shutdown.
 *
 * A write request is always logged to the PPL instance stored on the parity
 * disk of the corresponding stripe. For each member disk there is one r5l_log
 * used to handle logging for this disk, independently from others. They are
 * grouped in child_logs array in struct ppl_conf, which is assigned to a
 * common parent r5l_log. This parent log serves as a proxy and is used in
 * raid5 personality code - it is assigned as _the_ log in r5conf->log.
 *
 * r5l_io_unit represents a full PPL write, meta_page contains the ppl_header.
 * PPL entries for logged stripes are added in ppl_log_stripe(). A stripe can
 * be appended to the last entry if the chunks to write are the same, otherwise
 * a new entry is added. Checksums of entries are calculated incrementally as
 * stripes containing partial parity are being added to entries.
 * ppl_submit_iounit() calculates the checksum of the header and submits a bio
 * containing the meta_page (ppl_header) and partial parity pages (sh->ppl_page)
 * for all stripes of the io_unit. When the PPL write completes, the stripes
 * associated with the io_unit are released and raid5d starts writing their data
 * and parity. When all stripes are written, the io_unit is freed and the next
 * can be submitted.
 *
 * An io_unit is used to gather stripes until it is submitted or becomes full
 * (if the maximum number of entries or size of PPL is reached). Another io_unit
 * can't be submitted until the previous has completed (PPL and stripe
 * data+parity is written). The log->running_ios list tracks all io_units of
 * a log (for a single member disk). New io_units are added to the end of the
 * list and the first io_unit is submitted, if it is not submitted already.
 * The current io_unit accepting new stripes is always the last on the list.
 */

struct ppl_conf {
	struct mddev *mddev;

	/* the log assigned to r5conf->log */
	struct r5l_log *parent_log;

	/* array of child logs, one for each raid disk */
	struct r5l_log *child_logs;
	int count;

	/* the logical block size used for data_sector in ppl_header_entry */
	int block_size;

	/* used only for recovery */
	int recovered_entries;
	int mismatch_count;
};

static struct r5l_io_unit *ppl_new_iounit(struct r5l_log *log,
					  struct stripe_head *sh)
{
	struct r5l_io_unit *io;
	struct ppl_header *pplhdr;
	struct ppl_conf *ppl_conf = log->private;
	struct r5l_log *parent_log = ppl_conf->parent_log;

	io = mempool_alloc(log->io_pool, GFP_ATOMIC);
	if (!io)
		return NULL;

	memset(io, 0, sizeof(*io));
	io->log = log;
	INIT_LIST_HEAD(&io->log_sibling);
	INIT_LIST_HEAD(&io->stripe_list);
	io->state = IO_UNIT_RUNNING;

	io->meta_page = mempool_alloc(log->meta_pool, GFP_NOIO);
	pplhdr = page_address(io->meta_page);
	clear_page(pplhdr);
	memset(pplhdr->reserved, 0xff, PPL_HDR_RESERVED);
	pplhdr->signature = cpu_to_le32(log->uuid_checksum);

	spin_lock(&parent_log->io_list_lock);
	io->seq = ++parent_log->seq;
	spin_unlock(&parent_log->io_list_lock);
	pplhdr->generation = cpu_to_le64(io->seq);

	return io;
}

static int ppl_log_stripe(struct r5l_log *log, struct stripe_head *sh)
{
	struct r5l_io_unit *io = NULL;
	struct ppl_header *pplhdr;
	struct ppl_header_entry *e = NULL;
	int i;
	sector_t data_sector = 0;
	int data_disks = 0;
	unsigned int entries_count;
	unsigned int entry_space = (log->rdev->ppl.size << 9) - PPL_HEADER_SIZE;
	struct r5conf *conf = sh->raid_conf;

	pr_debug("%s: stripe: %llu\n", __func__, (unsigned long long)sh->sector);

	if (log->current_io) {
		io = log->current_io;
		pplhdr = page_address(io->meta_page);
		entries_count = le32_to_cpu(pplhdr->entries_count);

		/* check if current io_unit is full */
		if (io->meta_offset >= entry_space ||
		    entries_count == PPL_HDR_MAX_ENTRIES) {
			pr_debug("%s: add io_unit blocked by seq: %llu\n",
				 __func__, io->seq);
			io = NULL;
		}
	}

	/* add a new unit if there is none or the current is full */
	if (!io) {
		io = ppl_new_iounit(log, sh);
		if (!io)
			return -ENOMEM;
		spin_lock_irq(&log->io_list_lock);
		list_add_tail(&io->log_sibling, &log->running_ios);
		spin_unlock_irq(&log->io_list_lock);

		log->current_io = io;
		pplhdr = page_address(io->meta_page);
		entries_count = 0;
	}

	for (i = 0; i < sh->disks; i++) {
		struct r5dev *dev = &sh->dev[i];
		if (i != sh->pd_idx && test_bit(R5_Wantwrite, &dev->flags)) {
			if (!data_disks || dev->sector < data_sector)
				data_sector = dev->sector;
			data_disks++;
		}
	}
	BUG_ON(!data_disks);

	pr_debug("%s: seq: %llu data_sector: %llu data_disks: %d\n", __func__,
		 io->seq, (unsigned long long)data_sector, data_disks);

	if (entries_count > 0) {
		struct ppl_header_entry *prev =
				&pplhdr->entries[entries_count - 1];
		u64 data_sector_prev = le64_to_cpu(prev->data_sector);
		u32 data_size_prev = le32_to_cpu(prev->data_size);
		u32 pp_size_prev = le32_to_cpu(prev->pp_size);

		/*
		 * Check if we can merge with the previous entry. Must be on
		 * the same stripe and disks. Use bit shift and logarithm
		 * to avoid 64-bit division.
		 */
		if ((data_sector >> ilog2(conf->chunk_sectors) ==
		     data_sector_prev >> ilog2(conf->chunk_sectors)) &&
		    ((pp_size_prev == 0 &&
		      test_bit(STRIPE_FULL_WRITE, &sh->state)) ||
		     ((data_sector_prev + (pp_size_prev >> 9) == data_sector) &&
		      (data_size_prev == pp_size_prev * data_disks))))
			e = prev;
	}

	if (!e) {
		e = &pplhdr->entries[entries_count++];
		pplhdr->entries_count = cpu_to_le32(entries_count);
		e->data_sector = cpu_to_le64(data_sector);
		e->parity_disk = cpu_to_le32(sh->pd_idx);
		e->checksum = cpu_to_le32(~0);
	}

	le32_add_cpu(&e->data_size, data_disks << PAGE_SHIFT);

	/* don't write any PP if full stripe write */
	if (!test_bit(STRIPE_FULL_WRITE, &sh->state)) {
		le32_add_cpu(&e->pp_size, PAGE_SIZE);
		io->meta_offset += PAGE_SIZE;
		e->checksum = cpu_to_le32(crc32c_le(le32_to_cpu(e->checksum),
						    page_address(sh->ppl_page),
						    PAGE_SIZE));
	}

	list_add_tail(&sh->log_list, &io->stripe_list);
	atomic_inc(&io->pending_stripe);
	sh->log_io = io;

	return 0;
}

static int ppl_write_stripe(struct r5l_log *log, struct stripe_head *sh)
{
	struct r5l_io_unit *io = sh->log_io;

	if (io || test_bit(STRIPE_SYNCING, &sh->state) ||
	    !test_bit(R5_Wantwrite, &sh->dev[sh->pd_idx].flags) ||
	    !test_bit(R5_Insync, &sh->dev[sh->pd_idx].flags)) {
		clear_bit(STRIPE_LOG_TRAPPED, &sh->state);
		return -EAGAIN;
	}

	mutex_lock(&log->io_mutex);

	if (!log->rdev || test_bit(Faulty, &log->rdev->flags)) {
		mutex_unlock(&log->io_mutex);
		return -EAGAIN;
	}

	set_bit(STRIPE_LOG_TRAPPED, &sh->state);
	clear_bit(STRIPE_DELAYED, &sh->state);
	atomic_inc(&sh->count);

	if (ppl_log_stripe(log, sh)) {
		spin_lock_irq(&log->io_list_lock);
		list_add_tail(&sh->log_list, &log->no_mem_stripes);
		spin_unlock_irq(&log->io_list_lock);
	}

	mutex_unlock(&log->io_mutex);

	return 0;
}

static void ppl_log_endio(struct bio *bio)
{
	struct r5l_io_unit *io = bio->bi_private;
	struct r5l_log *log = io->log;
	struct ppl_conf *ppl_conf = log->private;
	unsigned long flags;

	pr_debug("%s: seq: %llu\n", __func__, io->seq);

	if (bio->bi_error)
		md_error(ppl_conf->mddev, log->rdev);

	bio_put(bio);
	mempool_free(io->meta_page, log->meta_pool);

	spin_lock_irqsave(&log->io_list_lock, flags);
	__r5l_set_io_unit_state(io, IO_UNIT_IO_END);
	r5l_io_run_stripes(io);
	spin_unlock_irqrestore(&log->io_list_lock, flags);
}

static void ppl_submit_iounit(struct r5l_io_unit *io)
{
	struct r5l_log *log = io->log;
	struct ppl_conf *ppl_conf = log->private;
	struct r5conf *conf = ppl_conf->mddev->private;
	struct ppl_header *pplhdr = page_address(io->meta_page);
	struct bio *bio;
	struct stripe_head *sh;
	int i;
	struct bio_list bios = BIO_EMPTY_LIST;
	char b[BDEVNAME_SIZE];

	bio = bio_alloc_bioset(GFP_NOIO, BIO_MAX_PAGES, log->bs);
	bio->bi_private = io;

	if (!log->rdev || test_bit(Faulty, &log->rdev->flags)) {
		ppl_log_endio(bio);
		return;
	}

	bio->bi_end_io = ppl_log_endio;
	bio->bi_opf = REQ_OP_WRITE | REQ_FUA;
	bio->bi_bdev = log->rdev->bdev;
	bio->bi_iter.bi_sector = log->rdev->ppl.sector;
	bio_add_page(bio, io->meta_page, PAGE_SIZE, 0);
	bio_list_add(&bios, bio);

	sh = list_first_entry(&io->stripe_list, struct stripe_head, log_list);

	for (i = 0; i < le32_to_cpu(pplhdr->entries_count); i++) {
		struct ppl_header_entry *e = &pplhdr->entries[i];
		u32 pp_size = le32_to_cpu(e->pp_size);
		u32 data_size = le32_to_cpu(e->data_size);
		u64 data_sector = le64_to_cpu(e->data_sector);
		int stripes_count;

		if (pp_size > 0)
			stripes_count = pp_size >> PAGE_SHIFT;
		else
			stripes_count = (data_size /
					 (conf->raid_disks -
					  conf->max_degraded)) >> PAGE_SHIFT;

		while (stripes_count--) {
			/*
			 * if entry without partial parity just skip its stripes
			 * without adding pages to bio
			 */
			if (pp_size > 0 &&
			    !bio_add_page(bio, sh->ppl_page, PAGE_SIZE, 0)) {
				struct bio *prev = bio;

				bio = bio_alloc_bioset(GFP_NOIO, BIO_MAX_PAGES,
						       log->bs);
				bio->bi_opf = prev->bi_opf;
				bio->bi_bdev = prev->bi_bdev;
				bio->bi_iter.bi_sector = bio_end_sector(prev);
				bio_add_page(bio, sh->ppl_page, PAGE_SIZE, 0);
				bio_chain(bio, prev);
				bio_list_add(&bios, bio);
			}
			sh = list_next_entry(sh, log_list);
		}

		pr_debug("%s: seq: %llu entry: %d data_sector: %llu pp_size: %u data_size: %u\n",
			 __func__, io->seq, i, data_sector, pp_size, data_size);

		e->data_sector = cpu_to_le64(data_sector >>
					     ilog2(ppl_conf->block_size >> 9));
		e->checksum = cpu_to_le32(~le32_to_cpu(e->checksum));
	}

	pplhdr->checksum = cpu_to_le32(~crc32c_le(~0, pplhdr, PPL_HEADER_SIZE));

	while ((bio = bio_list_pop(&bios))) {
		pr_debug("%s: seq: %llu submit_bio() size: %u sector: %llu dev: %s\n",
			 __func__, io->seq, bio->bi_iter.bi_size,
			 (unsigned long long)bio->bi_iter.bi_sector,
			 bdevname(bio->bi_bdev, b));
		submit_bio(bio);
	}
}

static void ppl_submit_current_io(struct r5l_log *log)
{
	struct r5l_io_unit *io;

	spin_lock_irq(&log->io_list_lock);

	io = list_first_entry_or_null(&log->running_ios,
				      struct r5l_io_unit, log_sibling);

	if (io && io->state == IO_UNIT_RUNNING) {
		__r5l_set_io_unit_state(io, IO_UNIT_IO_START);
		spin_unlock_irq(&log->io_list_lock);

		if (io == log->current_io)
			log->current_io = NULL;

		ppl_submit_iounit(io);
		return;
	}

	spin_unlock_irq(&log->io_list_lock);
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

	pr_debug("%s: seq: %llu\n", __func__, io->seq);

	spin_lock_irqsave(&log->io_list_lock, flags);

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

/*
 * PPL recovery strategy: xor partial parity and data from all modified data
 * disks within a stripe and write the result as the new stripe parity. If all
 * stripe data disks are modified (full stripe write), no partial parity is
 * available, so just xor the data disks.
 *
 * Recovery of a PPL entry shall occur only if all modified data disks are
 * available and read from all of them succeeds.
 *
 * A PPL entry applies to a stripe, partial parity size for an entry is at most
 * the size of the chunk. Examples of possible cases for a single entry:
 *
 * case 0: single data disk write:
 *   data0    data1    data2     ppl        parity
 * +--------+--------+--------+           +--------------------+
 * | ------ | ------ | ------ | +----+    | (no change)        |
 * | ------ | -data- | ------ | | pp | -> | data1 ^ pp         |
 * | ------ | -data- | ------ | | pp | -> | data1 ^ pp         |
 * | ------ | ------ | ------ | +----+    | (no change)        |
 * +--------+--------+--------+           +--------------------+
 * pp_size = data_size
 *
 * case 1: more than one data disk write:
 *   data0    data1    data2     ppl        parity
 * +--------+--------+--------+           +--------------------+
 * | ------ | ------ | ------ | +----+    | (no change)        |
 * | -data- | -data- | ------ | | pp | -> | data0 ^ data1 ^ pp |
 * | -data- | -data- | ------ | | pp | -> | data0 ^ data1 ^ pp |
 * | ------ | ------ | ------ | +----+    | (no change)        |
 * +--------+--------+--------+           +--------------------+
 * pp_size = data_size / modified_data_disks
 *
 * case 2: write to all data disks (also full stripe write):
 *   data0    data1    data2                parity
 * +--------+--------+--------+           +--------------------+
 * | ------ | ------ | ------ |           | (no change)        |
 * | -data- | -data- | -data- | --------> | xor all data       |
 * | ------ | ------ | ------ | --------> | (no change)        |
 * | ------ | ------ | ------ |           | (no change)        |
 * +--------+--------+--------+           +--------------------+
 * pp_size = 0
 *
 * The following cases are possible only in other implementations. The recovery
 * code can handle them, but they are not generated at runtime because they can
 * be reduced to cases 0, 1 and 2:
 *
 * case 3:
 *   data0    data1    data2     ppl        parity
 * +--------+--------+--------+ +----+    +--------------------+
 * | ------ | -data- | -data- | | pp |    | data1 ^ data2 ^ pp |
 * | ------ | -data- | -data- | | pp | -> | data1 ^ data2 ^ pp |
 * | -data- | -data- | -data- | | -- | -> | xor all data       |
 * | -data- | -data- | ------ | | pp |    | data0 ^ data1 ^ pp |
 * +--------+--------+--------+ +----+    +--------------------+
 * pp_size = chunk_size
 *
 * case 4:
 *   data0    data1    data2     ppl        parity
 * +--------+--------+--------+ +----+    +--------------------+
 * | ------ | -data- | ------ | | pp |    | data1 ^ pp         |
 * | ------ | ------ | ------ | | -- | -> | (no change)        |
 * | ------ | ------ | ------ | | -- | -> | (no change)        |
 * | -data- | ------ | ------ | | pp |    | data0 ^ pp         |
 * +--------+--------+--------+ +----+    +--------------------+
 * pp_size = chunk_size
 */
static int ppl_recover_entry(struct r5l_log *log, struct ppl_header_entry *e,
			     sector_t ppl_sector)
{
	struct ppl_conf *ppl_conf = log->private;
	struct mddev *mddev = ppl_conf->mddev;
	struct r5conf *conf = mddev->private;
	int block_size = ppl_conf->block_size;
	struct page *pages;
	struct page *page1;
	struct page *page2;
	sector_t r_sector_first;
	sector_t r_sector_last;
	int strip_sectors;
	int data_disks;
	int i;
	int ret = 0;
	char b[BDEVNAME_SIZE];
	unsigned int pp_size = le32_to_cpu(e->pp_size);
	unsigned int data_size = le32_to_cpu(e->data_size);

	r_sector_first = le64_to_cpu(e->data_sector) * (block_size >> 9);

	if ((pp_size >> 9) < conf->chunk_sectors) {
		if (pp_size > 0) {
			data_disks = data_size / pp_size;
			strip_sectors = pp_size >> 9;
		} else {
			data_disks = conf->raid_disks - conf->max_degraded;
			strip_sectors = (data_size >> 9) / data_disks;
		}
		r_sector_last = r_sector_first +
				(data_disks - 1) * conf->chunk_sectors +
				strip_sectors;
	} else {
		data_disks = conf->raid_disks - conf->max_degraded;
		strip_sectors = conf->chunk_sectors;
		r_sector_last = r_sector_first + (data_size >> 9);
	}

	pages = alloc_pages(GFP_KERNEL, 1);
	if (!pages)
		return -ENOMEM;
	page1 = pages;
	page2 = pages + 1;

	pr_debug("%s: array sector first: %llu last: %llu\n", __func__,
		 (unsigned long long)r_sector_first,
		 (unsigned long long)r_sector_last);

	/* if start and end is 4k aligned, use a 4k block */
	if (block_size == 512 &&
	    (r_sector_first & (STRIPE_SECTORS - 1)) == 0 &&
	    (r_sector_last & (STRIPE_SECTORS - 1)) == 0)
		block_size = STRIPE_SIZE;

	/* iterate through blocks in strip */
	for (i = 0; i < strip_sectors; i += (block_size >> 9)) {
		bool update_parity = false;
		sector_t parity_sector;
		struct md_rdev *parity_rdev;
		struct stripe_head sh;
		int disk;
		int indent = 0;

		pr_debug("%s:%*s iter %d start\n", __func__, indent, "", i);
		indent += 2;

		memset(page_address(page1), 0, PAGE_SIZE);

		/* iterate through data member disks */
		for (disk = 0; disk < data_disks; disk++) {
			int dd_idx;
			struct md_rdev *rdev;
			sector_t sector;
			sector_t r_sector = r_sector_first + i +
					    (disk * conf->chunk_sectors);

			pr_debug("%s:%*s data member disk %d start\n",
				 __func__, indent, "", disk);
			indent += 2;

			if (r_sector >= r_sector_last) {
				pr_debug("%s:%*s array sector %llu doesn't need parity update\n",
					 __func__, indent, "",
					 (unsigned long long)r_sector);
				indent -= 2;
				continue;
			}

			update_parity = true;

			/* map raid sector to member disk */
			sector = raid5_compute_sector(conf, r_sector, 0,
						      &dd_idx, NULL);
			pr_debug("%s:%*s processing array sector %llu => data member disk %d, sector %llu\n",
				 __func__, indent, "",
				 (unsigned long long)r_sector, dd_idx,
				 (unsigned long long)sector);

			rdev = conf->disks[dd_idx].rdev;
			if (!rdev) {
				pr_debug("%s:%*s data member disk %d missing\n",
					 __func__, indent, "", dd_idx);
				update_parity = false;
				break;
			}

			pr_debug("%s:%*s reading data member disk %s sector %llu\n",
				 __func__, indent, "", bdevname(rdev->bdev, b),
				 (unsigned long long)sector);
			if (!sync_page_io(rdev, sector, block_size, page2,
					REQ_OP_READ, 0, false)) {
				md_error(mddev, rdev);
				pr_debug("%s:%*s read failed!\n", __func__,
					 indent, "");
				ret = -EIO;
				goto out;
			}

			ppl_xor(block_size, page1, page2, page1);

			indent -= 2;
		}

		if (!update_parity)
			continue;

		if (pp_size > 0) {
			pr_debug("%s:%*s reading pp disk sector %llu\n",
				 __func__, indent, "",
				 (unsigned long long)(ppl_sector + i));
			if (!sync_page_io(log->rdev,
					ppl_sector - log->rdev->data_offset + i,
					block_size, page2, REQ_OP_READ, 0,
					false)) {
				pr_debug("%s:%*s read failed!\n", __func__,
					 indent, "");
				md_error(mddev, log->rdev);
				ret = -EIO;
				goto out;
			}

			ppl_xor(block_size, page1, page2, page1);
		}

		/* map raid sector to parity disk */
		parity_sector = raid5_compute_sector(conf, r_sector_first + i,
				0, &disk, &sh);
		BUG_ON(sh.pd_idx != le32_to_cpu(e->parity_disk));
		parity_rdev = conf->disks[sh.pd_idx].rdev;

		BUG_ON(parity_rdev->bdev->bd_dev != log->rdev->bdev->bd_dev);
		pr_debug("%s:%*s write parity at sector %llu, disk %s\n",
			 __func__, indent, "",
			 (unsigned long long)parity_sector,
			 bdevname(parity_rdev->bdev, b));
		if (!sync_page_io(parity_rdev, parity_sector, block_size,
				page1, REQ_OP_WRITE, 0, false)) {
			pr_debug("%s:%*s parity write error!\n", __func__,
				 indent, "");
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
	struct ppl_conf *ppl_conf = log->private;
	struct md_rdev *rdev = log->rdev;
	struct mddev *mddev = rdev->mddev;
	sector_t ppl_sector = rdev->ppl.sector + (PPL_HEADER_SIZE >> 9);
	struct page *page;
	int i;
	int ret = 0;

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	/* iterate through all PPL entries saved */
	for (i = 0; i < le32_to_cpu(pplhdr->entries_count); i++) {
		struct ppl_header_entry *e = &pplhdr->entries[i];
		u32 pp_size = le32_to_cpu(e->pp_size);
		sector_t sector = ppl_sector;
		int ppl_entry_sectors = pp_size >> 9;
		u32 crc, crc_stored;

		pr_debug("%s: disk: %d entry: %d ppl_sector: %llu pp_size: %u\n",
			 __func__, rdev->raid_disk, i,
			 (unsigned long long)ppl_sector, pp_size);

		crc = ~0;
		crc_stored = le32_to_cpu(e->checksum);

		/* read parial parity for this entry and calculate its checksum */
		while (pp_size) {
			int s = pp_size > PAGE_SIZE ? PAGE_SIZE : pp_size;

			if (!sync_page_io(rdev, sector - rdev->data_offset,
					s, page, REQ_OP_READ, 0, false)) {
				md_error(mddev, rdev);
				ret = -EIO;
				goto out;
			}

			crc = crc32c_le(crc, page_address(page), s);

			pp_size -= s;
			sector += s >> 9;
		}

		crc = ~crc;

		if (crc != crc_stored) {
			/*
			 * Don't recover this entry if the checksum does not
			 * match, but keep going and try to recover other
			 * entries.
			 */
			pr_debug("%s: ppl entry crc does not match: stored: 0x%x calculated: 0x%x\n",
				 __func__, crc_stored, crc);
			ppl_conf->mismatch_count++;
		} else {
			ret = ppl_recover_entry(log, e, ppl_sector);
			if (ret)
				goto out;
			ppl_conf->recovered_entries++;
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
	struct md_rdev *rdev = log->rdev;
	int ret = 0;

	pr_debug("%s: disk: %d ppl_sector: %llu\n", __func__,
		 rdev->raid_disk, (unsigned long long)rdev->ppl.sector);

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return -ENOMEM;

	pplhdr = page_address(page);
	memset(pplhdr->reserved, 0xff, PPL_HDR_RESERVED);
	pplhdr->signature = cpu_to_le32(log->uuid_checksum);
	pplhdr->checksum = cpu_to_le32(~crc32c_le(~0, pplhdr, PAGE_SIZE));

	if (!sync_page_io(rdev, rdev->ppl.sector - rdev->data_offset,
			  PPL_HEADER_SIZE, page, REQ_OP_WRITE, 0, false)) {
		md_error(rdev->mddev, rdev);
		ret = -EIO;
	}

	__free_page(page);
	return ret;
}

static int ppl_load_distributed(struct r5l_log *log)
{
	struct ppl_conf *ppl_conf = log->private;
	struct md_rdev *rdev = log->rdev;
	struct mddev *mddev = rdev->mddev;
	struct page *page;
	struct ppl_header *pplhdr;
	u32 crc, crc_stored;
	u32 signature;
	int ret = 0;

	pr_debug("%s: disk: %d\n", __func__, rdev->raid_disk);

	/* read PPL header */
	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	if (!sync_page_io(rdev, rdev->ppl.sector - rdev->data_offset,
			  PAGE_SIZE, page, REQ_OP_READ, 0, false)) {
		md_error(mddev, rdev);
		ret = -EIO;
		goto out;
	}
	pplhdr = page_address(page);

	/* check header validity */
	crc_stored = le32_to_cpu(pplhdr->checksum);
	pplhdr->checksum = 0;
	crc = ~crc32c_le(~0, pplhdr, PAGE_SIZE);

	if (crc_stored != crc) {
		pr_debug("%s: ppl header crc does not match: stored: 0x%x calculated: 0x%x\n",
			 __func__, crc_stored, crc);
		ppl_conf->mismatch_count++;
		goto out;
	}

	signature = le32_to_cpu(pplhdr->signature);

	if (mddev->external) {
		/*
		 * For external metadata the header signature is set and
		 * validated in userspace.
		 */
		log->uuid_checksum = signature;
	} else if (log->uuid_checksum != signature) {
		pr_debug("%s: ppl header signature does not match: stored: 0x%x configured: 0x%x\n",
			 __func__, signature, log->uuid_checksum);
		ppl_conf->mismatch_count++;
		goto out;
	}

	/* attempt to recover from log if we are starting a dirty array */
	if (!mddev->pers && mddev->recovery_cp != MaxSector)
		ret = ppl_recover(log, pplhdr);
out:
	if (!ret && (le32_to_cpu(pplhdr->entries_count) > 0 ||
		     ppl_conf->mismatch_count > 0))
		ret = ppl_write_empty_header(log);

	__free_page(page);

	pr_debug("%s: return: %d mismatch_count: %d recovered_entries: %d\n",
		 __func__, ret, ppl_conf->mismatch_count,
		 ppl_conf->recovered_entries);
	return ret;
}

static int ppl_load(struct r5l_log *log)
{
	struct ppl_conf *ppl_conf = log->private;
	int ret = 0;
	int i;
	u32 *signature = NULL;

	for (i = 0; i < ppl_conf->count; i++) {
		struct r5l_log *log_child = &ppl_conf->child_logs[i];

		/* skip missing drive */
		if (!log_child->rdev)
			continue;

		ret = ppl_load_distributed(log_child);
		if (ret)
			break;

		/*
		 * For external metadata we can't check if the signature is
		 * correct on a single drive, but we can check if it is the same
		 * on all drives.
		 */
		if (ppl_conf->mddev->external) {
			if (!signature) {
				signature = &log_child->uuid_checksum;
			} else if (*signature != log_child->uuid_checksum) {
				pr_warn("md/raid:%s: PPL header signature does not match on all member drives\n",
					mdname(ppl_conf->mddev));
				ret = -EINVAL;
				break;
			}
		}
	}

	if (signature)
		log->uuid_checksum = *signature;

	pr_debug("%s: return: %d mismatch_count: %d recovered_entries: %d\n",
		 __func__, ret, ppl_conf->mismatch_count,
		 ppl_conf->recovered_entries);
	return ret;
}

static void __ppl_exit_log(struct r5l_log *log)
{
	struct ppl_conf *ppl_conf = log->private;

	if (ppl_conf->child_logs) {
		struct r5l_log *log_child;
		int i;

		for (i = 0; i < ppl_conf->count; i++) {
			log_child = &ppl_conf->child_logs[i];
			if (log_child->rdev) {
				log_child->rdev->ppl.offset = 0;
				log_child->rdev->ppl.sector = 0;
				log_child->rdev->ppl.size = 0;
			}
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

static int __ppl_init_log(struct r5l_log *log, struct r5conf *conf)
{
	struct ppl_conf *ppl_conf;
	struct mddev *mddev = conf->mddev;
	int ret = 0;
	int i;

	if (PAGE_SIZE != 4096)
		return -EINVAL;

	if (mddev->bitmap) {
		pr_warn("md/raid:%s PPL is not compatible with bitmap.\n",
			mdname(mddev));
		return -EINVAL;
	}

	ppl_conf = kzalloc(sizeof(struct ppl_conf), GFP_KERNEL);
	if (!ppl_conf)
		return -ENOMEM;
	log->private = ppl_conf;

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

	ppl_conf->parent_log = log;
	ppl_conf->mddev = mddev;
	ppl_conf->count = conf->raid_disks;
	ppl_conf->child_logs = kcalloc(ppl_conf->count, sizeof(struct r5l_log),
				       GFP_KERNEL);
	if (!ppl_conf->child_logs) {
		ret = -ENOMEM;
		goto err;
	}

	if (!mddev->external) {
		log->uuid_checksum = ~crc32c_le(~0, mddev->uuid,
						sizeof(mddev->uuid));
		ppl_conf->block_size = 512;
	} else {
		ppl_conf->block_size = queue_logical_block_size(mddev->queue);
	}

	for (i = 0; i < ppl_conf->count; i++) {
		struct r5l_log *log_child = &ppl_conf->child_logs[i];
		struct md_rdev *rdev = conf->disks[i].rdev;

		mutex_init(&log_child->io_mutex);
		spin_lock_init(&log_child->io_list_lock);
		INIT_LIST_HEAD(&log_child->running_ios);
		INIT_LIST_HEAD(&log_child->no_mem_stripes);

		log_child->rdev = rdev;
		log_child->private = log->private;
		log_child->io_kc = log->io_kc;
		log_child->io_pool = log->io_pool;
		log_child->bs = log->bs;
		log_child->meta_pool = log->meta_pool;
		log_child->uuid_checksum = log->uuid_checksum;
		log_child->policy = log->policy;

		if (rdev) {
			struct request_queue *q = bdev_get_queue(rdev->bdev);

			if (test_bit(QUEUE_FLAG_WC, &q->queue_flags))
				log->need_cache_flush = true;

			if (rdev->ppl.size < (PPL_HEADER_SIZE +
					      STRIPE_SIZE) >> 9) {
				char b[BDEVNAME_SIZE];
				pr_warn("md/raid:%s: PPL space too small on %s.\n",
					mdname(mddev), bdevname(rdev->bdev, b));
				ret = -ENOSPC;
			}
		}
	}

	if (ret)
		goto err;

	if (log->need_cache_flush)
		pr_warn("md/raid:%s: Volatile write-back cache should be disabled on all member drives when using PPL!\n",
			mdname(mddev));

	/* load and possibly recover the logs from the member disks */
	ret = ppl_load(log);

	if (ret) {
		goto err;
	} else if (!mddev->pers &&
		   mddev->recovery_cp == 0 && !mddev->degraded &&
		   ppl_conf->recovered_entries > 0 &&
		   ppl_conf->mismatch_count == 0) {
		/*
		 * If we are starting a dirty array and the recovery succeeds
		 * without any issues, set the array as clean.
		 */
		mddev->recovery_cp = MaxSector;
		set_bit(MD_SB_CHANGE_CLEAN, &mddev->sb_flags);
	} else if (mddev->pers && ppl_conf->mismatch_count > 0) {
		/* no mismatch allowed when enabling PPL for a running array */
		ret = -EINVAL;
		goto err;
	}

	conf->log = log;

	return 0;
err:
	__ppl_exit_log(log);
	return ret;
}

static int __ppl_modify_log(struct r5l_log *log, struct md_rdev *rdev,
			    enum r5l_modify_log_operation operation)
{
	struct r5l_log *log_child;
	struct ppl_conf *ppl_conf = log->private;
	int ret = 0;
	char b[BDEVNAME_SIZE];

	if (!rdev)
		return -EINVAL;

	pr_debug("%s: disk: %d operation: %s dev: %s\n",
		 __func__, rdev->raid_disk,
		 operation == R5L_MODIFY_LOG_DISK_REMOVE ? "remove" :
		 (operation == R5L_MODIFY_LOG_DISK_ADD ? "add" : "?"),
		 bdevname(rdev->bdev, b));

	if (rdev->raid_disk < 0)
		return 0;

	if (rdev->raid_disk >= ppl_conf->count)
		return -ENODEV;

	log_child = &ppl_conf->child_logs[rdev->raid_disk];

	mutex_lock(&log_child->io_mutex);
	if (operation == R5L_MODIFY_LOG_DISK_REMOVE) {
		log_child->rdev = NULL;
	} else if (operation == R5L_MODIFY_LOG_DISK_ADD) {
		log_child->rdev = rdev;
		if (rdev->mddev->external)
			log_child->uuid_checksum = log->uuid_checksum;
		ret = ppl_write_empty_header(log_child);
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&log_child->io_mutex);

	return ret;
}

static int __ppl_write_stripe(struct r5l_log *log, struct stripe_head *sh)
{
	struct ppl_conf *ppl_conf = log->private;

	return ppl_write_stripe(&ppl_conf->child_logs[sh->pd_idx], sh);
}

static void __ppl_write_stripe_run(struct r5l_log *log)
{
	struct ppl_conf *ppl_conf = log->private;
	int i;

	for (i = 0; i < ppl_conf->count; i++)
		ppl_write_stripe_run(&ppl_conf->child_logs[i]);
}

struct r5l_policy r5l_ppl = {
	.init_log = __ppl_init_log,
	.exit_log = __ppl_exit_log,
	.modify_log = __ppl_modify_log,
	.write_stripe = __ppl_write_stripe,
	.write_stripe_run = __ppl_write_stripe_run,
	.flush_stripe_to_raid = NULL,
	.stripe_write_finished = __ppl_stripe_write_finished,
	.handle_flush_request = NULL,
	.quiesce = NULL,
};
