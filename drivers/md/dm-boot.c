// SPDX-License-Identifier: GPL-2.0

/*
 * dm-boot.c
 * Copyright (C) 2017 The Chromium OS Authors <chromium-os-dev@chromium.org>
 *
 * This file is released under the GPLv2.
 */

#include <linux/ctype.h>
#include <linux/device-mapper.h>
#include <linux/list.h>

#define DM_MSG_PREFIX "dm"
#define DM_MAX_DEVICES 256

/* See Documentation/device-mapper/dm-boot.txt for dm="..." format details. */

struct target {
	unsigned long long start;
	unsigned long long length;
	char *type;
	char *params;
	struct list_head list;
};

struct dm_device {
	int minor;
	int ro;
	char *name;
	char *uuid;
	int table_count;
	struct list_head table;
	struct list_head list;
};

/**
 * _align - align address with the next block
 * @ptr: the pointer to be aligned.
 * @a: the size of the block to align the pointer. Must be a power of two.
 */
static void __init *_align(void *ptr, unsigned int a)
{
	register unsigned long agn = --a;

	return (void *) (((unsigned long) ptr + agn) & ~agn);
}

const char *dm_allowed_types[] __initconst = {
	/* "cache", constrained, requires userspace validation */
	"crypt",
	"delay",
	"era",
	"error",
	"flakey",
	"integrity",
	"linear",
	"log-writes",
	"mirror",
	"multipath",
	"raid",
	"snapshot",
	"snapshot-origin",
	"striped",
	"switch",
	/* "thin", constrained, requires userspace validation */
	/* "thin-pool", constrained, requires userspace validation */
	"unstriped",
	"verity",
	"writecache",
	/* "zero", constrained, requires userspace validation */
	"zoned",
};

static int __init dm_verify_type(const char *type)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dm_allowed_types); i++) {
		if (!strcmp(dm_allowed_types[i], type))
			return 0;
	}
	return -EINVAL;
}

static struct target __init *dm_parse_table_entry(char *str)
{
	char type[DM_MAX_TYPE_NAME], *ptr;
	struct target *table;
	int n;

	/* trim trailing space */
	for (ptr = str + strlen(str) - 1;
	     ptr >= str && isspace(*ptr); ptr--)
		;
	*(++ptr) = '\0';

	/* trim leading space */
	for (ptr = str; *ptr && isspace(*ptr); ptr++)
		;
	if (!*ptr)
		return NULL;

	table = kzalloc(sizeof(struct target), GFP_KERNEL);
	if (!table)
		return NULL;

	if (sscanf(ptr, "%llu %llu %s %n", &table->start, &table->length,
		   type, &n) < 3) {
		DMERR("invalid format of table \"%s\"", str);
		goto err_free_table;
	}

	if (dm_verify_type(type)) {
		DMERR("invalid type \"%s\"", type);
		goto err_free_table;
	}

	table->type = kstrndup(type, strlen(type), GFP_KERNEL);
	if (!table->type) {
		DMERR("invalid type of table");
		goto err_free_table;
	}

	ptr += n;
	table->params = kstrndup(ptr, strlen(ptr), GFP_KERNEL);
	if (!table->params) {
		DMERR("invalid params for table");
		goto err_free_type;
	}

	return table;

err_free_type:
	kfree(table->type);
err_free_table:
	kfree(table);
	return NULL;
}

/* Parse multiple lines of table */
static int __init dm_parse_table(struct dm_device *dev, char *str)
{
	char *pos = str, *next_pos;
	struct target *table;

	do {
		/* Identify and terminate each line */
		next_pos = strchr(pos, ',');
		if (next_pos)
			*next_pos++ = '\0';
		table = dm_parse_table_entry(pos);
		if (!table) {
			DMERR("Couldn't parse table \"%s\"", pos);
			return -EINVAL;
		}
		dev->table_count++;
		list_add_tail(&table->list, &dev->table);
	} while ((pos = next_pos));

	return 0;
}

static void __init dm_setup_cleanup(struct list_head *devices)
{
	struct dm_device *dev, *d_tmp;
	struct target *target, *t_tmp;

	list_for_each_entry_safe(dev, d_tmp, devices, list) {
		list_del(&dev->list);
		list_for_each_entry_safe(target, t_tmp, &dev->table, list) {
			list_del(&target->list);
			kfree(target->type);
			kfree(target->params);
			kfree(target);
		}
		kfree(dev);
	}
}

/* code based on _create_concise function from dmsetup.c (lvm2) */
static int __init dm_parse_args(struct list_head *devices, char *str)
{
	/* name,uuid,minor,flags,table */
	char *fields[5] = { NULL };
	unsigned long ndev = 0;
	struct dm_device *dev;
	char *c, *n;
	int f = 0;

	/*
	 * Work through input string c, parsing into sets of 5 fields.
	 * Strip out any characters quoted by backslashes in-place.
	 * Read characters from c and prepare them in situ for final processing
	 * at n
	 */
	c = n = fields[f] = str;

	while (*c) {
		/* Quoted character?  Skip past quote. */
		if (*c == '\\') {
			if (!*(++c)) {
				DMERR("Backslash must be followed by another character at end of string.");
				*n = '\0';
				DMERR("Parsed %d fields: name: %s uuid: %s minor: %s flags: %s table: %s",
				      f + 1, fields[0], fields[1], fields[2],
				      fields[3], fields[4]);
				goto out;
			}
			/* Don't interpret next character */
			*n++ = *c++;
			continue;
		}

		/* Comma marking end of field? */
		if (*c == ',' && f < 4) {
			/* Terminate string */
			*n++ = '\0', c++;
			/* Store start of next field */
			fields[++f] = n;
			/* Skip any whitespace after field-separating commas */
			while (isspace(*c))
				c++;
			continue;
		}

		/* Semi-colon marking end of device? */
		if (*c == ';' || *(c + 1) == '\0') {
			/* End of input? */
			if (*c != ';')
				/* Copy final character */
				*n++ = *c;

			/* Terminate string */
			*n++ = '\0', c++;

			if (f != 4) {
				DMERR("Five comma-separated fields are required for each device");
				DMERR("Parsed %d fields: name: %s uuid: %s minor: %s flags: %s table: %s",
				      f + 1, fields[0], fields[1], fields[2],
				      fields[3], fields[4]);
				goto out;
			}

			dev = kzalloc(sizeof(*dev), GFP_KERNEL);
			if (!dev)
				goto out;
			INIT_LIST_HEAD(&dev->table);
			list_add_tail(&dev->list, devices);
			if (++ndev > DM_MAX_DEVICES) {
				DMERR("too many devices %lu > %d",
				      ndev, DM_MAX_DEVICES);
				goto out;
			}

			/* Set up parameters */
			dev->name = fields[0];
			dev->uuid = fields[1];

			if (!*fields[2])
				dev->minor = DM_ANY_MINOR;
			else if (kstrtoint(fields[2], 0, &dev->minor))
				goto out;

			if (!strcmp(fields[3], "ro"))
				dev->ro = 1;
			else if (*fields[3] && strcmp(fields[3], "rw")) {
				DMERR("Invalid flags parameter '%s' must be 'ro' or 'rw' or empty.", fields[3]);
				goto out;
			}

			if (*fields[4] && dm_parse_table(dev, fields[4]))
				goto out;

			/* Clear parameters ready for any further devices */
			f = 0;
			fields[0] = n;
			fields[1] = fields[2] = fields[3] = fields[4] = NULL;

			/* Skip any whitespace after semi-colons */
			while (isspace(*c))
				c++;

			continue;
		}

		/* Normal character */
		*n++ = *c++;
	}

	if (fields[0] != n) {
		*n = '\0';
		DMERR("Incomplete entry: five comma-separated fields are required for each device");
		DMERR("Parsed %d fields: name: %s uuid: %s minor: %s flags: %s table: %s",
		      f + 1, fields[0], fields[1], fields[2], fields[3],
		      fields[4]);
		goto out;
	}

	return 0;

out:
	dm_setup_cleanup(devices);
	return -EINVAL;
}

static char __init *dm_add_target(const struct target *const table,
				  char *const buf, char *const end)
{
	struct dm_target_spec sp;
	char *p = buf;
	size_t len;

	len = strlen(table->params);
	if (sizeof(struct dm_target_spec) + len >= end - p) {
		DMERR("ran out of memory building ioctl parameter");
		return NULL;
	}

	p += sizeof(struct dm_target_spec);
	strcpy(p, table->params);
	p += len + 1;
	/* align next block */
	p = _align(p, 8);

	sp.status = 0;
	sp.sector_start = table->start;
	sp.length = table->length;
	strscpy(sp.target_type, table->type, sizeof(sp.target_type));
	sp.next = p - buf;
	memcpy(buf, &sp, sizeof(struct dm_target_spec));

	return p;
}

static int dm_setup_ioctl(struct dm_ioctl *dmi, size_t len,
			  struct dm_device *dev, int flags)
{
	struct target *table;
	char *b, *e;

	memset(dmi, 0, len);
	dmi->version[0] = 4;
	dmi->version[1] = 0;
	dmi->version[2] = 0;
	dmi->data_size = len;
	dmi->data_start = sizeof(struct dm_ioctl);
	dmi->flags = flags;
	dmi->target_count = dev->table_count;
	dmi->event_nr = 1;

	/* Only one between uuid, name and dev should be filled */
	if (*dev->uuid)
		strscpy(dmi->uuid, dev->uuid, sizeof(dmi->uuid));
	else if (*dev->name)
		strscpy(dmi->name, dev->name, sizeof(dmi->name));
	else if (dev->minor > 0)
		dmi->dev = dev->minor;
	else {
		DMERR("device name or uuid or minor number should be provided");
		return -EINVAL;
	}

	b = (char *) (dmi + 1);
	e = (char *) dmi + len;

	list_for_each_entry(table, &dev->table, list) {
		DMDEBUG("device %s adding table '%llu %llu %s %s'",
			dev->name,
			(unsigned long long) table->start,
			(unsigned long long) table->length,
			table->type, table->params);
		b = dm_add_target(table, b, e);
		if (!b) {
			kfree(dmi);
			return -EINVAL;
		}
	}

	return 0;
}

void __init dm_boot_setup_drives(char *boot_param)
{
	const size_t min_size = 16 * 1024;
	const size_t len = sizeof(struct dm_ioctl) > min_size ?
			   sizeof(struct dm_ioctl) : min_size;
	LIST_HEAD(devices);
	struct dm_device *dev;
	struct dm_ioctl *dmi;
	int flags;

	if (dm_parse_args(&devices, boot_param))
		return;

	dmi = kmalloc(len, GFP_KERNEL);
	if (!dmi)
		return;

	list_for_each_entry(dev, &devices, list) {
		flags = dev->minor < 0 ? 0 : DM_PERSISTENT_DEV_FLAG;
		if (dm_setup_ioctl(dmi, len, dev, flags))
			goto out_free;
		dmi->dev = dev->minor;
		/* create a new device */
		if (dm_ioctl_cmd(DM_DEV_CREATE, dmi)) {
			DMERR("failed to create device %s", dev->name);
			goto out_free;
		}

		flags = dev->ro ? DM_READONLY_FLAG : 0;
		if (dm_setup_ioctl(dmi, len, dev, flags))
			goto out_free;
		/* load a table into the 'inactive' slot for the device. */
		if (dm_ioctl_cmd(DM_TABLE_LOAD, dmi)) {
			DMERR("failed to load device %s tables", dev->name);
			goto out_free;
		}

		if (dm_setup_ioctl(dmi, len, dev, 0))
			goto out_free;
		/* resume and the device should be ready. */
		if (dm_ioctl_cmd(DM_DEV_SUSPEND, dmi)) {
			DMERR("failed to resume device %s", dev->name);
			goto out_free;
		}

		DMINFO("dm-%d (%s) is ready", dev->minor, dev->name);
	}

out_free:
	kfree(dmi);
}
