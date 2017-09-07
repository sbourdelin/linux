/*
 *  Zoned MQ Deadline i/o scheduler - adaptation of the MQ deadline scheduler,
 *  for zoned block devices used with the blk-mq scheduling framework
 *
 *  Copyright (C) 2016 Jens Axboe <axboe@kernel.dk>
 *  Copyright (C) 2017 Damien Le Moal <damien.lemoal@wdc.com>
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/blk-mq-sched.h>
#include <linux/blk-mq-debugfs.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>
#include <linux/sbitmap.h>
#include <linux/seq_file.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

#include "sd.h"
#include "sd_zbc.h"

/*
 * See Documentation/block/deadline-iosched.txt
 */

/* max time before a read is submitted. */
static const int read_expire = HZ / 2;

/* ditto for writes, these limits are SOFT! */
static const int write_expire = 5 * HZ;

/* max times reads can starve a write */
static const int writes_starved = 2;

/*
 * Number of sequential requests treated as one by the above parameters.
 * For throughput.
 */
static const int fifo_batch = 16;

/*
 * Run time data.
 */
struct zoned_data {
	/*
	 * requests (zoned_rq s) are present on both sort_list and fifo_list
	 */
	struct rb_root sort_list[2];
	struct list_head fifo_list[2];

	/*
	 * next in sort order. read, write or both are NULL
	 */
	struct request *next_rq[2];
	unsigned int batching;		/* number of sequential requests made */
	unsigned int starved;		/* times reads have starved writes */

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	int fifo_expire[2];
	int fifo_batch;
	int writes_starved;
	int front_merges;

	spinlock_t lock;
	struct list_head dispatch;

	struct scsi_disk *sdkp;

	spinlock_t zones_lock;
	unsigned long *zones_wlock;
	unsigned long *seq_zones;
};

static inline struct rb_root *
zoned_rb_root(struct zoned_data *zd, struct request *rq)
{
	return &zd->sort_list[rq_data_dir(rq)];
}

/*
 * get the request after `rq' in sector-sorted order
 */
static inline struct request *
zoned_latter_request(struct request *rq)
{
	struct rb_node *node = rb_next(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

static void
zoned_add_rq_rb(struct zoned_data *zd, struct request *rq)
{
	struct rb_root *root = zoned_rb_root(zd, rq);

	elv_rb_add(root, rq);
}

static inline void
zoned_del_rq_rb(struct zoned_data *zd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	if (zd->next_rq[data_dir] == rq)
		zd->next_rq[data_dir] = zoned_latter_request(rq);

	elv_rb_del(zoned_rb_root(zd, rq), rq);
}

/*
 * remove rq from rbtree and fifo.
 */
static void zoned_remove_request(struct request_queue *q, struct request *rq)
{
	struct zoned_data *zd = q->elevator->elevator_data;

	list_del_init(&rq->queuelist);

	/*
	 * We might not be on the rbtree, if we are doing an insert merge
	 */
	if (!RB_EMPTY_NODE(&rq->rb_node))
		zoned_del_rq_rb(zd, rq);

	elv_rqhash_del(q, rq);
	if (q->last_merge == rq)
		q->last_merge = NULL;
}

static void zd_request_merged(struct request_queue *q, struct request *req,
			      enum elv_merge type)
{
	struct zoned_data *zd = q->elevator->elevator_data;

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(zoned_rb_root(zd, req), req);
		zoned_add_rq_rb(zd, req);
	}
}

static void zd_merged_requests(struct request_queue *q, struct request *req,
			       struct request *next)
{
	/*
	 * if next expires before rq, assign its expire time to rq
	 * and move into next position (next will be deleted) in fifo
	 */
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before((unsigned long)next->fifo_time,
				(unsigned long)req->fifo_time)) {
			list_move(&req->queuelist, &next->queuelist);
			req->fifo_time = next->fifo_time;
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	zoned_remove_request(q, next);
}

/*
 * Return true if a request is a write requests that needs zone
 * write locking.
 */
static inline bool zoned_request_needs_wlock(struct zoned_data *zd,
					     struct request *rq)
{
	unsigned int zno = sd_zbc_request_zone_no(zd->sdkp, rq);

	if (blk_rq_is_passthrough(rq))
		return false;

	if (!test_bit(zno, zd->seq_zones))
		return false;

	switch (req_op(rq)) {
	case REQ_OP_WRITE_ZEROES:
	case REQ_OP_WRITE_SAME:
	case REQ_OP_WRITE:
		return true;
	default:
		return false;
	}
}

/*
 * Abuse the elv.priv[0] pointer to indicate if a request
 * has locked its target zone.
 */
#define RQ_LOCKED_ZONE		((void *)1UL)
static inline void zoned_set_request_lock(struct request *rq)
{
	rq->elv.priv[0] = RQ_LOCKED_ZONE;
}

#define RQ_ZONE_NO_LOCK		((void *)0UL)
static inline void zoned_clear_request_lock(struct request *rq)
{
	rq->elv.priv[0] = RQ_ZONE_NO_LOCK;
}

static inline bool zoned_request_has_lock(struct request *rq)
{
	return rq->elv.priv[0] == RQ_LOCKED_ZONE;
}

/*
 * Write lock the target zone of a write request.
 */
static void zoned_wlock_request_zone(struct zoned_data *zd, struct request *rq)
{
	unsigned int zno = sd_zbc_request_zone_no(zd->sdkp, rq);

	WARN_ON_ONCE(zoned_request_has_lock(rq));
	WARN_ON_ONCE(test_and_set_bit(zno, zd->zones_wlock));
	zoned_set_request_lock(rq);
}

/*
 * Write unlock the target zone of a write request.
 */
static void zoned_wunlock_request_zone(struct zoned_data *zd,
				       struct request *rq)
{
	unsigned int zno = sd_zbc_request_zone_no(zd->sdkp, rq);
	unsigned long flags;

	/*
	 * Dispatch may be running on a different CPU.
	 * So do not unlock the zone until it is done or
	 * a write request in the middle of a sequence may end up
	 * being dispatched.
	 */
	spin_lock_irqsave(&zd->zones_lock, flags);

	WARN_ON_ONCE(!test_and_clear_bit(zno, zd->zones_wlock));
	zoned_clear_request_lock(rq);

	spin_unlock_irqrestore(&zd->zones_lock, flags);
}

/*
 * Test the write lock state of the target zone of a write request.
 */
static inline bool zoned_request_zone_is_wlocked(struct zoned_data *zd,
						 struct request *rq)
{
	unsigned int zno = sd_zbc_request_zone_no(zd->sdkp, rq);

	return test_bit(zno, zd->zones_wlock);
}

/*
 * move an entry to dispatch queue
 */
static void zoned_move_request(struct zoned_data *zd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	zd->next_rq[READ] = NULL;
	zd->next_rq[WRITE] = NULL;
	zd->next_rq[data_dir] = zoned_latter_request(rq);

	/*
	 * take it off the sort and fifo list
	 */
	zoned_remove_request(rq->q, rq);
}

/*
 * zoned_check_fifo returns 0 if there are no expired requests on the fifo,
 * 1 otherwise. Requires !list_empty(&zd->fifo_list[data_dir])
 */
static inline int zoned_check_fifo(struct zoned_data *zd, int ddir)
{
	struct request *rq = rq_entry_fifo(zd->fifo_list[ddir].next);

	/*
	 * rq is expired!
	 */
	if (time_after_eq(jiffies, (unsigned long)rq->fifo_time))
		return 1;

	return 0;
}

/*
 * Test if a request can be dispatched.
 */
static inline bool zoned_can_dispatch_request(struct zoned_data *zd,
					      struct request *rq)
{
	return !zoned_request_needs_wlock(zd, rq) ||
		!zoned_request_zone_is_wlocked(zd, rq);
}

/*
 * For the specified data direction, find the next request that can be
 * dispatched. Search in increasing sector position.
 */
static struct request *
zoned_next_request(struct zoned_data *zd, int data_dir)
{
	struct request *rq = zd->next_rq[data_dir];
	unsigned long flags;

	if (data_dir == READ)
		return rq;

	spin_lock_irqsave(&zd->zones_lock, flags);
	while (rq) {
		if (zoned_can_dispatch_request(zd, rq))
			break;
		rq = zoned_latter_request(rq);
	}
	spin_unlock_irqrestore(&zd->zones_lock, flags);

	return rq;
}

/*
 * For the specified data direction, find the next request that can be
 * dispatched. Search in arrival order from the oldest request.
 */
static struct request *
zoned_fifo_request(struct zoned_data *zd, int data_dir)
{
	struct request *rq;
	unsigned long flags;

	if (list_empty(&zd->fifo_list[data_dir]))
		return NULL;

	if (data_dir == READ)
		return rq_entry_fifo(zd->fifo_list[READ].next);

	spin_lock_irqsave(&zd->zones_lock, flags);

	list_for_each_entry(rq, &zd->fifo_list[WRITE], queuelist) {
		if (zoned_can_dispatch_request(zd, rq))
			goto out;
	}
	rq = NULL;

out:
	spin_unlock_irqrestore(&zd->zones_lock, flags);

	return rq;
}

/*
 * __zd_dispatch_request selects the best request according to
 * read/write batch expiration, fifo_batch, target zone lock state, etc
 */
static struct request *__zd_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct zoned_data *zd = hctx->queue->elevator->elevator_data;
	struct request *rq = NULL, *next_rq;
	bool reads, writes;
	int data_dir;

	if (!list_empty(&zd->dispatch)) {
		rq = list_first_entry(&zd->dispatch, struct request, queuelist);
		list_del_init(&rq->queuelist);
		goto done;
	}

	reads = !list_empty(&zd->fifo_list[READ]);
	writes = !list_empty(&zd->fifo_list[WRITE]);

	/*
	 * batches are currently reads XOR writes
	 */
	rq = zoned_next_request(zd, WRITE);
	if (!rq)
		rq = zoned_next_request(zd, READ);
	if (rq && zd->batching < zd->fifo_batch)
		/* we have a next request are still entitled to batch */
		goto dispatch_request;

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */

	if (reads) {
		if (writes && (zd->starved++ >= zd->writes_starved))
			goto dispatch_writes;

		data_dir = READ;

		goto dispatch_find_request;
	}

	/*
	 * there are either no reads or writes have been starved
	 */

	if (writes) {
dispatch_writes:
		zd->starved = 0;

		/* Really select writes if at least one can be dispatched */
		if (zoned_fifo_request(zd, WRITE))
			data_dir = WRITE;
		else
			data_dir = READ;

		goto dispatch_find_request;
	}

	return NULL;

dispatch_find_request:
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	next_rq = zoned_next_request(zd, data_dir);
	if (zoned_check_fifo(zd, data_dir) || !next_rq) {
		/*
		 * A deadline has expired, the last request was in the other
		 * direction, or we have run out of higher-sectored requests.
		 * Start again from the request with the earliest expiry time.
		 */
		rq = zoned_fifo_request(zd, data_dir);
	} else {
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		rq = next_rq;
	}

	if (!rq)
		return NULL;

	zd->batching = 0;

dispatch_request:
	/*
	 * rq is the selected appropriate request.
	 */
	zd->batching++;
	zoned_move_request(zd, rq);

done:
	/*
	 * If the request needs its target zone locked, do it.
	 */
	if (zoned_request_needs_wlock(zd, rq))
		zoned_wlock_request_zone(zd, rq);
	rq->rq_flags |= RQF_STARTED;
	return rq;
}

static struct request *zd_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct zoned_data *zd = hctx->queue->elevator->elevator_data;
	struct request *rq;

	spin_lock(&zd->lock);
	rq = __zd_dispatch_request(hctx);
	spin_unlock(&zd->lock);

	return rq;
}

static int zd_request_merge(struct request_queue *q, struct request **rq,
			    struct bio *bio)
{
	struct zoned_data *zd = q->elevator->elevator_data;
	sector_t sector = bio_end_sector(bio);
	struct request *__rq;

	if (!zd->front_merges)
		return ELEVATOR_NO_MERGE;

	__rq = elv_rb_find(&zd->sort_list[bio_data_dir(bio)], sector);
	if (__rq) {
		if (WARN_ON(sector != blk_rq_pos(__rq)))
			return ELEVATOR_NO_MERGE;

		if (elv_bio_merge_ok(__rq, bio)) {
			*rq = __rq;
			return ELEVATOR_FRONT_MERGE;
		}
	}

	return ELEVATOR_NO_MERGE;
}

static bool zd_bio_merge(struct blk_mq_hw_ctx *hctx, struct bio *bio)
{
	struct request_queue *q = hctx->queue;
	struct zoned_data *zd = q->elevator->elevator_data;
	struct request *free = NULL;
	bool ret;

	spin_lock(&zd->lock);
	ret = blk_mq_sched_try_merge(q, bio, &free);
	spin_unlock(&zd->lock);

	if (free)
		blk_mq_free_request(free);

	return ret;
}

/*
 * add rq to rbtree and fifo
 */
static void __zd_insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
				bool at_head)
{
	struct request_queue *q = hctx->queue;
	struct zoned_data *zd = q->elevator->elevator_data;
	const int data_dir = rq_data_dir(rq);

	if (blk_mq_sched_try_insert_merge(q, rq))
		return;

	blk_mq_sched_request_inserted(rq);

	if (at_head || blk_rq_is_passthrough(rq)) {
		if (at_head)
			list_add(&rq->queuelist, &zd->dispatch);
		else
			list_add_tail(&rq->queuelist, &zd->dispatch);
	} else {
		zoned_add_rq_rb(zd, rq);

		if (rq_mergeable(rq)) {
			elv_rqhash_add(q, rq);
			if (!q->last_merge)
				q->last_merge = rq;
		}

		/*
		 * set expire time and add to fifo list
		 */
		rq->fifo_time = jiffies + zd->fifo_expire[data_dir];
		list_add_tail(&rq->queuelist, &zd->fifo_list[data_dir]);
	}
}

static void zd_insert_requests(struct blk_mq_hw_ctx *hctx,
			       struct list_head *list, bool at_head)
{
	struct request_queue *q = hctx->queue;
	struct zoned_data *zd = q->elevator->elevator_data;

	spin_lock(&zd->lock);
	while (!list_empty(list)) {
		struct request *rq;

		rq = list_first_entry(list, struct request, queuelist);
		list_del_init(&rq->queuelist);

		/*
		 * This may be a requeue of a request that has locked its
		 * target zone. If this is the case, release the zone lock.
		 */
		if (zoned_request_has_lock(rq))
			zoned_wunlock_request_zone(zd, rq);

		__zd_insert_request(hctx, rq, at_head);
	}
	spin_unlock(&zd->lock);
}

/*
 * Write unlock the target zone of a completed write request.
 */
static void zd_completed_request(struct request *rq)
{

	if (zoned_request_has_lock(rq)) {
		struct zoned_data *zd = rq->q->elevator->elevator_data;

		zoned_wunlock_request_zone(zd, rq);
	}
}

static bool zd_has_work(struct blk_mq_hw_ctx *hctx)
{
	struct zoned_data *zd = hctx->queue->elevator->elevator_data;

	return !list_empty_careful(&zd->dispatch) ||
		!list_empty_careful(&zd->fifo_list[0]) ||
		!list_empty_careful(&zd->fifo_list[1]);
}

static struct scsi_disk *zoned_lookup_disk(struct request_queue *q)
{
	struct scsi_disk *sdkp;

	if (!blk_queue_is_zoned(q)) {
		pr_err("zoned: Not a zoned block device\n");
		return NULL;
	}

	sdkp = scsi_disk_from_queue(q);
	if (!sdkp) {
		pr_err("zoned: Not a SCSI disk\n");
		return NULL;
	}

	/* Paranoia check */
	if (WARN_ON(sdkp->disk->queue != q))
		return NULL;

	return sdkp;
}

/*
 * Initialize elevator private data (zoned_data).
 */
static int zd_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct scsi_disk *sdkp;
	struct zoned_data *zd;
	struct elevator_queue *eq;
	int ret;

	sdkp = zoned_lookup_disk(q);
	if (!sdkp)
		return -ENODEV;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	zd = kzalloc_node(sizeof(*zd), GFP_KERNEL, q->node);
	if (!zd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&zd->fifo_list[READ]);
	INIT_LIST_HEAD(&zd->fifo_list[WRITE]);
	zd->sort_list[READ] = RB_ROOT;
	zd->sort_list[WRITE] = RB_ROOT;
	zd->fifo_expire[READ] = read_expire;
	zd->fifo_expire[WRITE] = write_expire;
	zd->writes_starved = writes_starved;
	zd->front_merges = 1;
	zd->fifo_batch = fifo_batch;
	spin_lock_init(&zd->lock);
	INIT_LIST_HEAD(&zd->dispatch);

	zd->sdkp = sdkp;
	spin_lock_init(&zd->zones_lock);

	zd->zones_wlock = sdkp->zones_wlock;
	zd->seq_zones = sdkp->seq_zones;
	if (!zd->zones_wlock || !zd->seq_zones) {
		ret = -ENOMEM;
		goto err;
	}

	eq->elevator_data = zd;
	q->elevator = eq;

	return 0;

err:
	kfree(zd);
	kobject_put(&eq->kobj);

	return ret;
}

static void zd_exit_queue(struct elevator_queue *e)
{
	struct zoned_data *zd = e->elevator_data;

	WARN_ON(!list_empty(&zd->fifo_list[READ]));
	WARN_ON(!list_empty(&zd->fifo_list[WRITE]));

	kfree(zd);
}

/*
 * sysfs parts below
 */
static ssize_t
zoned_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
zoned_var_store(int *var, const char *page, size_t count)
{
	char *p = (char *) page;
	int ret;

	ret = kstrtoint(p, 10, var);
	if (ret)
		return ret;

	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct zoned_data *zd = e->elevator_data;			\
	int __data = __VAR;						\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return zoned_var_show(__data, (page));			\
}
SHOW_FUNCTION(zoned_read_expire_show, zd->fifo_expire[READ], 1);
SHOW_FUNCTION(zoned_write_expire_show, zd->fifo_expire[WRITE], 1);
SHOW_FUNCTION(zoned_writes_starved_show, zd->writes_starved, 0);
SHOW_FUNCTION(zoned_front_merges_show, zd->front_merges, 0);
SHOW_FUNCTION(zoned_fifo_batch_show, zd->fifo_batch, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e,				\
		      const char *page, size_t count)			\
{									\
	struct zoned_data *zd = e->elevator_data;			\
	int __data;							\
	int ret = zoned_var_store(&__data, (page), count);		\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	if (__CONV)							\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else								\
		*(__PTR) = __data;					\
	return ret;							\
}
STORE_FUNCTION(zoned_read_expire_store, &zd->fifo_expire[READ],
	       0, INT_MAX, 1);
STORE_FUNCTION(zoned_write_expire_store, &zd->fifo_expire[WRITE],
	       0, INT_MAX, 1);
STORE_FUNCTION(zoned_writes_starved_store, &zd->writes_starved,
	       INT_MIN, INT_MAX, 0);
STORE_FUNCTION(zoned_front_merges_store, &zd->front_merges,
	       0, 1, 0);
STORE_FUNCTION(zoned_fifo_batch_store, &zd->fifo_batch,
	       0, INT_MAX, 0);
#undef STORE_FUNCTION

#define DD_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, zoned_##name##_show, \
				      zoned_##name##_store)

static struct elv_fs_entry zoned_attrs[] = {
	DD_ATTR(read_expire),
	DD_ATTR(write_expire),
	DD_ATTR(writes_starved),
	DD_ATTR(front_merges),
	DD_ATTR(fifo_batch),
	__ATTR_NULL
};

#ifdef CONFIG_BLK_DEBUG_FS
#define ZONED_DEBUGFS_DDIR_ATTRS(ddir, name)				\
static void *zoned_##name##_fifo_start(struct seq_file *m,		\
					  loff_t *pos)			\
	__acquires(&zd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct zoned_data *zd = q->elevator->elevator_data;		\
									\
	spin_lock(&zd->lock);						\
	return seq_list_start(&zd->fifo_list[ddir], *pos);		\
}									\
									\
static void *zoned_##name##_fifo_next(struct seq_file *m, void *v,	\
					 loff_t *pos)			\
{									\
	struct request_queue *q = m->private;				\
	struct zoned_data *zd = q->elevator->elevator_data;		\
									\
	return seq_list_next(v, &zd->fifo_list[ddir], pos);		\
}									\
									\
static void zoned_##name##_fifo_stop(struct seq_file *m, void *v)	\
	__releases(&zd->lock)						\
{									\
	struct request_queue *q = m->private;				\
	struct zoned_data *zd = q->elevator->elevator_data;		\
									\
	spin_unlock(&zd->lock);						\
}									\
									\
static const struct seq_operations zoned_##name##_fifo_seq_ops = {	\
	.start	= zoned_##name##_fifo_start,				\
	.next	= zoned_##name##_fifo_next,				\
	.stop	= zoned_##name##_fifo_stop,				\
	.show	= blk_mq_debugfs_rq_show,				\
};									\
									\
static int zoned_##name##_next_rq_show(void *data,			\
					  struct seq_file *m)		\
{									\
	struct request_queue *q = data;					\
	struct zoned_data *zd = q->elevator->elevator_data;		\
	struct request *rq = zd->next_rq[ddir];				\
									\
	if (rq)								\
		__blk_mq_debugfs_rq_show(m, rq);			\
	return 0;							\
}
ZONED_DEBUGFS_DDIR_ATTRS(READ, read)
ZONED_DEBUGFS_DDIR_ATTRS(WRITE, write)
#undef ZONED_DEBUGFS_DDIR_ATTRS

static int zoned_batching_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct zoned_data *zd = q->elevator->elevator_data;

	seq_printf(m, "%u\n", zd->batching);
	return 0;
}

static int zoned_starved_show(void *data, struct seq_file *m)
{
	struct request_queue *q = data;
	struct zoned_data *zd = q->elevator->elevator_data;

	seq_printf(m, "%u\n", zd->starved);
	return 0;
}

static void *zoned_dispatch_start(struct seq_file *m, loff_t *pos)
	__acquires(&zd->lock)
{
	struct request_queue *q = m->private;
	struct zoned_data *zd = q->elevator->elevator_data;

	spin_lock(&zd->lock);
	return seq_list_start(&zd->dispatch, *pos);
}

static void *zoned_dispatch_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct request_queue *q = m->private;
	struct zoned_data *zd = q->elevator->elevator_data;

	return seq_list_next(v, &zd->dispatch, pos);
}

static void zoned_dispatch_stop(struct seq_file *m, void *v)
	__releases(&zd->lock)
{
	struct request_queue *q = m->private;
	struct zoned_data *zd = q->elevator->elevator_data;

	spin_unlock(&zd->lock);
}

static const struct seq_operations zoned_dispatch_seq_ops = {
	.start	= zoned_dispatch_start,
	.next	= zoned_dispatch_next,
	.stop	= zoned_dispatch_stop,
	.show	= blk_mq_debugfs_rq_show,
};

#define ZONED_QUEUE_DDIR_ATTRS(name)					     \
	{#name "_fifo_list", 0400, .seq_ops = &zoned_##name##_fifo_seq_ops}, \
	{#name "_next_rq", 0400, zoned_##name##_next_rq_show}
static const struct blk_mq_debugfs_attr zoned_queue_debugfs_attrs[] = {
	ZONED_QUEUE_DDIR_ATTRS(read),
	ZONED_QUEUE_DDIR_ATTRS(write),
	{"batching", 0400, zoned_batching_show},
	{"starved", 0400, zoned_starved_show},
	{"dispatch", 0400, .seq_ops = &zoned_dispatch_seq_ops},
	{},
};
#undef ZONED_QUEUE_DDIR_ATTRS
#endif

static struct elevator_type zoned_elv = {
	.ops.mq = {
		.insert_requests	= zd_insert_requests,
		.dispatch_request	= zd_dispatch_request,
		.completed_request	= zd_completed_request,
		.next_request		= elv_rb_latter_request,
		.former_request		= elv_rb_former_request,
		.bio_merge		= zd_bio_merge,
		.request_merge		= zd_request_merge,
		.requests_merged	= zd_merged_requests,
		.request_merged		= zd_request_merged,
		.has_work		= zd_has_work,
		.init_sched		= zd_init_queue,
		.exit_sched		= zd_exit_queue,
	},

	.uses_mq	= true,
#ifdef CONFIG_BLK_DEBUG_FS
	.queue_debugfs_attrs = zoned_queue_debugfs_attrs,
#endif
	.elevator_attrs = zoned_attrs,
	.elevator_name = "zoned",
	.elevator_owner = THIS_MODULE,
};

static int __init zoned_init(void)
{
	return elv_register(&zoned_elv);
}

static void __exit zoned_exit(void)
{
	elv_unregister(&zoned_elv);
}

module_init(zoned_init);
module_exit(zoned_exit);

MODULE_AUTHOR("Damien Le Moal");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Zoned MQ deadline IO scheduler");
