/* Marvell Bluetooth driver: platform specific driver
 *
 * Copyright (C) 2015, Marvell International Ltd.
 *
 * This software file (the "File") is distributed by Marvell International
 * Ltd. under the terms of the GNU General Public License Version 2, June 1991
 * (the "License").  You may use, redistribute and/or modify this File in
 * accordance with the terms and conditions of the License, a copy of which
 * is available on the worldwide web at
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE
 * ARE EXPRESSLY DISCLAIMED.  The License provides additional details about
 * this warranty disclaimer.
 */

#include "btmrvl_drv.h"

struct platform_device *btmrvl_plt_dev;

static int btmrvl_plt_probe(struct platform_device *pdev)
{
	btmrvl_plt_dev = pdev;
	return 0;
}

static int btmrvl_plt_remove(struct platform_device *pdev)
{
	btmrvl_plt_dev = NULL;
	return 0;
}

static const struct of_device_id btmrvl_dt_match[] = {
	{
		.compatible = "marvell,btmrvl",
	},
	{},
};

MODULE_DEVICE_TABLE(of, btmrvl_dt_match);

static struct platform_driver btmrvl_platform_driver = {
	.probe		= btmrvl_plt_probe,
	.remove		= btmrvl_plt_remove,
	.driver = {
		.name	= "btmrvl_plt",
		.of_match_table = btmrvl_dt_match,
	}
};

int btmrvl_platform_drv_init(void)
{
	return platform_driver_register(&btmrvl_platform_driver);
}

void btmrvl_platform_drv_exit(void)
{
	platform_driver_unregister(&btmrvl_platform_driver);
}
