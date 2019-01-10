// SPDX-License-Identifier: GPL-2.0
/*
 * Shared application/kernel submission and completion ring pairs, for
 * supporting fast/efficient IO.
 *
 * Copyright (C) 2019 Jens Axboe
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/syscalls.h>
#include <linux/refcount.h>
#include <linux/uio.h>

#include <linux/sched/signal.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmu_context.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/blkdev.h>
#include <linux/anon_inodes.h>

#include <linux/uaccess.h>
#include <linux/nospec.h>

#include <uapi/linux/io_uring.h>

#include "internal.h"

struct io_uring {
	u32 head ____cacheline_aligned_in_smp;
	u32 tail ____cacheline_aligned_in_smp;
};

struct io_sq_ring {
	struct io_uring		r;
	u32			ring_mask;
	u32			ring_entries;
	u32			dropped;
	u32			flags;
	u32			array[];
};

struct io_cq_ring {
	struct io_uring		r;
	u32			ring_mask;
	u32			ring_entries;
	u32			overflow;
	struct io_uring_cqe	cqes[];
};

struct io_ring_ctx {
	struct percpu_ref	refs;

	unsigned int		flags;

	/* SQ ring */
	struct io_sq_ring	*sq_ring;
	unsigned		sq_entries;
	unsigned		sq_mask;
	struct io_uring_sqe	*sq_sqes;

	/* CQ ring */
	struct io_cq_ring	*cq_ring;
	unsigned		cq_entries;
	unsigned		cq_mask;

	struct completion	ctx_done;

	/* iopoll submission state */
	struct {
		spinlock_t		poll_lock;
		struct list_head	poll_submitted;
	} ____cacheline_aligned_in_smp;

	struct {
		struct list_head	poll_completing;
		spinlock_t		completion_lock;
		struct mutex		uring_lock;
		wait_queue_head_t	wait;
	} ____cacheline_aligned_in_smp;
};

struct fsync_iocb {
	struct work_struct	work;
	struct file		*file;
	bool			datasync;
};

struct io_kiocb {
	union {
		struct kiocb		rw;
		struct fsync_iocb	fsync;
	};

	struct io_ring_ctx	*ki_ctx;
	unsigned long		ki_index;
	struct list_head	ki_list;
	unsigned long		ki_flags;
#define KIOCB_F_IOPOLL_COMPLETED	0	/* polled IO has completed */
#define KIOCB_F_IOPOLL_EAGAIN		1	/* submission got EAGAIN */
};

#define IO_PLUG_THRESHOLD		2
#define IO_IOPOLL_BATCH			8

struct sqe_submit {
	const struct io_uring_sqe *sqe;
	unsigned index;
};

struct io_submit_state {
	struct io_ring_ctx *ctx;

	struct blk_plug plug;
#ifdef CONFIG_BLOCK
	struct blk_plug_cb plug_cb;
#endif

	/*
	 * Polled iocbs that have been submitted, but not added to the ctx yet
	 */
	struct list_head req_list;
	unsigned int req_count;
};

static struct kmem_cache *kiocb_cachep;

static const struct file_operations io_scqring_fops;

static void io_ring_ctx_ref_free(struct percpu_ref *ref)
{
	struct io_ring_ctx *ctx = container_of(ref, struct io_ring_ctx, refs);

	complete(&ctx->ctx_done);
}

static struct io_ring_ctx *io_ring_ctx_alloc(struct io_uring_params *p)
{
	struct io_ring_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	if (percpu_ref_init(&ctx->refs, io_ring_ctx_ref_free, 0, GFP_KERNEL)) {
		kfree(ctx);
		return NULL;
	}

	ctx->flags = p->flags;

	init_completion(&ctx->ctx_done);

	spin_lock_init(&ctx->completion_lock);
	init_waitqueue_head(&ctx->wait);
	spin_lock_init(&ctx->poll_lock);
	INIT_LIST_HEAD(&ctx->poll_submitted);
	INIT_LIST_HEAD(&ctx->poll_completing);
	mutex_init(&ctx->uring_lock);

	return ctx;
}

static void io_inc_cqring(struct io_ring_ctx *ctx)
{
	struct io_cq_ring *ring = ctx->cq_ring;

	ring->r.tail++;
	smp_wmb();
}

static struct io_uring_cqe *io_peek_cqring(struct io_ring_ctx *ctx)
{
	struct io_cq_ring *ring = ctx->cq_ring;
	unsigned tail;

	smp_rmb();
	tail = READ_ONCE(ring->r.tail);
	if (tail + 1 == READ_ONCE(ring->r.head))
		return NULL;

	return &ring->cqes[tail & ctx->cq_mask];
}

static struct io_kiocb *io_get_kiocb(struct io_ring_ctx *ctx)
{
	struct io_kiocb *req;

	if (!percpu_ref_tryget(&ctx->refs))
		return NULL;

	req = kmem_cache_alloc(kiocb_cachep, GFP_KERNEL);
	if (!req)
		return NULL;

	req->ki_ctx = ctx;
	INIT_LIST_HEAD(&req->ki_list);
	req->ki_flags = 0;
	return req;
}

static void io_ring_drop_ctx_ref(struct io_ring_ctx *ctx, unsigned refs)
{
	percpu_ref_put_many(&ctx->refs, refs);

	if (waitqueue_active(&ctx->wait))
		wake_up(&ctx->wait);
}

static void io_free_kiocb_many(struct io_ring_ctx *ctx, void **iocbs, int *nr)
{
	if (*nr) {
		kmem_cache_free_bulk(kiocb_cachep, *nr, iocbs);
		io_ring_drop_ctx_ref(ctx, *nr);
		*nr = 0;
	}
}

static void io_free_kiocb(struct io_kiocb *iocb)
{
	kmem_cache_free(kiocb_cachep, iocb);
	io_ring_drop_ctx_ref(iocb->ki_ctx, 1);
}

/*
 * Find and free completed poll iocbs
 */
static void io_iopoll_reap(struct io_ring_ctx *ctx, unsigned int *nr_events)
{
	void *iocbs[IO_IOPOLL_BATCH];
	struct io_kiocb *iocb, *n;
	int to_free = 0;

	list_for_each_entry_safe(iocb, n, &ctx->poll_completing, ki_list) {
		if (!test_bit(KIOCB_F_IOPOLL_COMPLETED, &iocb->ki_flags))
			continue;
		if (to_free == ARRAY_SIZE(iocbs))
			io_free_kiocb_many(ctx, iocbs, &to_free);

		list_del(&iocb->ki_list);
		iocbs[to_free++] = iocb;

		fput(iocb->rw.ki_filp);
		(*nr_events)++;
	}

	if (to_free)
		io_free_kiocb_many(ctx, iocbs, &to_free);
}

/*
 * Poll for a mininum of 'min' events, and a maximum of 'max'. Note that if
 * min == 0 we consider that a non-spinning poll check - we'll still enter
 * the driver poll loop, but only as a non-spinning completion check.
 */
static int io_iopoll_getevents(struct io_ring_ctx *ctx, unsigned int *nr_events,
				long min)
{
	struct io_kiocb *iocb;
	int found, polled, ret;

	/*
	 * Check if we already have done events that satisfy what we need
	 */
	if (!list_empty(&ctx->poll_completing)) {
		io_iopoll_reap(ctx, nr_events);
		if (min && *nr_events >= min)
			return 0;
	}

	/*
	 * Take in a new working set from the submitted list, if possible.
	 */
	if (!list_empty_careful(&ctx->poll_submitted)) {
		spin_lock(&ctx->poll_lock);
		list_splice_init(&ctx->poll_submitted, &ctx->poll_completing);
		spin_unlock(&ctx->poll_lock);
	}

	if (list_empty(&ctx->poll_completing))
		return 0;

	/*
	 * Check again now that we have a new batch.
	 */
	io_iopoll_reap(ctx, nr_events);
	if (min && *nr_events >= min)
		return 0;

	polled = found = 0;
	list_for_each_entry(iocb, &ctx->poll_completing, ki_list) {
		/*
		 * Poll for needed events with spin == true, anything after
		 * that we just check if we have more, up to max.
		 */
		bool spin = !polled || *nr_events < min;
		struct kiocb *kiocb = &iocb->rw;

		if (test_bit(KIOCB_F_IOPOLL_COMPLETED, &iocb->ki_flags))
			break;

		found++;
		ret = kiocb->ki_filp->f_op->iopoll(kiocb, spin);
		if (ret < 0)
			return ret;

		polled += ret;
	}

	io_iopoll_reap(ctx, nr_events);
	if (*nr_events >= min)
		return 0;
	return found;
}

/*
 * We can't just wait for polled events to come to us, we have to actively
 * find and complete them.
 */
static void io_iopoll_reap_events(struct io_ring_ctx *ctx)
{
	if (!(ctx->flags & IORING_SETUP_IOPOLL))
		return;

	mutex_lock(&ctx->uring_lock);
	while (!list_empty_careful(&ctx->poll_submitted) ||
	       !list_empty(&ctx->poll_completing)) {
		unsigned int nr_events = 0;

		io_iopoll_getevents(ctx, &nr_events, 1);
	}
	mutex_unlock(&ctx->uring_lock);
}

static int io_iopoll_check(struct io_ring_ctx *ctx, unsigned *nr_events,
			   long min)
{
	int ret = 0;

	while (!*nr_events || !need_resched()) {
		int tmin = 0;

		if (*nr_events < min)
			tmin = min - *nr_events;

		ret = io_iopoll_getevents(ctx, nr_events, tmin);
		if (ret <= 0)
			break;
		ret = 0;
	}

	return ret;
}

static void kiocb_end_write(struct kiocb *kiocb)
{
	if (kiocb->ki_flags & IOCB_WRITE) {
		struct inode *inode = file_inode(kiocb->ki_filp);

		/*
		 * Tell lockdep we inherited freeze protection from submission
		 * thread.
		 */
		if (S_ISREG(inode->i_mode))
			__sb_writers_acquired(inode->i_sb, SB_FREEZE_WRITE);
		file_end_write(kiocb->ki_filp);
	}
}

static void __io_cqring_fill_event(struct io_ring_ctx *ctx, unsigned ki_index,
				   long res, unsigned ev_flags)
{
	struct io_uring_cqe *cqe;

	/*
	 * If we can't get a cq entry, userspace overflowed the
	 * submission (by quite a lot). Increment the overflow count in
	 * the ring.
	 */
	cqe = io_peek_cqring(ctx);
	if (cqe) {
		cqe->index = ki_index;
		cqe->res = res;
		cqe->flags = ev_flags;
		smp_wmb();
		io_inc_cqring(ctx);
	} else
		ctx->cq_ring->overflow++;
}

static void io_cqring_fill_event(struct io_ring_ctx *ctx, unsigned ki_index,
				 long res, unsigned ev_flags)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->completion_lock, flags);
	__io_cqring_fill_event(ctx, ki_index, res, ev_flags);
	spin_unlock_irqrestore(&ctx->completion_lock, flags);
}

static void io_complete_scqring_rw(struct kiocb *kiocb, long res, long res2)
{
	struct io_kiocb *iocb = container_of(kiocb, struct io_kiocb, rw);

	kiocb_end_write(kiocb);

	fput(kiocb->ki_filp);
	io_cqring_fill_event(iocb->ki_ctx, iocb->ki_index, res, 0);
	io_free_kiocb(iocb);
}

static void io_complete_scqring_iopoll(struct kiocb *kiocb, long res, long res2)
{
	struct io_kiocb *iocb = container_of(kiocb, struct io_kiocb, rw);

	kiocb_end_write(kiocb);

	if (unlikely(res == -EAGAIN)) {
		set_bit(KIOCB_F_IOPOLL_EAGAIN, &iocb->ki_flags);
	} else {
		__io_cqring_fill_event(iocb->ki_ctx, iocb->ki_index, res, 0);
		set_bit(KIOCB_F_IOPOLL_COMPLETED, &iocb->ki_flags);
	}
}

static int io_prep_rw(struct io_kiocb *kiocb, const struct io_uring_sqe *sqe)
{
	struct io_ring_ctx *ctx = kiocb->ki_ctx;
	struct kiocb *req = &kiocb->rw;
	int ret;

	req->ki_filp = fget(sqe->fd);
	if (unlikely(!req->ki_filp))
		return -EBADF;
	req->ki_pos = sqe->off;
	req->ki_flags = iocb_flags(req->ki_filp);
	req->ki_hint = ki_hint_validate(file_write_hint(req->ki_filp));
	if (sqe->ioprio) {
		ret = ioprio_check_cap(sqe->ioprio);
		if (ret)
			goto out_fput;

		req->ki_ioprio = sqe->ioprio;
	} else
		req->ki_ioprio = get_current_ioprio();

	ret = kiocb_set_rw_flags(req, sqe->rw_flags);
	if (unlikely(ret))
		goto out_fput;

	if (ctx->flags & IORING_SETUP_IOPOLL) {
		ret = -EOPNOTSUPP;
		if (!(req->ki_flags & IOCB_DIRECT) ||
		    !req->ki_filp->f_op->iopoll)
			goto out_fput;

		req->ki_flags |= IOCB_HIPRI;
		req->ki_complete = io_complete_scqring_iopoll;
	} else {
		if (req->ki_flags & IOCB_HIPRI) {
			ret = -EINVAL;
			goto out_fput;
		}
		req->ki_complete = io_complete_scqring_rw;
	}
	return 0;
out_fput:
	fput(req->ki_filp);
	return ret;
}

static inline void io_rw_done(struct kiocb *req, ssize_t ret)
{
	switch (ret) {
	case -EIOCBQUEUED:
		break;
	case -ERESTARTSYS:
	case -ERESTARTNOINTR:
	case -ERESTARTNOHAND:
	case -ERESTART_RESTARTBLOCK:
		/*
		 * There's no easy way to restart the syscall since other AIO's
		 * may be already running. Just fail this IO with EINTR.
		 */
		ret = -EINTR;
		/*FALLTHRU*/
	default:
		req->ki_complete(req, ret, 0);
	}
}

/*
 * Called either at the end of IO submission, or through a plug callback
 * because we're going to schedule. Moves out local batch of requests to
 * the ctx poll list, so they can be found for polling + reaping.
 */
static void io_flush_state_reqs(struct io_ring_ctx *ctx,
				 struct io_submit_state *state)
{
	spin_lock(&ctx->poll_lock);
	list_splice_tail_init(&state->req_list, &ctx->poll_submitted);
	spin_unlock(&ctx->poll_lock);
	state->req_count = 0;
}

static void io_iopoll_iocb_add_list(struct io_kiocb *kiocb)
{
	const int front = test_bit(KIOCB_F_IOPOLL_COMPLETED, &kiocb->ki_flags);
	struct io_ring_ctx *ctx = kiocb->ki_ctx;

	/*
	 * For fast devices, IO may have already completed. If it has, add
	 * it to the front so we find it first. We can't add to the poll_done
	 * list as that's unlocked from the completion side.
	 */
	spin_lock(&ctx->poll_lock);
	if (front)
		list_add(&kiocb->ki_list, &ctx->poll_submitted);
	else
		list_add_tail(&kiocb->ki_list, &ctx->poll_submitted);
	spin_unlock(&ctx->poll_lock);
}

static void io_iopoll_iocb_add_state(struct io_submit_state *state,
				     struct io_kiocb *kiocb)
{
	if (test_bit(KIOCB_F_IOPOLL_COMPLETED, &kiocb->ki_flags))
		list_add(&kiocb->ki_list, &state->req_list);
	else
		list_add_tail(&kiocb->ki_list, &state->req_list);

	if (++state->req_count >= IO_IOPOLL_BATCH)
		io_flush_state_reqs(state->ctx, state);
}

/*
 * After the iocb has been issued, it's safe to be found on the poll list.
 * Adding the kiocb to the list AFTER submission ensures that we don't
 * find it from a io_getevents() thread before the issuer is done accessing
 * the kiocb cookie.
 */
static void io_iopoll_kiocb_issued(struct io_submit_state *state,
				   struct io_kiocb *kiocb)
{
	if (!state || !IS_ENABLED(CONFIG_BLOCK))
		io_iopoll_iocb_add_list(kiocb);
	else
		io_iopoll_iocb_add_state(state, kiocb);
}

static ssize_t io_read(struct io_kiocb *kiocb, const struct io_uring_sqe *sqe)
{
	struct iovec inline_vecs[UIO_FASTIOV], *iovec = inline_vecs;
	void __user *buf = (void __user *) (uintptr_t) sqe->addr;
	struct kiocb *req = &kiocb->rw;
	struct iov_iter iter;
	struct file *file;
	ssize_t ret;

	ret = io_prep_rw(kiocb, sqe);
	if (ret)
		return ret;
	file = req->ki_filp;

	ret = -EBADF;
	if (unlikely(!(file->f_mode & FMODE_READ)))
		goto out_fput;
	ret = -EINVAL;
	if (unlikely(!file->f_op->read_iter))
		goto out_fput;

	ret = import_iovec(READ, buf, sqe->len, UIO_FASTIOV, &iovec, &iter);
	if (ret)
		goto out_fput;

	ret = rw_verify_area(READ, file, &req->ki_pos, iov_iter_count(&iter));
	if (!ret)
		io_rw_done(req, call_read_iter(file, req, &iter));
	kfree(iovec);
out_fput:
	if (unlikely(ret))
		fput(file);
	return ret;
}

static ssize_t io_write(struct io_kiocb *kiocb, const struct io_uring_sqe *sqe)
{
	struct iovec inline_vecs[UIO_FASTIOV], *iovec = inline_vecs;
	void __user *buf = (void __user *) (uintptr_t) sqe->addr;
	struct kiocb *req = &kiocb->rw;
	struct iov_iter iter;
	struct file *file;
	ssize_t ret;

	ret = io_prep_rw(kiocb, sqe);
	if (ret)
		return ret;
	file = req->ki_filp;

	ret = -EBADF;
	if (unlikely(!(file->f_mode & FMODE_WRITE)))
		goto out_fput;
	ret = -EINVAL;
	if (unlikely(!file->f_op->write_iter))
		goto out_fput;

	ret = import_iovec(WRITE, buf, sqe->len, UIO_FASTIOV, &iovec, &iter);
	if (ret)
		goto out_fput;

	ret = rw_verify_area(WRITE, file, &req->ki_pos, iov_iter_count(&iter));
	if (!ret) {
		/*
		 * Open-code file_start_write here to grab freeze protection,
		 * which will be released by another thread in
		 * io_complete_rw().  Fool lockdep by telling it the lock got
		 * released so that it doesn't complain about the held lock when
		 * we return to userspace.
		 */
		if (S_ISREG(file_inode(file)->i_mode)) {
			__sb_start_write(file_inode(file)->i_sb,
						SB_FREEZE_WRITE, true);
			__sb_writers_release(file_inode(file)->i_sb,
						SB_FREEZE_WRITE);
		}
		req->ki_flags |= IOCB_WRITE;
		io_rw_done(req, call_write_iter(file, req, &iter));
	}
out_fput:
	if (unlikely(ret))
		fput(file);
	return ret;
}

static void io_fsync_work(struct work_struct *work)
{
	struct fsync_iocb *req = container_of(work, struct fsync_iocb, work);
	struct io_kiocb *iocb = container_of(req, struct io_kiocb, fsync);
	int ret;

	ret = vfs_fsync(req->file, req->datasync);
	fput(req->file);

	io_cqring_fill_event(iocb->ki_ctx, iocb->ki_index, ret, 0);
	io_free_kiocb(iocb);
}

static int io_fsync(struct io_kiocb *kiocb, const struct io_uring_sqe *sqe,
		    bool datasync)
{
	struct fsync_iocb *req = &kiocb->fsync;

	if (kiocb->ki_ctx->flags & IORING_SETUP_IOPOLL)
		return -EINVAL;
	if (unlikely(sqe->addr || sqe->off || sqe->len || sqe->__resv))
		return -EINVAL;

	req->file = fget(sqe->fd);
	if (unlikely(!req->file))
		return -EBADF;
	if (unlikely(!req->file->f_op->fsync)) {
		fput(req->file);
		return -EINVAL;
	}

	req->datasync = datasync;
	INIT_WORK(&req->work, io_fsync_work);
	schedule_work(&req->work);
	return 0;
}

static int io_submit_sqe(struct io_ring_ctx *ctx, struct sqe_submit *s,
			 struct io_submit_state *state)
{
	const struct io_uring_sqe *sqe = s->sqe;
	struct io_kiocb *req;
	ssize_t ret;

	/* enforce forwards compatibility on users */
	if (unlikely(sqe->flags))
		return -EINVAL;

	req = io_get_kiocb(ctx);
	if (unlikely(!req))
		return -EAGAIN;

	ret = -EINVAL;
	if (s->index >= ctx->sq_entries)
		goto out_put_req;
	req->ki_index = s->index;

	ret = -EINVAL;
	switch (sqe->opcode) {
	case IORING_OP_READV:
		ret = io_read(req, sqe);
		break;
	case IORING_OP_WRITEV:
		ret = io_write(req, sqe);
		break;
	case IORING_OP_FSYNC:
		ret = io_fsync(req, sqe, false);
		break;
	case IORING_OP_FDSYNC:
		ret = io_fsync(req, sqe, true);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	/*
	 * If ret is 0, ->ki_complete() has either been called, or will get
	 * called later on. Anything else, we need to free the req.
	 */
	if (ret)
		goto out_put_req;
	if (ctx->flags & IORING_SETUP_IOPOLL) {
		if (test_bit(KIOCB_F_IOPOLL_EAGAIN, &req->ki_flags)) {
			ret = -EAGAIN;
			goto out_put_req;
		}
		io_iopoll_kiocb_issued(state, req);
	}
	return 0;
out_put_req:
	io_free_kiocb(req);
	return ret;
}

#ifdef CONFIG_BLOCK
static void io_state_unplug(struct blk_plug_cb *cb, bool from_schedule)
{
	struct io_submit_state *state;

	state = container_of(cb, struct io_submit_state, plug_cb);
	if (!list_empty(&state->req_list))
		io_flush_state_reqs(state->ctx, state);
}
#endif

/*
 * Batched submission is done, ensure local IO is flushed out.
 */
static void io_submit_state_end(struct io_submit_state *state)
{
	blk_finish_plug(&state->plug);
	if (!list_empty(&state->req_list))
		io_flush_state_reqs(state->ctx, state);
}

/*
 * Start submission side cache.
 */
static void io_submit_state_start(struct io_submit_state *state,
				  struct io_ring_ctx *ctx)
{
	state->ctx = ctx;
	INIT_LIST_HEAD(&state->req_list);
	state->req_count = 0;
#ifdef CONFIG_BLOCK
	state->plug_cb.callback = io_state_unplug;
	blk_start_plug(&state->plug);
	list_add(&state->plug_cb.list, &state->plug.cb_list);
#endif
}

static void io_inc_sqring(struct io_ring_ctx *ctx)
{
	struct io_sq_ring *ring = ctx->sq_ring;

	ring->r.head++;
	smp_wmb();
}

static bool io_peek_sqring(struct io_ring_ctx *ctx, struct sqe_submit *s)
{
	struct io_sq_ring *ring = ctx->sq_ring;
	unsigned head;

	smp_rmb();
	head = READ_ONCE(ring->r.head);
	if (head == READ_ONCE(ring->r.tail))
		return false;

	head = ring->array[head & ctx->sq_mask];
	if (head < ctx->sq_entries) {
		s->index = head;
		s->sqe = &ctx->sq_sqes[head];
		return true;
	}

	/* drop invalid entries */
	ring->r.head++;
	ring->dropped++;
	smp_wmb();
	return false;
}

static int io_ring_submit(struct io_ring_ctx *ctx, unsigned int to_submit)
{
	struct io_submit_state state, *statep = NULL;
	int i, ret = 0, submit = 0;

	if (to_submit > IO_PLUG_THRESHOLD) {
		io_submit_state_start(&state, ctx);
		statep = &state;
	}

	for (i = 0; i < to_submit; i++) {
		struct sqe_submit s;

		if (!io_peek_sqring(ctx, &s))
			break;

		ret = io_submit_sqe(ctx, &s, statep);
		if (ret)
			break;

		submit++;
		io_inc_sqring(ctx);
	}

	if (statep)
		io_submit_state_end(statep);

	return submit ? submit : ret;
}

/*
 * Wait until events become available, if we don't already have some. The
 * application must reap them itself, as they reside on the shared cq ring.
 */
static int io_cqring_wait(struct io_ring_ctx *ctx, int min_events)
{
	struct io_cq_ring *ring = ctx->cq_ring;
	DEFINE_WAIT(wait);
	int ret = 0;

	smp_rmb();
	if (ring->r.head != ring->r.tail)
		return 0;
	if (!min_events)
		return 0;

	do {
		prepare_to_wait(&ctx->wait, &wait, TASK_INTERRUPTIBLE);

		ret = 0;
		smp_rmb();
		if (ring->r.head != ring->r.tail)
			break;

		schedule();

		ret = -EINTR;
		if (signal_pending(current))
			break;
	} while (1);

	finish_wait(&ctx->wait, &wait);
	return ring->r.head == ring->r.tail ? ret : 0;
}

static int __io_uring_enter(struct io_ring_ctx *ctx, unsigned to_submit,
			    unsigned min_complete, unsigned flags)
{
	int ret = 0;

	if (to_submit) {
		ret = io_ring_submit(ctx, to_submit);
		if (ret < 0)
			return ret;
	}
	if (flags & IORING_ENTER_GETEVENTS) {
		unsigned nr_events = 0;
		int get_ret;

		if (!ret && to_submit)
			min_complete = 0;

		if (ctx->flags & IORING_SETUP_IOPOLL)
			get_ret = io_iopoll_check(ctx, &nr_events,
							min_complete);
		else
			get_ret = io_cqring_wait(ctx, min_complete);
		if (get_ret < 0 && !ret)
			ret = get_ret;
	}

	return ret;
}

static void io_free_scq_urings(struct io_ring_ctx *ctx)
{
	if (ctx->sq_ring) {
		page_frag_free(ctx->sq_ring);
		ctx->sq_ring = NULL;
	}
	if (ctx->sq_sqes) {
		page_frag_free(ctx->sq_sqes);
		ctx->sq_sqes = NULL;
	}
	if (ctx->cq_ring) {
		page_frag_free(ctx->cq_ring);
		ctx->cq_ring = NULL;
	}
}

static void io_ring_ctx_free(struct io_ring_ctx *ctx)
{
	io_iopoll_reap_events(ctx);
	io_free_scq_urings(ctx);
	percpu_ref_exit(&ctx->refs);
	kfree(ctx);
}

static void io_ring_ctx_wait_and_kill(struct io_ring_ctx *ctx)
{
	percpu_ref_kill(&ctx->refs);
	io_iopoll_reap_events(ctx);
	wait_for_completion(&ctx->ctx_done);
	io_ring_ctx_free(ctx);
}

static int io_scqring_release(struct inode *inode, struct file *file)
{
	struct io_ring_ctx *ctx = file->private_data;

	file->private_data = NULL;
	io_ring_ctx_wait_and_kill(ctx);
	return 0;
}

static int io_scqring_mmap(struct file *file, struct vm_area_struct *vma)
{
	loff_t offset = (loff_t) vma->vm_pgoff << PAGE_SHIFT;
	unsigned long sz = vma->vm_end - vma->vm_start;
	struct io_ring_ctx *ctx = file->private_data;
	unsigned long pfn;
	struct page *page;
	void *ptr;

	switch (offset) {
	case IORING_OFF_SQ_RING:
		ptr = ctx->sq_ring;
		break;
	case IORING_OFF_SQES:
		ptr = ctx->sq_sqes;
		break;
	case IORING_OFF_CQ_RING:
		ptr = ctx->cq_ring;
		break;
	default:
		return -EINVAL;
	}

	page = virt_to_head_page(ptr);
	if (sz > (PAGE_SIZE << compound_order(page)))
		return -EINVAL;

	pfn = virt_to_phys(ptr) >> PAGE_SHIFT;
	return remap_pfn_range(vma, vma->vm_start, pfn, sz, vma->vm_page_prot);
}

SYSCALL_DEFINE4(io_uring_enter, unsigned int, fd, u32, to_submit,
		u32, min_complete, u32, flags)
{
	struct io_ring_ctx *ctx;
	long ret = -EBADF;
	struct fd f;

	f = fdget(fd);
	if (!f.file)
		return -EBADF;

	ret = -EOPNOTSUPP;
	if (f.file->f_op != &io_scqring_fops)
		goto out_fput;

	ret = -EINVAL;
	ctx = f.file->private_data;
	if (!percpu_ref_tryget(&ctx->refs))
		goto out_fput;

	ret = -EBUSY;
	if (mutex_trylock(&ctx->uring_lock)) {
		ret = __io_uring_enter(ctx, to_submit, min_complete, flags);
		mutex_unlock(&ctx->uring_lock);
	}
	io_ring_drop_ctx_ref(ctx, 1);
out_fput:
	fdput(f);
	return ret;
}

static const struct file_operations io_scqring_fops = {
	.release	= io_scqring_release,
	.mmap		= io_scqring_mmap,
};

static void *io_mem_alloc(size_t size)
{
	gfp_t gfp_flags = GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN | __GFP_COMP |
				__GFP_NORETRY;

	return (void *) __get_free_pages(gfp_flags, get_order(size));
}

static int io_allocate_scq_urings(struct io_ring_ctx *ctx,
				  struct io_uring_params *p)
{
	struct io_sq_ring *sq_ring;
	struct io_cq_ring *cq_ring;
	size_t size;
	int ret;

	sq_ring = io_mem_alloc(struct_size(sq_ring, array, p->sq_entries));
	if (!sq_ring)
		return -ENOMEM;

	ctx->sq_ring = sq_ring;
	sq_ring->ring_mask = p->sq_entries - 1;
	sq_ring->ring_entries = p->sq_entries;
	ctx->sq_mask = sq_ring->ring_mask;
	ctx->sq_entries = sq_ring->ring_entries;

	ret = -EOVERFLOW;
	size = array_size(sizeof(struct io_uring_sqe), p->sq_entries);
	if (size == SIZE_MAX)
		goto err;
	ret = -ENOMEM;
	ctx->sq_sqes = io_mem_alloc(size);
	if (!ctx->sq_sqes)
		goto err;

	cq_ring = io_mem_alloc(struct_size(cq_ring, cqes, p->cq_entries));
	if (!cq_ring)
		goto err;

	ctx->cq_ring = cq_ring;
	cq_ring->ring_mask = p->cq_entries - 1;
	cq_ring->ring_entries = p->cq_entries;
	ctx->cq_mask = cq_ring->ring_mask;
	ctx->cq_entries = cq_ring->ring_entries;
	return 0;
err:
	io_free_scq_urings(ctx);
	return ret;
}

static void io_fill_offsets(struct io_uring_params *p)
{
	memset(&p->sq_off, 0, sizeof(p->sq_off));
	p->sq_off.head = offsetof(struct io_sq_ring, r.head);
	p->sq_off.tail = offsetof(struct io_sq_ring, r.tail);
	p->sq_off.ring_mask = offsetof(struct io_sq_ring, ring_mask);
	p->sq_off.ring_entries = offsetof(struct io_sq_ring, ring_entries);
	p->sq_off.flags = offsetof(struct io_sq_ring, flags);
	p->sq_off.dropped = offsetof(struct io_sq_ring, dropped);
	p->sq_off.array = offsetof(struct io_sq_ring, array);

	memset(&p->cq_off, 0, sizeof(p->cq_off));
	p->cq_off.head = offsetof(struct io_cq_ring, r.head);
	p->cq_off.tail = offsetof(struct io_cq_ring, r.tail);
	p->cq_off.ring_mask = offsetof(struct io_cq_ring, ring_mask);
	p->cq_off.ring_entries = offsetof(struct io_cq_ring, ring_entries);
	p->cq_off.overflow = offsetof(struct io_cq_ring, overflow);
	p->cq_off.cqes = offsetof(struct io_cq_ring, cqes);
}

static int io_uring_create(unsigned entries, struct io_uring_params *p)
{
	struct io_ring_ctx *ctx;
	int ret;

	/*
	 * Use twice as many entries for the CQ ring. It's possible for the
	 * application to drive a higher depth than the size of the SQ ring,
	 * since the sqes are only used at submission time. This allows for
	 * some flexibility in overcommitting a bit.
	 */
	p->sq_entries = roundup_pow_of_two(entries);
	p->cq_entries = 2 * p->sq_entries;

	ctx = io_ring_ctx_alloc(p);
	if (!ctx)
		return -ENOMEM;

	ret = io_allocate_scq_urings(ctx, p);
	if (ret)
		goto err;

	ret = anon_inode_getfd("[io_uring]", &io_scqring_fops, ctx,
				O_RDWR | O_CLOEXEC);
	if (ret < 0)
		goto err;

	io_fill_offsets(p);
	return ret;
err:
	io_ring_ctx_wait_and_kill(ctx);
	return ret;
}

/*
 * Sets up an aio uring context, and returns the fd. Applications asks for a
 * ring size, we return the actual sq/cq ring sizes (among other things) in the
 * params structure passed in.
 */
SYSCALL_DEFINE3(io_uring_setup, u32, entries, struct iovec __user *, iovecs,
		struct io_uring_params __user *, params)
{
	struct io_uring_params p;
	long ret;
	int i;

	if (copy_from_user(&p, params, sizeof(p)))
		return -EFAULT;
	for (i = 0; i < ARRAY_SIZE(p.resv); i++) {
		if (p.resv[i])
			return -EINVAL;
	}

	if (p.flags & ~IORING_SETUP_IOPOLL)
		return -EINVAL;
	if (iovecs)
		return -EINVAL;

	ret = io_uring_create(entries, &p);
	if (ret < 0)
		return ret;

	if (copy_to_user(params, &p, sizeof(p)))
		return -EFAULT;

	return ret;
}

static int __init io_uring_setup(void)
{
	kiocb_cachep = KMEM_CACHE(io_kiocb, SLAB_HWCACHE_ALIGN | SLAB_PANIC);
	return 0;
};
__initcall(io_uring_setup);
