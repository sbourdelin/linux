// SPDX-License-Identifier: GPL-2.0
/*
 * CRC test driver
 *
 * Copyright (C) 2018 Coly Li <colyli@suse.de>
 *
 * This module provides an simple framework to check the consistency of
 * Linux kernel CRC calculation routines in lib/crc*.c. This driver
 * requires CONFIG_CRC* items to be enabled if the associated routines are
 * tested here. The test results will be printed to kernel message
 * when this test driver is loaded.
 *
 * Current test routines are,
 * - crc64()
 * - crc64_bch()
 * - crc64_update()
 *
 */

#include <linux/async.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/crc64.h>

struct crc_test_record {
	char	*name;
	u64	data[4];
	u64	initval;
	u64	expval;
	int	(*handler)(struct crc_test_record *rec);
};

static int chk_and_msg(const char *name, u64 crc, u64 expval)
{
	int ret = 0;

	if (crc == expval) {
		pr_info("test_crc: %s: PASSED:(0x%016llx, expected 0x%016llx)\n",
			name, crc, expval);
	} else {
		pr_err("test_crc: %s: FAILED:(0x%016llx, expected 0x%016llx)\n",
			name, crc, expval);
		ret = -EINVAL;
	}

	return ret;
}

/* Add your crc test cases here */
static int test_crc64(struct crc_test_record *rec)
{
	u64 crc;

	crc = crc64(rec->data, sizeof(rec->data));
	return chk_and_msg(rec->name, crc, rec->expval);
}

static int test_crc64_bch(struct crc_test_record *rec)
{
	u64 crc;

	crc = crc64_bch(rec->data, sizeof(rec->data));
	return chk_and_msg(rec->name, crc, rec->expval);
}

static int test_crc64_update(struct crc_test_record *rec)
{
	u64 crc = rec->initval;

	crc = crc64_update(crc, rec->data, sizeof(rec->data));
	return chk_and_msg(rec->name, crc, rec->expval);
}

/*
 * Set up your crc test initial data here.
 * Do not change the existing items, they are hard coded with
 * pre-calculated values.
 */
static struct crc_test_record test_data[] = {
	{ .name		= "crc64",
	  .data		= { 0x42F0E1EBA9EA3693, 0x85E1C3D753D46D26,
			    0xC711223CFA3E5BB5, 0x493366450E42ECDF },
	  .initval	= 0,
	  .expval	= 0xe2b9911e7b997201,
	  .handler	= test_crc64,
	},
	{ .name		= "crc64_bch",
	  .data		= { 0x42F0E1EBA9EA3693, 0x85E1C3D753D46D26,
			    0xC711223CFA3E5BB5, 0x493366450E42ECDF },
	  .initval	= 0,
	  .expval	= 0xd2753a20fd862892,
	  .handler	= test_crc64_bch,
	},
	{ .name		= "crc64_update",
	  .data		= { 0x42F0E1EBA9EA3693, 0x85E1C3D753D46D26,
			    0xC711223CFA3E5BB5, 0x493366450E42ECDF },
	  .initval	= 0x61C8864680B583EB,
	  .expval	= 0xb2c863673f4292bf,
	  .handler	= test_crc64_update,
	},
	{}
};

static int __init test_crc_init(void)
{
	int i;
	int v, err = 0;

	pr_info("Kernel CRC consitency testing:\n");
	for (i = 0; test_data[i].name; i++) {
		v = test_data[i].handler(&test_data[i]);
		if (v < 0)
			err++;
	}

	if (err == 0)
		pr_info("test_crc: all %d tests passed\n", i);
	else
		pr_err("test_crc: %d cases tested, %d passed, %d failed\n",
		       i, i - err, err);

	return (err == 0) ? 0 : -EINVAL;
}
late_initcall(test_crc_init);

static void __exit test_crc_exit(void) { }
module_exit(test_crc_exit);

MODULE_DESCRIPTION("CRC consistency testing driver");
MODULE_AUTHOR("Coly Li <colyli@suse.de>");
MODULE_LICENSE("GPL v2");
