// SPDX-License-Identifier: GPL-2.0
/*
 *
 * romzone.c: ROM Oops/Panic logger
 *
 * Copyright (C) 2019 liaoweixiong <liaoweixiong@gallwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "romzone: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/pstore.h>
#include <linux/mount.h>
#include <linux/pstore_rom.h>

/**
 * struct romz_head - head of zone to flush to storage
 *
 * @sig: signature to indicate header (ROM_SIG xor ROMZONE-type value)
 * @datalen: length of data in @data
 * @start: offset into @data where the beginning of the stored bytes begin
 * @data: zone data.
 */
struct romz_buffer {
#define ROM_SIG (0x43474244) /* DBGC */
	uint32_t sig;
	atomic_t datalen;
	atomic_t start;
	uint8_t data[0];
};

/**
 * sruct romz_dmesg_header: dmesg information
 *
 * @magic: magic num for dmesg header
 * @time: trigger time
 * @compressed: whether conpressed
 * @count: oops/panic counter
 * @reason: identify oops or panic
 */
struct romz_dmesg_header {
#define DMESG_HEADER_MAGIC 0x4dfc3ae5
	uint32_t magic;
	struct timespec64 time;
	bool compressed;
	uint32_t counter;
	enum kmsg_dump_reason reason;
	uint8_t data[0];
};

/**
 * struct romz_zone - zone information
 * @off:
 *	zone offset of partition
 * @type:
 *	frontent type for this zone
 * @name:
 *	frontent name for this zone
 * @buffer:
 *	pointer to data buffer managed by this zone
 * @oldbuf:
 *	pointer to old data buffer. It is used for zone who have a single-boot
 *	lifetime, which means that this zone gets wiped after its contents get
 *	copied out after boot.
 * @buffer_size:
 *	bytes in @buffer->data
 * @should_recover:
 *	should recover from storage
 * @dirty:
 *	mark whether the data in @buffer are dirty (not flush to storage yet)
 */
struct romz_zone {
	unsigned long off;
	const char *name;
	enum pstore_type_id type;

	struct romz_buffer *buffer;
	struct romz_buffer *oldbuf;
	size_t buffer_size;
	bool should_recover;
	atomic_t dirty;
};

struct romoops_context {
	struct romz_zone **drzs;	/* Oops dump zones */
	struct romz_zone *prz;		/* Pmsg dump zones */
	unsigned int dmesg_max_cnt;
	unsigned int dmesg_read_cnt;
	unsigned int pmsg_read_cnt;
	unsigned int dmesg_write_cnt;
	/**
	 * the counter should be recovered when do recovery
	 * It records the oops/panic times after burning rather than booting.
	 */
	unsigned int oops_counter;
	unsigned int panic_counter;
	atomic_t blkdev_up;
	atomic_t recovery;
	atomic_t on_panic;

	/*
	 * rzinfo_lock just protects "rzinfo" during calls to
	 * romz_register/romz_unregister
	 */
	spinlock_t rzinfo_lock;
	struct romz_info *rzinfo;
	struct pstore_info pstore;
};
static struct romoops_context romz_cxt;

enum romz_flush_mode {
	FLUSH_NONE = 0,
	FLUSH_PART,
	FLUSH_META,
	FLUSH_ALL,
};

static inline int buffer_datalen(struct romz_zone *zone)
{
	return atomic_read(&zone->buffer->datalen);
}

static inline int buffer_start(struct romz_zone *zone)
{
	return atomic_read(&zone->buffer->start);
}

static inline bool is_on_panic(void)
{
	struct romoops_context *cxt = &romz_cxt;

	return atomic_read(&cxt->on_panic);
}

static inline bool is_blkdev_up(void)
{
	struct romoops_context *cxt = &romz_cxt;
	const char *devpath = cxt->rzinfo->part_path;

	if (atomic_read(&cxt->blkdev_up))
		return true;
	if (devpath && name_to_dev_t(devpath))
		goto mark_blkdev_up;
	if (is_on_panic())
		goto mark_blkdev_up;

	return false;
mark_blkdev_up:
	atomic_set(&cxt->blkdev_up, 1);
	return true;
}

static int romz_zone_read(struct romz_zone *zone, char *buf,
		size_t len, unsigned long off)
{
	if (!buf || !zone->buffer)
		return -EINVAL;
	len = min((size_t)len, (size_t)(zone->buffer_size - off));
	memcpy(buf, zone->buffer->data + off, len);
	return 0;
}

static int romz_zone_write(struct romz_zone *zone,
		enum romz_flush_mode flush_mode, const char *buf,
		size_t len, unsigned long off)
{
	struct romz_info *info = romz_cxt.rzinfo;
	ssize_t wcnt;
	ssize_t (*writeop)(const char *buf, size_t bytes, loff_t pos);
	size_t wlen;

	wlen = min((size_t)len, (size_t)(zone->buffer_size - off));
	if (flush_mode != FLUSH_META && flush_mode != FLUSH_NONE) {
		if (buf && zone->buffer)
			memcpy(zone->buffer->data + off, buf, wlen);
		atomic_set(&zone->buffer->datalen, wlen + off);
	}

	if (!is_blkdev_up())
		goto set_dirty;

	if (is_on_panic())
		writeop = info->panic_write;
	else
		writeop = info->write;

	switch (flush_mode) {
	case FLUSH_NONE:
		return 0;
	case FLUSH_PART:
		wcnt = writeop((const char *)zone->buffer->data + off, wlen,
				zone->off + sizeof(*zone->buffer) + off);
		if (wcnt != wlen)
			goto set_dirty;
	case FLUSH_META:
		wlen = sizeof(struct romz_buffer);
		wcnt = writeop((const char *)zone->buffer, wlen, zone->off);
		if (wcnt != wlen)
			goto set_dirty;
		break;
	case FLUSH_ALL:
		wlen = buffer_datalen(zone) + sizeof(*zone->buffer);
		wcnt = writeop((const char *)zone->buffer, wlen, zone->off);
		if (wcnt != wlen)
			goto set_dirty;
		break;
	}

	return 0;
set_dirty:
	atomic_set(&zone->dirty, true);
	return -EBUSY;
}

/*
 * romz_move_zone: move data from a old zone to a new zone
 *
 * @old: the old zone
 * @new: the new zone
 *
 * NOTE:
 *	Call romz_zone_write to copy and flush data. If it failed, we
 *	should reset new->dirty, because the new zone not realy dirty.
 */
static int romz_move_zone(struct romz_zone *old, struct romz_zone *new)
{
	const char *data = (const char *)old->buffer->data;
	int ret;

	ret = romz_zone_write(new, FLUSH_ALL, data, buffer_datalen(old), 0);
	if (ret) {
		atomic_set(&new->buffer->datalen, 0);
		atomic_set(&new->dirty, false);
		return ret;
	}
	atomic_set(&old->buffer->datalen, 0);
	return 0;
}

static int romz_recover_dmesg_data(struct romoops_context *cxt)
{
	struct romz_info *info = cxt->rzinfo;
	struct romz_zone *zone = NULL;
	struct romz_buffer *buf;
	unsigned long i;
	ssize_t (*readop)(char *buf, size_t bytes, loff_t pos);
	ssize_t rcnt;

	readop = info->read;
	if (is_on_panic())
		readop = info->panic_read;
	if (!readop)
		return -EINVAL;

	for (i = 0; i < cxt->dmesg_max_cnt; i++) {
		zone = cxt->drzs[i];
		if (!zone)
			return -EINVAL;
		if (atomic_read(&zone->dirty)) {
			unsigned int wcnt = cxt->dmesg_write_cnt;
			struct romz_zone *new = cxt->drzs[wcnt];
			int ret;

			ret = romz_move_zone(zone, new);
			if (ret) {
				pr_err("move zone from %lu to %d failed\n",
						i, wcnt);
				return ret;
			}
			cxt->dmesg_write_cnt = (wcnt + 1) % cxt->dmesg_max_cnt;
		}
		if (!zone->should_recover)
			continue;
		buf = zone->buffer;
		rcnt = readop((char *)buf, zone->buffer_size + sizeof(*buf),
				zone->off);
		if (rcnt != zone->buffer_size + sizeof(*buf))
			return rcnt;
	}
	return 0;
}

/*
 * romz_recover_dmesg_meta: recover meta datas of dmesg
 *
 * Recover datas as follow:
 * @cxt->dmesg_write_cnt
 * @cxt->oops_counter
 * @cxt->panic_counter
 */
static int romz_recover_dmesg_meta(struct romoops_context *cxt)
{
	unsigned long i;
	struct romz_zone *zone;
	size_t rcnt, len;
	struct romz_buffer *buf;
	struct romz_dmesg_header *hdr;
	ssize_t (*readop)(char *buf, size_t bytes, loff_t pos);
	int err = -EINVAL;
	struct timespec64 time = {0};

	readop = cxt->rzinfo->read;
	if (is_on_panic())
		readop = cxt->rzinfo->panic_read;
	if (!readop)
		return -EINVAL;

	len = sizeof(*buf) + sizeof(*hdr);
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < cxt->dmesg_max_cnt; i++) {
		zone = cxt->drzs[i];
		if (!zone)
			goto free_buf;

		rcnt = readop((char *)buf, len, zone->off);
		if (rcnt != len)
			goto free_buf;

		/*
		 * If sig NOT match, it means this zone never used before,
		 * because we write one by one, and we never modify sig even
		 * when erase. So, we do not need to check next one.
		 */
		if (buf->sig != zone->buffer->sig) {
			cxt->dmesg_write_cnt = i;
			pr_debug("no valid data in dmesg zone %lu\n", i);
			break;
		}

		if (zone->buffer_size < atomic_read(&buf->datalen)) {
			pr_info("found overtop zone: %s: id %lu, off %lu, size %zu\n",
					zone->name, i, zone->off,
					zone->buffer_size);
			continue;
		}

		hdr = (struct romz_dmesg_header *)buf->data;
		if (hdr->magic != DMESG_HEADER_MAGIC) {
			pr_info("found invalid zone: %s: id %lu, off %lu, size %zu\n",
					zone->name, i, zone->off,
					zone->buffer_size);
			continue;
		}

		/*
		 * we get the newest zone, and the next one must be the oldest
		 * or unused zone, because we do write one by one like a circle.
		 */
		if (hdr->time.tv_sec > time.tv_sec) {
			time.tv_sec = hdr->time.tv_sec;
			cxt->dmesg_write_cnt = (i + 1) % cxt->dmesg_max_cnt;
		}

		if (hdr->reason == KMSG_DUMP_OOPS)
			cxt->oops_counter =
				max(cxt->oops_counter, hdr->counter);
		else
			cxt->panic_counter =
				max(cxt->panic_counter, hdr->counter);

		if (!atomic_read(&buf->datalen)) {
			pr_debug("found erased zone: %s: id %ld, off %lu, size %zu, datalen %d\n",
					zone->name, i, zone->off,
					zone->buffer_size,
					atomic_read(&buf->datalen));
			continue;
		}

		zone->should_recover = true;
		pr_debug("found nice zone: %s: id %ld, off %lu, size %zu, datalen %d\n",
				zone->name, i, zone->off,
				zone->buffer_size, atomic_read(&buf->datalen));
	}

	err = 0;
free_buf:
	kfree(buf);
	return err;
}

static int romz_recover_dmesg(struct romoops_context *cxt)
{
	int ret;

	ret = romz_recover_dmesg_meta(cxt);
	if (ret)
		goto recover_fail;

	ret = romz_recover_dmesg_data(cxt);
	if (ret)
		goto recover_fail;

	return 0;
recover_fail:
	pr_debug("recovery dmesg failed\n");
	return ret;
}

static int romz_recover_pmsg(struct romoops_context *cxt)
{
	struct romz_info *info = cxt->rzinfo;
	struct romz_buffer *oldbuf;
	struct romz_zone *zone = NULL;
	ssize_t (*readop)(char *buf, size_t bytes, loff_t pos);
	int ret = 0;
	ssize_t rcnt, len;

	zone = cxt->prz;
	if (!zone)
		return -EINVAL;

	if (zone->oldbuf)
		return 0;

	readop = info->read;
	if (is_on_panic())
		readop = info->panic_read;
	if (!readop)
		return -EINVAL;

	len = zone->buffer_size + sizeof(*oldbuf);
	oldbuf = kzalloc(len, GFP_KERNEL);
	if (!oldbuf)
		return -ENOMEM;

	rcnt = readop((char *)oldbuf, len, zone->off);
	if (rcnt != len) {
		pr_debug("recovery pmsg failed\n");
		ret = -EIO;
		goto free_oldbuf;
	}

	if (oldbuf->sig != zone->buffer->sig) {
		pr_debug("no valid data in zone %s\n", zone->name);
		goto free_oldbuf;
	}

	if (zone->buffer_size < atomic_read(&oldbuf->datalen) ||
		zone->buffer_size < atomic_read(&oldbuf->start)) {
		pr_info("found overtop zone: %s: off %lu, size %zu\n",
				zone->name, zone->off, zone->buffer_size);
		goto free_oldbuf;
	}

	if (atomic_read(&zone->dirty))
		romz_zone_write(zone, FLUSH_ALL, NULL, buffer_datalen(zone), 0);
	else
		romz_zone_write(zone, FLUSH_META, NULL, 0, 0);

	zone->oldbuf = oldbuf;
	return 0;

free_oldbuf:
	kfree(oldbuf);
	return ret;
}

static inline int romz_recovery(struct romoops_context *cxt)
{
	int ret = -EBUSY;

	if (atomic_read(&cxt->recovery))
		return 0;

	if (!is_blkdev_up())
		goto recover_fail;

	ret = romz_recover_dmesg(cxt);
	if (ret)
		goto recover_fail;

	ret = romz_recover_pmsg(cxt);
	if (ret)
		goto recover_fail;

	atomic_set(&cxt->recovery, 1);
	pr_debug("recover end!\n");
	return 0;

recover_fail:
	pr_debug("recovery failed, handle buffer\n");
	return ret;
}

static int romoops_pstore_open(struct pstore_info *psi)
{
	struct romoops_context *cxt = psi->data;

	cxt->dmesg_read_cnt = 0;
	return 0;
}

static int romoops_pstore_erase(struct pstore_record *record)
{
	struct romoops_context *cxt = record->psi->data;
	struct romz_zone *zone;

	/*
	 * before read/erase, we must recover from storage.
	 * if recover failed, handle buffer
	 */
	romz_recovery(cxt);

	if (record->type != PSTORE_TYPE_DMESG)
		return -EINVAL;

	zone = cxt->drzs[record->id];
	if (!zone)
		return -EINVAL;

	if (!buffer_datalen(zone))
		return 0;

	atomic_set(&zone->buffer->datalen, 0);
	return romz_zone_write(zone, FLUSH_META, NULL, 0, 0);
}

static void romoops_write_kmsg_hdr(struct romz_zone *zone,
		struct pstore_record *record)
{
	struct romoops_context *cxt = record->psi->data;
	struct romz_buffer *buffer = zone->buffer;
	struct romz_dmesg_header *hdr =
		(struct romz_dmesg_header *)buffer->data;

	hdr->magic = DMESG_HEADER_MAGIC;
	hdr->compressed = record->compressed;
	hdr->time.tv_sec = record->time.tv_sec;
	hdr->time.tv_nsec = record->time.tv_nsec;
	hdr->reason = record->reason;
	if (hdr->reason == KMSG_DUMP_OOPS)
		hdr->counter = ++cxt->oops_counter;
	else
		hdr->counter = ++cxt->panic_counter;
}

static int notrace romz_dmesg_write(struct romoops_context *cxt,
		struct pstore_record *record)
{
	struct romz_info *info = cxt->rzinfo;
	struct romz_zone *zone;
	size_t size, hlen;

	/*
	 * Out of the various dmesg dump types, ramoops is currently designed
	 * to only store crash logs, rather than storing general kernel logs.
	 */
	if (record->reason != KMSG_DUMP_OOPS &&
			record->reason != KMSG_DUMP_PANIC)
		return -EINVAL;

	/* Skip Oopes when configured to do so. */
	if (record->reason == KMSG_DUMP_OOPS && !info->dump_oops)
		return -EINVAL;

	/*
	 * Explicitly only take the first part of any new crash.
	 * If our buffer is larger than kmsg_bytes, this can never happen,
	 * and if our buffer is smaller than kmsg_bytes, we don't want the
	 * report split across multiple records.
	 */
	if (record->part != 1)
		return -ENOSPC;

	if (!cxt->drzs)
		return -ENOSPC;

	zone = cxt->drzs[cxt->dmesg_write_cnt];
	if (!zone)
		return -ENOSPC;

	romoops_write_kmsg_hdr(zone, record);
	hlen = sizeof(struct romz_dmesg_header);
	size = record->size;
	if (size + hlen > zone->buffer_size)
		size = zone->buffer_size - hlen;
	romz_zone_write(zone, FLUSH_ALL, record->buf, size, hlen);

	pr_debug("write %s to zone id %d\n", zone->name, cxt->dmesg_write_cnt);
	cxt->dmesg_write_cnt = (cxt->dmesg_write_cnt + 1) % cxt->dmesg_max_cnt;
	return 0;
}

static int notrace romz_pmsg_write(struct romoops_context *cxt,
		struct pstore_record *record)
{
	struct romz_zone *zone;
	size_t start, rem;
	int cnt = record->size;
	bool is_full_data = false;
	char *buf = record->buf;

	zone = cxt->prz;
	if (!zone)
		return -ENOSPC;

	if (atomic_read(&zone->buffer->datalen) >= zone->buffer_size)
		is_full_data = true;

	if (unlikely(cnt > zone->buffer_size)) {
		buf += cnt - zone->buffer_size;
		cnt = zone->buffer_size;
	}

	start = buffer_start(zone);
	rem = zone->buffer_size - start;
	if (unlikely(rem < cnt)) {
		romz_zone_write(zone, FLUSH_PART, buf, rem, start);
		buf += rem;
		cnt -= rem;
		start = 0;
		is_full_data = true;
	}

	atomic_set(&zone->buffer->start, cnt + start);
	romz_zone_write(zone, FLUSH_PART, buf, cnt, start);

	/**
	 * romz_zone_write will set datalen as start + cnt.
	 * It work if actual data length lesser than buffer size.
	 * If data length greater than buffer size, pmsg will rewrite to
	 * beginning of zone, which make buffer->datalen wrongly.
	 * So we should reset datalen as buffer size once actual data length
	 * greater than buffer size.
	 */
	if (is_full_data) {
		atomic_set(&zone->buffer->datalen, zone->buffer_size);
		romz_zone_write(zone, FLUSH_META, NULL, 0, 0);
	}
	return 0;
}

static int notrace romoops_pstore_write(struct pstore_record *record)
{
	struct romoops_context *cxt = record->psi->data;

	if (record->type == PSTORE_TYPE_DMESG &&
			record->reason == KMSG_DUMP_PANIC)
		atomic_set(&cxt->on_panic, 1);

	/*
	 * before read/erase, we must recover from storage.
	 * if recover failed, handle buffer
	 */
	romz_recovery(cxt);

	switch (record->type) {
	case PSTORE_TYPE_DMESG:
		return romz_dmesg_write(cxt, record);
	case PSTORE_TYPE_PMSG:
		return romz_pmsg_write(cxt, record);
	default:
		return -EINVAL;
	}
}

static inline bool romz_ok(struct romz_zone *zone)
{
	if (zone && zone->oldbuf && atomic_read(&zone->oldbuf->datalen))
		return true;
	if (zone && zone->buffer && buffer_datalen(zone))
		return true;
	return false;
}

#define READ_NEXT_ZONE ((ssize_t)(-1024))
static struct romz_zone *romz_read_next_zone(struct romoops_context *cxt)
{
	struct romz_zone *zone = NULL;

	while (cxt->dmesg_read_cnt < cxt->dmesg_max_cnt) {
		zone = cxt->drzs[cxt->dmesg_read_cnt++];
		if (romz_ok(zone))
			return zone;
	}

	if (cxt->pmsg_read_cnt == 0) {
		cxt->pmsg_read_cnt++;
		zone = cxt->prz;
		if (romz_ok(zone))
			return zone;
	}

	return NULL;
}

static int romoops_read_dmesg_hdr(struct romz_zone *zone,
		struct pstore_record *record)
{
	struct romz_buffer *buffer = zone->buffer;
	struct romz_dmesg_header *hdr =
		(struct romz_dmesg_header *)buffer->data;

	if (hdr->magic != DMESG_HEADER_MAGIC)
		return -EINVAL;
	record->compressed = hdr->compressed;
	record->time.tv_sec = hdr->time.tv_sec;
	record->time.tv_nsec = hdr->time.tv_nsec;
	record->reason = hdr->reason;
	record->count = hdr->counter;
	return 0;
}

static ssize_t romz_dmesg_read(struct romz_zone *zone,
		struct pstore_record *record)
{
	size_t size, hlen = 0;

	size = buffer_datalen(zone);
	/* Clear and skip this DMESG record if it has no valid header */
	if (romoops_read_dmesg_hdr(zone, record)) {
		atomic_set(&zone->buffer->datalen, 0);
		atomic_set(&zone->dirty, 0);
		return READ_NEXT_ZONE;
	}
	size -= sizeof(struct romz_dmesg_header);

	if (!record->compressed) {
		char *buf = kasprintf(GFP_KERNEL,
				"romoops: %s: Total %d times\n",
				record->reason == KMSG_DUMP_OOPS ? "Oops" :
				"Panic", record->count);
		hlen = strlen(buf);
		record->buf = krealloc(buf, hlen + size, GFP_KERNEL);
		if (!record->buf) {
			kfree(buf);
			return -ENOMEM;
		}
	} else {
		record->buf = kmalloc(size, GFP_KERNEL);
		if (!record->buf)
			return -ENOMEM;
	}

	if (unlikely(romz_zone_read(zone, record->buf + hlen, size,
				sizeof(struct romz_dmesg_header)) < 0)) {
		kfree(record->buf);
		return READ_NEXT_ZONE;
	}

	return size + hlen;
}

static ssize_t romz_pmsg_read(struct romz_zone *zone,
		struct pstore_record *record)
{
	size_t size, start;
	struct romz_buffer *buf;

	buf = (struct romz_buffer *)zone->oldbuf;
	if (!buf)
		return READ_NEXT_ZONE;

	size = atomic_read(&buf->datalen);
	start = atomic_read(&buf->start);

	record->buf = kmalloc(size, GFP_KERNEL);
	if (!record->buf)
		return -ENOMEM;

	memcpy(record->buf, buf->data + start, size - start);
	memcpy(record->buf + size - start, buf->data, start);

	return size;
}

static ssize_t romoops_pstore_read(struct pstore_record *record)
{
	struct romoops_context *cxt = record->psi->data;
	ssize_t (*romz_read)(struct romz_zone *zone,
			struct pstore_record *record);
	struct romz_zone *zone;
	ssize_t ret;

	/*
	 * before read/erase, we must recover from storage.
	 * if recover failed, handle buffer
	 */
	romz_recovery(cxt);

next_zone:
	zone = romz_read_next_zone(cxt);
	if (!zone)
		return 0;

	record->id = 0;
	record->type = zone->type;

	record->time.tv_sec = 0;
	record->time.tv_nsec = 0;
	record->compressed = false;

	switch (record->type) {
	case PSTORE_TYPE_DMESG:
		romz_read = romz_dmesg_read;
		record->id = cxt->dmesg_read_cnt - 1;
		break;
	case PSTORE_TYPE_PMSG:
		romz_read = romz_pmsg_read;
		break;
	default:
		goto next_zone;
	}

	ret = romz_read(zone, record);
	if (ret == READ_NEXT_ZONE)
		goto next_zone;
	return ret;
}

static struct romoops_context romz_cxt = {
	.rzinfo_lock =  __SPIN_LOCK_UNLOCKED(romz_cxt.rzinfo_lock),
	.blkdev_up = ATOMIC_INIT(0),
	.recovery = ATOMIC_INIT(0),
	.on_panic = ATOMIC_INIT(0),
	.pstore = {
		.owner = THIS_MODULE,
		.name = "romoops",
		.open = romoops_pstore_open,
		.read = romoops_pstore_read,
		.write = romoops_pstore_write,
		.erase = romoops_pstore_erase,
	},
};

static ssize_t romz_sample_read(char *buf, size_t bytes, loff_t pos)
{
	struct romoops_context *cxt = &romz_cxt;
	const char *devpath = cxt->rzinfo->part_path;
	struct file *filp;
	ssize_t ret;

	if (!is_blkdev_up())
		return -EBUSY;

	filp = filp_open(devpath, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_debug("open %s failed, maybe unready\n", devpath);
		return -EACCES;
	}
	ret = kernel_read(filp, buf, bytes, &pos);
	filp_close(filp, NULL);

	return ret;
}

static ssize_t romz_sample_write(const char *buf, size_t bytes, loff_t pos)
{
	struct romoops_context *cxt = &romz_cxt;
	const char *devpath = cxt->rzinfo->part_path;
	struct file *filp;
	ssize_t ret;

	if (!is_blkdev_up())
		return -EBUSY;

	filp = filp_open(devpath, O_WRONLY, 0);
	if (IS_ERR(filp)) {
		pr_debug("open %s failed, maybe unready\n", devpath);
		return -EACCES;
	}
	ret = kernel_write(filp, buf, bytes, &pos);
	vfs_fsync(filp, 0);
	filp_close(filp, NULL);

	return ret;
}

static struct romz_zone *romz_init_zone(enum pstore_type_id type,
		unsigned long *off, size_t size)
{
	struct romz_info *info = romz_cxt.rzinfo;
	struct romz_zone *zone;
	const char *name = pstore_type_to_name(type);

	if (!size)
		return NULL;

	if (*off + size > info->part_size) {
		pr_err("no room for %s (0x%zx@0x%lx over 0x%lx)\n",
			name, size, *off, info->part_size);
		return ERR_PTR(-ENOMEM);
	}

	zone = kzalloc(sizeof(struct romz_zone), GFP_KERNEL);
	if (!zone)
		return ERR_PTR(-ENOMEM);

	/**
	 * NOTE: allocate buffer for rom zones for two reasons:
	 * 1. It can temporarily hold the data before sample_read/write
	 *    are useable.
	 * 2. It makes pstore usable even if no persistent storage. Most
	 *    events of pstore except panic are suitable!!
	 */
	zone->buffer = kmalloc(size, GFP_KERNEL);
	if (!zone->buffer) {
		kfree(zone);
		return ERR_PTR(-ENOMEM);
	}
	memset(zone->buffer, 0xFF, size);
	zone->off = *off;
	zone->name = name;
	zone->type = type;
	zone->buffer_size = size - sizeof(struct romz_buffer);
	zone->buffer->sig = type ^ ROM_SIG;
	zone->oldbuf = NULL;
	atomic_set(&zone->dirty, 0);
	atomic_set(&zone->buffer->datalen, 0);
	atomic_set(&zone->buffer->start, 0);

	*off += size;

	pr_debug("romzone %s: off 0x%lx, %zu header, %zu data\n", zone->name,
			zone->off, sizeof(*zone->buffer), zone->buffer_size);
	return zone;
}

static struct romz_zone **romz_init_zones(enum pstore_type_id type,
	unsigned long *off, size_t total_size, ssize_t record_size,
	unsigned int *cnt)
{
	struct romz_info *info = romz_cxt.rzinfo;
	struct romz_zone **zones, *zone;
	const char *name = pstore_type_to_name(type);
	int c, i;

	if (!total_size || !record_size)
		return NULL;

	if (*off + total_size > info->part_size) {
		pr_err("no room for zones %s (0x%zx@0x%lx over 0x%lx)\n",
			name, total_size, *off, info->part_size);
		return ERR_PTR(-ENOMEM);
	}

	c = total_size / record_size;
	zones = kcalloc(c, sizeof(*zones), GFP_KERNEL);
	if (!zones) {
		pr_err("allocate for zones %s failed\n", name);
		return ERR_PTR(-ENOMEM);
	}
	memset(zones, 0, c * sizeof(*zones));

	for (i = 0; i < c; i++) {
		zone = romz_init_zone(type, off, record_size);
		if (!zone || IS_ERR(zone)) {
			pr_err("initialize zones %s failed\n", name);
			while (--i >= 0)
				kfree(zones[i]);
			kfree(zones);
			return (void *)zone;
		}
		zones[i] = zone;
	}

	*cnt = c;
	return zones;
}

static void romz_free_zone(struct romz_zone **romzone)
{
	struct romz_zone *zone = *romzone;

	if (!zone)
		return;

	kfree(zone->buffer);
	kfree(zone);
	*romzone = NULL;
}

static void romz_free_zones(struct romz_zone ***romzones, unsigned int *cnt)
{
	struct romz_zone **zones = *romzones;

	while (*cnt > 0) {
		romz_free_zone(&zones[*cnt]);
		(*cnt)--;
	}
	kfree(zones);
	*romzones = NULL;
}

static int romz_cut_zones(struct romoops_context *cxt)
{
	struct romz_info *info = cxt->rzinfo;
	unsigned long off = 0;
	int err;
	size_t size;

	size = info->part_size - info->pmsg_size;
	cxt->drzs = romz_init_zones(PSTORE_TYPE_DMESG, &off, size,
			info->dmesg_size, &cxt->dmesg_max_cnt);
	if (IS_ERR(cxt->drzs)) {
		err = PTR_ERR(cxt->drzs);
		goto fail_out;
	}

	size = info->pmsg_size;
	cxt->prz = romz_init_zone(PSTORE_TYPE_PMSG, &off, size);
	if (IS_ERR(cxt->prz)) {
		err = PTR_ERR(cxt->prz);
		goto free_dmesg_zones;
	}

	return 0;
free_dmesg_zones:
	romz_free_zones(&cxt->drzs, &cxt->dmesg_max_cnt);
fail_out:
	return err;
}

int romz_register(struct romz_info *info)
{
	int err = -EINVAL;
	struct romoops_context *cxt = &romz_cxt;
	struct module *owner = info->owner;

	if (!info->part_size || (!info->dmesg_size && !info->pmsg_size)) {
		pr_warn("The memory size and the dmesg size must be non-zero\n");
		return -EINVAL;
	}

	if (info->part_size < 4096) {
		pr_err("partition size must be over 4096 bytes\n");
		return -EINVAL;
	}

#define check_size(name, size) {					\
		if (info->name & (size)) {				\
			pr_err(#name " must be a multiple of %d\n",	\
					(size));			\
			return -EINVAL;					\
		}							\
	}

	check_size(part_size, 4096 - 1);
	check_size(dmesg_size, SECTOR_SIZE - 1);
	check_size(pmsg_size, SECTOR_SIZE - 1);

#undef check_size

	if (!info->read)
		info->read = romz_sample_read;
	if (!info->write)
		info->write = romz_sample_write;

	if (owner && !try_module_get(owner))
		return -EINVAL;

	spin_lock(&cxt->rzinfo_lock);
	if (cxt->rzinfo) {
		pr_warn("rom '%s' already loaded: ignoring '%s'\n",
				cxt->rzinfo->name, info->name);
		spin_unlock(&cxt->rzinfo_lock);
		return -EBUSY;
	}
	cxt->rzinfo = info;
	spin_unlock(&cxt->rzinfo_lock);

	if (romz_cut_zones(cxt)) {
		pr_err("cut zones fialed\n");
		goto fail_out;
	}

	cxt->pstore.bufsize = cxt->drzs[0]->buffer_size -
			sizeof(struct romz_dmesg_header);
	cxt->pstore.buf = kzalloc(cxt->pstore.bufsize, GFP_KERNEL);
	if (!cxt->pstore.buf) {
		pr_err("cannot allocate pstore crash dump buffer\n");
		err = -ENOMEM;
		goto fail_out;
	}
	cxt->pstore.data = cxt;
	cxt->pstore.flags = PSTORE_FLAGS_DMESG;
	if (info->pmsg_size)
		cxt->pstore.flags |= PSTORE_FLAGS_PMSG;

	err = pstore_register(&cxt->pstore);
	if (err) {
		pr_err("registering with pstore failed\n");
		goto free_pstore_buf;
	}

	pr_info("Registered %s as romzone backend for %s%s%s\n", info->name,
			cxt->drzs && cxt->rzinfo->dump_oops ? "Oops " : "",
			cxt->drzs ? "Panic " : "",
			cxt->prz ? "Pmsg" : "");

	module_put(owner);
	return 0;

free_pstore_buf:
	kfree(cxt->pstore.buf);
fail_out:
	spin_lock(&romz_cxt.rzinfo_lock);
	romz_cxt.rzinfo = NULL;
	spin_unlock(&romz_cxt.rzinfo_lock);
	return err;
}
EXPORT_SYMBOL_GPL(romz_register);

void romz_unregister(struct romz_info *info)
{
	struct romoops_context *cxt = &romz_cxt;

	pstore_unregister(&cxt->pstore);
	kfree(cxt->pstore.buf);
	cxt->pstore.bufsize = 0;

	spin_lock(&cxt->rzinfo_lock);
	romz_cxt.rzinfo = NULL;
	spin_unlock(&cxt->rzinfo_lock);

	romz_free_zones(&cxt->drzs, &cxt->dmesg_max_cnt);
	romz_free_zone(&cxt->prz);
}
EXPORT_SYMBOL_GPL(romz_unregister);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("liaoweixiong <liaoweixiong@allwinnertech.com>");
MODULE_DESCRIPTION("Block device Oops/Panic logger");
