// SPDX-License-Identifier: GPL-2.0
/*
 * CRC test driver
 *
 * Copyright (C) 2018 Coly Li <colyli@suse.de>
 *
 * This module provides an simple framework to check the consistency of
 * Linux kernel crc calculation routines in lib/crc*.c. This driver
 * requires CONFIG_CRC* items to be enabled if the associated routines are
 * tested here. The test results will be printed to kernel message
 * when this test driver is loaded.
 *
 * Current test routines are,
 * - crc64_le()
 * - crc64_le_bch()
 * - crc64_le_update()
 *
 */

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/async.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/crc64.h>

struct crc_test_record {

	char	*name;
	__le64	data[4];
	__le64	initval;
	__le64	expval;
	int	(*handler)(struct crc_test_record *rec);
};

static int chk_and_msg(const char *name, __le64 crc, __le64 expval)
{
	int ret = 0;

	if (crc == expval) {
		pr_info("test_crc: %s: PASSED:(0x%016llx, expval 0x%016llx)",
			name, crc, expval);
	} else {
		pr_err("test_crc: %s: FAILED:(0x%016llx, expval 0x%016llx)",
			name, crc, expval);
		ret = -EINVAL;
	}

	return ret;
}

/* Add your crc test caese here */
static int test_crc64_le(struct crc_test_record *rec)
{
	__le64 crc;

	crc = crc64_le(rec->data, sizeof(rec->data));
	return chk_and_msg(rec->name, crc, rec->expval);

}

static int test_crc64_le_bch(struct crc_test_record *rec)
{
	__le64 crc;

	crc = crc64_le_bch(rec->data, sizeof(rec->data));
	return chk_and_msg(rec->name, crc, rec->expval);
}

static int test_crc64_le_update(struct crc_test_record *rec)
{
	__le64 crc = rec->initval;

	crc = crc64_le_update(crc, rec->data, sizeof(rec->data));
	return chk_and_msg(rec->name, crc, rec->expval);
}

/*
 * Set up your crc test initial data here.
 * Do not change the existing items, they are hard coded with
 * pre-calculated values.
 */
static struct crc_test_record test_data[] = {
	{ .name		= "crc64_le",
	  .data		= { 0x42F0E1EBA9EA3693, 0x85E1C3D753D46D26,
			    0xC711223CFA3E5BB5, 0x493366450E42ECDF },
	  .initval	= 0,
	  .expval	= 0xe2b9911e7b997201,
	  .handler	= test_crc64_le,
	},
	{ .name		= "crc64_le_bch",
	  .data		= { 0x42F0E1EBA9EA3693, 0x85E1C3D753D46D26,
			    0xC711223CFA3E5BB5, 0x493366450E42ECDF },
	  .initval	= 0,
	  .expval	= 0xd2753a20fd862892,
	  .handler	= test_crc64_le_bch,
	},
	{ .name		= "crc64_le_update",
	  .data		= { 0x42F0E1EBA9EA3693, 0x85E1C3D753D46D26,
			    0xC711223CFA3E5BB5, 0x493366450E42ECDF },
	  .initval	= 0x61C8864680B583EB,
	  .expval	= 0xb2c863673f4292bf,
	  .handler	= test_crc64_le_update,
	},
	{ .name = NULL, }
};


static int __init test_crc_init(void)
{
	int i;
	int v, ret = 0;

	pr_info("Kernel crc consitency testing:");
	for (i = 0; test_data[i].name; i++) {
		v = test_data[i].handler(&test_data[i]);
		if (v < 0 && ret == 0)
			ret = -EINVAL;
	}

	return ret;
}
late_initcall(test_crc_init);

static void __exit test_crc_exit(void) { }
module_exit(test_crc_exit);

MODULE_DESCRIPTION("CRC consistency testing driver");
MODULE_AUTHOR("Coly Li <colyli@suse.de>");
MODULE_LICENSE("GPL");
