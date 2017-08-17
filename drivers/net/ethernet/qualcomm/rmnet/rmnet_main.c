/* Copyright (c) 2013-2014, 2016-2017 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *
 * RMNET Data generic framework
 *
 */

#include <linux/module.h>
#include "rmnet_private.h"
#include "rmnet_config.h"
#include "rmnet_vnd.h"

/* Startup/Shutdown */

static int __init rmnet_init(void)
{
	rmnet_config_init();
	return 0;
}

static void __exit rmnet_exit(void)
{
	rmnet_config_exit();
}

module_init(rmnet_init)
module_exit(rmnet_exit)
MODULE_LICENSE("GPL v2");
