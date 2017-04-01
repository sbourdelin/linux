/*
 * Random early detection I/O scheduler.
 *
 * Copyright (C) 2017 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/elevator.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/sbitmap.h>

#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-sched.h"
#include "blk-mq-tag.h"

enum {
	RED_DEFAULT_MIN_THRESH = 16,
	RED_DEFAULT_MAX_THRESH = 256,
	RED_MAX_MAX_THRESH = 256,
};

struct red_queue_data {
	struct request_queue *q;
	unsigned int min_thresh, max_thresh;
};

static int red_init_sched(struct request_queue *q, struct elevator_type *e)
{
	struct red_queue_data *rqd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	rqd = kmalloc_node(sizeof(*rqd), GFP_KERNEL, q->node);
	if (!rqd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	rqd->min_thresh = RED_DEFAULT_MIN_THRESH;
	rqd->max_thresh = RED_DEFAULT_MAX_THRESH;

	eq->elevator_data = rqd;
	q->elevator = eq;

	return 0;
}

static void red_exit_sched(struct elevator_queue *e)
{
	struct red_queue_data *rqd = e->elevator_data;

	kfree(rqd);
}

static struct request *red_get_request(struct request_queue *q,
				       unsigned int op,
				       struct blk_mq_alloc_data *data)
{
	struct red_queue_data *rqd = q->elevator->elevator_data;
	unsigned int queue_length;
	u32 drop_prob;

	queue_length = sbitmap_weight(&data->hctx->sched_tags->bitmap_tags.sb);
	if (queue_length <= rqd->min_thresh)
		goto enqueue;
	else if (queue_length >= rqd->max_thresh)
		goto drop;

	drop_prob = (U32_MAX / (rqd->max_thresh - rqd->min_thresh) *
		     (queue_length - rqd->min_thresh));

	if (prandom_u32() <= drop_prob)
		goto drop;

enqueue:
	return __blk_mq_alloc_request(data, op);

drop:
	/*
	 * Non-blocking callers will return EWOULDBLOCK; blocking callers should
	 * check the return code and retry.
	 */
	return NULL;
}

static ssize_t red_min_thresh_show(struct elevator_queue *e, char *page)
{
	struct red_queue_data *rqd = e->elevator_data;

	return sprintf(page, "%u\n", rqd->min_thresh);
}

static ssize_t red_min_thresh_store(struct elevator_queue *e, const char *page,
				    size_t count)
{
	struct red_queue_data *rqd = e->elevator_data;
	unsigned int thresh;
	int ret;

	ret = kstrtouint(page, 10, &thresh);
	if (ret)
		return ret;

	if (thresh >= rqd->max_thresh)
		return -EINVAL;

	rqd->min_thresh = thresh;

	return count;
}

static ssize_t red_max_thresh_show(struct elevator_queue *e, char *page)
{
	struct red_queue_data *rqd = e->elevator_data;

	return sprintf(page, "%u\n", rqd->max_thresh);
}

static ssize_t red_max_thresh_store(struct elevator_queue *e, const char *page,
				    size_t count)
{
	struct red_queue_data *rqd = e->elevator_data;
	unsigned int thresh;
	int ret;

	ret = kstrtouint(page, 10, &thresh);
	if (ret)
		return ret;

	if (thresh <= rqd->min_thresh || thresh > RED_MAX_MAX_THRESH)
		return -EINVAL;

	rqd->max_thresh = thresh;

	return count;
}

#define RED_THRESH_ATTR(which) __ATTR(which##_thresh, 0644, red_##which##_thresh_show, red_##which##_thresh_store)
static struct elv_fs_entry red_sched_attrs[] = {
	RED_THRESH_ATTR(min),
	RED_THRESH_ATTR(max),
	__ATTR_NULL
};
#undef RED_THRESH_ATTR

static struct elevator_type red_sched = {
	.ops.mq = {
		.init_sched = red_init_sched,
		.exit_sched = red_exit_sched,
		.get_request = red_get_request,
	},
	.uses_mq = true,
	.elevator_attrs = red_sched_attrs,
	.elevator_name = "red",
	.elevator_owner = THIS_MODULE,
};

static int __init red_init(void)
{
	return elv_register(&red_sched);
}

static void __exit red_exit(void)
{
	elv_unregister(&red_sched);
}

module_init(red_init);
module_exit(red_exit);

MODULE_AUTHOR("Omar Sandoval");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Random early detection I/O scheduler");
