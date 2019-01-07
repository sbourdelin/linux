// SPDX-License-Identifier: GPL-2.0
/*
 *
 * blkzone.c: Block device Oops/Panic logger
 *
 * Copyright (C) 2019 liaoweixiong <liaoweixiong@gallwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "blkzone: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/pstore.h>
#include <linux/mount.h>
#include <linux/fs.h>
#include <linux/pstore_blk.h>

/**
 * struct blkz_head - head of zone to flush to storage
 *
 * @sig: signature to indicate header (BLK_SIG xor BLKZONE-type value)
 * @datalen: length of data in @data
 * @data: zone data.
 */
struct blkz_buffer {
#define BLK_SIG (0x43474244) /* DBGC */
	uint32_t sig;
	atomic_t datalen;
	uint8_t data[0];
};

/**
 * sruct blkz_dmesg_header: dmesg information
 *
 * @magic: magic num for dmesg header
 * @time: trigger time
 * @compressed: whether conpressed
 * @count: oops/panic counter
 * @reason: identify oops or panic
 */
struct blkz_dmesg_header {
#define DMESG_HEADER_MAGIC 0x4dfc3ae5
	uint32_t magic;
	struct timespec64 time;
	bool compressed;
	uint32_t counter;
	enum kmsg_dump_reason reason;
	uint8_t data[0];
};

/**
 * struct blkz_zone - zone information
 * @off:
 *	zone offset of partition
 * @type:
 *	frontent type for this zone
 * @name:
 *	frontent name for this zone
 * @buffer:
 *	pointer to data buffer managed by this zone
 * @buffer_size:
 *	bytes in @buffer->data
 * @should_recover:
 *	should recover from storage
 * @dirty:
 *	mark whether the data in @buffer are dirty (not flush to storage yet)
 */
struct blkz_zone {
	unsigned long off;
	const char *name;
	enum pstore_type_id type;

	struct blkz_buffer *buffer;
	size_t buffer_size;
	bool should_recover;
	atomic_t dirty;
};

struct blkoops_context {
	struct blkz_zone **dbzs;	/* dmesg block zones */
	unsigned int dmesg_max_cnt;
	unsigned int dmesg_read_cnt;
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
	 * bzinfo_lock just protects "bzinfo" during calls to
	 * blkz_register/blkz_unregister
	 */
	spinlock_t bzinfo_lock;
	struct blkz_info *bzinfo;
	struct pstore_info pstore;
};
static struct blkoops_context blkz_cxt;

enum blkz_flush_mode {
	FLUSH_NONE = 0,
	FLUSH_PART,
	FLUSH_META,
	FLUSH_ALL,
};

static inline int buffer_datalen(struct blkz_zone *zone)
{
	return atomic_read(&zone->buffer->datalen);
}

static inline bool is_on_panic(void)
{
	struct blkoops_context *cxt = &blkz_cxt;

	return atomic_read(&cxt->on_panic);
}

static inline bool is_blkdev_up(void)
{
	struct blkoops_context *cxt = &blkz_cxt;
	const char *devpath = cxt->bzinfo->part_path;

	if (atomic_read(&cxt->blkdev_up))
		return true;
	if (is_on_panic())
		goto set_up;
	if (devpath && !name_to_dev_t(devpath))
		return false;
set_up:
	atomic_set(&cxt->blkdev_up, 1);
	return true;
}

static int blkz_zone_read(struct blkz_zone *zone, char *buf,
		size_t len, unsigned long off)
{
	if (!buf || !zone->buffer)
		return -EINVAL;
	len = min((size_t)len, (size_t)(zone->buffer_size - off));
	memcpy(buf, zone->buffer->data + off, len);
	return 0;
}

static int blkz_zone_write(struct blkz_zone *zone,
		enum blkz_flush_mode flush_mode, const char *buf,
		size_t len, unsigned long off)
{
	struct blkz_info *info = blkz_cxt.bzinfo;
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

	writeop = is_on_panic() ? info->panic_write : info->write;
	if (!writeop)
		return -EINVAL;

	switch (flush_mode) {
	case FLUSH_NONE:
		return 0;
	case FLUSH_PART:
		wcnt = writeop((const char *)zone->buffer->data + off, wlen,
				zone->off + sizeof(*zone->buffer) + off);
		if (wcnt != wlen)
			goto set_dirty;
	case FLUSH_META:
		wlen = sizeof(struct blkz_buffer);
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
 * blkz_move_zone: move data from a old zone to a new zone
 *
 * @old: the old zone
 * @new: the new zone
 *
 * NOTE:
 *	Call blkz_zone_write to copy and flush data. If it failed, we
 *	should reset new->dirty, because the new zone not realy dirty.
 */
static int blkz_move_zone(struct blkz_zone *old, struct blkz_zone *new)
{
	const char *data = (const char *)old->buffer->data;
	int ret;

	ret = blkz_zone_write(new, FLUSH_ALL, data, buffer_datalen(old), 0);
	if (ret) {
		atomic_set(&new->buffer->datalen, 0);
		atomic_set(&new->dirty, false);
		return ret;
	}
	atomic_set(&old->buffer->datalen, 0);
	return 0;
}

static int blkz_recover_dmesg_data(struct blkoops_context *cxt)
{
	struct blkz_info *info = cxt->bzinfo;
	struct blkz_zone *zone = NULL;
	struct blkz_buffer *buf;
	unsigned long i;
	ssize_t (*readop)(char *buf, size_t bytes, loff_t pos);
	ssize_t rcnt;

	readop = is_on_panic() ? info->panic_read : info->read;
	if (!readop)
		return -EINVAL;

	for (i = 0; i < cxt->dmesg_max_cnt; i++) {
		zone = cxt->dbzs[i];
		if (unlikely(!zone))
			return -EINVAL;
		if (atomic_read(&zone->dirty)) {
			unsigned int wcnt = cxt->dmesg_write_cnt;
			struct blkz_zone *new = cxt->dbzs[wcnt];
			int ret;

			ret = blkz_move_zone(zone, new);
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
			return (int)rcnt < 0 ? (int)rcnt : -EIO;
	}
	return 0;
}

/*
 * blkz_recover_dmesg_meta: recover meta datas of dmesg
 *
 * Recover datas as follow:
 * @cxt->dmesg_write_cnt
 * @cxt->oops_counter
 * @cxt->panic_counter
 */
static int blkz_recover_dmesg_meta(struct blkoops_context *cxt)
{
	struct blkz_info *info = cxt->bzinfo;
	struct blkz_zone *zone;
	size_t rcnt, len;
	struct blkz_buffer *buf;
	struct blkz_dmesg_header *hdr;
	ssize_t (*readop)(char *buf, size_t bytes, loff_t pos);
	struct timespec64 time = {0};
	unsigned long i;
	/**
	 * Recover may on panic, we can't allocate any memory by kmalloc.
	 * So, we use local array instead.
	 */
	char buffer_header[sizeof(*buf) + sizeof(*hdr)] = {0};

	readop = is_on_panic() ? info->panic_read : info->read;
	if (!readop)
		return -EINVAL;

	len = sizeof(*buf) + sizeof(*hdr);
	buf = (struct blkz_buffer *)buffer_header;
	for (i = 0; i < cxt->dmesg_max_cnt; i++) {
		zone = cxt->dbzs[i];
		if (unlikely(!zone))
			return -EINVAL;

		rcnt = readop((char *)buf, len, zone->off);
		if (rcnt != len)
			return (int)rcnt < 0 ? (int)rcnt : -EIO;

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

		hdr = (struct blkz_dmesg_header *)buf->data;
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
		if (hdr->time.tv_sec >= time.tv_sec) {
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

	return 0;
}

static int blkz_recover_dmesg(struct blkoops_context *cxt)
{
	int ret;

	if (!cxt->dbzs)
		return 0;

	ret = blkz_recover_dmesg_meta(cxt);
	if (ret)
		goto recover_fail;

	ret = blkz_recover_dmesg_data(cxt);
	if (ret)
		goto recover_fail;

	return 0;
recover_fail:
	pr_debug("recovery dmesg failed\n");
	return ret;
}

static inline int blkz_recovery(struct blkoops_context *cxt)
{
	int ret = -EBUSY;

	if (atomic_read(&cxt->recovery))
		return 0;

	if (!is_blkdev_up())
		goto recover_fail;

	ret = blkz_recover_dmesg(cxt);
	if (ret)
		goto recover_fail;

	atomic_set(&cxt->recovery, 1);
	pr_debug("recover end!\n");
	return 0;

recover_fail:
	pr_debug("recovery failed, handle buffer\n");
	return ret;
}

static int blkoops_pstore_open(struct pstore_info *psi)
{
	struct blkoops_context *cxt = psi->data;

	cxt->dmesg_read_cnt = 0;
	return 0;
}

static inline bool blkz_ok(struct blkz_zone *zone)
{
	if (!zone || !zone->buffer || !buffer_datalen(zone))
		return false;
	return true;
}

static int blkoops_pstore_erase(struct pstore_record *record)
{
	struct blkoops_context *cxt = record->psi->data;
	struct blkz_zone *zone = NULL;

	/*
	 * before read/erase, we must recover from storage.
	 * if recover failed, handle buffer
	 */
	blkz_recovery(cxt);

	if (record->type == PSTORE_TYPE_DMESG)
		zone = cxt->dbzs[record->id];
	if (!blkz_ok(zone))
		return 0;

	atomic_set(&zone->buffer->datalen, 0);
	return blkz_zone_write(zone, FLUSH_META, NULL, 0, 0);
}

static void blkoops_write_kmsg_hdr(struct blkz_zone *zone,
		struct pstore_record *record)
{
	struct blkoops_context *cxt = record->psi->data;
	struct blkz_buffer *buffer = zone->buffer;
	struct blkz_dmesg_header *hdr =
		(struct blkz_dmesg_header *)buffer->data;

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

static int notrace blkz_dmesg_write(struct blkoops_context *cxt,
		struct pstore_record *record)
{
	struct blkz_info *info = cxt->bzinfo;
	struct blkz_zone *zone;
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

	if (!cxt->dbzs)
		return -ENOSPC;

	zone = cxt->dbzs[cxt->dmesg_write_cnt];
	if (!zone)
		return -ENOSPC;

	blkoops_write_kmsg_hdr(zone, record);
	hlen = sizeof(struct blkz_dmesg_header);
	size = record->size;
	if (size + hlen > zone->buffer_size)
		size = zone->buffer_size - hlen;
	blkz_zone_write(zone, FLUSH_ALL, record->buf, size, hlen);

	pr_debug("write %s to zone id %d\n", zone->name, cxt->dmesg_write_cnt);
	cxt->dmesg_write_cnt = (cxt->dmesg_write_cnt + 1) % cxt->dmesg_max_cnt;
	return 0;
}

static int notrace blkoops_pstore_write(struct pstore_record *record)
{
	struct blkoops_context *cxt = record->psi->data;

	if (record->type == PSTORE_TYPE_DMESG &&
			record->reason == KMSG_DUMP_PANIC)
		atomic_set(&cxt->on_panic, 1);

	/*
	 * before read/erase, we must recover from storage.
	 * if recover failed, handle buffer
	 */
	blkz_recovery(cxt);

	switch (record->type) {
	case PSTORE_TYPE_DMESG:
		return blkz_dmesg_write(cxt, record);
	default:
		return -EINVAL;
	}
}

#define READ_NEXT_ZONE ((ssize_t)(-1024))
static struct blkz_zone *blkz_read_next_zone(struct blkoops_context *cxt)
{
	struct blkz_zone *zone = NULL;

	while (cxt->dmesg_read_cnt < cxt->dmesg_max_cnt) {
		zone = cxt->dbzs[cxt->dmesg_read_cnt++];
		if (blkz_ok(zone))
			return zone;
	}

	return NULL;
}

static int blkoops_read_dmesg_hdr(struct blkz_zone *zone,
		struct pstore_record *record)
{
	struct blkz_buffer *buffer = zone->buffer;
	struct blkz_dmesg_header *hdr =
		(struct blkz_dmesg_header *)buffer->data;

	if (hdr->magic != DMESG_HEADER_MAGIC)
		return -EINVAL;
	record->compressed = hdr->compressed;
	record->time.tv_sec = hdr->time.tv_sec;
	record->time.tv_nsec = hdr->time.tv_nsec;
	record->reason = hdr->reason;
	record->count = hdr->counter;
	return 0;
}

static ssize_t blkz_dmesg_read(struct blkz_zone *zone,
		struct pstore_record *record)
{
	size_t size, hlen = 0;

	size = buffer_datalen(zone);
	/* Clear and skip this DMESG record if it has no valid header */
	if (blkoops_read_dmesg_hdr(zone, record)) {
		atomic_set(&zone->buffer->datalen, 0);
		atomic_set(&zone->dirty, 0);
		return READ_NEXT_ZONE;
	}
	size -= sizeof(struct blkz_dmesg_header);

	if (!record->compressed) {
		char *buf = kasprintf(GFP_KERNEL,
				"blkoops: %s: Total %d times\n",
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

	if (unlikely(blkz_zone_read(zone, record->buf + hlen, size,
				sizeof(struct blkz_dmesg_header)) < 0)) {
		kfree(record->buf);
		return READ_NEXT_ZONE;
	}

	return size + hlen;
}

static ssize_t blkoops_pstore_read(struct pstore_record *record)
{
	struct blkoops_context *cxt = record->psi->data;
	ssize_t (*blkz_read)(struct blkz_zone *zone,
			struct pstore_record *record);
	struct blkz_zone *zone;
	ssize_t ret;

	/*
	 * before read/erase, we must recover from storage.
	 * if recover failed, handle buffer
	 */
	blkz_recovery(cxt);

next_zone:
	zone = blkz_read_next_zone(cxt);
	if (!zone)
		return 0;

	record->id = 0;
	record->type = zone->type;

	record->time.tv_sec = 0;
	record->time.tv_nsec = 0;
	record->compressed = false;

	switch (record->type) {
	case PSTORE_TYPE_DMESG:
		blkz_read = blkz_dmesg_read;
		record->id = cxt->dmesg_read_cnt - 1;
		break;
	default:
		goto next_zone;
	}

	ret = blkz_read(zone, record);
	if (ret == READ_NEXT_ZONE)
		goto next_zone;
	return ret;
}

static struct blkoops_context blkz_cxt = {
	.bzinfo_lock = __SPIN_LOCK_UNLOCKED(blkz_cxt.bzinfo_lock),
	.blkdev_up = ATOMIC_INIT(0),
	.recovery = ATOMIC_INIT(0),
	.on_panic = ATOMIC_INIT(0),
	.pstore = {
		.owner = THIS_MODULE,
		.name = "blkoops",
		.open = blkoops_pstore_open,
		.read = blkoops_pstore_read,
		.write = blkoops_pstore_write,
		.erase = blkoops_pstore_erase,
	},
};

static ssize_t blkz_sample_read(char *buf, size_t bytes, loff_t pos)
{
	struct blkoops_context *cxt = &blkz_cxt;
	const char *devpath = cxt->bzinfo->part_path;
	struct file *filp;
	ssize_t ret;

	if (!devpath)
		return -EINVAL;

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

static ssize_t blkz_sample_write(const char *buf, size_t bytes, loff_t pos)
{
	struct blkoops_context *cxt = &blkz_cxt;
	const char *devpath = cxt->bzinfo->part_path;
	struct file *filp;
	ssize_t ret;

	if (!devpath)
		return -EINVAL;

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

static struct blkz_zone *blkz_init_zone(enum pstore_type_id type,
		unsigned long *off, size_t size)
{
	struct blkz_info *info = blkz_cxt.bzinfo;
	struct blkz_zone *zone;
	const char *name = pstore_type_to_name(type);

	if (!size)
		return NULL;

	if (*off + size > info->part_size) {
		pr_err("no room for %s (0x%zx@0x%lx over 0x%lx)\n",
			name, size, *off, info->part_size);
		return ERR_PTR(-ENOMEM);
	}

	zone = kzalloc(sizeof(struct blkz_zone), GFP_KERNEL);
	if (!zone)
		return ERR_PTR(-ENOMEM);

	/**
	 * NOTE: allocate buffer for blk zones for two reasons:
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
	zone->buffer_size = size - sizeof(struct blkz_buffer);
	zone->buffer->sig = type ^ BLK_SIG;
	atomic_set(&zone->dirty, 0);
	atomic_set(&zone->buffer->datalen, 0);

	*off += size;

	pr_debug("blkzone %s: off 0x%lx, %zu header, %zu data\n", zone->name,
			zone->off, sizeof(*zone->buffer), zone->buffer_size);
	return zone;
}

static struct blkz_zone **blkz_init_zones(enum pstore_type_id type,
	unsigned long *off, size_t total_size, ssize_t record_size,
	unsigned int *cnt)
{
	struct blkz_info *info = blkz_cxt.bzinfo;
	struct blkz_zone **zones, *zone;
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
		zone = blkz_init_zone(type, off, record_size);
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

static void blkz_free_zone(struct blkz_zone **blkzone)
{
	struct blkz_zone *zone = *blkzone;

	if (!zone)
		return;

	kfree(zone->buffer);
	kfree(zone);
	*blkzone = NULL;
}

static void blkz_free_zones(struct blkz_zone ***blkzones, unsigned int *cnt)
{
	struct blkz_zone **zones = *blkzones;

	while (*cnt > 0) {
		blkz_free_zone(&zones[*cnt]);
		(*cnt)--;
	}
	kfree(zones);
	*blkzones = NULL;
}

static int blkz_cut_zones(struct blkoops_context *cxt)
{
	struct blkz_info *info = cxt->bzinfo;
	unsigned long off = 0;
	int err;
	size_t size;

	size = info->part_size;
	cxt->dbzs = blkz_init_zones(PSTORE_TYPE_DMESG, &off, size,
			info->dmesg_size, &cxt->dmesg_max_cnt);
	if (IS_ERR(cxt->dbzs)) {
		err = PTR_ERR(cxt->dbzs);
		goto fail_out;
	}

	return 0;
fail_out:
	return err;
}

int blkz_register(struct blkz_info *info)
{
	int err = -EINVAL;
	struct blkoops_context *cxt = &blkz_cxt;
	struct module *owner = info->owner;

	if (!info->part_size || !info->dmesg_size) {
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

#undef check_size

	if (!info->read)
		info->read = blkz_sample_read;
	if (!info->write)
		info->write = blkz_sample_write;

	if (owner && !try_module_get(owner))
		return -EINVAL;

	spin_lock(&cxt->bzinfo_lock);
	if (cxt->bzinfo) {
		pr_warn("blk '%s' already loaded: ignoring '%s'\n",
				cxt->bzinfo->name, info->name);
		spin_unlock(&cxt->bzinfo_lock);
		return -EBUSY;
	}
	cxt->bzinfo = info;
	spin_unlock(&cxt->bzinfo_lock);

	if (blkz_cut_zones(cxt)) {
		pr_err("cut zones fialed\n");
		goto fail_out;
	}

	cxt->pstore.bufsize = cxt->dbzs[0]->buffer_size -
			sizeof(struct blkz_dmesg_header);
	cxt->pstore.buf = kzalloc(cxt->pstore.bufsize, GFP_KERNEL);
	if (!cxt->pstore.buf) {
		pr_err("cannot allocate pstore crash dump buffer\n");
		err = -ENOMEM;
		goto fail_out;
	}
	cxt->pstore.data = cxt;
	cxt->pstore.flags = PSTORE_FLAGS_DMESG;

	err = pstore_register(&cxt->pstore);
	if (err) {
		pr_err("registering with pstore failed\n");
		goto free_pstore_buf;
	}

	pr_info("Registered %s as blkzone backend for %s%s\n", info->name,
			cxt->dbzs && cxt->bzinfo->dump_oops ? "Oops " : "",
			cxt->dbzs && cxt->bzinfo->panic_write ? "Panic " : "");

	module_put(owner);
	return 0;

free_pstore_buf:
	kfree(cxt->pstore.buf);
fail_out:
	spin_lock(&blkz_cxt.bzinfo_lock);
	blkz_cxt.bzinfo = NULL;
	spin_unlock(&blkz_cxt.bzinfo_lock);
	return err;
}
EXPORT_SYMBOL_GPL(blkz_register);

void blkz_unregister(struct blkz_info *info)
{
	struct blkoops_context *cxt = &blkz_cxt;

	pstore_unregister(&cxt->pstore);
	kfree(cxt->pstore.buf);
	cxt->pstore.bufsize = 0;

	spin_lock(&cxt->bzinfo_lock);
	blkz_cxt.bzinfo = NULL;
	spin_unlock(&cxt->bzinfo_lock);

	blkz_free_zones(&cxt->dbzs, &cxt->dmesg_max_cnt);

}
EXPORT_SYMBOL_GPL(blkz_unregister);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("liaoweixiong <liaoweixiong@allwinnertech.com>");
MODULE_DESCRIPTION("Block device Oops/Panic logger");
