/* SPDX-License-Identifier: GPL-2.0 */
/* 
* Copyright (C) 2017 Jiaxun Yang <jiaxun.yang@flygoat.com>
*
*/

#include <linux/err.h>
#include <linux/platform_device.h>

#include <asm/bootinfo.h>

static struct platform_device yeeloong_pdev = {
	.name = "yeeloong_laptop",
	.id = -1,
};

static int __init lemote2f_platform_init(void)
{
	if (mips_machtype != MACH_LEMOTE_YL2F89)
		return -ENODEV;

	return platform_device_register(&yeeloong_pdev);
}

arch_initcall(lemote2f_platform_init);
