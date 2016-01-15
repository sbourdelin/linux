/*
 * drivers/base/sw_sync.c
 *
 * Copyright (C) 2012 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

#include "sw_sync.h"

static const struct fence_ops sw_sync_fence_ops = {
	.get_driver_name = fence_default_get_driver_name,
	.get_timeline_name = fence_default_get_timeline_name,
	.enable_signaling = fence_default_enable_signaling,
	.signaled = fence_default_signaled,
	.wait = fence_default_wait,
	.release = fence_default_release,
	.fill_driver_data = fence_default_fill_driver_data,
	.fence_value_str = fence_default_value_str,
	.timeline_value_str = fence_default_timeline_value_str,
};

struct fence *sw_sync_pt_create(struct sw_sync_timeline *obj, u32 value)
{
	return fence_create_on_timeline(&obj->obj, &sw_sync_fence_ops,
					 sizeof(struct fence), value);
}
EXPORT_SYMBOL(sw_sync_pt_create);

struct sw_sync_timeline *sw_sync_timeline_create(const char *name)
{
	struct sw_sync_timeline *obj = (struct sw_sync_timeline *)
		fence_timeline_create(1, sizeof(struct sw_sync_timeline),
				     "sw_sync", name);

	return obj;
}
EXPORT_SYMBOL(sw_sync_timeline_create);

void sw_sync_timeline_inc(struct sw_sync_timeline *obj, u32 inc)
{
	fence_timeline_signal(&obj->obj, inc);
}
EXPORT_SYMBOL(sw_sync_timeline_inc);
