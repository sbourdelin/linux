/*
 *  CFQ, or complete fairness queueing, disk scheduler.
 *
 *  Based on ideas from a previously unfinished io
 *  scheduler (round robin per-process disk scheduling) and Andrea Arcangeli.
 *
 *  Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/jiffies.h>
#include <linux/rbtree.h>
#include <linux/ioprio.h>
#include <linux/blktrace_api.h>
#include "blk.h"

/*
 * tunables
 */
/* max queue in one round of service */
static const int cfq_quantum = 8;
static const int cfq_fifo_expire[2] = { HZ / 4, HZ / 8 };
/* maximum backwards seek, in KiB */
static const int cfq_back_max = 16 * 1024;
/* penalty of a backwards seek */
static const int cfq_back_penalty = 2;
static const int cfq_slice_sync = HZ / 10;
static int cfq_slice_async = HZ / 25;
static const int cfq_slice_async_rq = 2;
static int cfq_slice_idle = HZ / 125;
static const int cfq_target_latency = HZ * 3/10; /* 300 ms */
static const int cfq_hist_divisor = 4;

/*
 * offset from end of service tree
 */
#define CFQ_IDLE_DELAY		(HZ / 5)

/*
 * below this threshold, we consider thinktime immediate
 */
#define CFQ_MIN_TT		(2)

#define CFQ_SLICE_SCALE		(5)
#define CFQ_HW_QUEUE_MIN	(5)
#define CFQ_SERVICE_SHIFT       12

#define CFQQ_SEEK_THR		(sector_t)(8 * 100)
#define CFQQ_CLOSE_THR		(sector_t)(8 * 1024)
#define CFQQ_SEEKY(cfqq)	(hweight32(cfqq->seek_history) > 32/8)

#define RQ_CIC(rq)		icq_to_cic((rq)->elv.icq)
#define RQ_CFQQ(rq)		(struct cfq_queue *) ((rq)->elv.priv[0])

static struct kmem_cache *cfq_pool;

#define CFQ_PRIO_LISTS		IOPRIO_BE_NR
#define cfq_class_idle(cfqq)	((cfqq)->ioprio_class == IOPRIO_CLASS_IDLE)
#define cfq_class_rt(cfqq)	((cfqq)->ioprio_class == IOPRIO_CLASS_RT)

#define sample_valid(samples)	((samples) > 80)

struct cfq_ttime {
	unsigned long last_end_request;

	unsigned long ttime_total;
	unsigned long ttime_samples;
	unsigned long ttime_mean;
};

/*
 * Most of our rbtree usage is for sorting with min extraction, so
 * if we cache the leftmost node we don't have to walk down the tree
 * to find it. Idea borrowed from Ingo Molnars CFS scheduler. We should
 * move this into the elevator for the rq sorting as well.
 */
struct cfq_rb_root {
	struct rb_root rb;
	struct rb_node *left;
	unsigned count;
	u64 min_vdisktime;
	struct cfq_ttime ttime;
};
#define CFQ_RB_ROOT	(struct cfq_rb_root) { .rb = RB_ROOT, \
			.ttime = {.last_end_request = jiffies,},}

/*
 * Per process-grouping structure
 */
struct cfq_queue {
	/* reference count */
	int ref;
	/* various state flags, see below */
	unsigned int flags;
	/* parent cfq_data */
	struct cfq_data *cfqd;
	/* service_tree member */
	struct rb_node rb_node;
	/* service_tree key */
	unsigned long rb_key;
	/* prio tree member */
	struct rb_node p_node;
	/* prio tree root we belong to, if any */
	struct rb_root *p_root;
	/* sorted list of pending requests */
	struct rb_root sort_list;
	/* if fifo isn't expired, next request to serve */
	struct request *next_rq;
	/* requests queued in sort_list */
	int queued[2];
	/* currently allocated requests */
	int allocated[2];
	/* fifo list of requests in sort_list */
	struct list_head fifo;

	/* time when queue got scheduled in to dispatch first request. */
	unsigned long dispatch_start;
	unsigned int allocated_slice;
	unsigned int slice_dispatch;
	/* time when first request from queue completed and slice started. */
	unsigned long slice_start;
	unsigned long slice_end;
	long slice_resid;

	/* pending priority requests */
	int prio_pending;
	/* number of requests that are on the dispatch list or inside driver */
	int dispatched;

	/* io prio of this group */
	unsigned short ioprio, org_ioprio;
	unsigned short ioprio_class;

	pid_t pid;

	u32 seek_history;
	sector_t last_request_pos;

	struct cfq_rb_root *service_tree;
	struct cfq_queue *new_cfqq;
	/* Number of sectors dispatched from queue in single dispatch round */
	unsigned long nr_sectors;
};

/*
 * First index in the service_trees.
 * IDLE is handled separately, so it has negative index
 */
enum wl_class_t {
	BE_WORKLOAD = 0,
	RT_WORKLOAD = 1,
	IDLE_WORKLOAD = 2,
	CFQ_PRIO_NR,
};

struct cfq_io_cq {
	struct io_cq		icq;		/* must be the first member */
	struct cfq_queue	*cfqq[2];
	struct cfq_ttime	ttime;
	int			ioprio;		/* the current ioprio */
};

/*
 * Per block device queue structure
 */
struct cfq_data {
	struct request_queue *queue;

	/*
	 * rr lists of queues with requests. We maintain service trees for
	 * RT and BE classes.
	 * Counts are embedded in the cfq_rb_root
	 */
	struct cfq_rb_root service_trees[2];
	struct cfq_rb_root service_tree_idle;

	/*
	 * The priority currently being served
	 */
	enum wl_class_t serving_wl_class;
	unsigned long workload_expires;

	unsigned int busy_queues;
	unsigned int busy_sync_queues;

	int rq_in_driver;
	int rq_in_flight[2];

	/*
	 * queue-depth detection
	 */
	int rq_queued;
	int hw_tag;
	/*
	 * hw_tag can be
	 * -1 => indeterminate, (cfq will behave as if NCQ is present, to allow better detection)
	 *  1 => NCQ is present (hw_tag_est_depth is the estimated max depth)
	 *  0 => no NCQ
	 */
	int hw_tag_est_depth;
	unsigned int hw_tag_samples;

	/*
	 * idle window management
	 */
	struct timer_list idle_slice_timer;
	struct work_struct unplug_work;

	struct cfq_queue *active_queue;
	struct cfq_io_cq *active_cic;

	/* async queue for each priority case */
	struct cfq_queue *async_cfqq[2][IOPRIO_BE_NR];
	struct cfq_queue *async_idle_cfqq;

	sector_t last_position;

	/*
	 * tunables, see top of file
	 */
	unsigned int cfq_quantum;
	unsigned int cfq_fifo_expire[2];
	unsigned int cfq_back_penalty;
	unsigned int cfq_back_max;
	unsigned int cfq_slice[2];
	unsigned int cfq_slice_async_rq;
	unsigned int cfq_slice_idle;
	unsigned int cfq_latency;
	unsigned int cfq_target_latency;

	/*
	 * Fallback dummy cfqq for extreme OOM conditions
	 */
	struct cfq_queue oom_cfqq;

	unsigned long last_delayed_sync;
};

enum cfqq_state_flags {
	CFQ_CFQQ_FLAG_on_rr = 0,	/* on round-robin busy list */
	CFQ_CFQQ_FLAG_wait_request,	/* waiting for a request */
	CFQ_CFQQ_FLAG_must_dispatch,	/* must be allowed a dispatch */
	CFQ_CFQQ_FLAG_must_alloc_slice,	/* per-slice must_alloc flag */
	CFQ_CFQQ_FLAG_fifo_expire,	/* FIFO checked in this slice */
	CFQ_CFQQ_FLAG_idle_window,	/* slice idling enabled */
	CFQ_CFQQ_FLAG_prio_changed,	/* task priority has changed */
	CFQ_CFQQ_FLAG_slice_new,	/* no requests dispatched in slice */
	CFQ_CFQQ_FLAG_sync,		/* synchronous queue */
	CFQ_CFQQ_FLAG_wait_busy,	/* Waiting for next request */
};

#define CFQ_CFQQ_FNS(name)						\
static inline void cfq_mark_cfqq_##name(struct cfq_queue *cfqq)		\
{									\
	(cfqq)->flags |= (1 << CFQ_CFQQ_FLAG_##name);			\
}									\
static inline void cfq_clear_cfqq_##name(struct cfq_queue *cfqq)	\
{									\
	(cfqq)->flags &= ~(1 << CFQ_CFQQ_FLAG_##name);			\
}									\
static inline int cfq_cfqq_##name(const struct cfq_queue *cfqq)		\
{									\
	return ((cfqq)->flags & (1 << CFQ_CFQQ_FLAG_##name)) != 0;	\
}

CFQ_CFQQ_FNS(on_rr);
CFQ_CFQQ_FNS(wait_request);
CFQ_CFQQ_FNS(must_dispatch);
CFQ_CFQQ_FNS(must_alloc_slice);
CFQ_CFQQ_FNS(fifo_expire);
CFQ_CFQQ_FNS(idle_window);
CFQ_CFQQ_FNS(prio_changed);
CFQ_CFQQ_FNS(slice_new);
CFQ_CFQQ_FNS(sync);
CFQ_CFQQ_FNS(wait_busy);
#undef CFQ_CFQQ_FNS

#define cfq_log_cfqq(cfqd, cfqq, fmt, args...)	\
	blk_add_trace_msg((cfqd)->queue, "cfq%d%c " fmt, (cfqq)->pid,	\
			cfq_cfqq_sync((cfqq)) ? 'S' : 'A',		\
				##args)
#define cfq_log(cfqd, fmt, args...)	\
	blk_add_trace_msg((cfqd)->queue, "cfq " fmt, ##args)

/* Traverses through cfq service trees */
#define for_each_st(cfqd, i, j, st) \
	for (i = 0; i <= IDLE_WORKLOAD; i++) \
		for (j = 0, st = i < IDLE_WORKLOAD ? &cfqd->service_trees[i]\
			: &cfqd->service_tree_idle; \
			(i < IDLE_WORKLOAD) || \
			(i == IDLE_WORKLOAD); \
			st = i < IDLE_WORKLOAD ? \
			&cfqd->service_trees[i] : NULL) \

static inline bool cfq_io_thinktime_big(struct cfq_data *cfqd,
	struct cfq_ttime *ttime)
{
	unsigned long slice;
	if (!sample_valid(ttime->ttime_samples))
		return false;
	slice = cfqd->cfq_slice_idle;
	return ttime->ttime_mean > slice;
}

static inline enum wl_class_t cfqq_class(struct cfq_queue *cfqq)
{
	if (cfq_class_idle(cfqq))
		return IDLE_WORKLOAD;
	if (cfq_class_rt(cfqq))
		return RT_WORKLOAD;
	return BE_WORKLOAD;
}

static void cfq_dispatch_insert(struct request_queue *, struct request *);
static struct cfq_queue *cfq_get_queue(struct cfq_data *cfqd, bool is_sync,
				       struct cfq_io_cq *cic, struct bio *bio);

static inline struct cfq_io_cq *icq_to_cic(struct io_cq *icq)
{
	/* cic->icq is the first member, %NULL will convert to %NULL */
	return container_of(icq, struct cfq_io_cq, icq);
}

static inline struct cfq_io_cq *cfq_cic_lookup(struct cfq_data *cfqd,
					       struct io_context *ioc)
{
	if (ioc)
		return icq_to_cic(ioc_lookup_icq(ioc, cfqd->queue));
	return NULL;
}

static inline struct cfq_queue *cic_to_cfqq(struct cfq_io_cq *cic, bool is_sync)
{
	return cic->cfqq[is_sync];
}

static inline void cic_set_cfqq(struct cfq_io_cq *cic, struct cfq_queue *cfqq,
				bool is_sync)
{
	cic->cfqq[is_sync] = cfqq;
}

static inline struct cfq_data *cic_to_cfqd(struct cfq_io_cq *cic)
{
	return cic->icq.q->elevator->elevator_data;
}

/*
 * We regard a request as SYNC, if it's either a read or has the SYNC bit
 * set (in which case it could also be direct WRITE).
 */
static inline bool cfq_bio_sync(struct bio *bio)
{
	return bio_data_dir(bio) == READ || (bio->bi_rw & REQ_SYNC);
}

/*
 * scheduler run of queue, if there are requests pending and no one in the
 * driver that will restart queueing
 */
static inline void cfq_schedule_dispatch(struct cfq_data *cfqd)
{
	if (cfqd->busy_queues) {
		cfq_log(cfqd, "schedule dispatch");
		kblockd_schedule_work(&cfqd->unplug_work);
	}
}

/*
 * Scale schedule slice based on io priority. Use the sync time slice only
 * if a queue is marked sync and has sync io queued. A sync queue with async
 * io only, should not get full sync slice length.
 */
static inline int cfq_prio_slice(struct cfq_data *cfqd, bool sync,
				 unsigned short prio)
{
	const int base_slice = cfqd->cfq_slice[sync];

	WARN_ON(prio >= IOPRIO_BE_NR);

	return base_slice + (base_slice/CFQ_SLICE_SCALE * (4 - prio));
}

static inline int
cfq_prio_to_slice(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	return cfq_prio_slice(cfqd, cfq_cfqq_sync(cfqq), cfqq->ioprio);
}

static inline u64 max_vdisktime(u64 min_vdisktime, u64 vdisktime)
{
	s64 delta = (s64)(vdisktime - min_vdisktime);
	if (delta > 0)
		min_vdisktime = vdisktime;

	return min_vdisktime;
}

static inline u64 min_vdisktime(u64 min_vdisktime, u64 vdisktime)
{
	s64 delta = (s64)(vdisktime - min_vdisktime);
	if (delta < 0)
		min_vdisktime = vdisktime;

	return min_vdisktime;
}

static inline unsigned
cfq_scaled_cfqq_slice(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	return cfq_prio_to_slice(cfqd, cfqq);
}

static inline void
cfq_set_prio_slice(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	unsigned slice = cfq_scaled_cfqq_slice(cfqd, cfqq);

	cfqq->slice_start = jiffies;
	cfqq->slice_end = jiffies + slice;
	cfqq->allocated_slice = slice;
	cfq_log_cfqq(cfqd, cfqq, "set_slice=%lu", cfqq->slice_end - jiffies);
}

/*
 * We need to wrap this check in cfq_cfqq_slice_new(), since ->slice_end
 * isn't valid until the first request from the dispatch is activated
 * and the slice time set.
 */
static inline bool cfq_slice_used(struct cfq_queue *cfqq)
{
	if (cfq_cfqq_slice_new(cfqq))
		return false;
	if (time_before(jiffies, cfqq->slice_end))
		return false;

	return true;
}

/*
 * Lifted from AS - choose which of rq1 and rq2 that is best served now.
 * We choose the request that is closest to the head right now. Distance
 * behind the head is penalized and only allowed to a certain extent.
 */
static struct request *
cfq_choose_req(struct cfq_data *cfqd, struct request *rq1, struct request *rq2, sector_t last)
{
	sector_t s1, s2, d1 = 0, d2 = 0;
	unsigned long back_max;
#define CFQ_RQ1_WRAP	0x01 /* request 1 wraps */
#define CFQ_RQ2_WRAP	0x02 /* request 2 wraps */
	unsigned wrap = 0; /* bit mask: requests behind the disk head? */

	if (rq1 == NULL || rq1 == rq2)
		return rq2;
	if (rq2 == NULL)
		return rq1;

	if (rq_is_sync(rq1) != rq_is_sync(rq2))
		return rq_is_sync(rq1) ? rq1 : rq2;

	if ((rq1->cmd_flags ^ rq2->cmd_flags) & REQ_PRIO)
		return rq1->cmd_flags & REQ_PRIO ? rq1 : rq2;

	s1 = blk_rq_pos(rq1);
	s2 = blk_rq_pos(rq2);

	/*
	 * by definition, 1KiB is 2 sectors
	 */
	back_max = cfqd->cfq_back_max * 2;

	/*
	 * Strict one way elevator _except_ in the case where we allow
	 * short backward seeks which are biased as twice the cost of a
	 * similar forward seek.
	 */
	if (s1 >= last)
		d1 = s1 - last;
	else if (s1 + back_max >= last)
		d1 = (last - s1) * cfqd->cfq_back_penalty;
	else
		wrap |= CFQ_RQ1_WRAP;

	if (s2 >= last)
		d2 = s2 - last;
	else if (s2 + back_max >= last)
		d2 = (last - s2) * cfqd->cfq_back_penalty;
	else
		wrap |= CFQ_RQ2_WRAP;

	/* Found required data */

	/*
	 * By doing switch() on the bit mask "wrap" we avoid having to
	 * check two variables for all permutations: --> faster!
	 */
	switch (wrap) {
	case 0: /* common case for CFQ: rq1 and rq2 not wrapped */
		if (d1 < d2)
			return rq1;
		else if (d2 < d1)
			return rq2;
		if (s1 >= s2)
			return rq1;
		else
			return rq2;

	case CFQ_RQ2_WRAP:
		return rq1;
	case CFQ_RQ1_WRAP:
		return rq2;
	case (CFQ_RQ1_WRAP|CFQ_RQ2_WRAP): /* both rqs wrapped */
	default:
		/*
		 * Since both rqs are wrapped,
		 * start with the one that's further behind head
		 * (--> only *one* back seek required),
		 * since back seek takes more time than forward.
		 */
		if (s1 <= s2)
			return rq1;
		else
			return rq2;
	}
}

/*
 * The below is leftmost cache rbtree addon
 */
static struct cfq_queue *cfq_rb_first(struct cfq_rb_root *root)
{
	/* Service tree is empty */
	if (!root->count)
		return NULL;

	if (!root->left)
		root->left = rb_first(&root->rb);

	if (root->left)
		return rb_entry(root->left, struct cfq_queue, rb_node);

	return NULL;
}

static void rb_erase_init(struct rb_node *n, struct rb_root *root)
{
	rb_erase(n, root);
	RB_CLEAR_NODE(n);
}

static void cfq_rb_erase(struct rb_node *n, struct cfq_rb_root *root)
{
	if (root->left == n)
		root->left = NULL;
	rb_erase_init(n, &root->rb);
	--root->count;
}

/*
 * would be nice to take fifo expire time into account as well
 */
static struct request *
cfq_find_next_rq(struct cfq_data *cfqd, struct cfq_queue *cfqq,
		  struct request *last)
{
	struct rb_node *rbnext = rb_next(&last->rb_node);
	struct rb_node *rbprev = rb_prev(&last->rb_node);
	struct request *next = NULL, *prev = NULL;

	if (rbprev)
		prev = rb_entry_rq(rbprev);

	if (rbnext)
		next = rb_entry_rq(rbnext);
	else {
		rbnext = rb_first(&cfqq->sort_list);
		if (rbnext && rbnext != &last->rb_node)
			next = rb_entry_rq(rbnext);
	}

	return cfq_choose_req(cfqd, next, prev, blk_rq_pos(last));
}

static unsigned long cfq_slice_offset(struct cfq_data *cfqd,
				      struct cfq_queue *cfqq)
{
	/*
	 * just an approximation, should be ok.
	 */
	return (cfqd->busy_queues - 1) * (cfq_prio_slice(cfqd, 1, 0) -
		       cfq_prio_slice(cfqd, cfq_cfqq_sync(cfqq), cfqq->ioprio));
}

static inline unsigned int cfq_cfqq_slice_usage(struct cfq_queue *cfqq,
						unsigned int *unaccounted_time)
{
	unsigned int slice_used;

	/*
	 * Queue got expired before even a single request completed or
	 * got expired immediately after first request completion.
	 */
	if (!cfqq->slice_start ||
	    time_in_range(cfqq->slice_start, jiffies, jiffies))
		slice_used = max_t(unsigned, (jiffies - cfqq->dispatch_start),
					1);
	else {
		slice_used = jiffies - cfqq->slice_start;
		if (slice_used > cfqq->allocated_slice) {
			*unaccounted_time = slice_used - cfqq->allocated_slice;
			slice_used = cfqq->allocated_slice;
		}
		if (time_after(cfqq->slice_start, cfqq->dispatch_start))
			*unaccounted_time += cfqq->slice_start -
					cfqq->dispatch_start;
	}

	return slice_used;
}

/*
 * The cfqd->service_trees holds all pending cfq_queue's that have
 * requests waiting to be processed. It is sorted in the order that
 * we will service the queues.
 */
static void cfq_service_tree_add(struct cfq_data *cfqd, struct cfq_queue *cfqq,
				 bool add_front)
{
	struct rb_node **p, *parent;
	struct cfq_queue *__cfqq;
	unsigned long rb_key;
	struct cfq_rb_root *st;
	int left;
	int new_cfqq = 1;

	st = &cfqd->service_trees[cfqq_class(cfqq)];
	if (cfq_class_idle(cfqq)) {
		rb_key = CFQ_IDLE_DELAY;
		parent = rb_last(&st->rb);
		if (parent && parent != &cfqq->rb_node) {
			__cfqq = rb_entry(parent, struct cfq_queue, rb_node);
			rb_key += __cfqq->rb_key;
		} else
			rb_key += jiffies;
	} else if (!add_front) {
		/*
		 * Get our rb key offset. Subtract any residual slice
		 * value carried from last service. A negative resid
		 * count indicates slice overrun, and this should position
		 * the next service time further away in the tree.
		 */
		rb_key = cfq_slice_offset(cfqd, cfqq) + jiffies;
		rb_key -= cfqq->slice_resid;
		cfqq->slice_resid = 0;
	} else {
		rb_key = -HZ;
		__cfqq = cfq_rb_first(st);
		rb_key += __cfqq ? __cfqq->rb_key : jiffies;
	}

	if (!RB_EMPTY_NODE(&cfqq->rb_node)) {
		new_cfqq = 0;
		/*
		 * same position, nothing more to do
		 */
		if (rb_key == cfqq->rb_key && cfqq->service_tree == st)
			return;

		cfq_rb_erase(&cfqq->rb_node, cfqq->service_tree);
		cfqq->service_tree = NULL;
	}

	left = 1;
	parent = NULL;
	cfqq->service_tree = st;
	p = &st->rb.rb_node;
	while (*p) {
		parent = *p;
		__cfqq = rb_entry(parent, struct cfq_queue, rb_node);

		/*
		 * sort by key, that represents service time.
		 */
		if (time_before(rb_key, __cfqq->rb_key))
			p = &parent->rb_left;
		else {
			p = &parent->rb_right;
			left = 0;
		}
	}

	if (left)
		st->left = &cfqq->rb_node;

	cfqq->rb_key = rb_key;
	rb_link_node(&cfqq->rb_node, parent, p);
	rb_insert_color(&cfqq->rb_node, &st->rb);
	st->count++;
	if (add_front || !new_cfqq)
		return;
}

/*
 * Update cfqq's position in the service tree.
 */
static void cfq_resort_rr_list(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	/*
	 * Resorting requires the cfqq to be on the RR list already.
	 */
	if (cfq_cfqq_on_rr(cfqq))
		cfq_service_tree_add(cfqd, cfqq, 0);
}

/*
 * add to busy list of queues for service, trying to be fair in ordering
 * the pending list according to last request service
 */
static void cfq_add_cfqq_rr(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	cfq_log_cfqq(cfqd, cfqq, "add_to_rr");
	BUG_ON(cfq_cfqq_on_rr(cfqq));
	cfq_mark_cfqq_on_rr(cfqq);
	cfqd->busy_queues++;
	if (cfq_cfqq_sync(cfqq))
		cfqd->busy_sync_queues++;

	cfq_resort_rr_list(cfqd, cfqq);
}

/*
 * Called when the cfqq no longer has requests pending, remove it from
 * the service tree.
 */
static void cfq_del_cfqq_rr(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	cfq_log_cfqq(cfqd, cfqq, "del_from_rr");
	BUG_ON(!cfq_cfqq_on_rr(cfqq));
	cfq_clear_cfqq_on_rr(cfqq);

	if (!RB_EMPTY_NODE(&cfqq->rb_node)) {
		cfq_rb_erase(&cfqq->rb_node, cfqq->service_tree);
		cfqq->service_tree = NULL;
	}
	if (cfqq->p_root) {
		rb_erase(&cfqq->p_node, cfqq->p_root);
		cfqq->p_root = NULL;
	}

	BUG_ON(!cfqd->busy_queues);
	cfqd->busy_queues--;
	if (cfq_cfqq_sync(cfqq))
		cfqd->busy_sync_queues--;
}

/*
 * rb tree support functions
 */
static void cfq_del_rq_rb(struct request *rq)
{
	struct cfq_queue *cfqq = RQ_CFQQ(rq);
	const int sync = rq_is_sync(rq);

	BUG_ON(!cfqq->queued[sync]);
	cfqq->queued[sync]--;

	elv_rb_del(&cfqq->sort_list, rq);

	if (cfq_cfqq_on_rr(cfqq) && RB_EMPTY_ROOT(&cfqq->sort_list)) {
		/*
		 * Queue will be deleted from service tree when we actually
		 * expire it later. Right now just remove it from prio tree
		 * as it is empty.
		 */
		if (cfqq->p_root) {
			rb_erase(&cfqq->p_node, cfqq->p_root);
			cfqq->p_root = NULL;
		}
	}
}

static void cfq_add_rq_rb(struct request *rq)
{
	struct cfq_queue *cfqq = RQ_CFQQ(rq);
	struct cfq_data *cfqd = cfqq->cfqd;
	struct request *prev;

	cfqq->queued[rq_is_sync(rq)]++;

	elv_rb_add(&cfqq->sort_list, rq);

	if (!cfq_cfqq_on_rr(cfqq))
		cfq_add_cfqq_rr(cfqd, cfqq);

	/*
	 * check if this request is a better next-serve candidate
	 */
	prev = cfqq->next_rq;
	cfqq->next_rq = cfq_choose_req(cfqd, cfqq->next_rq, rq, cfqd->last_position);

	BUG_ON(!cfqq->next_rq);
}

static void cfq_reposition_rq_rb(struct cfq_queue *cfqq, struct request *rq)
{
	elv_rb_del(&cfqq->sort_list, rq);
	cfqq->queued[rq_is_sync(rq)]--;
	cfq_add_rq_rb(rq);
}

static struct request *
cfq_find_rq_fmerge(struct cfq_data *cfqd, struct bio *bio)
{
	struct task_struct *tsk = current;
	struct cfq_io_cq *cic;
	struct cfq_queue *cfqq;

	cic = cfq_cic_lookup(cfqd, tsk->io_context);
	if (!cic)
		return NULL;

	cfqq = cic_to_cfqq(cic, cfq_bio_sync(bio));
	if (cfqq)
		return elv_rb_find(&cfqq->sort_list, bio_end_sector(bio));

	return NULL;
}

static void cfq_activate_request(struct request_queue *q, struct request *rq)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;

	cfqd->rq_in_driver++;
	cfq_log_cfqq(cfqd, RQ_CFQQ(rq), "activate rq, drv=%d",
						cfqd->rq_in_driver);

	cfqd->last_position = blk_rq_pos(rq) + blk_rq_sectors(rq);
}

static void cfq_deactivate_request(struct request_queue *q, struct request *rq)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;

	WARN_ON(!cfqd->rq_in_driver);
	cfqd->rq_in_driver--;
	cfq_log_cfqq(cfqd, RQ_CFQQ(rq), "deactivate rq, drv=%d",
						cfqd->rq_in_driver);
}

static void cfq_remove_request(struct request *rq)
{
	struct cfq_queue *cfqq = RQ_CFQQ(rq);

	if (cfqq->next_rq == rq)
		cfqq->next_rq = cfq_find_next_rq(cfqq->cfqd, cfqq, rq);

	list_del_init(&rq->queuelist);
	cfq_del_rq_rb(rq);

	cfqq->cfqd->rq_queued--;
	if (rq->cmd_flags & REQ_PRIO) {
		WARN_ON(!cfqq->prio_pending);
		cfqq->prio_pending--;
	}
}

static int cfq_merge(struct request_queue *q, struct request **req,
		     struct bio *bio)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct request *__rq;

	__rq = cfq_find_rq_fmerge(cfqd, bio);
	if (__rq && elv_rq_merge_ok(__rq, bio)) {
		*req = __rq;
		return ELEVATOR_FRONT_MERGE;
	}

	return ELEVATOR_NO_MERGE;
}

static void cfq_merged_request(struct request_queue *q, struct request *req,
			       int type)
{
	if (type == ELEVATOR_FRONT_MERGE) {
		struct cfq_queue *cfqq = RQ_CFQQ(req);

		cfq_reposition_rq_rb(cfqq, req);
	}
}

static void
cfq_merged_requests(struct request_queue *q, struct request *rq,
		    struct request *next)
{
	struct cfq_queue *cfqq = RQ_CFQQ(rq);
	struct cfq_data *cfqd = q->elevator->elevator_data;

	/*
	 * reposition in fifo if next is older than rq
	 */
	if (!list_empty(&rq->queuelist) && !list_empty(&next->queuelist) &&
	    time_before(next->fifo_time, rq->fifo_time) &&
	    cfqq == RQ_CFQQ(next)) {
		list_move(&rq->queuelist, &next->queuelist);
		rq->fifo_time = next->fifo_time;
	}

	if (cfqq->next_rq == next)
		cfqq->next_rq = rq;
	cfq_remove_request(next);

	cfqq = RQ_CFQQ(next);
	/*
	 * all requests of this queue are merged to other queues, delete it
	 * from the service tree. If it's the active_queue,
	 * cfq_dispatch_requests() will choose to expire it or do idle
	 */
	if (cfq_cfqq_on_rr(cfqq) && RB_EMPTY_ROOT(&cfqq->sort_list) &&
	    cfqq != cfqd->active_queue)
		cfq_del_cfqq_rr(cfqd, cfqq);
}

static int cfq_allow_merge(struct request_queue *q, struct request *rq,
			   struct bio *bio)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct cfq_io_cq *cic;
	struct cfq_queue *cfqq;

	/*
	 * Disallow merge of a sync bio into an async request.
	 */
	if (cfq_bio_sync(bio) && !rq_is_sync(rq))
		return false;

	/*
	 * Lookup the cfqq that this bio will be queued with and allow
	 * merge only if rq is queued there.
	 */
	cic = cfq_cic_lookup(cfqd, current->io_context);
	if (!cic)
		return false;

	cfqq = cic_to_cfqq(cic, cfq_bio_sync(bio));
	return cfqq == RQ_CFQQ(rq);
}

static inline void cfq_del_timer(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	del_timer(&cfqd->idle_slice_timer);
}

static void __cfq_set_active_queue(struct cfq_data *cfqd,
				   struct cfq_queue *cfqq)
{
	if (cfqq) {
		cfq_log_cfqq(cfqd, cfqq, "set_active wl_class:%d",
				cfqd->serving_wl_class);
		cfqq->slice_start = 0;
		cfqq->dispatch_start = jiffies;
		cfqq->allocated_slice = 0;
		cfqq->slice_end = 0;
		cfqq->slice_dispatch = 0;
		cfqq->nr_sectors = 0;

		cfq_clear_cfqq_wait_request(cfqq);
		cfq_clear_cfqq_must_dispatch(cfqq);
		cfq_clear_cfqq_must_alloc_slice(cfqq);
		cfq_clear_cfqq_fifo_expire(cfqq);
		cfq_mark_cfqq_slice_new(cfqq);

		cfq_del_timer(cfqd, cfqq);
	}

	cfqd->active_queue = cfqq;
}

/*
 * current cfqq expired its slice (or was too idle), select new one
 */
static void
__cfq_slice_expired(struct cfq_data *cfqd, struct cfq_queue *cfqq,
		    bool timed_out)
{
	cfq_log_cfqq(cfqd, cfqq, "slice expired t=%d", timed_out);

	if (cfq_cfqq_wait_request(cfqq))
		cfq_del_timer(cfqd, cfqq);

	cfq_clear_cfqq_wait_request(cfqq);
	cfq_clear_cfqq_wait_busy(cfqq);

	/*
	 * store what was left of this slice, if the queue idled/timed out
	 */
	if (timed_out) {
		if (cfq_cfqq_slice_new(cfqq))
			cfqq->slice_resid = cfq_scaled_cfqq_slice(cfqd, cfqq);
		else
			cfqq->slice_resid = cfqq->slice_end - jiffies;
		cfq_log_cfqq(cfqd, cfqq, "resid=%ld", cfqq->slice_resid);
	}

	if (cfq_cfqq_on_rr(cfqq) && RB_EMPTY_ROOT(&cfqq->sort_list))
		cfq_del_cfqq_rr(cfqd, cfqq);

	cfq_resort_rr_list(cfqd, cfqq);

	if (cfqq == cfqd->active_queue)
		cfqd->active_queue = NULL;

	if (cfqd->active_cic) {
		put_io_context(cfqd->active_cic->icq.ioc);
		cfqd->active_cic = NULL;
	}
}

static inline void cfq_slice_expired(struct cfq_data *cfqd, bool timed_out)
{
	struct cfq_queue *cfqq = cfqd->active_queue;

	if (cfqq)
		__cfq_slice_expired(cfqd, cfqq, timed_out);
}

/*
 * Get next queue for service.
 */
static struct cfq_queue *cfq_get_next_queue(struct cfq_data *cfqd)
{
	struct cfq_rb_root *st = &cfqd->service_trees[cfqd->serving_wl_class];

	if (!cfqd->rq_queued)
		return NULL;

	/* There is nothing to dispatch */
	if (!st)
		return NULL;
	if (RB_EMPTY_ROOT(&st->rb))
		return NULL;
	return cfq_rb_first(st);
}

static struct cfq_queue *cfq_get_next_queue_forced(struct cfq_data *cfqd)
{
	struct cfq_queue *cfqq;
	int i, j;
	struct cfq_rb_root *st;

	if (!cfqd->rq_queued)
		return NULL;

	for_each_st(cfqd, i, j, st)
		if ((cfqq = cfq_rb_first(st)) != NULL)
			return cfqq;
	return NULL;
}

/*
 * Get and set a new active queue for service.
 */
static struct cfq_queue *cfq_set_active_queue(struct cfq_data *cfqd,
					      struct cfq_queue *cfqq)
{
	if (!cfqq)
		cfqq = cfq_get_next_queue(cfqd);

	__cfq_set_active_queue(cfqd, cfqq);
	return cfqq;
}

static inline sector_t cfq_dist_from_last(struct cfq_data *cfqd,
					  struct request *rq)
{
	if (blk_rq_pos(rq) >= cfqd->last_position)
		return blk_rq_pos(rq) - cfqd->last_position;
	else
		return cfqd->last_position - blk_rq_pos(rq);
}

/*
 * Determine whether we should enforce idle window for this queue.
 */

static bool cfq_should_idle(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	enum wl_class_t wl_class = cfqq_class(cfqq);
	struct cfq_rb_root *st = cfqq->service_tree;

	BUG_ON(!st);
	BUG_ON(!st->count);

	if (!cfqd->cfq_slice_idle)
		return false;

	/* We never do for idle class queues. */
	if (wl_class == IDLE_WORKLOAD)
		return false;

	/* We do for queues that were marked with idle window flag. */
	if (cfq_cfqq_idle_window(cfqq))
		return true;

	/*
	 * Otherwise, we do only if they are the last ones
	 * in their service tree.
	 */
	if (st->count == 1 && cfq_cfqq_sync(cfqq) &&
	   !cfq_io_thinktime_big(cfqd, &st->ttime))
		return true;
	cfq_log_cfqq(cfqd, cfqq, "Not idling. st->count:%d", st->count);
	return false;
}

static void cfq_arm_slice_timer(struct cfq_data *cfqd)
{
	struct cfq_queue *cfqq = cfqd->active_queue;
	struct cfq_io_cq *cic;
	unsigned long sl;

	WARN_ON(!RB_EMPTY_ROOT(&cfqq->sort_list));
	WARN_ON(cfq_cfqq_slice_new(cfqq));

	/*
	 * still active requests from this queue, don't idle
	 */
	if (cfqq->dispatched)
		return;

	/*
	 * task has exited, don't wait
	 */
	cic = cfqd->active_cic;
	if (!cic || !atomic_read(&cic->icq.ioc->active_ref))
		return;

	/*
	 * If our average think time is larger than the remaining time
	 * slice, then don't idle. This avoids overrunning the allotted
	 * time slice.
	 */
	if (sample_valid(cic->ttime.ttime_samples) &&
	    (cfqq->slice_end - jiffies < cic->ttime.ttime_mean)) {
		cfq_log_cfqq(cfqd, cfqq, "Not idling. think_time:%lu",
			     cic->ttime.ttime_mean);
		return;
	}

	cfq_mark_cfqq_wait_request(cfqq);

	sl = cfqd->cfq_slice_idle;

	mod_timer(&cfqd->idle_slice_timer, jiffies + sl);
	cfq_log_cfqq(cfqd, cfqq, "arm_idle: %lu", sl);
}

static inline int cfq_busy_queues_wl(enum wl_class_t wl_class,
				     struct cfq_data *cfqd)
{
	if (wl_class == IDLE_WORKLOAD)
		return cfqd->service_tree_idle.count;

	return cfqd->service_trees[wl_class].count;
}

/*
 * Move request from internal lists to the request queue dispatch list.
 */
static void cfq_dispatch_insert(struct request_queue *q, struct request *rq)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct cfq_queue *cfqq = RQ_CFQQ(rq);

	cfq_log_cfqq(cfqd, cfqq, "dispatch_insert");

	cfqq->next_rq = cfq_find_next_rq(cfqd, cfqq, rq);
	cfq_remove_request(rq);
	cfqq->dispatched++;
	elv_dispatch_sort(q, rq);

	cfqd->rq_in_flight[cfq_cfqq_sync(cfqq)]++;
	cfqq->nr_sectors += blk_rq_sectors(rq);
}

/*
 * return expired entry, or NULL to just start from scratch in rbtree
 */
static struct request *cfq_check_fifo(struct cfq_queue *cfqq)
{
	struct request *rq = NULL;

	if (cfq_cfqq_fifo_expire(cfqq))
		return NULL;

	cfq_mark_cfqq_fifo_expire(cfqq);

	if (list_empty(&cfqq->fifo))
		return NULL;

	rq = rq_entry_fifo(cfqq->fifo.next);
	if (time_before(jiffies, rq->fifo_time))
		rq = NULL;

	cfq_log_cfqq(cfqq->cfqd, cfqq, "fifo=%p", rq);
	return rq;
}

static inline int
cfq_prio_to_maxrq(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	const int base_rq = cfqd->cfq_slice_async_rq;

	WARN_ON(cfqq->ioprio >= IOPRIO_BE_NR);

	return 2 * base_rq * (IOPRIO_BE_NR - cfqq->ioprio);
}

static void
choose_wl_class_and_type(struct cfq_data *cfqd)
{
	unsigned slice;
	unsigned count;
	struct cfq_rb_root *st;
	enum wl_class_t original_class = cfqd->serving_wl_class;

	/* Choose next priority. RT > BE > IDLE */
	if (cfq_busy_queues_wl(RT_WORKLOAD, cfqd))
		cfqd->serving_wl_class = RT_WORKLOAD;
	else if (cfq_busy_queues_wl(BE_WORKLOAD, cfqd))
		cfqd->serving_wl_class = BE_WORKLOAD;
	else {
		cfqd->serving_wl_class = IDLE_WORKLOAD;
		cfqd->workload_expires = jiffies + 1;
		return;
	}

	if (original_class != cfqd->serving_wl_class)
		goto new_workload;

	st = &cfqd->service_trees[cfqd->serving_wl_class];
	count = st->count;

	/*
	 * check workload expiration, and that we still have other queues ready
	 */
	if (count && !time_after(jiffies, cfqd->workload_expires))
		return;

new_workload:
	st = &cfqd->service_trees[cfqd->serving_wl_class];
	count = st->count;

	/* sync workload slice is 2 * cfq_slice_idle */
	slice = max_t(unsigned, 2 * cfqd->cfq_slice_idle, CFQ_MIN_TT);
	cfq_log(cfqd, "workload slice:%d", slice);
	cfqd->workload_expires = jiffies + slice;
}

/*
 * Select a queue for service. If we have a current active queue,
 * check whether to continue servicing it, or retrieve and set a new one.
 */
static struct cfq_queue *cfq_select_queue(struct cfq_data *cfqd)
{
	struct cfq_queue *cfqq, *new_cfqq = NULL;

	cfqq = cfqd->active_queue;
	if (!cfqq)
		goto new_queue;

	if (!cfqd->rq_queued)
		return NULL;

	if (cfq_cfqq_wait_busy(cfqq) && !RB_EMPTY_ROOT(&cfqq->sort_list))
		goto expire;

	/*
	 * The active queue has run out of time, expire it and select new.
	 */
	if (cfq_slice_used(cfqq) && !cfq_cfqq_must_dispatch(cfqq)) {
		/*
		 * If slice had not expired at the completion of last request
		 * we might not have turned on wait_busy flag. Don't expire
		 * the queue yet. Allow the device to get backlogged.
		 *
		 * The very fact that we have used the slice, that means we
		 * have been idling all along on this queue and it should be
		 * ok to wait for this request to complete.
		 */
		if (cfqd->busy_queues == 1 && RB_EMPTY_ROOT(&cfqq->sort_list)
		    && cfqq->dispatched && cfq_should_idle(cfqd, cfqq)) {
			cfqq = NULL;
			goto keep_queue;
		}
	}

	/*
	 * The active queue has requests and isn't expired, allow it to
	 * dispatch.
	 */
	if (!RB_EMPTY_ROOT(&cfqq->sort_list))
		goto keep_queue;

	/*
	 * No requests pending. If the active queue still has requests in
	 * flight or is idling for a new request, allow either of these
	 * conditions to happen (or time out) before selecting a new queue.
	 */
	if (timer_pending(&cfqd->idle_slice_timer)) {
		cfqq = NULL;
		goto keep_queue;
	}

	/*
	 * The device is much faster than the queue can deliver: don't idle
	 **/
	if (CFQQ_SEEKY(cfqq) && cfq_cfqq_idle_window(cfqq) &&
	    (cfq_cfqq_slice_new(cfqq) ||
	     (time_after(cfqq->slice_end - jiffies,
			 jiffies - cfqq->slice_start))))
		cfq_clear_cfqq_idle_window(cfqq);

	if (cfqq->dispatched && cfq_should_idle(cfqd, cfqq)) {
		cfqq = NULL;
		goto keep_queue;
	}

expire:
	cfq_slice_expired(cfqd, 0);
new_queue:
	/*
	 * Current queue expired. Check if we have to switch to a new
	 * service tree
	 */
	if (!new_cfqq)
		choose_wl_class_and_type(cfqd);

	cfqq = cfq_set_active_queue(cfqd, new_cfqq);
keep_queue:
	return cfqq;
}

static int __cfq_forced_dispatch_cfqq(struct cfq_queue *cfqq)
{
	int dispatched = 0;

	while (cfqq->next_rq) {
		cfq_dispatch_insert(cfqq->cfqd->queue, cfqq->next_rq);
		dispatched++;
	}

	BUG_ON(!list_empty(&cfqq->fifo));

	/* By default cfqq is not expired if it is empty. Do it explicitly */
	__cfq_slice_expired(cfqq->cfqd, cfqq, 0);
	return dispatched;
}

/*
 * Drain our current requests. Used for barriers and when switching
 * io schedulers on-the-fly.
 */
static int cfq_forced_dispatch(struct cfq_data *cfqd)
{
	struct cfq_queue *cfqq;
	int dispatched = 0;

	/* Expire the timeslice of the current active queue first */
	cfq_slice_expired(cfqd, 0);
	while ((cfqq = cfq_get_next_queue_forced(cfqd)) != NULL) {
		__cfq_set_active_queue(cfqd, cfqq);
		dispatched += __cfq_forced_dispatch_cfqq(cfqq);
	}

	BUG_ON(cfqd->busy_queues);

	cfq_log(cfqd, "forced_dispatch=%d", dispatched);
	return dispatched;
}

static inline bool cfq_slice_used_soon(struct cfq_data *cfqd,
	struct cfq_queue *cfqq)
{
	/* the queue hasn't finished any request, can't estimate */
	if (cfq_cfqq_slice_new(cfqq))
		return true;
	if (time_after(jiffies + cfqd->cfq_slice_idle * cfqq->dispatched,
		cfqq->slice_end))
		return true;

	return false;
}

static bool cfq_may_dispatch(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	unsigned int max_dispatch;

	/*
	 * Drain async requests before we start sync IO
	 */
	if (cfq_should_idle(cfqd, cfqq) && cfqd->rq_in_flight[BLK_RW_ASYNC])
		return false;

	/*
	 * If this is an async queue and we have sync IO in flight, let it wait
	 */
	if (cfqd->rq_in_flight[BLK_RW_SYNC] && !cfq_cfqq_sync(cfqq))
		return false;

	max_dispatch = max_t(unsigned int, cfqd->cfq_quantum / 2, 1);
	if (cfq_class_idle(cfqq))
		max_dispatch = 1;

	/*
	 * Does this cfqq already have too much IO in flight?
	 */
	if (cfqq->dispatched >= max_dispatch) {
		bool promote_sync = false;
		/*
		 * idle queue must always only have a single IO in flight
		 */
		if (cfq_class_idle(cfqq))
			return false;

		/*
		 * If there is only one sync queue
		 * we can ignore async queue here and give the sync
		 * queue no dispatch limit. The reason is a sync queue can
		 * preempt async queue, limiting the sync queue doesn't make
		 * sense. This is useful for aiostress test.
		 */
		if (cfq_cfqq_sync(cfqq) && cfqd->busy_sync_queues == 1)
			promote_sync = true;

		/*
		 * We have other queues, don't allow more IO from this one
		 */
		if (cfqd->busy_queues > 1 && cfq_slice_used_soon(cfqd, cfqq) &&
				!promote_sync)
			return false;

		/*
		 * Sole queue user, no limit
		 */
		if (cfqd->busy_queues == 1 || promote_sync)
			max_dispatch = -1;
		else
			/*
			 * Normally we start throttling cfqq when cfq_quantum/2
			 * requests have been dispatched. But we can drive
			 * deeper queue depths at the beginning of slice
			 * subjected to upper limit of cfq_quantum.
			 * */
			max_dispatch = cfqd->cfq_quantum;
	}

	/*
	 * Async queues must wait a bit before being allowed dispatch.
	 * We also ramp up the dispatch depth gradually for async IO,
	 * based on the last sync IO we serviced
	 */
	if (!cfq_cfqq_sync(cfqq) && cfqd->cfq_latency) {
		unsigned long last_sync = jiffies - cfqd->last_delayed_sync;
		unsigned int depth;

		depth = last_sync / cfqd->cfq_slice[1];
		if (!depth && !cfqq->dispatched)
			depth = 1;
		if (depth < max_dispatch)
			max_dispatch = depth;
	}

	/*
	 * If we're below the current max, allow a dispatch
	 */
	return cfqq->dispatched < max_dispatch;
}

/*
 * Dispatch a request from cfqq, moving them to the request queue
 * dispatch list.
 */
static bool cfq_dispatch_request(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	struct request *rq;

	BUG_ON(RB_EMPTY_ROOT(&cfqq->sort_list));

	if (!cfq_may_dispatch(cfqd, cfqq))
		return false;

	/*
	 * follow expired path, else get first next available
	 */
	rq = cfq_check_fifo(cfqq);
	if (!rq)
		rq = cfqq->next_rq;

	/*
	 * insert request into driver dispatch list
	 */
	cfq_dispatch_insert(cfqd->queue, rq);

	if (!cfqd->active_cic) {
		struct cfq_io_cq *cic = RQ_CIC(rq);

		atomic_long_inc(&cic->icq.ioc->refcount);
		cfqd->active_cic = cic;
	}

	return true;
}

/*
 * Find the cfqq that we need to service and move a request from that to the
 * dispatch list
 */
static int cfq_dispatch_requests(struct request_queue *q, int force)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct cfq_queue *cfqq;

	if (!cfqd->busy_queues)
		return 0;

	if (unlikely(force))
		return cfq_forced_dispatch(cfqd);

	cfqq = cfq_select_queue(cfqd);
	if (!cfqq)
		return 0;

	/*
	 * Dispatch a request from this cfqq, if it is allowed
	 */
	if (!cfq_dispatch_request(cfqd, cfqq))
		return 0;

	cfqq->slice_dispatch++;
	cfq_clear_cfqq_must_dispatch(cfqq);

	/*
	 * expire an async queue immediately if it has used up its slice. idle
	 * queue always expire after 1 dispatch round.
	 */
	if (cfqd->busy_queues > 1 && ((!cfq_cfqq_sync(cfqq) &&
	    cfqq->slice_dispatch >= cfq_prio_to_maxrq(cfqd, cfqq)) ||
	    cfq_class_idle(cfqq))) {
		cfqq->slice_end = jiffies + 1;
		cfq_slice_expired(cfqd, 0);
	}

	cfq_log_cfqq(cfqd, cfqq, "dispatched a request");
	return 1;
}

/*
 * task holds one reference to the queue, dropped when task exits. each rq
 * in-flight on this queue also holds a reference, dropped when rq is freed.
 *
 * Queue lock must be held here.
 */
static void cfq_put_queue(struct cfq_queue *cfqq)
{
	struct cfq_data *cfqd = cfqq->cfqd;

	BUG_ON(cfqq->ref <= 0);

	cfqq->ref--;
	if (cfqq->ref)
		return;

	cfq_log_cfqq(cfqd, cfqq, "put_queue");
	BUG_ON(rb_first(&cfqq->sort_list));
	BUG_ON(cfqq->allocated[READ] + cfqq->allocated[WRITE]);

	if (unlikely(cfqd->active_queue == cfqq)) {
		__cfq_slice_expired(cfqd, cfqq, 0);
		cfq_schedule_dispatch(cfqd);
	}

	BUG_ON(cfq_cfqq_on_rr(cfqq));
	kmem_cache_free(cfq_pool, cfqq);
}

static void cfq_exit_cfqq(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	if (unlikely(cfqq == cfqd->active_queue)) {
		__cfq_slice_expired(cfqd, cfqq, 0);
		cfq_schedule_dispatch(cfqd);
	}

	cfq_put_queue(cfqq);
}

static void cfq_init_icq(struct io_cq *icq)
{
	struct cfq_io_cq *cic = icq_to_cic(icq);

	cic->ttime.last_end_request = jiffies;
}

static void cfq_exit_icq(struct io_cq *icq)
{
	struct cfq_io_cq *cic = icq_to_cic(icq);
	struct cfq_data *cfqd = cic_to_cfqd(cic);

	if (cic_to_cfqq(cic, false)) {
		cfq_exit_cfqq(cfqd, cic_to_cfqq(cic, false));
		cic_set_cfqq(cic, NULL, false);
	}

	if (cic_to_cfqq(cic, true)) {
		cfq_exit_cfqq(cfqd, cic_to_cfqq(cic, true));
		cic_set_cfqq(cic, NULL, true);
	}
}

static void cfq_init_prio_data(struct cfq_queue *cfqq, struct cfq_io_cq *cic)
{
	struct task_struct *tsk = current;
	int ioprio_class;

	if (!cfq_cfqq_prio_changed(cfqq))
		return;

	ioprio_class = IOPRIO_PRIO_CLASS(cic->ioprio);
	switch (ioprio_class) {
	default:
		printk(KERN_ERR "cfq: bad prio %x\n", ioprio_class);
	case IOPRIO_CLASS_NONE:
		/*
		 * no prio set, inherit CPU scheduling settings
		 */
		cfqq->ioprio = task_nice_ioprio(tsk);
		cfqq->ioprio_class = task_nice_ioclass(tsk);
		break;
	case IOPRIO_CLASS_RT:
		cfqq->ioprio = IOPRIO_PRIO_DATA(cic->ioprio);
		cfqq->ioprio_class = IOPRIO_CLASS_RT;
		break;
	case IOPRIO_CLASS_BE:
		cfqq->ioprio = IOPRIO_PRIO_DATA(cic->ioprio);
		cfqq->ioprio_class = IOPRIO_CLASS_BE;
		break;
	case IOPRIO_CLASS_IDLE:
		cfqq->ioprio_class = IOPRIO_CLASS_IDLE;
		cfqq->ioprio = 7;
		cfq_clear_cfqq_idle_window(cfqq);
		break;
	}

	/*
	 * keep track of original prio settings in case we have to temporarily
	 * elevate the priority of this queue
	 */
	cfqq->org_ioprio = cfqq->ioprio;
	cfq_clear_cfqq_prio_changed(cfqq);
}

static void check_ioprio_changed(struct cfq_io_cq *cic, struct bio *bio)
{
	int ioprio = cic->icq.ioc->ioprio;
	struct cfq_data *cfqd = cic_to_cfqd(cic);
	struct cfq_queue *cfqq;

	/*
	 * Check whether ioprio has changed.  The condition may trigger
	 * spuriously on a newly created cic but there's no harm.
	 */
	if (unlikely(!cfqd) || likely(cic->ioprio == ioprio))
		return;

	cfqq = cic_to_cfqq(cic, false);
	if (cfqq) {
		cfq_put_queue(cfqq);
		cfqq = cfq_get_queue(cfqd, BLK_RW_ASYNC, cic, bio);
		cic_set_cfqq(cic, cfqq, false);
	}

	cfqq = cic_to_cfqq(cic, true);
	if (cfqq)
		cfq_mark_cfqq_prio_changed(cfqq);

	cic->ioprio = ioprio;
}

static void cfq_init_cfqq(struct cfq_data *cfqd, struct cfq_queue *cfqq,
			  pid_t pid, bool is_sync)
{
	RB_CLEAR_NODE(&cfqq->rb_node);
	RB_CLEAR_NODE(&cfqq->p_node);
	INIT_LIST_HEAD(&cfqq->fifo);

	cfqq->ref = 0;
	cfqq->cfqd = cfqd;

	cfq_mark_cfqq_prio_changed(cfqq);

	if (is_sync) {
		if (!cfq_class_idle(cfqq))
			cfq_mark_cfqq_idle_window(cfqq);
		cfq_mark_cfqq_sync(cfqq);
	}
	cfqq->pid = pid;
}

static struct cfq_queue **
cfq_async_queue_prio(struct cfq_data *cfqd, int ioprio_class, int ioprio)
{
	switch (ioprio_class) {
	case IOPRIO_CLASS_RT:
		return &cfqd->async_cfqq[0][ioprio];
	case IOPRIO_CLASS_NONE:
		ioprio = IOPRIO_NORM;
		/* fall through */
	case IOPRIO_CLASS_BE:
		return &cfqd->async_cfqq[1][ioprio];
	case IOPRIO_CLASS_IDLE:
		return &cfqd->async_idle_cfqq;
	default:
		BUG();
	}
}

static struct cfq_queue *
cfq_get_queue(struct cfq_data *cfqd, bool is_sync, struct cfq_io_cq *cic,
	      struct bio *bio)
{
	int ioprio_class = IOPRIO_PRIO_CLASS(cic->ioprio);
	int ioprio = IOPRIO_PRIO_DATA(cic->ioprio);
	struct cfq_queue **async_cfqq = NULL;
	struct cfq_queue *cfqq;

	rcu_read_lock();

	if (!is_sync) {
		if (!ioprio_valid(cic->ioprio)) {
			struct task_struct *tsk = current;
			ioprio = task_nice_ioprio(tsk);
			ioprio_class = task_nice_ioclass(tsk);
		}
		async_cfqq = cfq_async_queue_prio(cfqd, ioprio_class, ioprio);
		cfqq = *async_cfqq;
		if (cfqq)
			goto out;
	}

	cfqq = kmem_cache_alloc_node(cfq_pool, GFP_NOWAIT | __GFP_ZERO,
				     cfqd->queue->node);
	if (!cfqq) {
		cfqq = &cfqd->oom_cfqq;
		goto out;
	}

	cfq_init_cfqq(cfqd, cfqq, current->pid, is_sync);
	cfq_init_prio_data(cfqq, cic);
	cfq_log_cfqq(cfqd, cfqq, "alloced");

	if (async_cfqq) {
		/* a new async queue is created, pin and remember */
		cfqq->ref++;
		*async_cfqq = cfqq;
	}
out:
	cfqq->ref++;
	rcu_read_unlock();
	return cfqq;
}

static void
__cfq_update_io_thinktime(struct cfq_ttime *ttime, unsigned long slice_idle)
{
	unsigned long elapsed = jiffies - ttime->last_end_request;
	elapsed = min(elapsed, 2UL * slice_idle);

	ttime->ttime_samples = (7*ttime->ttime_samples + 256) / 8;
	ttime->ttime_total = (7*ttime->ttime_total + 256*elapsed) / 8;
	ttime->ttime_mean = (ttime->ttime_total + 128) / ttime->ttime_samples;
}

static void
cfq_update_io_thinktime(struct cfq_data *cfqd, struct cfq_queue *cfqq,
			struct cfq_io_cq *cic)
{
	if (cfq_cfqq_sync(cfqq)) {
		__cfq_update_io_thinktime(&cic->ttime, cfqd->cfq_slice_idle);
		__cfq_update_io_thinktime(&cfqq->service_tree->ttime,
			cfqd->cfq_slice_idle);
	}
}

static void
cfq_update_io_seektime(struct cfq_data *cfqd, struct cfq_queue *cfqq,
		       struct request *rq)
{
	sector_t sdist = 0;
	if (cfqq->last_request_pos) {
		if (cfqq->last_request_pos < blk_rq_pos(rq))
			sdist = blk_rq_pos(rq) - cfqq->last_request_pos;
		else
			sdist = cfqq->last_request_pos - blk_rq_pos(rq);
	}

	cfqq->seek_history <<= 1;
	cfqq->seek_history |= (sdist > CFQQ_SEEK_THR);
}

/*
 * Disable idle window if the process thinks too long or seeks so much that
 * it doesn't matter
 */
static void
cfq_update_idle_window(struct cfq_data *cfqd, struct cfq_queue *cfqq,
		       struct cfq_io_cq *cic)
{
	int old_idle, enable_idle;

	/*
	 * Don't idle for async or idle io prio class
	 */
	if (!cfq_cfqq_sync(cfqq) || cfq_class_idle(cfqq))
		return;

	enable_idle = old_idle = cfq_cfqq_idle_window(cfqq);

	if (cfqq->next_rq && (cfqq->next_rq->cmd_flags & REQ_NOIDLE))
		enable_idle = 0;
	else if (!atomic_read(&cic->icq.ioc->active_ref) ||
		 !cfqd->cfq_slice_idle || CFQQ_SEEKY(cfqq))
		enable_idle = 0;
	else if (sample_valid(cic->ttime.ttime_samples)) {
		if (cic->ttime.ttime_mean > cfqd->cfq_slice_idle)
			enable_idle = 0;
		else
			enable_idle = 1;
	}

	if (old_idle != enable_idle) {
		cfq_log_cfqq(cfqd, cfqq, "idle=%d", enable_idle);
		if (enable_idle)
			cfq_mark_cfqq_idle_window(cfqq);
		else
			cfq_clear_cfqq_idle_window(cfqq);
	}
}

/*
 * Called when a new fs request (rq) is added (to cfqq). Check if there's
 * something we should do about it
 */
static void
cfq_rq_enqueued(struct cfq_data *cfqd, struct cfq_queue *cfqq,
		struct request *rq)
{
	struct cfq_io_cq *cic = RQ_CIC(rq);

	cfqd->rq_queued++;
	if (rq->cmd_flags & REQ_PRIO)
		cfqq->prio_pending++;

	cfq_update_io_thinktime(cfqd, cfqq, cic);
	cfq_update_io_seektime(cfqd, cfqq, rq);
	cfq_update_idle_window(cfqd, cfqq, cic);

	cfqq->last_request_pos = blk_rq_pos(rq) + blk_rq_sectors(rq);

	if (cfqq == cfqd->active_queue) {
		/*
		 * Remember that we saw a request from this process, but
		 * don't start queuing just yet. Otherwise we risk seeing lots
		 * of tiny requests, because we disrupt the normal plugging
		 * and merging. If the request is already larger than a single
		 * page, let it rip immediately. For that case we assume that
		 * merging is already done. Ditto for a busy system that
		 * has other work pending, don't risk delaying until the
		 * idle timer unplug to continue working.
		 */
		if (cfq_cfqq_wait_request(cfqq)) {
			if (blk_rq_bytes(rq) > PAGE_CACHE_SIZE ||
			    cfqd->busy_queues > 1) {
				cfq_del_timer(cfqd, cfqq);
				cfq_clear_cfqq_wait_request(cfqq);
				__blk_run_queue(cfqd->queue);
			} else
				cfq_mark_cfqq_must_dispatch(cfqq);
		}
	}
}

static void cfq_insert_request(struct request_queue *q, struct request *rq)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct cfq_queue *cfqq = RQ_CFQQ(rq);

	cfq_log_cfqq(cfqd, cfqq, "insert_request");
	cfq_init_prio_data(cfqq, RQ_CIC(rq));

	rq->fifo_time = jiffies + cfqd->cfq_fifo_expire[rq_is_sync(rq)];
	list_add_tail(&rq->queuelist, &cfqq->fifo);
	cfq_add_rq_rb(rq);
	cfq_rq_enqueued(cfqd, cfqq, rq);
}

/*
 * Update hw_tag based on peak queue depth over 50 samples under
 * sufficient load.
 */
static void cfq_update_hw_tag(struct cfq_data *cfqd)
{
	struct cfq_queue *cfqq = cfqd->active_queue;

	if (cfqd->rq_in_driver > cfqd->hw_tag_est_depth)
		cfqd->hw_tag_est_depth = cfqd->rq_in_driver;

	if (cfqd->hw_tag == 1)
		return;

	if (cfqd->rq_queued <= CFQ_HW_QUEUE_MIN &&
	    cfqd->rq_in_driver <= CFQ_HW_QUEUE_MIN)
		return;

	/*
	 * If active queue hasn't enough requests and can idle, cfq might not
	 * dispatch sufficient requests to hardware. Don't zero hw_tag in this
	 * case
	 */
	if (cfqq && cfq_cfqq_idle_window(cfqq) &&
	    cfqq->dispatched + cfqq->queued[0] + cfqq->queued[1] <
	    CFQ_HW_QUEUE_MIN && cfqd->rq_in_driver < CFQ_HW_QUEUE_MIN)
		return;

	if (cfqd->hw_tag_samples++ < 50)
		return;

	if (cfqd->hw_tag_est_depth >= CFQ_HW_QUEUE_MIN)
		cfqd->hw_tag = 1;
	else
		cfqd->hw_tag = 0;
}

static bool cfq_should_wait_busy(struct cfq_data *cfqd, struct cfq_queue *cfqq)
{
	struct cfq_io_cq *cic = cfqd->active_cic;

	/* If the queue already has requests, don't wait */
	if (!RB_EMPTY_ROOT(&cfqq->sort_list))
		return false;

	if (cfq_slice_used(cfqq))
		return true;

	/* if slice left is less than think time, wait busy */
	if (cic && sample_valid(cic->ttime.ttime_samples)
	    && (cfqq->slice_end - jiffies < cic->ttime.ttime_mean))
		return true;

	/*
	 * If think times is less than a jiffy than ttime_mean=0 and above
	 * will not be true. It might happen that slice has not expired yet
	 * but will expire soon (4-5 ns) during select_queue(). To cover the
	 * case where think time is less than a jiffy, mark the queue wait
	 * busy if only 1 jiffy is left in the slice.
	 */
	if (cfqq->slice_end - jiffies == 1)
		return true;

	return false;
}

static void cfq_completed_request(struct request_queue *q, struct request *rq)
{
	struct cfq_queue *cfqq = RQ_CFQQ(rq);
	struct cfq_data *cfqd = cfqq->cfqd;
	const int sync = rq_is_sync(rq);
	unsigned long now;

	now = jiffies;
	cfq_log_cfqq(cfqd, cfqq, "complete rqnoidle %d",
		     !!(rq->cmd_flags & REQ_NOIDLE));

	cfq_update_hw_tag(cfqd);

	WARN_ON(!cfqd->rq_in_driver);
	WARN_ON(!cfqq->dispatched);
	cfqd->rq_in_driver--;
	cfqq->dispatched--;

	cfqd->rq_in_flight[cfq_cfqq_sync(cfqq)]--;

	if (sync) {
		struct cfq_rb_root *st;

		RQ_CIC(rq)->ttime.last_end_request = now;

		if (cfq_cfqq_on_rr(cfqq))
			st = cfqq->service_tree;
		else
			st = &cfqd->service_trees[cfqq_class(cfqq)];

		st->ttime.last_end_request = now;
		if (!time_after(rq->start_time + cfqd->cfq_fifo_expire[1], now))
			cfqd->last_delayed_sync = now;
	}

	/*
	 * If this is the active queue, check if it needs to be expired,
	 * or if we want to idle in case it has no pending requests.
	 */
	if (cfqd->active_queue == cfqq) {
		const bool cfqq_empty = RB_EMPTY_ROOT(&cfqq->sort_list);

		if (cfq_cfqq_slice_new(cfqq)) {
			cfq_set_prio_slice(cfqd, cfqq);
			cfq_clear_cfqq_slice_new(cfqq);
		}

		/*
		 * Should we wait for next request to come in before we expire
		 * the queue.
		 */
		if (cfq_should_wait_busy(cfqd, cfqq)) {
			unsigned long extend_sl = cfqd->cfq_slice_idle;
			cfqq->slice_end = jiffies + extend_sl;
			cfq_mark_cfqq_wait_busy(cfqq);
			cfq_log_cfqq(cfqd, cfqq, "will busy wait");
		}

		/*
		 * Idling is not enabled on:
		 * - expired queues
		 * - idle-priority queues
		 * - async queues
		 * - queues with still some requests queued
		 */
		if (cfq_slice_used(cfqq) || cfq_class_idle(cfqq))
			cfq_slice_expired(cfqd, 1);
		else if (sync && cfqq_empty)
			cfq_arm_slice_timer(cfqd);
	}

	if (!cfqd->rq_in_driver)
		cfq_schedule_dispatch(cfqd);
}

static inline int __cfq_may_queue(struct cfq_queue *cfqq)
{
	if (cfq_cfqq_wait_request(cfqq) && !cfq_cfqq_must_alloc_slice(cfqq)) {
		cfq_mark_cfqq_must_alloc_slice(cfqq);
		return ELV_MQUEUE_MUST;
	}

	return ELV_MQUEUE_MAY;
}

static int cfq_may_queue(struct request_queue *q, int rw)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct task_struct *tsk = current;
	struct cfq_io_cq *cic;
	struct cfq_queue *cfqq;

	/*
	 * don't force setup of a queue from here, as a call to may_queue
	 * does not necessarily imply that a request actually will be queued.
	 * so just lookup a possibly existing queue, or return 'may queue'
	 * if that fails
	 */
	cic = cfq_cic_lookup(cfqd, tsk->io_context);
	if (!cic)
		return ELV_MQUEUE_MAY;

	cfqq = cic_to_cfqq(cic, rw_is_sync(rw));
	if (cfqq) {
		cfq_init_prio_data(cfqq, cic);

		return __cfq_may_queue(cfqq);
	}

	return ELV_MQUEUE_MAY;
}

/*
 * queue lock held here
 */
static void cfq_put_request(struct request *rq)
{
	struct cfq_queue *cfqq = RQ_CFQQ(rq);

	if (cfqq) {
		const int rw = rq_data_dir(rq);

		BUG_ON(!cfqq->allocated[rw]);
		cfqq->allocated[rw]--;

		rq->elv.priv[0] = NULL;
		rq->elv.priv[1] = NULL;

		cfq_put_queue(cfqq);
	}
}

/*
 * Allocate cfq data structures associated with this request.
 */
static int
cfq_set_request(struct request_queue *q, struct request *rq, struct bio *bio,
		gfp_t gfp_mask)
{
	struct cfq_data *cfqd = q->elevator->elevator_data;
	struct cfq_io_cq *cic = icq_to_cic(rq->elv.icq);
	const int rw = rq_data_dir(rq);
	const bool is_sync = rq_is_sync(rq);
	struct cfq_queue *cfqq;

	spin_lock_irq(q->queue_lock);

	check_ioprio_changed(cic, bio);

	cfqq = cic_to_cfqq(cic, is_sync);
	if (!cfqq || cfqq == &cfqd->oom_cfqq) {
		if (cfqq)
			cfq_put_queue(cfqq);
		cfqq = cfq_get_queue(cfqd, is_sync, cic, bio);
		cic_set_cfqq(cic, cfqq, is_sync);
	}

	cfqq->allocated[rw]++;

	cfqq->ref++;
	rq->elv.priv[0] = cfqq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void cfq_kick_queue(struct work_struct *work)
{
	struct cfq_data *cfqd =
		container_of(work, struct cfq_data, unplug_work);
	struct request_queue *q = cfqd->queue;

	spin_lock_irq(q->queue_lock);
	__blk_run_queue(cfqd->queue);
	spin_unlock_irq(q->queue_lock);
}

/*
 * Timer running if the active_queue is currently idling inside its time slice
 */
static void cfq_idle_slice_timer(unsigned long data)
{
	struct cfq_data *cfqd = (struct cfq_data *) data;
	struct cfq_queue *cfqq;
	unsigned long flags;
	int timed_out = 1;

	cfq_log(cfqd, "idle timer fired");

	spin_lock_irqsave(cfqd->queue->queue_lock, flags);

	cfqq = cfqd->active_queue;
	if (cfqq) {
		timed_out = 0;

		/*
		 * We saw a request before the queue expired, let it through
		 */
		if (cfq_cfqq_must_dispatch(cfqq))
			goto out_kick;

		/*
		 * expired
		 */
		if (cfq_slice_used(cfqq))
			goto expire;

		/*
		 * only expire and reinvoke request handler, if there are
		 * other queues with pending requests
		 */
		if (!cfqd->busy_queues)
			goto out_cont;

		/*
		 * not expired and it has a request pending, let it dispatch
		 */
		if (!RB_EMPTY_ROOT(&cfqq->sort_list))
			goto out_kick;
	}
expire:
	cfq_slice_expired(cfqd, timed_out);
out_kick:
	cfq_schedule_dispatch(cfqd);
out_cont:
	spin_unlock_irqrestore(cfqd->queue->queue_lock, flags);
}

static void cfq_shutdown_timer_wq(struct cfq_data *cfqd)
{
	del_timer_sync(&cfqd->idle_slice_timer);
	cancel_work_sync(&cfqd->unplug_work);
}

static void cfq_exit_queue(struct elevator_queue *e)
{
	struct cfq_data *cfqd = e->elevator_data;
	struct request_queue *q = cfqd->queue;

	cfq_shutdown_timer_wq(cfqd);

	spin_lock_irq(q->queue_lock);

	if (cfqd->active_queue)
		__cfq_slice_expired(cfqd, cfqd->active_queue, 0);

	spin_unlock_irq(q->queue_lock);

	cfq_shutdown_timer_wq(cfqd);

	kfree(cfqd);
}

static int cfq_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct cfq_data *cfqd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	cfqd = kzalloc_node(sizeof(*cfqd), GFP_KERNEL, q->node);
	if (!cfqd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = cfqd;

	cfqd->queue = q;
	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	/*
	 * Our fallback cfqq if cfq_get_queue() runs into OOM issues.
	 * Grab a permanent reference to it, so that the normal code flow
	 * will not attempt to free it.
	 */
	cfq_init_cfqq(cfqd, &cfqd->oom_cfqq, 1, 0);
	cfqd->oom_cfqq.ref++;

	spin_lock_irq(q->queue_lock);
	spin_unlock_irq(q->queue_lock);

	init_timer(&cfqd->idle_slice_timer);
	cfqd->idle_slice_timer.function = cfq_idle_slice_timer;
	cfqd->idle_slice_timer.data = (unsigned long) cfqd;

	INIT_WORK(&cfqd->unplug_work, cfq_kick_queue);

	cfqd->cfq_quantum = cfq_quantum;
	cfqd->cfq_fifo_expire[0] = cfq_fifo_expire[0];
	cfqd->cfq_fifo_expire[1] = cfq_fifo_expire[1];
	cfqd->cfq_back_max = cfq_back_max;
	cfqd->cfq_back_penalty = cfq_back_penalty;
	cfqd->cfq_slice[0] = cfq_slice_async;
	cfqd->cfq_slice[1] = cfq_slice_sync;
	cfqd->cfq_target_latency = cfq_target_latency;
	cfqd->cfq_slice_async_rq = cfq_slice_async_rq;
	cfqd->cfq_slice_idle = cfq_slice_idle;
	cfqd->cfq_latency = 1;
	cfqd->hw_tag = -1;
	/*
	 * we optimistically start assuming sync ops weren't delayed in last
	 * second, in order to have larger depth for async operations.
	 */
	cfqd->last_delayed_sync = jiffies - HZ;
	return 0;
}

static void cfq_registered_queue(struct request_queue *q)
{
	struct elevator_queue *e = q->elevator;
	struct cfq_data *cfqd = e->elevator_data;

	/*
	 * Default to IOPS mode with no idling for SSDs
	 */
	if (blk_queue_nonrot(q))
		cfqd->cfq_slice_idle = 0;
}

/*
 * sysfs parts below -->
 */
static ssize_t
cfq_var_show(unsigned int var, char *page)
{
	return sprintf(page, "%u\n", var);
}

static ssize_t
cfq_var_store(unsigned int *var, const char *page, size_t count)
{
	char *p = (char *) page;

	*var = simple_strtoul(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct cfq_data *cfqd = e->elevator_data;			\
	unsigned int __data = __VAR;					\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return cfq_var_show(__data, (page));				\
}
SHOW_FUNCTION(cfq_quantum_show, cfqd->cfq_quantum, 0);
SHOW_FUNCTION(cfq_fifo_expire_sync_show, cfqd->cfq_fifo_expire[1], 1);
SHOW_FUNCTION(cfq_fifo_expire_async_show, cfqd->cfq_fifo_expire[0], 1);
SHOW_FUNCTION(cfq_back_seek_max_show, cfqd->cfq_back_max, 0);
SHOW_FUNCTION(cfq_back_seek_penalty_show, cfqd->cfq_back_penalty, 0);
SHOW_FUNCTION(cfq_slice_idle_show, cfqd->cfq_slice_idle, 1);
SHOW_FUNCTION(cfq_slice_sync_show, cfqd->cfq_slice[1], 1);
SHOW_FUNCTION(cfq_slice_async_show, cfqd->cfq_slice[0], 1);
SHOW_FUNCTION(cfq_slice_async_rq_show, cfqd->cfq_slice_async_rq, 0);
SHOW_FUNCTION(cfq_low_latency_show, cfqd->cfq_latency, 0);
SHOW_FUNCTION(cfq_target_latency_show, cfqd->cfq_target_latency, 1);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct cfq_data *cfqd = e->elevator_data;			\
	unsigned int __data;						\
	int ret = cfq_var_store(&__data, (page), count);		\
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
STORE_FUNCTION(cfq_quantum_store, &cfqd->cfq_quantum, 1, UINT_MAX, 0);
STORE_FUNCTION(cfq_fifo_expire_sync_store, &cfqd->cfq_fifo_expire[1], 1,
		UINT_MAX, 1);
STORE_FUNCTION(cfq_fifo_expire_async_store, &cfqd->cfq_fifo_expire[0], 1,
		UINT_MAX, 1);
STORE_FUNCTION(cfq_back_seek_max_store, &cfqd->cfq_back_max, 0, UINT_MAX, 0);
STORE_FUNCTION(cfq_back_seek_penalty_store, &cfqd->cfq_back_penalty, 1,
		UINT_MAX, 0);
STORE_FUNCTION(cfq_slice_idle_store, &cfqd->cfq_slice_idle, 0, UINT_MAX, 1);
STORE_FUNCTION(cfq_slice_sync_store, &cfqd->cfq_slice[1], 1, UINT_MAX, 1);
STORE_FUNCTION(cfq_slice_async_store, &cfqd->cfq_slice[0], 1, UINT_MAX, 1);
STORE_FUNCTION(cfq_slice_async_rq_store, &cfqd->cfq_slice_async_rq, 1,
		UINT_MAX, 0);
STORE_FUNCTION(cfq_low_latency_store, &cfqd->cfq_latency, 0, 1, 0);
STORE_FUNCTION(cfq_target_latency_store, &cfqd->cfq_target_latency, 1, UINT_MAX, 1);
#undef STORE_FUNCTION

#define CFQ_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, cfq_##name##_show, cfq_##name##_store)

static struct elv_fs_entry cfq_attrs[] = {
	CFQ_ATTR(quantum),
	CFQ_ATTR(fifo_expire_sync),
	CFQ_ATTR(fifo_expire_async),
	CFQ_ATTR(back_seek_max),
	CFQ_ATTR(back_seek_penalty),
	CFQ_ATTR(slice_sync),
	CFQ_ATTR(slice_async),
	CFQ_ATTR(slice_async_rq),
	CFQ_ATTR(slice_idle),
	CFQ_ATTR(low_latency),
	CFQ_ATTR(target_latency),
	__ATTR_NULL
};

static struct elevator_type iosched_cfq = {
	.ops = {
		.elevator_merge_fn = 		cfq_merge,
		.elevator_merged_fn =		cfq_merged_request,
		.elevator_merge_req_fn =	cfq_merged_requests,
		.elevator_allow_merge_fn =	cfq_allow_merge,
		.elevator_dispatch_fn =		cfq_dispatch_requests,
		.elevator_add_req_fn =		cfq_insert_request,
		.elevator_activate_req_fn =	cfq_activate_request,
		.elevator_deactivate_req_fn =	cfq_deactivate_request,
		.elevator_completed_req_fn =	cfq_completed_request,
		.elevator_former_req_fn =	elv_rb_former_request,
		.elevator_latter_req_fn =	elv_rb_latter_request,
		.elevator_init_icq_fn =		cfq_init_icq,
		.elevator_exit_icq_fn =		cfq_exit_icq,
		.elevator_set_req_fn =		cfq_set_request,
		.elevator_put_req_fn =		cfq_put_request,
		.elevator_may_queue_fn =	cfq_may_queue,
		.elevator_init_fn =		cfq_init_queue,
		.elevator_exit_fn =		cfq_exit_queue,
		.elevator_registered_fn =	cfq_registered_queue,
	},
	.icq_size	=	sizeof(struct cfq_io_cq),
	.icq_align	=	__alignof__(struct cfq_io_cq),
	.elevator_attrs =	cfq_attrs,
	.elevator_name	=	"cfq",
	.elevator_owner =	THIS_MODULE,
};

static int __init cfq_init(void)
{
	int ret;

	/*
	 * could be 0 on HZ < 1000 setups
	 */
	if (!cfq_slice_async)
		cfq_slice_async = 1;
	if (!cfq_slice_idle)
		cfq_slice_idle = 1;

	ret = -ENOMEM;
	cfq_pool = KMEM_CACHE(cfq_queue, 0);
	if (!cfq_pool)
		return ret;

	ret = elv_register(&iosched_cfq);
	if (ret)
		goto err_free_pool;

	return 0;

err_free_pool:
	kmem_cache_destroy(cfq_pool);
	return ret;
}

static void __exit cfq_exit(void)
{
	elv_unregister(&iosched_cfq);
	kmem_cache_destroy(cfq_pool);
}

module_init(cfq_init);
module_exit(cfq_exit);

MODULE_AUTHOR("Jens Axboe");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Completely Fair Queueing IO scheduler");
