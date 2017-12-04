/*
 * Copyright (C) 2017 Jiaxun Yang.
 * Author: Jiaxun Yang, jiaxun.yang@flygoat.com

 * Copyright (C) 2009 Lemote Inc.
 * Author: Wu Zhangjin, wuzhangjin@gmail.com
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/err.h>
#include <linux/platform_device.h>

#include <asm/bootinfo.h>

static int __init lemote2f_platform_init(void)
{
	if (mips_machtype != MACH_LEMOTE_YL2F89)
		return -ENODEV;

	return platform_device_register_simple("yeeloong_laptop", -1, NULL, 0);
}

arch_initcall(lemote2f_platform_init);