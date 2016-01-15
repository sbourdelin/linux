/*
 * drivers/dma-buf/fence_debug.c
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

#include <linux/seq_file.h>
#include <linux/fence.h>

#ifdef CONFIG_DEBUG_FS

static LIST_HEAD(fence_timeline_list_head);
static DEFINE_SPINLOCK(fence_timeline_list_lock);

void fence_timeline_debug_add(struct fence_timeline *obj)
{
	unsigned long flags;

	spin_lock_irqsave(&fence_timeline_list_lock, flags);
	list_add_tail(&obj->fence_timeline_list, &fence_timeline_list_head);
	spin_unlock_irqrestore(&fence_timeline_list_lock, flags);
}
EXPORT_SYMBOL(fence_timeline_debug_add);

void fence_timeline_debug_remove(struct fence_timeline *obj)
{
	unsigned long flags;

	spin_lock_irqsave(&fence_timeline_list_lock, flags);
	list_del(&obj->fence_timeline_list);
	spin_unlock_irqrestore(&fence_timeline_list_lock, flags);
}
EXPORT_SYMBOL(fence_timeline_debug_remove);

const char *fence_status_str(int status)
{
	if (status == 0)
		return "signaled";

	if (status > 0)
		return "active";

	return "error";
}
EXPORT_SYMBOL(fence_status_str);

void fence_print(struct seq_file *s, struct fence *fence, bool show)
{
	int status = 1;
	struct fence_timeline *parent = fence_parent(fence);

	if (fence_is_signaled_locked(fence))
		status = fence->status;

	seq_printf(s, "  %s%sfence %s",
		   show ? parent->name : "",
		   show ? "_" : "",
		   fence_status_str(status));

	if (status <= 0) {
		struct timespec64 ts64 =
			ktime_to_timespec64(fence->timestamp);

		seq_printf(s, "@%lld.%09ld", (s64)ts64.tv_sec, ts64.tv_nsec);
	}

	if (fence->ops->timeline_value_str &&
	    fence->ops->fence_value_str) {
		char value[64];

		fence->ops->fence_value_str(fence, value, sizeof(value));
		seq_printf(s, ": %s", value);
		if (show) {
			fence->ops->timeline_value_str(fence, value,
						       sizeof(value));
			seq_printf(s, " / %s", value);
		}
	}

	seq_puts(s, "\n");
}
EXPORT_SYMBOL(fence_print);

void fence_timeline_print(struct seq_file *s, struct fence_timeline *obj)
{
	struct list_head *pos;
	unsigned long flags;

	seq_printf(s, "%s %s: %u\n", obj->name, obj->drv_name, obj->value);

	spin_lock_irqsave(&obj->lock, flags);
	list_for_each(pos, &obj->child_list_head) {
		struct fence *fence =
			container_of(pos, struct fence, child_list);
		fence_print(s, fence, false);
	}
	spin_unlock_irqrestore(&obj->lock, flags);
}
EXPORT_SYMBOL(fence_timeline_print);

void fence_timeline_print_all(struct seq_file* s)
{
	unsigned long flags;
	struct list_head *pos;

	spin_lock_irqsave(&fence_timeline_list_lock, flags);
	list_for_each(pos, &fence_timeline_list_head) {
		struct fence_timeline *obj =
			container_of(pos, struct fence_timeline,
				     fence_timeline_list);

		fence_timeline_print(s, obj);
		seq_puts(s, "\n");
	}
	spin_unlock_irqrestore(&fence_timeline_list_lock, flags);
}
EXPORT_SYMBOL(fence_timeline_print_all);
#endif
