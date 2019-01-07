// SPDX-License-Identifier: GPL-2.0
/*
 *
 * blkbuf.c: Block device Oops/Panic logger
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
#define pr_fmt(fmt) "blkbuf: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pstore_blk.h>

struct blkz_info blkbuf_info = {
	.owner = THIS_MODULE,
	.name = "blkbuf",
	.part_size = 512 * 1024,
	.dmesg_size = 64 * 1024,
	.dump_oops = true,
};

static int __init blkbuf_init(void)
{
	return blkz_register(&blkbuf_info);
}
module_init(blkbuf_init);

static void __exit blkbuf_exit(void)
{
	blkz_unregister(&blkbuf_info);
}
module_exit(blkbuf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("liaoweixiong <liaoweixiong@allwinnertech.com>");
MODULE_DESCRIPTION("Sample for Pstore BLK with Oops logger");
