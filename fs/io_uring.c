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
#include <linux/fdtable.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmu_context.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/blkdev.h>
#include <linux/anon_inodes.h>
#include <linux/sizes.h>
#include <linux/nospec.h>
#include <linux/kthread.h>
#include <linux/sched/mm.h>

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
	u32			array[0];
};

struct io_cq_ring {
	struct io_uring		r;
	u32			ring_mask;
	u32			ring_entries;
	u32			overflow;
	struct io_uring_event	events[0];
};

struct io_iocb_ring {
	struct			io_sq_ring *ring;
	unsigned		entries;
	unsigned		ring_mask;
	unsigned		sq_thread_cpu;
	struct io_uring_iocb	*iocbs;
};

struct io_event_ring {
	struct io_cq_ring	*ring;
	unsigned		entries;
	unsigned		ring_mask;
};

struct io_mapped_ubuf {
	u64		ubuf;
	size_t		len;
	struct		bio_vec *bvec;
	unsigned int	nr_bvecs;
};

struct io_sq_offload {
	struct task_struct	*thread;	/* if using a thread */
	bool			thread_poll;
	struct workqueue_struct	*wq;		/* wq offload */
	struct mm_struct	*mm;
	struct files_struct	*files;
	wait_queue_head_t	wait;
};

struct io_ring_ctx {
	struct percpu_ref	refs;

	unsigned int		flags;
	unsigned int		max_reqs;

	struct io_iocb_ring	sq_ring;
	struct io_event_ring	cq_ring;

	/* if used, fixed mapped user buffers */
	struct io_mapped_ubuf	*user_bufs;

	struct work_struct	work;

	/* sq ring submitter thread, if used */
	struct io_sq_offload	sq_offload;

	/* iopoll submission state */
	struct {
		spinlock_t poll_lock;
		struct list_head poll_submitted;
	} ____cacheline_aligned_in_smp;

	struct {
		struct list_head poll_completing;
		struct mutex uring_lock;
	} ____cacheline_aligned_in_smp;

	struct {
		struct mutex    ring_lock;
		wait_queue_head_t wait;
	} ____cacheline_aligned_in_smp;

	struct {
		spinlock_t      completion_lock;
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
#define KIOCB_F_FORCE_NONBLOCK		2	/* inline submission attempt */
};

#define IO_PLUG_THRESHOLD		2

#define IO_IOPOLL_BATCH	8

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

	/*
	 * io_kiocb alloc cache
	 */
	void *iocbs[IO_IOPOLL_BATCH];
	unsigned int free_iocbs;
	unsigned int cur_iocb;

	/*
	 * File reference cache
	 */
	struct file *file;
	unsigned int fd;
	unsigned int has_refs;
	unsigned int used_refs;
	unsigned int ios_left;
};

struct iocb_submit {
	const struct io_uring_iocb *iocb;
	unsigned int index;
};

struct io_work {
	struct work_struct work;
	struct io_ring_ctx *ctx;
	struct io_uring_iocb iocb;
	unsigned iocb_index;
};

static struct kmem_cache *kiocb_cachep, *ioctx_cachep;

static const struct file_operations io_scqring_fops;

static void io_ring_ctx_free(struct work_struct *work);
static void io_ring_ctx_ref_free(struct percpu_ref *ref);
static void io_sq_wq_submit_work(struct work_struct *work);

static struct io_ring_ctx *io_ring_ctx_alloc(struct io_uring_params *p)
{
	struct io_ring_ctx *ctx;

	ctx = kmem_cache_zalloc(ioctx_cachep, GFP_KERNEL);
	if (!ctx)
		return NULL;

	if (percpu_ref_init(&ctx->refs, io_ring_ctx_ref_free, 0, GFP_KERNEL)) {
		kmem_cache_free(ioctx_cachep, ctx);
		return NULL;
	}

	ctx->flags = p->flags;
	ctx->max_reqs = p->sq_entries;

	INIT_WORK(&ctx->work, io_ring_ctx_free);

	spin_lock_init(&ctx->completion_lock);
	mutex_init(&ctx->ring_lock);
	init_waitqueue_head(&ctx->wait);
	spin_lock_init(&ctx->poll_lock);
	INIT_LIST_HEAD(&ctx->poll_submitted);
	INIT_LIST_HEAD(&ctx->poll_completing);
	mutex_init(&ctx->uring_lock);

	return ctx;
}

static void io_inc_cqring(struct io_ring_ctx *ctx)
{
	struct io_cq_ring *ring = ctx->cq_ring.ring;

	ring->r.tail++;
	smp_wmb();
}

static struct io_uring_event *io_peek_cqring(struct io_ring_ctx *ctx)
{
	struct io_cq_ring *ring = ctx->cq_ring.ring;
	unsigned tail;

	smp_rmb();
	tail = READ_ONCE(ring->r.tail);
	if (tail + 1 == READ_ONCE(ring->r.head))
		return NULL;

	return &ring->events[tail & ctx->cq_ring.ring_mask];
}

static void io_req_init(struct io_ring_ctx *ctx, struct io_kiocb *req)
{
	percpu_ref_get(&ctx->refs);
	req->ki_ctx = ctx;
	INIT_LIST_HEAD(&req->ki_list);
	req->ki_flags = 0;
}

static struct io_kiocb *io_get_req(struct io_ring_ctx *ctx,
				   struct io_submit_state *state)
{
	struct io_kiocb *req;

	if (!state)
		req = kmem_cache_alloc(kiocb_cachep, GFP_KERNEL);
	else if (!state->free_iocbs) {
		size_t size;
		int ret;

		size = min_t(size_t, state->ios_left, ARRAY_SIZE(state->iocbs));
		ret = kmem_cache_alloc_bulk(kiocb_cachep, GFP_KERNEL, size,
						state->iocbs);
		if (ret <= 0)
			return ERR_PTR(-ENOMEM);
		state->free_iocbs = ret - 1;
		state->cur_iocb = 1;
		req = state->iocbs[0];
	} else {
		req = state->iocbs[state->cur_iocb];
		state->free_iocbs--;
		state->cur_iocb++;
	}

	if (req)
		io_req_init(ctx, req);
	return req;
}

static inline void iocb_put(struct io_kiocb *iocb)
{
	percpu_ref_put(&iocb->ki_ctx->refs);
	kmem_cache_free(kiocb_cachep, iocb);
}

static void iocb_put_many(struct io_ring_ctx *ctx, void **iocbs, int *nr)
{
	if (*nr) {
		percpu_ref_put_many(&ctx->refs, *nr);
		kmem_cache_free_bulk(kiocb_cachep, *nr, iocbs);
		*nr = 0;
	}
}

static void io_complete_iocb(struct io_ring_ctx *ctx, struct io_kiocb *iocb)
{
	if (waitqueue_active(&ctx->wait))
		wake_up(&ctx->wait);
	iocb_put(iocb);
}

/*
 * Find and free completed poll iocbs
 */
static void io_iopoll_reap(struct io_ring_ctx *ctx, unsigned int *nr_events)
{
	void *iocbs[IO_IOPOLL_BATCH];
	struct io_kiocb *iocb, *n;
	int file_count, to_free = 0;
	struct file *file = NULL;

	list_for_each_entry_safe(iocb, n, &ctx->poll_completing, ki_list) {
		if (!test_bit(KIOCB_F_IOPOLL_COMPLETED, &iocb->ki_flags))
			continue;
		if (to_free == ARRAY_SIZE(iocbs))
			iocb_put_many(ctx, iocbs, &to_free);

		list_del(&iocb->ki_list);
		iocbs[to_free++] = iocb;

		/*
		 * Batched puts of the same file, to avoid dirtying the
		 * file usage count multiple times, if avoidable.
		 */
		if (!file) {
			file = iocb->rw.ki_filp;
			file_count = 1;
		} else if (file == iocb->rw.ki_filp) {
			file_count++;
		} else {
			fput_many(file, file_count);
			file = iocb->rw.ki_filp;
			file_count = 1;
		}

		(*nr_events)++;
	}

	if (file)
		fput_many(file, file_count);

	if (to_free)
		iocb_put_many(ctx, iocbs, &to_free);
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

	while (!list_empty_careful(&ctx->poll_submitted) ||
	       !list_empty(&ctx->poll_completing)) {
		unsigned int nr_events = 0;

		io_iopoll_getevents(ctx, &nr_events, 1);
	}
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

static void io_fill_event(struct io_uring_event *ev, unsigned ki_index,
			  long res, unsigned flags)
{
	ev->index = ki_index;
	ev->res = res;
	ev->flags = flags;
}

static void io_cqring_fill_event(struct io_ring_ctx *ctx, unsigned ki_index,
				 long res, unsigned ev_flags)
{
	struct io_uring_event *ev;
	unsigned long flags;

	/*
	 * If we can't get a cq entry, userspace overflowed the
	 * submission (by quite a lot). Increment the overflow count in
	 * the ring.
	 */
	spin_lock_irqsave(&ctx->completion_lock, flags);
	ev = io_peek_cqring(ctx);
	if (ev) {
		io_fill_event(ev, ki_index, res, ev_flags);
		io_inc_cqring(ctx);
	} else
		ctx->cq_ring.ring->overflow++;
	spin_unlock_irqrestore(&ctx->completion_lock, flags);
}

static void io_complete_scqring(struct io_kiocb *iocb, long res, unsigned flags)
{
	io_cqring_fill_event(iocb->ki_ctx, iocb->ki_index, res, flags);
	io_complete_iocb(iocb->ki_ctx, iocb);
}

static void io_fill_cq_error(struct io_ring_ctx *ctx, unsigned ki_index,
			     long error)
{
	io_cqring_fill_event(ctx, ki_index, error, 0);

	/*
	 * for thread offload, app could already be sleeping in io_ring_enter()
	 * before we get to flag the error. wake them up, if needed.
	 */
	if (ctx->flags & (IORING_SETUP_SQTHREAD | IORING_SETUP_SQWQ))
		if (waitqueue_active(&ctx->wait))
			wake_up(&ctx->wait);
}

static void io_complete_scqring_rw(struct kiocb *kiocb, long res, long res2)
{
	struct io_kiocb *iocb = container_of(kiocb, struct io_kiocb, rw);

	kiocb_end_write(kiocb);

	fput(kiocb->ki_filp);

	io_complete_scqring(iocb, res, 0);
}

static void io_complete_scqring_iopoll(struct kiocb *kiocb, long res, long res2)
{
	struct io_kiocb *iocb = container_of(kiocb, struct io_kiocb, rw);

	kiocb_end_write(kiocb);

	if (unlikely(res == -EAGAIN)) {
		set_bit(KIOCB_F_IOPOLL_EAGAIN, &iocb->ki_flags);
	} else {
		io_cqring_fill_event(iocb->ki_ctx, iocb->ki_index, res, 0);
		set_bit(KIOCB_F_IOPOLL_COMPLETED, &iocb->ki_flags);
	}
}

static void io_file_put(struct io_submit_state *state, struct file *file)
{
	if (!state) {
		fput(file);
	} else if (state->file) {
		int diff = state->has_refs - state->used_refs;

		if (diff)
			fput_many(state->file, diff);
		state->file = NULL;
	}
}

/*
 * Get as many references to a file as we have IOs left in this submission,
 * assuming most submissions are for one file, or at least that each file
 * has more than one submission.
 */
static struct file *io_file_get(struct io_submit_state *state, int fd)
{
	if (!state)
		return fget(fd);

	if (!state->file) {
get_file:
		state->file = fget_many(fd, state->ios_left);
		if (!state->file)
			return NULL;

		state->fd = fd;
		state->has_refs = state->ios_left;
		state->used_refs = 1;
		state->ios_left--;
		return state->file;
	}

	if (state->fd == fd) {
		state->used_refs++;
		state->ios_left--;
		return state->file;
	}

	io_file_put(state, NULL);
	goto get_file;
}

static int io_prep_rw(struct io_kiocb *kiocb, const struct io_uring_iocb *iocb,
		      struct io_submit_state *state, bool force_nonblock)
{
	struct io_ring_ctx *ctx = kiocb->ki_ctx;
	struct kiocb *req = &kiocb->rw;
	int ret;

	req->ki_filp = io_file_get(state, iocb->fd);
	if (unlikely(!req->ki_filp))
		return -EBADF;
	req->ki_pos = iocb->off;
	req->ki_flags = iocb_flags(req->ki_filp);
	req->ki_hint = ki_hint_validate(file_write_hint(req->ki_filp));
	if (iocb->ioprio) {
		ret = ioprio_check_cap(iocb->ioprio);
		if (ret)
			goto out_fput;

		req->ki_ioprio = iocb->ioprio;
	} else
		req->ki_ioprio = get_current_ioprio();

	ret = kiocb_set_rw_flags(req, iocb->rw_flags);
	if (unlikely(ret))
		goto out_fput;
	if (force_nonblock) {
		req->ki_flags |= IOCB_NOWAIT;
		set_bit(KIOCB_F_FORCE_NONBLOCK, &kiocb->ki_flags);
	}

	if (ctx->flags & IORING_SETUP_IOPOLL) {
		ret = -EOPNOTSUPP;
		if (!(req->ki_flags & IOCB_DIRECT) ||
		    !req->ki_filp->f_op->iopoll)
			goto out_fput;

		req->ki_flags |= IOCB_HIPRI;
		req->ki_complete = io_complete_scqring_iopoll;
	} else {
		/* no one is going to poll for this I/O */
		req->ki_flags &= ~IOCB_HIPRI;
		req->ki_complete = io_complete_scqring_rw;
	}
	return 0;
out_fput:
	io_file_put(state, req->ki_filp);
	return ret;
}

static int io_setup_rw(int rw, struct io_kiocb *kiocb,
		       const struct io_uring_iocb *iocb, struct iovec **iovec,
		       struct iov_iter *iter, bool kaddr)
{
	void __user *buf = (void __user *)(uintptr_t)iocb->addr;
	size_t ret;

	if (!kaddr) {
		ret = import_single_range(rw, buf, iocb->len, *iovec, iter);
	} else {
		struct io_ring_ctx *ctx = kiocb->ki_ctx;
		struct io_mapped_ubuf *imu;
		size_t len = iocb->len;
		size_t offset;
		int index;

		/* __io_submit_one() already validated the index */
		index = array_index_nospec(kiocb->ki_index,
						ctx->max_reqs);
		imu = &ctx->user_bufs[index];
		if ((unsigned long) iocb->addr < imu->ubuf ||
		    (unsigned long) iocb->addr + len > imu->ubuf + imu->len) {
			ret = -EFAULT;
			goto err;
		}

		/*
		 * May not be a start of buffer, set size appropriately
		 * and advance us to the beginning.
		 */
		offset = (unsigned long) iocb->addr - imu->ubuf;
		iov_iter_bvec(iter, rw, imu->bvec, imu->nr_bvecs,
				offset + len);
		if (offset)
			iov_iter_advance(iter, offset);
		ret = 0;

	}
err:
	*iovec = NULL;
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
static void io_iopoll_iocb_issued(struct io_submit_state *state,
				  struct io_kiocb *kiocb)
{
	if (!state || !IS_ENABLED(CONFIG_BLOCK))
		io_iopoll_iocb_add_list(kiocb);
	else
		io_iopoll_iocb_add_state(state, kiocb);
}

static ssize_t io_read(struct io_kiocb *kiocb, const struct io_uring_iocb *iocb,
		       struct io_submit_state *state, bool kaddr, bool nonblock)
{
	struct iovec inline_vecs[UIO_FASTIOV], *iovec = inline_vecs;
	struct kiocb *req = &kiocb->rw;
	struct iov_iter iter;
	struct file *file;
	ssize_t ret;

	ret = io_prep_rw(kiocb, iocb, state, nonblock);
	if (ret)
		return ret;
	file = req->ki_filp;

	ret = -EBADF;
	if (unlikely(!(file->f_mode & FMODE_READ)))
		goto out_fput;
	ret = -EINVAL;
	if (unlikely(!file->f_op->read_iter))
		goto out_fput;

	ret = io_setup_rw(READ, kiocb, iocb, &iovec, &iter, kaddr);
	if (ret)
		goto out_fput;

	ret = rw_verify_area(READ, file, &req->ki_pos, iov_iter_count(&iter));
	if (!ret) {
		ssize_t ret2;

		/*
		 * Catch -EAGAIN return for forced non-blocking submission
		 */
		ret2 = call_read_iter(file, req, &iter);
		if (!nonblock || ret2 != -EAGAIN)
			io_rw_done(req, ret2);
		else
			ret = -EAGAIN;
	}
	kfree(iovec);
out_fput:
	if (unlikely(ret))
		fput(file);
	return ret;
}

static ssize_t io_write(struct io_kiocb *kiocb,
			const struct io_uring_iocb *iocb,
			struct io_submit_state *state, bool kaddr)
{
	struct iovec inline_vecs[UIO_FASTIOV], *iovec = inline_vecs;
	struct kiocb *req = &kiocb->rw;
	struct iov_iter iter;
	struct file *file;
	ssize_t ret;

	ret = io_prep_rw(kiocb, iocb, state, false);
	if (ret)
		return ret;
	file = req->ki_filp;

	ret = -EBADF;
	if (unlikely(!(file->f_mode & FMODE_WRITE)))
		goto out_fput;
	ret = -EINVAL;
	if (unlikely(!file->f_op->write_iter))
		goto out_fput;

	ret = io_setup_rw(WRITE, kiocb, iocb, &iovec, &iter, kaddr);
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
			__sb_start_write(file_inode(file)->i_sb, SB_FREEZE_WRITE, true);
			__sb_writers_release(file_inode(file)->i_sb, SB_FREEZE_WRITE);
		}
		req->ki_flags |= IOCB_WRITE;
		io_rw_done(req, call_write_iter(file, req, &iter));
	}
	kfree(iovec);
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

	io_complete_scqring(iocb, ret, 0);
}

static int io_fsync(struct fsync_iocb *req, const struct io_uring_iocb *iocb,
		    bool datasync)
{
	if (unlikely(iocb->addr || iocb->off || iocb->len || iocb->__resv))
		return -EINVAL;

	req->file = fget(iocb->fd);
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

static int __io_submit_one(struct io_ring_ctx *ctx,
			   const struct io_uring_iocb *iocb,
			   unsigned long ki_index,
			   struct io_submit_state *state, bool force_nonblock)
{
	struct io_kiocb *req;
	ssize_t ret;

	/* enforce forwards compatibility on users */
	if (unlikely(iocb->flags))
		return -EINVAL;

	req = io_get_req(ctx, state);
	if (unlikely(!req))
		return -EAGAIN;

	ret = -EINVAL;
	if (ki_index >= ctx->max_reqs)
		goto out_put_req;
	req->ki_index = ki_index;

	ret = -EINVAL;
	switch (iocb->opcode) {
	case IORING_OP_READ:
		ret = io_read(req, iocb, state, false, force_nonblock);
		break;
	case IORING_OP_READ_FIXED:
		ret = io_read(req, iocb, state, true, force_nonblock);
		break;
	case IORING_OP_WRITE:
		ret = io_write(req, iocb, state, false);
		break;
	case IORING_OP_WRITE_FIXED:
		ret = io_write(req, iocb, state, true);
		break;
	case IORING_OP_FSYNC:
		if (ctx->flags & IORING_SETUP_IOPOLL)
			break;
		ret = io_fsync(&req->fsync, iocb, false);
		break;
	case IORING_OP_FDSYNC:
		if (ctx->flags & IORING_SETUP_IOPOLL)
			break;
		ret = io_fsync(&req->fsync, iocb, true);
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
		io_iopoll_iocb_issued(state, req);
	}
	return 0;
out_put_req:
	iocb_put(req);
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
	io_file_put(state, NULL);
	if (state->free_iocbs)
		kmem_cache_free_bulk(kiocb_cachep, state->free_iocbs,
					&state->iocbs[state->cur_iocb]);
}

/*
 * Start submission side cache.
 */
static void io_submit_state_start(struct io_submit_state *state,
				  struct io_ring_ctx *ctx, unsigned max_ios)
{
	state->ctx = ctx;
	INIT_LIST_HEAD(&state->req_list);
	state->req_count = 0;
	state->free_iocbs = 0;
	state->file = NULL;
	state->ios_left = max_ios;
#ifdef CONFIG_BLOCK
	state->plug_cb.callback = io_state_unplug;
	blk_start_plug(&state->plug);
	list_add(&state->plug_cb.list, &state->plug.cb_list);
#endif
}

static void io_inc_sqring(struct io_ring_ctx *ctx)
{
	struct io_sq_ring *ring = ctx->sq_ring.ring;

	ring->r.head++;
	smp_wmb();
}

static const struct io_uring_iocb *io_peek_sqring(struct io_ring_ctx *ctx,
						  unsigned *iocb_index)
{
	struct io_sq_ring *ring = ctx->sq_ring.ring;
	unsigned head;

	smp_rmb();
	head = READ_ONCE(ring->r.head);
	if (head == READ_ONCE(ring->r.tail))
		return NULL;

	head = ring->array[head & ctx->sq_ring.ring_mask];
	if (head < ctx->sq_ring.entries) {
		*iocb_index = head;
		return &ctx->sq_ring.iocbs[head];
	}

	/* drop invalid entries */
	ring->r.head++;
	ring->dropped++;
	smp_wmb();
	return NULL;
}

static int io_ring_submit(struct io_ring_ctx *ctx, unsigned int to_submit)
{
	struct io_submit_state state, *statep = NULL;
	int i, ret = 0, submit = 0;

	if (to_submit > IO_PLUG_THRESHOLD) {
		io_submit_state_start(&state, ctx, to_submit);
		statep = &state;
	}

	for (i = 0; i < to_submit; i++) {
		const struct io_uring_iocb *iocb;
		unsigned iocb_index;

		iocb = io_peek_sqring(ctx, &iocb_index);
		if (!iocb)
			break;

		ret = __io_submit_one(ctx, iocb, iocb_index, statep, false);
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
	struct io_cq_ring *ring = ctx->cq_ring.ring;
	DEFINE_WAIT(wait);
	int ret;

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

static int io_queue_async_work(struct io_ring_ctx *ctx, struct iocb_submit *is)
{
	struct io_work *work;

	work = kmalloc(sizeof(*work), GFP_KERNEL);
	if (work) {
		memcpy(&work->iocb, is->iocb, sizeof(*is->iocb));
		work->iocb_index = is->index;
		INIT_WORK(&work->work, io_sq_wq_submit_work);
		work->ctx = ctx;
		queue_work(ctx->sq_offload.wq, &work->work);
		return 0;
	}

	return -ENOMEM;
}

static int io_submit_iocbs(struct io_ring_ctx *ctx, struct iocb_submit *iocbs,
			   unsigned int nr, struct mm_struct *cur_mm,
			   bool mm_fault)
{
	struct io_submit_state state, *statep = NULL;
	int ret, i, submitted = 0;
	bool force_nonblock;

	if (nr > IO_PLUG_THRESHOLD) {
		io_submit_state_start(&state, ctx, nr);
		statep = &state;
	}

	/*
	 * Having both a thread and a workqueue only makes sense for buffered
	 * IO, where we can't submit in an async fashion. Use the NOWAIT
	 * trick from the SQ thread, and punt to the workqueue if we can't
	 * satisfy this iocb without blocking. This is only necessary
	 * for buffered IO with sqthread polled submission.
	 */
	force_nonblock = (ctx->flags & IORING_SETUP_SQWQ) != 0;

	for (i = 0; i < nr; i++) {
		if (unlikely(mm_fault)) {
			ret = -EFAULT;
		} else {
			ret = __io_submit_one(ctx, iocbs[i].iocb,
						iocbs[i].index, statep,
						force_nonblock);
			/* nogo, submit to workqueue */
			if (force_nonblock && ret == -EAGAIN)
				ret = io_queue_async_work(ctx, &iocbs[i]);
			if (!ret) {
				submitted++;
				continue;
			}
		}

		io_fill_cq_error(ctx, iocbs[i].index, ret);
	}

	if (statep)
		io_submit_state_end(&state);

	return submitted;
}

/*
 * SQ thread is woken if the app asked for offloaded submission. This can
 * be either O_DIRECT, in which case we do submissions directly, or it can
 * be buffered IO, in which case we do them inline if we can do so without
 * blocking. If we can't, then we punt to a workqueue.
 */
static int io_sq_thread(void *data)
{
	struct iocb_submit iocbs[IO_IOPOLL_BATCH];
	struct io_ring_ctx *ctx = data;
	struct io_sq_offload *sqo = &ctx->sq_offload;
	struct mm_struct *cur_mm = NULL;
	struct files_struct *old_files;
	mm_segment_t old_fs;
	DEFINE_WAIT(wait);
	unsigned inflight;
	unsigned long timeout;

	old_files = current->files;
	current->files = sqo->files;

	old_fs = get_fs();
	set_fs(USER_DS);

	timeout = inflight = 0;
	while (!kthread_should_stop()) {
		const struct io_uring_iocb *iocb;
		bool mm_fault = false;
		unsigned iocb_index;
		int i;

		if (sqo->thread_poll && inflight) {
			unsigned int nr_events = 0;

			/*
			 * Normal IO, just pretend everything completed.
			 * We don't have to poll completions for that.
			 */
			if (ctx->flags & IORING_SETUP_IOPOLL)
				io_iopoll_check(ctx, &nr_events, 0);
			else
				nr_events = inflight;

			inflight -= nr_events;
			if (!inflight)
				timeout = jiffies + HZ;
		}

		iocb = io_peek_sqring(ctx, &iocb_index);
		if (!iocb) {
			/*
			 * If we're polling, let us spin for a second without
			 * work before going to sleep.
			 */
			if (sqo->thread_poll) {
				if (inflight || !time_after(jiffies, timeout)) {
					cpu_relax();
					continue;
				}
			}

			/*
			 * Drop cur_mm before scheduling, we can't hold it for
			 * long periods (or over schedule()). Do this before
			 * adding ourselves to the waitqueue, as the unuse/drop
			 * may sleep.
			 */
			if (cur_mm) {
				unuse_mm(cur_mm);
				mmput(cur_mm);
				cur_mm = NULL;
			}

			prepare_to_wait(&sqo->wait, &wait, TASK_INTERRUPTIBLE);

			/* Tell userspace we may need a wakeup call */
			if (sqo->thread_poll) {
				struct io_sq_ring *ring;

				ring = ctx->sq_ring.ring;
				ring->flags |= IORING_SQ_NEED_WAKEUP;
				smp_wmb();
			}

			iocb = io_peek_sqring(ctx, &iocb_index);
			if (!iocb) {
				if (kthread_should_park())
					kthread_parkme();
				if (kthread_should_stop()) {
					finish_wait(&sqo->wait, &wait);
					break;
				}
				if (signal_pending(current))
					flush_signals(current);
				schedule();

				if (sqo->thread_poll) {
					struct io_sq_ring *ring;

					ring = ctx->sq_ring.ring;
					ring->flags &= ~IORING_SQ_NEED_WAKEUP;
				}
			}
			finish_wait(&sqo->wait, &wait);
			if (!iocb)
				continue;
		}

		/* If ->mm is set, we're not doing FIXEDBUFS */
		if (sqo->mm && !cur_mm) {
			mm_fault = !mmget_not_zero(sqo->mm);
			if (!mm_fault) {
				use_mm(sqo->mm);
				cur_mm = sqo->mm;
			}
		}

		i = 0;
		do {
			if (i == ARRAY_SIZE(iocbs))
				break;
			iocbs[i].iocb = iocb;
			iocbs[i].index = iocb_index;
			++i;
			io_inc_sqring(ctx);
		} while ((iocb = io_peek_sqring(ctx, &iocb_index)) != NULL);

		inflight += io_submit_iocbs(ctx, iocbs, i, cur_mm, mm_fault);
	}
	current->files = old_files;
	set_fs(old_fs);
	if (cur_mm) {
		unuse_mm(cur_mm);
		mmput(cur_mm);
	}
	return 0;
}

static void io_sq_wq_submit_work(struct work_struct *work)
{
	struct io_work *iw = container_of(work, struct io_work, work);
	struct io_ring_ctx *ctx = iw->ctx;
	struct io_sq_offload *sqo = &ctx->sq_offload;
	mm_segment_t old_fs = get_fs();
	struct files_struct *old_files;
	int ret;

	old_files = current->files;
	current->files = sqo->files;

	if (sqo->mm) {
		if (!mmget_not_zero(sqo->mm)) {
			ret = -EFAULT;
			goto err;
		}
		use_mm(sqo->mm);
	}

	set_fs(USER_DS);

	ret = __io_submit_one(ctx, &iw->iocb, iw->iocb_index, NULL, false);

	set_fs(old_fs);
	if (sqo->mm) {
		unuse_mm(sqo->mm);
		mmput(sqo->mm);
	}

err:
	if (ret)
		io_fill_cq_error(ctx, iw->iocb_index, ret);
	current->files = old_files;
	kfree(iw);
}

/*
 * If this is a read, try a cached inline read first. If the IO is in the
 * page cache, we can satisfy it without blocking and without having to
 * punt to a threaded execution. This is much faster, particularly for
 * lower queue depth IO, and it's always a lot more efficient.
 */
static bool io_sq_try_inline(struct io_ring_ctx *ctx,
			     const struct io_uring_iocb *iocb, unsigned index)
{
	int ret;

	if (iocb->opcode != IORING_OP_READ &&
	    iocb->opcode != IORING_OP_READ_FIXED)
		return false;

	ret = __io_submit_one(ctx, iocb, index, NULL, true);

	/*
	 * If we get -EAGAIN, return false to submit out-of-line. Any other
	 * result and we're done, call will fill in CQ ring event.
	 */
	return ret != -EAGAIN;
}

static int io_sq_wq_submit(struct io_ring_ctx *ctx, unsigned int to_submit)
{
	const struct io_uring_iocb *iocb;
	unsigned iocb_index;
	int ret, queued;

	ret = queued = 0;
	while ((iocb = io_peek_sqring(ctx, &iocb_index)) != NULL) {
		ret = io_sq_try_inline(ctx, iocb, iocb_index);
		if (!ret) {
			struct iocb_submit is = {
				.iocb = iocb,
				.index = iocb_index
			};

			ret = io_queue_async_work(ctx, &is);
			if (ret)
				break;
		}

		io_inc_sqring(ctx);
		queued++;
		if (queued == to_submit)
			break;
	}

	return queued ? queued : ret;
}

static int __io_uring_enter(struct io_ring_ctx *ctx, unsigned to_submit,
			    unsigned min_complete, unsigned flags)
{
	int ret = 0;

	if (to_submit) {
		/*
		 * Three options here:
		 * 1) We have an sq thread, just wake it up to do submissions
		 * 2) We have an sq wq, queue a work item for each iocb
		 * 3) Submit directly
		 */
		if (ctx->flags & IORING_SETUP_SQTHREAD) {
			wake_up(&ctx->sq_offload.wait);
			ret = to_submit;
		} else if (ctx->flags & IORING_SETUP_SQWQ) {
			ret = io_sq_wq_submit(ctx, to_submit);
		} else {
			ret = io_ring_submit(ctx, to_submit);
			if (ret < 0)
				return ret;
		}
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

static void io_iocb_buffer_unmap(struct io_ring_ctx *ctx)
{
	int i, j;

	if (!ctx->user_bufs)
		return;

	for (i = 0; i < ctx->max_reqs; i++) {
		struct io_mapped_ubuf *imu = &ctx->user_bufs[i];

		for (j = 0; j < imu->nr_bvecs; j++)
			put_page(imu->bvec[j].bv_page);

		kfree(imu->bvec);
		imu->nr_bvecs = 0;
	}

	kfree(ctx->user_bufs);
	ctx->user_bufs = NULL;
}

static int io_iocb_buffer_map(struct io_ring_ctx *ctx,
			      struct iovec __user *iovecs)
{
	unsigned long total_pages, page_limit;
	struct page **pages = NULL;
	int i, j, got_pages = 0;
	int ret = -EINVAL;

	ctx->user_bufs = kcalloc(ctx->max_reqs, sizeof(struct io_mapped_ubuf),
					GFP_KERNEL);
	if (!ctx->user_bufs)
		return -ENOMEM;

	/* Don't allow more pages than we can safely lock */
	total_pages = 0;
	page_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	for (i = 0; i < ctx->max_reqs; i++) {
		struct io_mapped_ubuf *imu = &ctx->user_bufs[i];
		unsigned long off, start, end, ubuf;
		int pret, nr_pages;
		struct iovec iov;
		size_t size;

		ret = -EFAULT;
		if (copy_from_user(&iov, &iovecs[i], sizeof(iov)))
			goto err;

		/*
		 * Don't impose further limits on the size and buffer
		 * constraints here, we'll -EINVAL later when IO is
		 * submitted if they are wrong.
		 */
		ret = -EFAULT;
		if (!iov.iov_base)
			goto err;

		/* arbitrary limit, but we need something */
		if (iov.iov_len > SZ_4M)
			goto err;

		ubuf = (unsigned long) iov.iov_base;
		end = (ubuf + iov.iov_len + PAGE_SIZE - 1) >> PAGE_SHIFT;
		start = ubuf >> PAGE_SHIFT;
		nr_pages = end - start;

		ret = -ENOMEM;
		if (total_pages + nr_pages > page_limit)
			goto err;

		if (!pages || nr_pages > got_pages) {
			kfree(pages);
			pages = kmalloc(nr_pages * sizeof(struct page *),
					GFP_KERNEL);
			if (!pages)
				goto err;
			got_pages = nr_pages;
		}

		imu->bvec = kmalloc(nr_pages * sizeof(struct bio_vec),
					GFP_KERNEL);
		if (!imu->bvec)
			goto err;

		down_write(&current->mm->mmap_sem);
		pret = get_user_pages(ubuf, nr_pages, 1, pages, NULL);
		up_write(&current->mm->mmap_sem);

		if (pret < nr_pages) {
			if (pret < 0)
				ret = pret;
			goto err;
		}

		off = ubuf & ~PAGE_MASK;
		size = iov.iov_len;
		for (j = 0; j < nr_pages; j++) {
			size_t vec_len;

			vec_len = min_t(size_t, size, PAGE_SIZE - off);
			imu->bvec[j].bv_page = pages[j];
			imu->bvec[j].bv_len = vec_len;
			imu->bvec[j].bv_offset = off;
			off = 0;
			size -= vec_len;
		}
		/* store original address for later verification */
		imu->ubuf = ubuf;
		imu->len = iov.iov_len;
		imu->nr_bvecs = nr_pages;
		total_pages += nr_pages;
	}
	kfree(pages);
	return 0;
err:
	kfree(pages);
	io_iocb_buffer_unmap(ctx);
	return ret;
}

static int io_sq_thread(void *);

static int io_sq_thread_start(struct io_ring_ctx *ctx)
{
	struct io_sq_offload *sqo = &ctx->sq_offload;
	struct io_iocb_ring *ring = &ctx->sq_ring;
	int ret;

	memset(sqo, 0, sizeof(*sqo));
	init_waitqueue_head(&sqo->wait);

	if (!(ctx->flags & IORING_SETUP_FIXEDBUFS))
		sqo->mm = current->mm;

	ret = -EBADF;
	sqo->files = get_files_struct(current);
	if (!sqo->files)
		goto err;

	if (ctx->flags & IORING_SETUP_SQPOLL)
		sqo->thread_poll = true;

	if (ctx->flags & IORING_SETUP_SQTHREAD) {
		sqo->thread = kthread_create_on_cpu(io_sq_thread, ctx,
							ring->sq_thread_cpu,
							"io_uring-sq");
		if (IS_ERR(sqo->thread)) {
			ret = PTR_ERR(sqo->thread);
			sqo->thread = NULL;
			goto err;
		}
		wake_up_process(sqo->thread);
	}
	if (ctx->flags & IORING_SETUP_SQWQ) {
		int concurrency;

		/* Do QD, or 2 * CPUS, whatever is smallest */
		concurrency = min(ring->entries - 1, 2 * num_online_cpus());
		sqo->wq = alloc_workqueue("io_ring-wq",
						WQ_UNBOUND | WQ_FREEZABLE,
						concurrency);
		if (!sqo->wq) {
			ret = -ENOMEM;
			goto err;
		}
	}

	return 0;
err:
	if (sqo->files) {
		put_files_struct(sqo->files);
		sqo->files = NULL;
	}
	if (sqo->mm)
		sqo->mm = NULL;
	return ret;
}

static void io_sq_thread_stop(struct io_ring_ctx *ctx)
{
	struct io_sq_offload *sqo = &ctx->sq_offload;

	if (sqo->thread) {
		kthread_park(sqo->thread);
		kthread_stop(sqo->thread);
		sqo->thread = NULL;
	}
	if (sqo->wq) {
		destroy_workqueue(sqo->wq);
		sqo->wq = NULL;
	}
	if (sqo->files) {
		put_files_struct(sqo->files);
		sqo->files = NULL;
	}
}

static void io_free_scq_urings(struct io_ring_ctx *ctx)
{
	if (ctx->sq_ring.ring) {
		page_frag_free(ctx->sq_ring.ring);
		ctx->sq_ring.ring = NULL;
	}
	if (ctx->sq_ring.iocbs) {
		page_frag_free(ctx->sq_ring.iocbs);
		ctx->sq_ring.iocbs = NULL;
	}
	if (ctx->cq_ring.ring) {
		page_frag_free(ctx->cq_ring.ring);
		ctx->cq_ring.ring = NULL;
	}
}

static void io_ring_ctx_free(struct work_struct *work)
{
	struct io_ring_ctx *ctx = container_of(work, struct io_ring_ctx, work);

	io_sq_thread_stop(ctx);
	io_iopoll_reap_events(ctx);
	io_free_scq_urings(ctx);
	io_iocb_buffer_unmap(ctx);
	percpu_ref_exit(&ctx->refs);
	kmem_cache_free(ioctx_cachep, ctx);
}

static void io_ring_ctx_ref_free(struct percpu_ref *ref)
{
	struct io_ring_ctx *ctx = container_of(ref, struct io_ring_ctx, refs);

	schedule_work(&ctx->work);
}

static int io_scqring_release(struct inode *inode, struct file *file)
{
	struct io_ring_ctx *ctx = file->private_data;

	file->private_data = NULL;
	percpu_ref_kill(&ctx->refs);
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
		ptr = ctx->sq_ring.ring;
		break;
	case IORING_OFF_IOCB:
		ptr = ctx->sq_ring.iocbs;
		break;
	case IORING_OFF_CQ_RING:
		ptr = ctx->cq_ring.ring;
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
	long ret = -EBADF;
	struct fd f;

	f = fdget(fd);
	if (f.file) {
		struct io_ring_ctx *ctx;

		ret = -EOPNOTSUPP;
		if (f.file->f_op != &io_scqring_fops)
			goto err;

		ctx = f.file->private_data;
		ret = -EBUSY;
		if (!mutex_trylock(&ctx->uring_lock))
			goto err;

		ret = __io_uring_enter(ctx, to_submit, min_complete, flags);
		mutex_unlock(&ctx->uring_lock);
err:
		fdput(f);
	}

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

	sq_ring = io_mem_alloc(struct_size(sq_ring, array, p->sq_entries));
	if (!sq_ring)
		return -ENOMEM;

	ctx->sq_ring.ring = sq_ring;
	sq_ring->ring_mask = p->sq_entries - 1;
	sq_ring->ring_entries = p->sq_entries;
	ctx->sq_ring.ring_mask = sq_ring->ring_mask;
	ctx->sq_ring.entries = sq_ring->ring_entries;

	ctx->sq_ring.iocbs = io_mem_alloc(sizeof(struct io_uring_iocb) *
						p->sq_entries);
	if (!ctx->sq_ring.iocbs)
		goto err;

	cq_ring = io_mem_alloc(struct_size(cq_ring, events, p->cq_entries));
	if (!cq_ring)
		goto err;

	ctx->cq_ring.ring = cq_ring;
	cq_ring->ring_mask = p->cq_entries - 1;
	cq_ring->ring_entries = p->cq_entries;
	ctx->cq_ring.ring_mask = cq_ring->ring_mask;
	ctx->cq_ring.entries = cq_ring->ring_entries;
	return 0;
err:
	io_free_scq_urings(ctx);
	return -ENOMEM;
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
	p->cq_off.events = offsetof(struct io_cq_ring, events);
}

static int io_uring_create(unsigned entries, struct io_uring_params *p,
			   struct iovec __user *iovecs)
{
	struct io_ring_ctx *ctx;
	int ret;

	/*
	 * We don't use the iovecs without fixed buffers being asked for.
	 * Error out if they don't match.
	 */
	if (!(p->flags & IORING_SETUP_FIXEDBUFS) && iovecs)
		return -EINVAL;

	/*
	 * Use twice as many entries for the CQ ring. It's possible for the
	 * application to drive a higher depth than the size of the SQ ring,
	 * since the iocbs are only used at submission time. This allows for
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

	if (p->flags & IORING_SETUP_FIXEDBUFS) {
		ret = io_iocb_buffer_map(ctx, iovecs);
		if (ret)
			goto err;
	}
	if (p->flags & (IORING_SETUP_SQTHREAD | IORING_SETUP_SQWQ)) {
		ctx->sq_ring.sq_thread_cpu = p->sq_thread_cpu;

		ret = io_sq_thread_start(ctx);
		if (ret)
			goto err;
	}

	ret = anon_inode_getfd("[io_uring]", &io_scqring_fops, ctx,
				O_RDWR | O_CLOEXEC);
	if (ret < 0)
		goto err;

	io_fill_offsets(p);
	return ret;
err:
	percpu_ref_kill(&ctx->refs);
	return ret;
}

/*
 * sys_io_uring_setup:
 *	Sets up an aio uring context, and returns the fd. Applications asks
 *	for a ring size, we return the actual sq/cq ring sizes (among other
 *	things) in the params structure passed in.
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

	if (p.flags & ~(IORING_SETUP_IOPOLL | IORING_SETUP_FIXEDBUFS |
			IORING_SETUP_SQTHREAD | IORING_SETUP_SQWQ |
			IORING_SETUP_SQPOLL))
		return -EINVAL;

	ret = io_uring_create(entries, &p, iovecs);
	if (ret < 0)
		return ret;

	if (copy_to_user(params, &p, sizeof(p)))
		return -EFAULT;

	return ret;
}

static int __init io_uring_setup(void)
{
	kiocb_cachep = KMEM_CACHE(io_kiocb, SLAB_HWCACHE_ALIGN | SLAB_PANIC);
	ioctx_cachep = KMEM_CACHE(io_ring_ctx, SLAB_HWCACHE_ALIGN | SLAB_PANIC);
	return 0;
};
__initcall(io_uring_setup);
