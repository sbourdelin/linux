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

static void sw_sync_cleanup(struct fence *fence, void *user_data)
{
	struct sync_fence *sync_fence = user_data;

	sync_fence_cleanup(sync_fence);
}

static const struct fence_ops sw_sync_fence_ops = {
	.get_driver_name = fence_default_get_driver_name,
	.get_timeline_name = fence_default_get_timeline_name,
	.enable_signaling = fence_default_enable_signaling,
	.signaled = fence_default_signaled,
	.wait = fence_default_wait,
	.release = fence_default_release,
	.cleanup = sw_sync_cleanup,
	.fill_driver_data = fence_default_fill_driver_data,
	.fence_value_str = fence_default_value_str,
	.timeline_value_str = fence_default_timeline_value_str,
};

struct fence *sw_sync_pt_create(struct fence_timeline *obj, u32 value)
{
	return fence_create_on_timeline(obj, &sw_sync_fence_ops,
					 sizeof(struct fence), value);
}
EXPORT_SYMBOL(sw_sync_pt_create);

struct fence_timeline *sw_sync_timeline_create(const char *name)
{
	return fence_timeline_create(1, sizeof(struct fence_timeline),
				     "sw_sync", name);
}
EXPORT_SYMBOL(sw_sync_timeline_create);

void sw_sync_timeline_inc(struct fence_timeline *obj, u32 inc)
{
	fence_timeline_signal(obj, inc);
}
EXPORT_SYMBOL(sw_sync_timeline_inc);
