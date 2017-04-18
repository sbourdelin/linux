/*
 * do_mounts_dm.c
 * Copyright (C) 2010 The Chromium OS Authors <chromium-os-dev@chromium.org>
 * Based on do_mounts_md.c
 *
 * This file is released under the GPLv2.
 */
#include <linux/async.h>
#include <linux/ctype.h>
#include <linux/device-mapper.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/delay.h>

#include "do_mounts.h"

#define DM_MAX_DEVICES 256
#define DM_MAX_TARGETS 256
#define DM_MAX_NAME 32
#define DM_MAX_UUID 129
#define DM_NO_UUID "none"

#define DM_MSG_PREFIX "init"
#define DMERR_PARSE(fmt, args...) \
	DMERR("failed to parse " fmt " for device %s<%lu>", args)

/* Separators used for parsing the dm= argument. */
#define DM_FIELD_SEP " "
#define DM_LINE_SEP ","
#define DM_ANY_SEP DM_FIELD_SEP DM_LINE_SEP

/* See Documentation/device-mapper/boot.txt for dm="..." format details. */

struct dm_setup_table {
	sector_t begin;
	sector_t length;
	char *type;
	char *params;
	/* simple singly linked list */
	struct dm_setup_table *next;
};

struct dm_device {
	int minor;
	int ro;
	char name[DM_MAX_NAME];
	char uuid[DM_MAX_UUID];
	unsigned long num_tables;
	struct dm_setup_table *table;
	int table_count;
	struct dm_device *next;
};

struct dm_option {
	char *start;
	char *next;
	size_t len;
	char delim;
};

static struct {
	unsigned long num_devices;
	char *str;
} dm_setup_args __initdata;

static int dm_early_setup __initdata;

static int __init get_dm_option(struct dm_option *opt, const char *accept)
{
	char *str = opt->next;
	char *endp;

	if (!str)
		return 0;

	str = skip_spaces(str);
	opt->start = str;
	endp = strpbrk(str, accept);
	if (!endp) {  /* act like strchrnul */
		opt->len = strlen(str);
		endp = str + opt->len;
	} else {
		opt->len = endp - str;
	}
	opt->delim = *endp;
	if (*endp == 0) {
		/* Don't advance past the nul. */
		opt->next = endp;
	} else {
		opt->next = endp + 1;
	}
	return opt->len != 0;
}

static int __init get_dm_option_u64(struct dm_option *opt, const char *sep,
				    unsigned long long *result)
{
	char buf[32];

	if (!get_dm_option(opt, sep))
		return -EINVAL;

	strlcpy(buf, opt->start, min(sizeof(buf), opt->len + 1));
	return kstrtoull(buf, 0, result);
}

static void __init dm_setup_cleanup(struct dm_device *devices)
{
	struct dm_device *dev = devices;

	while (dev) {
		struct dm_device *old_dev = dev;
		struct dm_setup_table *table = dev->table;

		while (table) {
			struct dm_setup_table *old_table = table;

			kfree(table->type);
			kfree(table->params);
			table = table->next;
			kfree(old_table);
			dev->table_count--;
		}
		WARN_ON(dev->table_count);
		dev = dev->next;
		kfree(old_dev);
	}
}

static char * __init dm_parse_device(struct dm_device *dev, char *str,
				     unsigned long idx)
{
	struct dm_option opt;
	size_t len;
	unsigned long long num_tables;

	/* Grab the logical name of the device to be exported to udev */
	opt.next = str;
	if (!get_dm_option(&opt, DM_FIELD_SEP)) {
		DMERR_PARSE("name", "", idx);
		goto parse_fail;
	}
	len = min(opt.len + 1, sizeof(dev->name));
	strlcpy(dev->name, opt.start, len);  /* includes nul */

	/* Grab the UUID value or "none" */
	if (!get_dm_option(&opt, DM_FIELD_SEP)) {
		DMERR_PARSE("uuid", dev->name, idx);
		goto parse_fail;
	}
	len = min(opt.len + 1, sizeof(dev->uuid));
	strlcpy(dev->uuid, opt.start, len);

	/* Determine if the table/device will be read only or read-write */
	get_dm_option(&opt, DM_ANY_SEP);
	if (!strncmp("ro", opt.start, opt.len)) {
		dev->ro = 1;
	} else if (!strncmp("rw", opt.start, opt.len)) {
		dev->ro = 0;
	} else {
		DMERR_PARSE("table mode", dev->name, idx);
		goto parse_fail;
	}

	/* Optional number field */
	if (opt.delim == DM_FIELD_SEP[0]) {
		if (get_dm_option_u64(&opt, DM_LINE_SEP, &num_tables)) {
			DMERR_PARSE("number of tables", dev->name, idx);
			goto parse_fail;
		}
	} else {
		num_tables = 1;
	}
	if (num_tables > DM_MAX_TARGETS) {
		DMERR_PARSE("too many tables (%llu > %d)", num_tables,
			    DM_MAX_TARGETS, dev->name, idx);
	}
	dev->num_tables = num_tables;

	return opt.next;

parse_fail:
	return NULL;
}

static char * __init dm_parse_tables(struct dm_device *dev, char *str,
				     unsigned long idx)
{
	struct dm_option opt;
	struct dm_setup_table **table = &dev->table;
	unsigned long num_tables = dev->num_tables;
	unsigned long i;
	unsigned long long value;

	/*
	 * Tables are defined as per the normal table format but with a
	 * comma as a newline separator.
	 */
	opt.next = str;
	for (i = 0; i < num_tables; i++) {
		*table = kzalloc(sizeof(struct dm_setup_table), GFP_KERNEL);
		if (!*table) {
			DMERR_PARSE("table %lu (out of memory)", i, dev->name,
				    idx);
			goto parse_fail;
		}
		dev->table_count++;

		if (get_dm_option_u64(&opt, DM_FIELD_SEP, &value)) {
			DMERR_PARSE("starting sector for table %lu", i,
				    dev->name, idx);
			goto parse_fail;
		}
		(*table)->begin = value;

		if (get_dm_option_u64(&opt, DM_FIELD_SEP, &value)) {
			DMERR_PARSE("length for table %lu", i, dev->name, idx);
			goto parse_fail;
		}
		(*table)->length = value;

		if (get_dm_option(&opt, DM_FIELD_SEP))
			(*table)->type = kstrndup(opt.start, opt.len,
							GFP_KERNEL);
		if (!((*table)->type)) {
			DMERR_PARSE("type for table %lu", i, dev->name, idx);
			goto parse_fail;
		}
		if (get_dm_option(&opt, DM_LINE_SEP))
			(*table)->params = kstrndup(opt.start, opt.len,
						    GFP_KERNEL);
		if (!((*table)->params)) {
			DMERR_PARSE("params for table %lu", i, dev->name, idx);
			goto parse_fail;
		}
		table = &((*table)->next);
	}
	DMDEBUG("tables parsed: %d", dev->table_count);

	return opt.next;

parse_fail:
	return NULL;
}

static struct dm_device * __init dm_parse_args(void)
{
	struct dm_device *devices = NULL;
	struct dm_device **tail = &devices;
	struct dm_device *dev;
	char *str = dm_setup_args.str;
	unsigned long num_devices = dm_setup_args.num_devices;
	unsigned long i;

	if (!str)
		return NULL;
	for (i = 0; i < num_devices; i++) {
		dev = kzalloc(sizeof(*dev), GFP_KERNEL);
		if (!dev) {
			DMERR("failed to allocated memory for device %lu", i);
			goto error;
		}
		*tail = dev;
		tail = &dev->next;
		/*
		 * devices are given minor numbers 0 - n-1
		 * in the order they are found in the arg
		 * string.
		 */
		dev->minor = i;
		str = dm_parse_device(dev, str, i);
		if (!str)	/* NULL indicates error in parsing, bail */
			goto error;

		str = dm_parse_tables(dev, str, i);
		if (!str)
			goto error;
	}
	return devices;
error:
	dm_setup_cleanup(devices);
	return NULL;
}

/*
 * Parse the command-line parameters given our kernel, but do not
 * actually try to invoke the DM device now; that is handled by
 * dm_setup_drives after the low-level disk drivers have initialised.
 * dm format is described at the top of the file.
 *
 * Because dm minor numbers are assigned in assending order starting with 0,
 * You can assume the first device is /dev/dm-0, the next device is /dev/dm-1,
 * and so forth.
 */
static int __init dm_setup(char *str)
{
	struct dm_option opt;
	unsigned long long num_devices;

	if (!str) {
		DMERR("setup str is NULL");
		goto parse_fail;
	}

	DMDEBUG("Want to parse \"%s\"", str);
	opt.next = str;
	if (get_dm_option_u64(&opt, DM_FIELD_SEP, &num_devices))
		goto parse_fail;
	str = opt.next;
	if (num_devices > DM_MAX_DEVICES) {
		DMERR("too many devices %llu > %d", num_devices,
		      DM_MAX_DEVICES);
	}
	dm_setup_args.num_devices = num_devices;
	dm_setup_args.str = str;

	DMINFO("will configure %lu device%s", dm_setup_args.num_devices,
	       dm_setup_args.num_devices == 1 ? "" : "s");
	dm_early_setup = 1;
	return 1;

parse_fail:
	DMWARN("Invalid arguments supplied to dm=.");
	return 0;
}

static void __init dm_setup_drives(void)
{
	struct mapped_device *md = NULL;
	struct dm_table *tables = NULL;
	struct dm_setup_table *table;
	struct dm_device *dev;
	char *uuid;
	fmode_t fmode = FMODE_READ;
	struct dm_device *devices;

	devices = dm_parse_args();

	for (dev = devices; dev; dev = dev->next) {
		if (dm_create(dev->minor, &md)) {
			DMERR("failed to create device %s", dev->name);
			goto fail;
		}
		DMDEBUG("created device '%s'", dm_device_name(md));

		/*
		 * In addition to flagging the table below, the disk must be
		 * set explicitly ro/rw.
		 */
		set_disk_ro(dm_disk(md), dev->ro);

		if (!dev->ro)
			fmode |= FMODE_WRITE;
		if (dm_table_create(&tables, fmode, dev->table_count, md)) {
			DMERR("failed to create device %s tables", dev->name);
			goto fail_put;
		}

		dm_lock_md_type(md);

		for (table = dev->table; table; table = table->next) {
			DMINFO("device %s adding table '%llu %llu %s %s'",
			       dev->name,
			       (unsigned long long) table->begin,
			       (unsigned long long) table->length,
			       table->type, table->params);
			if (dm_table_add_target(tables, table->type,
						table->begin,
						table->length,
						table->params)) {
				DMERR("failed to add table to device %s",
					dev->name);
				goto fail_add_target;
			}
		}
		if (dm_table_complete(tables)) {
			DMERR("failed to complete device %s tables",
				dev->name);
			goto fail_add_target;
		}

		/* Suspend the device so that we can bind it to the tables. */
		if (dm_suspend(md, 0)) {
			DMERR("failed to suspend device %s pre-bind",
				dev->name);
			goto fail_add_target;
		}

		/* Initial table load: acquire type of table. */
		dm_set_md_type(md, dm_table_get_type(tables));

		/* Setup md->queue to reflect md's type. */
		if (dm_setup_md_queue(md, tables)) {
			DMERR("unable to set up device queue for new table.");
			goto fail_add_target;
		}

		/*
		 * Bind the tables to the device. This is the only way
		 * to associate md->map with the tables and set the disk
		 * capacity directly.
		 */
		if (dm_swap_table(md, tables)) {  /* should return NULL. */
			DMERR("failed to bind device %s to tables",
				dev->name);
			goto fail_add_target;
		}

		/* Finally, resume and the device should be ready. */
		if (dm_resume(md)) {
			DMERR("failed to resume device %s", dev->name);
			goto fail_add_target;
		}

		/* Export the dm device via the ioctl interface */
		if (!strcmp(DM_NO_UUID, dev->uuid))
			uuid = NULL;
		if (dm_ioctl_export(md, dev->name, uuid)) {
			DMERR("failed to export device %s", dev->name);
			goto fail_add_target;
		}

		dm_unlock_md_type(md);

		DMINFO("dm-%d (%s) is ready", dev->minor, dev->name);
	}
	dm_setup_cleanup(devices);
	return;

fail_add_target:
	dm_unlock_md_type(md);
	dm_table_destroy(tables);
fail_put:
	dm_put(md);
fail:
	DMERR("starting dm-%d (%s) failed", dev->minor, dev->name);
	dm_setup_cleanup(devices);
}

__setup("dm=", dm_setup);

void __init dm_run_setup(void)
{
	if (!dm_early_setup)
		return;
	DMINFO("attempting early device configuration.");
	dm_setup_drives();
}
