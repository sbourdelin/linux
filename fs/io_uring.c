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
#include <linux/compat.h>
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
#include <linux/kthread.h>
#include <linux/blkdev.h>
#include <linux/bvec.h>
#include <linux/anon_inodes.h>
#include <linux/sched/mm.h>
#include <linux/sizes.h>
#include <linux/nospec.h>

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

struct list_multi {
	struct list_head list;
	unsigned multi;
};

struct io_mapped_ubuf {
	u64		ubuf;
	size_t		len;
	struct		bio_vec *bvec;
	unsigned int	nr_bvecs;
};

struct io_ring_ctx {
	struct percpu_ref	refs;

	unsigned int		flags;
	bool			compat;

	/* SQ ring */
	struct io_sq_ring	*sq_ring;
	unsigned		sq_entries;
	unsigned		sq_mask;
	unsigned		sq_thread_cpu;
	struct io_uring_sqe	*sq_sqes;

	/* CQ ring */
	struct io_cq_ring	*cq_ring;
	unsigned		cq_entries;
	unsigned		cq_mask;

	/* IO offload */
	struct workqueue_struct	*sqo_wq;
	struct task_struct	*sqo_thread;	/* if using sq thread polling */
	struct mm_struct	*sqo_mm;
	struct files_struct	*sqo_files;
	wait_queue_head_t	sqo_wait;

	/* if used, fixed mapped user buffers */
	unsigned		nr_user_bufs;
	struct io_mapped_ubuf	*user_bufs;
	struct user_struct	*user;

	struct completion	ctx_done;

	struct {
		struct mutex		uring_lock;
		wait_queue_head_t	wait;
	} ____cacheline_aligned_in_smp;

	struct {
		spinlock_t		completion_lock;
		struct list_multi	poll_list;
	} ____cacheline_aligned_in_smp;
};

struct sqe_submit {
	const struct io_uring_sqe *sqe;
	unsigned index;
};

struct io_work {
	struct work_struct work;
	struct sqe_submit submit;
};

struct io_kiocb {
	union {
		struct kiocb		rw;
		struct io_work		work;
	};

	struct io_ring_ctx	*ctx;
	struct list_head	list;
	unsigned long		flags;
#define REQ_F_FORCE_NONBLOCK	1	/* inline submission attempt */
#define REQ_F_IOPOLL_COMPLETED	2	/* polled IO has completed */
#define REQ_F_IOPOLL_EAGAIN	4	/* submission got EAGAIN */
	u64			user_data;
	u64			res;
};

#define IO_PLUG_THRESHOLD		2
#define IO_IOPOLL_BATCH			8

struct io_submit_state {
	struct io_ring_ctx *ctx;

	struct blk_plug plug;
#ifdef CONFIG_BLOCK
	struct blk_plug_cb plug_cb;
#endif

	/*
	 * Polled iocbs that have been submitted, but not added to the ctx yet
	 */
	struct list_multi req_list;
	unsigned int req_count;

	/*
	 * io_kiocb alloc cache
	 */
	void *reqs[IO_IOPOLL_BATCH];
	unsigned int free_reqs;
	unsigned int cur_req;

	/*
	 * File reference cache
	 */
	struct file *file;
	unsigned int fd;
	unsigned int has_refs;
	unsigned int used_refs;
	unsigned int ios_left;
};

static struct kmem_cache *req_cachep;

static const struct file_operations io_uring_fops;

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
	INIT_LIST_HEAD(&ctx->poll_list.list);
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

static void __io_cqring_fill_event(struct io_ring_ctx *ctx, u64 ki_user_data,
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
		cqe->user_data = ki_user_data;
		cqe->res = res;
		cqe->flags = ev_flags;
		smp_wmb();
		io_inc_cqring(ctx);
	} else
		ctx->cq_ring->overflow++;
}

static void io_cqring_fill_event(struct io_ring_ctx *ctx, u64 ki_user_data,
				 long res, unsigned ev_flags)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->completion_lock, flags);
	__io_cqring_fill_event(ctx, ki_user_data, res, ev_flags);
	spin_unlock_irqrestore(&ctx->completion_lock, flags);
}

static void io_fill_cq_error(struct io_ring_ctx *ctx, struct sqe_submit *s,
			     long error)
{
	io_cqring_fill_event(ctx, s->index, error, 0);

	if (waitqueue_active(&ctx->wait))
		wake_up(&ctx->wait);
}

static void io_ring_drop_ctx_refs(struct io_ring_ctx *ctx, unsigned refs)
{
	percpu_ref_put_many(&ctx->refs, refs);

	if (waitqueue_active(&ctx->wait))
		wake_up(&ctx->wait);
}

static struct io_kiocb *io_get_req(struct io_ring_ctx *ctx,
				   struct io_submit_state *state)
{
	gfp_t gfp = GFP_ATOMIC | __GFP_NOWARN;
	struct io_kiocb *req;

	if (!percpu_ref_tryget(&ctx->refs))
		return NULL;

	if (!state)
		req = kmem_cache_alloc(req_cachep, gfp);
	else if (!state->free_reqs) {
		size_t sz;
		int ret;

		sz = min_t(size_t, state->ios_left, ARRAY_SIZE(state->reqs));
		ret = kmem_cache_alloc_bulk(req_cachep, gfp, sz,
						state->reqs);
		if (ret <= 0)
			goto out;
		state->free_reqs = ret - 1;
		state->cur_req = 1;
		req = state->reqs[0];
	} else {
		req = state->reqs[state->cur_req];
		state->free_reqs--;
		state->cur_req++;
	}

	if (req) {
		req->ctx = ctx;
		req->flags = 0;
		return req;
	}

out:
	io_ring_drop_ctx_refs(ctx, 1);
	return NULL;
}

static void io_free_req_many(struct io_ring_ctx *ctx, void **reqs, int *nr)
{
	if (*nr) {
		kmem_cache_free_bulk(req_cachep, *nr, reqs);
		io_ring_drop_ctx_refs(ctx, *nr);
		*nr = 0;
	}
}

static void io_free_req(struct io_kiocb *req)
{
	kmem_cache_free(req_cachep, req);
	io_ring_drop_ctx_refs(req->ctx, 1);
}

/*
 * Track whether we have multiple files in our lists. This will impact how
 * we do polling eventually, not spinning if we're on potentially on different
 * devices.
 */
static void io_multi_list_add(struct io_kiocb *req, struct list_multi *list)
{
	if (list_empty(&list->list)) {
		list->multi = 0;
	} else if (!list->multi) {
		struct io_kiocb *list_req;

		list_req = list_first_entry(&list->list, struct io_kiocb, list);
		if (list_req->rw.ki_filp != req->rw.ki_filp)
			list->multi = 1;
	}

	/*
	 * For fast devices, IO may have already completed. If it has, add
	 * it to the front so we find it first. We can't add to the poll_done
	 * list as that's unlocked from the completion side.
	 */
	if (req->flags & REQ_F_IOPOLL_COMPLETED)
		list_add(&req->list, &list->list);
	else
		list_add_tail(&req->list, &list->list);
}

static void io_multi_list_splice(struct list_multi *src, struct list_multi *dst)
{
	list_splice_tail_init(&src->list, &dst->list);
	dst->multi |= src->multi;
}

/*
 * Find and free completed poll iocbs
 */
static void io_iopoll_complete(struct io_ring_ctx *ctx, unsigned int *nr_events,
			       struct list_head *done)
{
	void *reqs[IO_IOPOLL_BATCH];
	int file_count, to_free;
	struct file *file = NULL;
	struct io_kiocb *req;

	file_count = to_free = 0;
	while (!list_empty(done)) {
		req = list_first_entry(done, struct io_kiocb, list);
		list_del(&req->list);

		__io_cqring_fill_event(ctx, req->user_data, req->res, 0);

		reqs[to_free++] = req;
		(*nr_events)++;

		/*
		 * Batched puts of the same file, to avoid dirtying the
		 * file usage count multiple times, if avoidable.
		 */
		if (!file) {
			file = req->rw.ki_filp;
			file_count = 1;
		} else if (file == req->rw.ki_filp) {
			file_count++;
		} else {
			fput_many(file, file_count);
			file = req->rw.ki_filp;
			file_count = 1;
		}

		if (to_free == ARRAY_SIZE(reqs))
			io_free_req_many(ctx, reqs, &to_free);
	}

	if (file)
		fput_many(file, file_count);
	if (to_free)
		io_free_req_many(ctx, reqs, &to_free);
}

static int io_do_iopoll(struct io_ring_ctx *ctx, unsigned int *nr_events,
			long min)
{
	struct io_kiocb *req, *tmp;
	int polled, found, ret;
	LIST_HEAD(done);
	bool spin;

	/*
	 * Only spin for completions if we don't have multiple devices hanging
	 * off our complete list, and we're under the requested amount.
	 */
	spin = !ctx->poll_list.multi && (*nr_events < min);

	ret = polled = found = 0;
	list_for_each_entry_safe(req, tmp, &ctx->poll_list.list, list) {
		struct kiocb *kiocb = &req->rw;

		if (req->flags & REQ_F_IOPOLL_COMPLETED) {
			list_move_tail(&req->list, &done);
			spin = false;
			continue;
		}

		ret = kiocb->ki_filp->f_op->iopoll(kiocb, spin);
		if (ret < 0)
			break;

		polled += ret;
		if (polled && spin)
			spin = false;
		ret = 0;
	}

	if (!list_empty(&done))
		io_iopoll_complete(ctx, nr_events, &done);

	return ret;
}

/*
 * Poll for a mininum of 'min' events, and a maximum of 'max'. Note that if
 * min == 0 we consider that a non-spinning poll check - we'll still enter
 * the driver poll loop, but only as a non-spinning completion check.
 */
static int io_iopoll_getevents(struct io_ring_ctx *ctx, unsigned int *nr_events,
				long min)
{
	int ret;

	do {
		if (list_empty(&ctx->poll_list.list))
			return 0;

		ret = io_do_iopoll(ctx, nr_events, min);
		if (ret < 0)
			break;
	} while (min && *nr_events < min);

	if (ret < 0)
		return ret;

	return *nr_events < min;
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
	while (!list_empty(&ctx->poll_list.list)) {
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

static void io_complete_rw(struct kiocb *kiocb, long res, long res2)
{
	struct io_kiocb *req = container_of(kiocb, struct io_kiocb, rw);

	kiocb_end_write(kiocb);

	fput(kiocb->ki_filp);
	io_cqring_fill_event(req->ctx, req->user_data, res, 0);
	io_free_req(req);
}

static void io_complete_rw_iopoll(struct kiocb *kiocb, long res, long res2)
{
	struct io_kiocb *req = container_of(kiocb, struct io_kiocb, rw);

	kiocb_end_write(kiocb);

	if (unlikely(res == -EAGAIN)) {
		req->flags |= REQ_F_IOPOLL_EAGAIN;
	} else {
		req->flags |= REQ_F_IOPOLL_COMPLETED;
		req->res = res;
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
	io_multi_list_splice(&state->req_list, &ctx->poll_list);
	state->req_count = 0;
}

static void io_iopoll_req_add_list(struct io_kiocb *req)
{
	struct io_ring_ctx *ctx = req->ctx;

	io_multi_list_add(req, &ctx->poll_list);
}

static void io_iopoll_req_add_state(struct io_submit_state *state,
				     struct io_kiocb *req)
{
	io_multi_list_add(req, &state->req_list);
	if (++state->req_count >= IO_IOPOLL_BATCH)
		io_flush_state_reqs(state->ctx, state);
}

/*
 * After the iocb has been issued, it's safe to be found on the poll list.
 * Adding the kiocb to the list AFTER submission ensures that we don't
 * find it from a io_getevents() thread before the issuer is done accessing
 * the kiocb cookie.
 */
static void io_iopoll_req_issued(struct io_submit_state *state,
				 struct io_kiocb *req)
{
	if (!state || !IS_ENABLED(CONFIG_BLOCK))
		io_iopoll_req_add_list(req);
	else
		io_iopoll_req_add_state(state, req);
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

	if (state->file) {
		if (state->fd == fd) {
			state->used_refs++;
			state->ios_left--;
			return state->file;
		}
		io_file_put(state, NULL);
	}
	state->file = fget_many(fd, state->ios_left);
	if (!state->file)
		return NULL;

	state->fd = fd;
	state->has_refs = state->ios_left;
	state->used_refs = 1;
	state->ios_left--;
	return state->file;
}

static int io_prep_rw(struct io_kiocb *req, const struct io_uring_sqe *sqe,
		      bool force_nonblock, struct io_submit_state *state)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct kiocb *kiocb = &req->rw;
	int ret;

	kiocb->ki_filp = io_file_get(state, sqe->fd);
	if (unlikely(!kiocb->ki_filp))
		return -EBADF;
	kiocb->ki_pos = sqe->off;
	kiocb->ki_flags = iocb_flags(kiocb->ki_filp);
	kiocb->ki_hint = ki_hint_validate(file_write_hint(kiocb->ki_filp));
	if (sqe->ioprio) {
		ret = ioprio_check_cap(sqe->ioprio);
		if (ret)
			goto out_fput;

		kiocb->ki_ioprio = sqe->ioprio;
	} else
		kiocb->ki_ioprio = get_current_ioprio();

	ret = kiocb_set_rw_flags(kiocb, sqe->rw_flags);
	if (unlikely(ret))
		goto out_fput;
	if (force_nonblock) {
		kiocb->ki_flags |= IOCB_NOWAIT;
		req->flags |= REQ_F_FORCE_NONBLOCK;
	}
	if (ctx->flags & IORING_SETUP_IOPOLL) {
		ret = -EOPNOTSUPP;
		if (!(kiocb->ki_flags & IOCB_DIRECT) ||
		    !kiocb->ki_filp->f_op->iopoll)
			goto out_fput;

		kiocb->ki_flags |= IOCB_HIPRI;
		kiocb->ki_complete = io_complete_rw_iopoll;
	} else {
		if (kiocb->ki_flags & IOCB_HIPRI) {
			ret = -EINVAL;
			goto out_fput;
		}
		kiocb->ki_complete = io_complete_rw;
	}
	return 0;
out_fput:
	io_file_put(state, kiocb->ki_filp);
	return ret;
}

static inline void io_rw_done(struct kiocb *kiocb, ssize_t ret)
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
		kiocb->ki_complete(kiocb, ret, 0);
	}
}

static int io_import_fixed(struct io_ring_ctx *ctx, int rw,
			   const struct io_uring_sqe *sqe,
			   struct iov_iter *iter)
{
	struct io_mapped_ubuf *imu;
	size_t len = sqe->len;
	size_t offset;
	int index;

	/* attempt to use fixed buffers without having provided iovecs */
	if (unlikely(!ctx->user_bufs))
		return -EFAULT;
	if (unlikely(sqe->buf_index >= ctx->nr_user_bufs))
		return -EFAULT;

	index = array_index_nospec(sqe->buf_index, ctx->sq_entries);
	imu = &ctx->user_bufs[index];
	if ((unsigned long) sqe->addr < imu->ubuf ||
	    (unsigned long) sqe->addr + len > imu->ubuf + imu->len)
		return -EFAULT;

	/*
	 * May not be a start of buffer, set size appropriately
	 * and advance us to the beginning.
	 */
	offset = (unsigned long) sqe->addr - imu->ubuf;
	iov_iter_bvec(iter, rw, imu->bvec, imu->nr_bvecs, offset + len);
	if (offset)
		iov_iter_advance(iter, offset);
	return 0;
}

static int io_import_iovec(struct io_ring_ctx *ctx, int rw,
			   const struct io_uring_sqe *sqe,
			   struct iovec **iovec, struct iov_iter *iter)
{
	void __user *buf = (void __user *) (uintptr_t) sqe->addr;

	if (sqe->opcode == IORING_OP_READ_FIXED ||
	    sqe->opcode == IORING_OP_WRITE_FIXED) {
		ssize_t ret = io_import_fixed(ctx, rw, sqe, iter);
		*iovec = NULL;
		return ret;
	}

#ifdef CONFIG_COMPAT
	if (ctx->compat)
		return compat_import_iovec(rw, buf, sqe->len, UIO_FASTIOV,
						iovec, iter);
#endif
	return import_iovec(rw, buf, sqe->len, UIO_FASTIOV, iovec, iter);
}

static ssize_t io_read(struct io_kiocb *req, const struct io_uring_sqe *sqe,
		       bool force_nonblock, struct io_submit_state *state)
{
	struct iovec inline_vecs[UIO_FASTIOV], *iovec = inline_vecs;
	struct kiocb *kiocb = &req->rw;
	struct iov_iter iter;
	struct file *file;
	ssize_t ret;

	ret = io_prep_rw(req, sqe, force_nonblock, state);
	if (ret)
		return ret;
	file = kiocb->ki_filp;

	ret = -EBADF;
	if (unlikely(!(file->f_mode & FMODE_READ)))
		goto out_fput;
	ret = -EINVAL;
	if (unlikely(!file->f_op->read_iter))
		goto out_fput;

	ret = io_import_iovec(req->ctx, READ, sqe, &iovec, &iter);
	if (ret)
		goto out_fput;

	ret = rw_verify_area(READ, file, &kiocb->ki_pos, iov_iter_count(&iter));
	if (!ret) {
		ssize_t ret2;

		/* Catch -EAGAIN return for forced non-blocking submission */
		ret2 = call_read_iter(file, kiocb, &iter);
		if (!force_nonblock || ret2 != -EAGAIN)
			io_rw_done(kiocb, ret2);
		else
			ret = -EAGAIN;
	}
	kfree(iovec);
out_fput:
	if (unlikely(ret))
		fput(file);
	return ret;
}

static ssize_t io_write(struct io_kiocb *req, const struct io_uring_sqe *sqe,
			bool force_nonblock, struct io_submit_state *state)
{
	struct iovec inline_vecs[UIO_FASTIOV], *iovec = inline_vecs;
	struct kiocb *kiocb = &req->rw;
	struct iov_iter iter;
	struct file *file;
	ssize_t ret;

	ret = io_prep_rw(req, sqe, force_nonblock, state);
	if (ret)
		return ret;
	file = kiocb->ki_filp;

	ret = -EAGAIN;
	if (force_nonblock && !(kiocb->ki_flags & IOCB_DIRECT))
		goto out_fput;

	ret = -EBADF;
	if (unlikely(!(file->f_mode & FMODE_WRITE)))
		goto out_fput;
	ret = -EINVAL;
	if (unlikely(!file->f_op->write_iter))
		goto out_fput;

	ret = io_import_iovec(req->ctx, WRITE, sqe, &iovec, &iter);
	if (ret)
		goto out_fput;

	ret = rw_verify_area(WRITE, file, &kiocb->ki_pos,
				iov_iter_count(&iter));
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
		kiocb->ki_flags |= IOCB_WRITE;
		io_rw_done(kiocb, call_write_iter(file, kiocb, &iter));
	}
out_fput:
	if (unlikely(ret))
		fput(file);
	return ret;
}

/*
 * IORING_OP_NOP just posts a completion event, nothing else.
 */
static int io_nop(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_ring_ctx *ctx = req->ctx;

	if (unlikely(ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;

	__io_cqring_fill_event(ctx, sqe->user_data, 0, 0);
	io_free_req(req);
	return 0;
}

static int io_fsync(struct io_kiocb *req, const struct io_uring_sqe *sqe,
		    bool force_nonblock)
{
	struct io_ring_ctx *ctx = req->ctx;
	loff_t end = sqe->off + sqe->len;
	struct file *file;
	int ret;

	/* fsync always requires a blocking context */
	if (force_nonblock)
		return -EAGAIN;

	if (unlikely(req->ctx->flags & IORING_SETUP_IOPOLL))
		return -EINVAL;
	if (unlikely(sqe->addr))
		return -EINVAL;
	if (unlikely(sqe->fsync_flags & ~IORING_FSYNC_DATASYNC))
		return -EINVAL;

	file = fget(sqe->fd);
	if (unlikely(!file))
		return -EBADF;

	ret = vfs_fsync_range(file, sqe->off, end > 0 ? end : LLONG_MAX,
			sqe->fsync_flags & IORING_FSYNC_DATASYNC);

	fput(file);
	io_cqring_fill_event(ctx, sqe->user_data, ret, 0);
	io_free_req(req);
	return 0;
}

static int __io_submit_sqe(struct io_ring_ctx *ctx, struct io_kiocb *req,
			   struct sqe_submit *s, bool force_nonblock,
			   struct io_submit_state *state)
{
	const struct io_uring_sqe *sqe = s->sqe;
	ssize_t ret;

	/* enforce forwards compatibility on users */
	if (unlikely(sqe->flags))
		return -EINVAL;

	if (unlikely(s->index >= ctx->sq_entries))
		return -EINVAL;
	req->user_data = sqe->user_data;

	ret = -EINVAL;
	switch (sqe->opcode) {
	case IORING_OP_NOP:
		ret = io_nop(req, sqe);
		break;
	case IORING_OP_READV:
		if (unlikely(sqe->buf_index))
			return -EINVAL;
		ret = io_read(req, sqe, force_nonblock, state);
		break;
	case IORING_OP_WRITEV:
		if (unlikely(sqe->buf_index))
			return -EINVAL;
		ret = io_write(req, sqe, force_nonblock, state);
		break;
	case IORING_OP_READ_FIXED:
		ret = io_read(req, sqe, force_nonblock, state);
		break;
	case IORING_OP_WRITE_FIXED:
		ret = io_write(req, sqe, force_nonblock, state);
		break;
	case IORING_OP_FSYNC:
		ret = io_fsync(req, sqe, force_nonblock);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		return ret;

	if (ctx->flags & IORING_SETUP_IOPOLL) {
		if (req->flags & REQ_F_IOPOLL_EAGAIN)
			return -EAGAIN;
		io_iopoll_req_issued(state, req);
	}

	return 0;
}

static void io_sq_wq_submit_work(struct work_struct *work)
{
	struct io_kiocb *req = container_of(work, struct io_kiocb, work.work);
	struct sqe_submit *s = &req->work.submit;
	struct io_ring_ctx *ctx = req->ctx;
	struct files_struct *old_files;
	mm_segment_t old_fs;
	bool needs_user;
	int ret;

	/*
	 * Ensure we clear previously set flags. even it NOWAIT was originally
	 * set, it's pointless now that we're in an async context.
	 */
	req->rw.ki_flags &= ~IOCB_NOWAIT;
	req->flags &= ~REQ_F_FORCE_NONBLOCK;

	old_files = current->files;
	current->files = ctx->sqo_files;

	/*
	 * If we're doing IO to fixed buffers, we don't need to get/set
	 * user context
	 */
	needs_user = true;
	if (s->sqe->opcode == IORING_OP_READ_FIXED ||
	    s->sqe->opcode == IORING_OP_WRITE_FIXED)
		needs_user = false;

	if (needs_user) {
		if (!mmget_not_zero(ctx->sqo_mm)) {
			ret = -EFAULT;
			goto err;
		}
		use_mm(ctx->sqo_mm);
		old_fs = get_fs();
		set_fs(USER_DS);
	}

	ret = __io_submit_sqe(ctx, req, &req->work.submit, false, NULL);

	if (needs_user) {
		set_fs(old_fs);
		unuse_mm(ctx->sqo_mm);
		mmput(ctx->sqo_mm);
	}
err:
	if (ret) {
		io_fill_cq_error(ctx, &req->work.submit, ret);
		io_free_req(req);
	}
	current->files = old_files;
}

static int io_submit_sqe(struct io_ring_ctx *ctx, struct sqe_submit *s,
			 struct io_submit_state *state)
{
	struct io_kiocb *req;
	ssize_t ret;

	req = io_get_req(ctx, state);
	if (unlikely(!req))
		return -EAGAIN;

	ret = __io_submit_sqe(ctx, req, s, true, state);
	if (ret == -EAGAIN) {
		memcpy(&req->work.submit, s, sizeof(*s));
		INIT_WORK(&req->work.work, io_sq_wq_submit_work);
		queue_work(ctx->sqo_wq, &req->work.work);
		ret = 0;
	}
	if (ret)
		io_free_req(req);

	return ret;
}

#ifdef CONFIG_BLOCK
static void io_state_unplug(struct blk_plug_cb *cb, bool from_schedule)
{
	struct io_submit_state *state;

	state = container_of(cb, struct io_submit_state, plug_cb);
	if (!list_empty(&state->req_list.list))
		io_flush_state_reqs(state->ctx, state);
}
#endif

/*
 * Batched submission is done, ensure local IO is flushed out.
 */
static void io_submit_state_end(struct io_submit_state *state)
{
	blk_finish_plug(&state->plug);
	if (!list_empty(&state->req_list.list))
		io_flush_state_reqs(state->ctx, state);
	io_file_put(state, NULL);
	if (state->free_reqs)
		kmem_cache_free_bulk(req_cachep, state->free_reqs,
					&state->reqs[state->cur_req]);
}

/*
 * Start submission side cache.
 */
static void io_submit_state_start(struct io_submit_state *state,
				  struct io_ring_ctx *ctx, unsigned max_ios)
{
	state->ctx = ctx;
	INIT_LIST_HEAD(&state->req_list.list);
	state->req_count = 0;
	state->free_reqs = 0;
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

static int io_submit_sqes(struct io_ring_ctx *ctx, struct sqe_submit *sqes,
			  unsigned int nr, bool mm_fault)
{
	struct io_submit_state state, *statep = NULL;
	int ret, i, submitted = 0;

	if (nr > IO_PLUG_THRESHOLD) {
		io_submit_state_start(&state, ctx, nr);
		statep = &state;
	}

	for (i = 0; i < nr; i++) {
		if (unlikely(mm_fault))
			ret = -EFAULT;
		else
			ret = io_submit_sqe(ctx, &sqes[i], statep);
		if (!ret) {
			submitted++;
			continue;
		}

		io_fill_cq_error(ctx, &sqes[i], ret);
	}

	if (statep)
		io_submit_state_end(&state);

	return submitted;
}

static int io_sq_thread(void *data)
{
	struct sqe_submit sqes[IO_IOPOLL_BATCH];
	struct io_ring_ctx *ctx = data;
	struct mm_struct *cur_mm = NULL;
	struct files_struct *old_files;
	mm_segment_t old_fs;
	DEFINE_WAIT(wait);
	unsigned inflight;
	unsigned long timeout;

	old_files = current->files;
	current->files = ctx->sqo_files;

	old_fs = get_fs();
	set_fs(USER_DS);

	timeout = inflight = 0;
	while (!kthread_should_stop()) {
		bool all_fixed, mm_fault = false;
		int i;

		if (inflight) {
			unsigned int nr_events = 0;

			/*
			 * Normal IO, just pretend everything completed.
			 * We don't have to poll completions for that.
			 */
			if (ctx->flags & IORING_SETUP_IOPOLL) {
				/*
				 * App should not use IORING_ENTER_GETEVENTS
				 * with thread polling, but if it does, then
				 * ensure we are mutually exclusive.
				 */
				if (mutex_trylock(&ctx->uring_lock)) {
					io_iopoll_check(ctx, &nr_events, 0);
					mutex_unlock(&ctx->uring_lock);
				}
			} else {
				nr_events = inflight;
			}

			inflight -= nr_events;
			if (!inflight)
				timeout = jiffies + HZ;
		}

		if (!io_peek_sqring(ctx, &sqes[0])) {
			/*
			 * We're polling, let us spin for a second without
			 * work before going to sleep.
			 */
			if (inflight || !time_after(jiffies, timeout)) {
				cpu_relax();
				continue;
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

			prepare_to_wait(&ctx->sqo_wait, &wait,
						TASK_INTERRUPTIBLE);

			/* Tell userspace we may need a wakeup call */
			ctx->sq_ring->flags |= IORING_SQ_NEED_WAKEUP;
			smp_wmb();

			if (!io_peek_sqring(ctx, &sqes[0])) {
				if (kthread_should_park())
					kthread_parkme();
				if (kthread_should_stop()) {
					finish_wait(&ctx->sqo_wait, &wait);
					break;
				}
				if (signal_pending(current))
					flush_signals(current);
				schedule();
				finish_wait(&ctx->sqo_wait, &wait);

				ctx->sq_ring->flags &= ~IORING_SQ_NEED_WAKEUP;
				smp_wmb();
				continue;
			}
			finish_wait(&ctx->sqo_wait, &wait);

			ctx->sq_ring->flags &= ~IORING_SQ_NEED_WAKEUP;
			smp_wmb();
		}

		i = 0;
		all_fixed = true;
		do {
			if (sqes[i].sqe->opcode != IORING_OP_READ_FIXED &&
			    sqes[i].sqe->opcode != IORING_OP_WRITE_FIXED)
				all_fixed = false;
			if (i + 1 == ARRAY_SIZE(sqes))
				break;
			i++;
			io_inc_sqring(ctx);
		} while (io_peek_sqring(ctx, &sqes[i]));

		/* Unless all new commands are FIXED regions, grab mm */
		if (!all_fixed && !cur_mm) {
			mm_fault = !mmget_not_zero(ctx->sqo_mm);
			if (!mm_fault) {
				use_mm(ctx->sqo_mm);
				cur_mm = ctx->sqo_mm;
			}
		}

		inflight += io_submit_sqes(ctx, sqes, i, mm_fault);
	}
	current->files = old_files;
	set_fs(old_fs);
	if (cur_mm) {
		unuse_mm(cur_mm);
		mmput(cur_mm);
	}
	return 0;
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
		if (ctx->flags & IORING_SETUP_SQPOLL) {
			wake_up(&ctx->sqo_wait);
			ret = to_submit;
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

static int io_sq_offload_start(struct io_ring_ctx *ctx,
			       struct io_uring_params *p)
{
	int ret;

	init_waitqueue_head(&ctx->sqo_wait);
	ctx->sqo_mm = current->mm;

	/*
	 * This is safe since 'current' has the fd installed, and if that
	 * gets closed on exit, then fops->release() is invoked which
	 * waits for the sq thread and sq workqueue to flush and exit
	 * before exiting.
	 */
	ret = -EBADF;
	ctx->sqo_files = current->files;
	if (!ctx->sqo_files)
		goto err;

	if (ctx->flags & IORING_SETUP_SQPOLL) {
		if (p->flags & IORING_SETUP_SQ_AFF) {
			ctx->sqo_thread = kthread_create_on_cpu(io_sq_thread,
							ctx, p->sq_thread_cpu,
							"io_uring-sq");
		} else {
			ctx->sqo_thread = kthread_create(io_sq_thread, ctx,
							"io_uring-sq");
		}
		if (IS_ERR(ctx->sqo_thread)) {
			ret = PTR_ERR(ctx->sqo_thread);
			ctx->sqo_thread = NULL;
			goto err;
		}
		wake_up_process(ctx->sqo_thread);
	} else if (p->flags & IORING_SETUP_SQ_AFF) {
		/* Can't have SQ_AFF without SQPOLL */
		ret = -EINVAL;
		goto err;
	}

	/* Do QD, or 2 * CPUS, whatever is smallest */
	ctx->sqo_wq = alloc_workqueue("io_ring-wq", WQ_UNBOUND | WQ_FREEZABLE,
			min(ctx->sq_entries - 1, 2 * num_online_cpus()));
	if (!ctx->sqo_wq) {
		ret = -ENOMEM;
		goto err;
	}

	return 0;
err:
	if (ctx->sqo_thread) {
		kthread_park(ctx->sqo_thread);
		kthread_stop(ctx->sqo_thread);
		ctx->sqo_thread = NULL;
	}
	if (ctx->sqo_files)
		ctx->sqo_files = NULL;
	ctx->sqo_mm = NULL;
	return ret;
}

static void io_sq_offload_stop(struct io_ring_ctx *ctx)
{
	if (ctx->sqo_thread) {
		kthread_park(ctx->sqo_thread);
		kthread_stop(ctx->sqo_thread);
		ctx->sqo_thread = NULL;
	}
	if (ctx->sqo_wq) {
		destroy_workqueue(ctx->sqo_wq);
		ctx->sqo_wq = NULL;
	}
}

static int io_sqe_user_account_mem(struct io_ring_ctx *ctx,
				   unsigned long nr_pages)
{
	unsigned long page_limit, cur_pages, new_pages;

	if (!ctx->user)
		return 0;

	/* Don't allow more pages than we can safely lock */
	page_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	do {
		cur_pages = atomic_long_read(&ctx->user->locked_vm);
		new_pages = cur_pages + nr_pages;
		if (new_pages > page_limit)
			return -ENOMEM;
	} while (atomic_long_cmpxchg(&ctx->user->locked_vm, cur_pages,
					new_pages) != cur_pages);

	return 0;
}

static int io_sqe_buffer_unregister(struct io_ring_ctx *ctx)
{
	int i, j;

	if (!ctx->user_bufs)
		return -EINVAL;

	for (i = 0; i < ctx->sq_entries; i++) {
		struct io_mapped_ubuf *imu = &ctx->user_bufs[i];

		for (j = 0; j < imu->nr_bvecs; j++) {
			set_page_dirty_lock(imu->bvec[j].bv_page);
			put_page(imu->bvec[j].bv_page);
		}

		if (ctx->user)
			atomic_long_sub(imu->nr_bvecs, &ctx->user->locked_vm);
		kfree(imu->bvec);
		imu->nr_bvecs = 0;
	}

	kfree(ctx->user_bufs);
	ctx->user_bufs = NULL;
	free_uid(ctx->user);
	ctx->user = NULL;
	return 0;
}

static int io_copy_iov(struct io_ring_ctx *ctx, struct iovec *dst,
		       struct io_uring_register_buffers *reg, unsigned index)
{
	struct iovec __user *src;

#ifdef CONFIG_COMPAT
	if (ctx->compat) {
		struct compat_iovec __user *ciovs;
		struct compat_iovec ciov;

		ciovs = (struct compat_iovec __user *) reg->iovecs;
		if (copy_from_user(&ciov, &ciovs[index], sizeof(ciov)))
			return -EFAULT;

		dst->iov_base = (void __user *) (unsigned long) ciov.iov_base;
		dst->iov_len = ciov.iov_len;
		return 0;
	}
#endif
	src = (struct iovec __user *) &reg->iovecs[index];
	if (copy_from_user(dst, src, sizeof(*dst)))
		return -EFAULT;
	return 0;
}

static int io_sqe_buffer_register(struct io_ring_ctx *ctx,
				  struct io_uring_register_buffers *reg)
{
	struct page **pages = NULL;
	int i, j, got_pages = 0;
	int ret = -EINVAL;

	if (reg->nr_iovecs > USHRT_MAX)
		return -EINVAL;

	ctx->user_bufs = kcalloc(reg->nr_iovecs, sizeof(struct io_mapped_ubuf),
					GFP_KERNEL);
	if (!ctx->user_bufs)
		return -ENOMEM;

	if (!capable(CAP_IPC_LOCK))
		ctx->user = get_uid(current_user());

	for (i = 0; i < reg->nr_iovecs; i++) {
		struct io_mapped_ubuf *imu = &ctx->user_bufs[i];
		unsigned long off, start, end, ubuf;
		int pret, nr_pages;
		struct iovec iov;
		size_t size;

		ret = io_copy_iov(ctx, &iov, reg, i);
		if (ret)
			break;

		/*
		 * Don't impose further limits on the size and buffer
		 * constraints here, we'll -EINVAL later when IO is
		 * submitted if they are wrong.
		 */
		ret = -EFAULT;
		if (!iov.iov_base)
			goto err;

		/* arbitrary limit, but we need something */
		if (iov.iov_len > SZ_1G)
			goto err;

		ubuf = (unsigned long) iov.iov_base;
		end = (ubuf + iov.iov_len + PAGE_SIZE - 1) >> PAGE_SHIFT;
		start = ubuf >> PAGE_SHIFT;
		nr_pages = end - start;

		ret = io_sqe_user_account_mem(ctx, nr_pages);
		if (ret)
			goto err;

		if (!pages || nr_pages > got_pages) {
			kfree(pages);
			pages = kmalloc_array(nr_pages, sizeof(struct page *),
						GFP_KERNEL);
			if (!pages)
				goto err;
			got_pages = nr_pages;
		}

		imu->bvec = kmalloc_array(nr_pages, sizeof(struct bio_vec),
						GFP_KERNEL);
		if (!imu->bvec)
			goto err;

		down_write(&current->mm->mmap_sem);
		pret = get_user_pages_longterm(ubuf, nr_pages, FOLL_WRITE,
						pages, NULL);
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
	}
	kfree(pages);
	ctx->nr_user_bufs = reg->nr_iovecs;
	return 0;
err:
	kfree(pages);
	io_sqe_buffer_unregister(ctx);
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
	io_sq_offload_stop(ctx);
	io_iopoll_reap_events(ctx);
	io_free_scq_urings(ctx);
	io_sqe_buffer_unregister(ctx);
	percpu_ref_exit(&ctx->refs);
	kfree(ctx);
}

static void io_ring_ctx_wait_and_kill(struct io_ring_ctx *ctx)
{
	mutex_lock(&ctx->uring_lock);
	percpu_ref_kill(&ctx->refs);
	mutex_unlock(&ctx->uring_lock);

	io_iopoll_reap_events(ctx);
	wait_for_completion(&ctx->ctx_done);
	io_ring_ctx_free(ctx);
}

static int io_uring_release(struct inode *inode, struct file *file)
{
	struct io_ring_ctx *ctx = file->private_data;

	file->private_data = NULL;
	io_ring_ctx_wait_and_kill(ctx);
	return 0;
}

static int io_uring_mmap(struct file *file, struct vm_area_struct *vma)
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
	if (f.file->f_op != &io_uring_fops)
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
	io_ring_drop_ctx_refs(ctx, 1);
out_fput:
	fdput(f);
	return ret;
}

static const struct file_operations io_uring_fops = {
	.release	= io_uring_release,
	.mmap		= io_uring_mmap,
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

static int io_uring_create(unsigned entries, struct io_uring_params *p,
			   bool compat)
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
	ctx->compat = compat;

	ret = io_allocate_scq_urings(ctx, p);
	if (ret)
		goto err;

	ret = io_sq_offload_start(ctx, p);
	if (ret)
		goto err;

	ret = anon_inode_getfd("[io_uring]", &io_uring_fops, ctx,
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
static long io_uring_setup(u32 entries, struct io_uring_params __user *params,
			   bool compat)
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

	if (p.flags & ~(IORING_SETUP_IOPOLL | IORING_SETUP_SQPOLL |
			IORING_SETUP_SQ_AFF))
		return -EINVAL;

	ret = io_uring_create(entries, &p, compat);
	if (ret < 0)
		return ret;

	if (copy_to_user(params, &p, sizeof(p)))
		return -EFAULT;

	return ret;
}

SYSCALL_DEFINE2(io_uring_setup, u32, entries,
		struct io_uring_params __user *, params)
{
	return io_uring_setup(entries, params, false);
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE2(io_uring_setup, u32, entries,
		       struct io_uring_params __user *, params)
{
	return io_uring_setup(entries, params, true);
}
#endif

static int __io_uring_register(struct io_ring_ctx *ctx, unsigned opcode,
			       void __user *arg)
{
	int ret;

	/* Drop our initial ref and wait for the ctx to be fully idle */
	percpu_ref_put(&ctx->refs);
	percpu_ref_kill(&ctx->refs);
	wait_for_completion(&ctx->ctx_done);

	switch (opcode) {
	case IORING_REGISTER_BUFFERS: {
		struct io_uring_register_buffers reg;

		ret = -EFAULT;
		if (copy_from_user(&reg, arg, sizeof(reg)))
			break;
		ret = io_sqe_buffer_register(ctx, &reg);
		break;
		}
	case IORING_UNREGISTER_BUFFERS:
		ret = -EINVAL;
		if (arg)
			break;
		ret = io_sqe_buffer_unregister(ctx);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	/* bring the ctx back to life */
	percpu_ref_resurrect(&ctx->refs);
	percpu_ref_get(&ctx->refs);
	return ret;
}

SYSCALL_DEFINE3(io_uring_register, unsigned int, fd, unsigned int, opcode,
		void __user *, arg)
{
	struct io_ring_ctx *ctx;
	long ret = -EBADF;
	struct fd f;

	f = fdget(fd);
	if (!f.file)
		return -EBADF;

	ret = -EOPNOTSUPP;
	if (f.file->f_op != &io_uring_fops)
		goto out_fput;

	ret = -EINVAL;
	ctx = f.file->private_data;
	if (!percpu_ref_tryget(&ctx->refs))
		goto out_fput;

	ret = -EBUSY;
	if (mutex_trylock(&ctx->uring_lock)) {
		ret = __io_uring_register(ctx, opcode, arg);
		mutex_unlock(&ctx->uring_lock);
	}
	io_ring_drop_ctx_refs(ctx, 1);
out_fput:
	fdput(f);
	return ret;
}

static int __init io_uring_init(void)
{
	req_cachep = KMEM_CACHE(io_kiocb, SLAB_HWCACHE_ALIGN | SLAB_PANIC);
	return 0;
};
__initcall(io_uring_init);
