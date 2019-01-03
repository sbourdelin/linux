/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ROMZONE_H_
#define __ROMZONE_H_

#include <linux/types.h>
#include <linux/blkdev.h>

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif

/**
 * struct romz_info - backend romzone driver structure
 *
 * @owner:
 *	module which is responsible for this backend driver
 * @name:
 *	name of the backend driver
 * @part_path:
 *	path of a storage partition. It's ok to keep it as NULL
 *	if you passing @read and @write in romz_info. @part_path
 *	is needed by stoz_simple_read/write. If both of @part_path,
 *	@read and @write are NULL, it will temporarity hold the data
 *	in buffer allocated by 'vmalloc'.
 * @part_size:
 *	total size of a storage partition in bytes. The partition
 *	will be used to save data of pstore.
 * @dmesg_size:
 *	the size of each zones for dmesg (oops & panic).
 * @dump_oops:
 *	dump oops and panic log or only panic.
 * @read:
 *	the normal (not panic) read operation. If NULL, replaced as
 *	stoz_sample_read. See also @part_path
 * @write:
 *	the normal (not panic) write operation. If NULL, replaced as
 *	stoz_sample_write. See also @part_path
 * @panic_read:
 *	the read operation only used for panic.
 * @panic_write:
 *	the write operation only used for panic.
 */
struct romz_info {
	struct module *owner;
	const char *name;

	const char *part_path;
	unsigned long part_size;
	unsigned long dmesg_size;
	int dump_oops;
	ssize_t (*read)(char *buf, size_t bytes, loff_t pos);
	ssize_t (*write)(const char *buf, size_t bytes, loff_t pos);
	ssize_t (*panic_read)(char *buf, size_t bytes, loff_t pos);
	ssize_t (*panic_write)(const char *buf, size_t bytes, loff_t pos);
};

extern int romz_register(struct romz_info *info);
extern void romz_unregister(struct romz_info *info);

#endif
