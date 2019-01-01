// SPDX-License-Identifier: GPL-2.0
/*
 *
 * rombuf.c: ROM Oops/Panic logger
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
#define pr_fmt(fmt) "rombuf: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pstore_rom.h>

struct romz_info rombuf_info = {
	.owner = THIS_MODULE,
	.name = "rombuf",
	.part_size = 512 * 1024,
	.dmesg_size = 64 * 1024,
	.dump_oops = true,
};

static int __init rombuf_init(void)
{
	return romz_register(&rombuf_info);
}
module_init(rombuf_init);

static void __exit rombuf_exit(void)
{
	romz_unregister(&rombuf_info);
}
module_exit(rombuf_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("liaoweixiong <liaoweixiong@allwinnertech.com>");
MODULE_DESCRIPTION("Sample for Pstore ROM with Oops logger");
