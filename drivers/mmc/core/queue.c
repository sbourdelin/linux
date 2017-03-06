/*
 *  Copyright (C) 2003 Russell King, All Rights Reserved.
 *  Copyright 2006-2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>

#include "queue.h"
#include "block.h"
#include "core.h"
#include "card.h"

#define MMC_QUEUE_BOUNCESZ	65536

/*
 * Prepare a MMC request. This just filters out odd stuff.
 */
static int mmc_prep_request(struct request_queue *q, struct request *req)
{
	struct mmc_queue *mq = q->queuedata;

	if (mq && (mmc_card_removed(mq->card) || mmc_access_rpmb(mq)))
		return BLKPREP_KILL;

	req->rq_flags |= RQF_DONTPREP;

	return BLKPREP_OK;
}

static void mmc_cqe_request_fn(struct request_queue *q)
{
	struct mmc_queue *mq = q->queuedata;
	struct request *req;

	if (!mq) {
		while ((req = blk_fetch_request(q)) != NULL) {
			req->rq_flags |= RQF_QUIET;
			__blk_end_request_all(req, -EIO);
		}
		return;
	}

	if (mq->asleep && !mq->cqe_busy)
		wake_up_process(mq->thread);
}

static inline bool mmc_cqe_dcmd_busy(struct mmc_queue *mq)
{
	/* Allow only 1 DCMD at a time */
	return mq->cqe_in_flight[MMC_ISSUE_DCMD];
}

static inline bool mmc_cqe_queue_full(struct mmc_queue *mq)
{
	return mmc_cqe_qcnt(mq) >= mq->qdepth;
}

void mmc_cqe_kick_queue(struct mmc_queue *mq)
{
	if ((mq->cqe_busy & MMC_CQE_DCMD_BUSY) && !mmc_cqe_dcmd_busy(mq))
		mq->cqe_busy &= ~MMC_CQE_DCMD_BUSY;

	if ((mq->cqe_busy & MMC_CQE_QUEUE_FULL) && !mmc_cqe_queue_full(mq))
		mq->cqe_busy &= ~MMC_CQE_QUEUE_FULL;

	if (mq->asleep && !mq->cqe_busy)
		__blk_run_queue(mq->queue);
}

static inline bool mmc_cqe_can_dcmd(struct mmc_host *host)
{
	return host->caps2 & MMC_CAP2_CQE_DCMD;
}

enum mmc_issue_type mmc_cqe_issue_type(struct mmc_host *host,
				       struct request *req)
{
	switch (req_op(req)) {
	case REQ_OP_DISCARD:
	case REQ_OP_SECURE_ERASE:
		return MMC_ISSUE_SYNC;
	case REQ_OP_FLUSH:
		return mmc_cqe_can_dcmd(host) ? MMC_ISSUE_DCMD : MMC_ISSUE_SYNC;
	default:
		return MMC_ISSUE_ASYNC;
	}
}

void mmc_queue_set_special(struct mmc_queue *mq, struct request *req)
{
	struct mmc_queue_req *mqrq = &mq->mqrq[req->tag];

	mqrq->req = req;
	req->special = mqrq;
}

void mmc_queue_clr_special(struct request *req)
{
	struct mmc_queue_req *mqrq = req->special;

	if (!mqrq)
		return;

	mqrq->req = NULL;
	req->special = NULL;
}

static void __mmc_cqe_recovery_notifier(struct mmc_queue *mq)
{
	if (!mq->cqe_recovery_needed) {
		mq->cqe_recovery_needed = true;
		wake_up_process(mq->thread);
	}
}

static void mmc_cqe_recovery_notifier(struct mmc_host *host,
				      struct mmc_request *mrq)
{
	struct mmc_queue_req *mqrq = container_of(mrq, struct mmc_queue_req,
						  brq.mrq);
	struct request *req = mqrq->req;
	struct request_queue *q = req->q;
	struct mmc_queue *mq = q->queuedata;
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	__mmc_cqe_recovery_notifier(mq);
	spin_unlock_irqrestore(q->queue_lock, flags);
}

static int mmc_cqe_thread(void *d)
{
	struct mmc_queue *mq = d;
	struct request_queue *q = mq->queue;
	struct mmc_card *card = mq->card;
	struct mmc_host *host = card->host;
	unsigned long flags;
	int get_put = 0;

	current->flags |= PF_MEMALLOC;

	down(&mq->thread_sem);
	spin_lock_irqsave(q->queue_lock, flags);
	while (1) {
		struct request *req = NULL;
		enum mmc_issue_type issue_type;
		bool retune_ok = false;

		if (mq->cqe_recovery_needed) {
			spin_unlock_irqrestore(q->queue_lock, flags);
			mmc_blk_cqe_recovery(mq);
			spin_lock_irqsave(q->queue_lock, flags);
			mq->cqe_recovery_needed = false;
		}

		set_current_state(TASK_INTERRUPTIBLE);

		if (!kthread_should_stop())
			req = blk_peek_request(q);

		if (req) {
			issue_type = mmc_cqe_issue_type(host, req);
			switch (issue_type) {
			case MMC_ISSUE_DCMD:
				if (mmc_cqe_dcmd_busy(mq)) {
					mq->cqe_busy |= MMC_CQE_DCMD_BUSY;
					req = NULL;
					break;
				}
				/* Fall through */
			case MMC_ISSUE_ASYNC:
				if (blk_queue_start_tag(q, req)) {
					mq->cqe_busy |= MMC_CQE_QUEUE_FULL;
					req = NULL;
				}
				break;
			default:
				/*
				 * Timeouts are handled by mmc core, so set a
				 * large value to avoid races.
				 */
				req->timeout = 600 * HZ;
				req->special = NULL;
				blk_start_request(req);
				break;
			}
			if (req) {
				mq->cqe_in_flight[issue_type] += 1;
				if (mmc_cqe_tot_in_flight(mq) == 1)
					get_put += 1;
				if (mmc_cqe_qcnt(mq) == 1)
					retune_ok = true;
			}
		}

		mq->asleep = !req;

		spin_unlock_irqrestore(q->queue_lock, flags);

		if (req) {
			enum mmc_issued issued;

			set_current_state(TASK_RUNNING);

			if (get_put) {
				get_put = 0;
				mmc_get_card(card);
			}

			if (host->need_retune && retune_ok &&
			    !host->hold_retune)
				host->retune_now = true;
			else
				host->retune_now = false;

			issued = mmc_blk_cqe_issue_rq(mq, req);

			cond_resched();

			spin_lock_irqsave(q->queue_lock, flags);

			switch (issued) {
			case MMC_REQ_STARTED:
				break;
			case MMC_REQ_BUSY:
				blk_requeue_request(q, req);
				goto finished;
			case MMC_REQ_FAILED_TO_START:
				__blk_end_request_all(req, -EIO);
				/* Fall through */
			case MMC_REQ_FINISHED:
finished:
				mq->cqe_in_flight[issue_type] -= 1;
				if (mmc_cqe_tot_in_flight(mq) == 0)
					get_put = -1;
			}
		} else {
			if (get_put < 0) {
				get_put = 0;
				mmc_put_card(card);
			}
			/*
			 * Do not stop with requests in flight in case recovery
			 * is needed.
			 */
			if (kthread_should_stop() &&
			    !mmc_cqe_tot_in_flight(mq)) {
				set_current_state(TASK_RUNNING);
				break;
			}
			up(&mq->thread_sem);
			schedule();
			down(&mq->thread_sem);
			spin_lock_irqsave(q->queue_lock, flags);
		}
	} /* loop */
	up(&mq->thread_sem);

	return 0;
}

static enum blk_eh_timer_return __mmc_cqe_timed_out(struct request *req)
{
	struct mmc_queue_req *mqrq = req->special;
	struct mmc_request *mrq = &mqrq->brq.mrq;
	struct mmc_queue *mq = req->q->queuedata;
	struct mmc_host *host = mq->card->host;
	enum mmc_issue_type issue_type = mmc_cqe_issue_type(host, req);
	bool recovery_needed = false;

	switch (issue_type) {
	case MMC_ISSUE_ASYNC:
	case MMC_ISSUE_DCMD:
		if (host->cqe_ops->cqe_timeout(host, mrq, &recovery_needed)) {
			if (recovery_needed)
				__mmc_cqe_recovery_notifier(mq);
			return BLK_EH_RESET_TIMER;
		}
		/* No timeout */
		return BLK_EH_HANDLED;
	default:
		/* Timeout is handled by mmc core */
		return BLK_EH_RESET_TIMER;
	}
}

static enum blk_eh_timer_return mmc_cqe_timed_out(struct request *req)
{
	struct mmc_queue *mq = req->q->queuedata;

	if (!req->special || mq->cqe_recovery_needed)
		return BLK_EH_RESET_TIMER;

	return __mmc_cqe_timed_out(req);
}

static int mmc_queue_thread(void *d)
{
	struct mmc_queue *mq = d;
	struct request_queue *q = mq->queue;
	struct mmc_context_info *cntx = &mq->card->host->context_info;

	current->flags |= PF_MEMALLOC;

	down(&mq->thread_sem);
	do {
		struct request *req = NULL;

		spin_lock_irq(q->queue_lock);
		set_current_state(TASK_INTERRUPTIBLE);
		req = blk_fetch_request(q);
		mq->asleep = false;
		cntx->is_waiting_last_req = false;
		cntx->is_new_req = false;
		if (!req) {
			/*
			 * Dispatch queue is empty so set flags for
			 * mmc_request_fn() to wake us up.
			 */
			if (mq->mqrq_prev->req)
				cntx->is_waiting_last_req = true;
			else
				mq->asleep = true;
		}
		mq->mqrq_cur->req = req;
		spin_unlock_irq(q->queue_lock);

		if (req || mq->mqrq_prev->req) {
			bool req_is_special = mmc_req_is_special(req);

			set_current_state(TASK_RUNNING);
			mmc_blk_issue_rq(mq, req);
			cond_resched();
			if (mq->new_request) {
				mq->new_request = false;
				continue; /* fetch again */
			}

			/*
			 * Current request becomes previous request
			 * and vice versa.
			 * In case of special requests, current request
			 * has been finished. Do not assign it to previous
			 * request.
			 */
			if (req_is_special)
				mq->mqrq_cur->req = NULL;

			mq->mqrq_prev->brq.mrq.data = NULL;
			mq->mqrq_prev->req = NULL;
			swap(mq->mqrq_prev, mq->mqrq_cur);
		} else {
			if (kthread_should_stop()) {
				set_current_state(TASK_RUNNING);
				break;
			}
			up(&mq->thread_sem);
			schedule();
			down(&mq->thread_sem);
		}
	} while (1);
	up(&mq->thread_sem);

	return 0;
}

/*
 * Generic MMC request handler.  This is called for any queue on a
 * particular host.  When the host is not busy, we look for a request
 * on any queue on this host, and attempt to issue it.  This may
 * not be the queue we were asked to process.
 */
static void mmc_request_fn(struct request_queue *q)
{
	struct mmc_queue *mq = q->queuedata;
	struct request *req;
	struct mmc_context_info *cntx;

	if (!mq) {
		while ((req = blk_fetch_request(q)) != NULL) {
			req->rq_flags |= RQF_QUIET;
			__blk_end_request_all(req, -EIO);
		}
		return;
	}

	cntx = &mq->card->host->context_info;

	if (cntx->is_waiting_last_req) {
		cntx->is_new_req = true;
		wake_up_interruptible(&cntx->wait);
	}

	if (mq->asleep)
		wake_up_process(mq->thread);
}

static struct scatterlist *mmc_alloc_sg(int sg_len)
{
	struct scatterlist *sg;

	sg = kmalloc_array(sg_len, sizeof(*sg), GFP_KERNEL);
	if (sg)
		sg_init_table(sg, sg_len);

	return sg;
}

static void mmc_queue_setup_discard(struct request_queue *q,
				    struct mmc_card *card)
{
	unsigned max_discard;

	max_discard = mmc_calc_max_discard(card);
	if (!max_discard)
		return;

	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, q);
	blk_queue_max_discard_sectors(q, max_discard);
	if (card->erased_byte == 0 && !mmc_can_discard(card))
		q->limits.discard_zeroes_data = 1;
	q->limits.discard_granularity = card->pref_erase << 9;
	/* granularity must not be greater than max. discard */
	if (card->pref_erase > max_discard)
		q->limits.discard_granularity = 0;
	if (mmc_can_secure_erase_trim(card))
		queue_flag_set_unlocked(QUEUE_FLAG_SECERASE, q);
}

static void mmc_queue_req_free_bufs(struct mmc_queue_req *mqrq)
{
	kfree(mqrq->bounce_sg);
	mqrq->bounce_sg = NULL;

	kfree(mqrq->sg);
	mqrq->sg = NULL;

	kfree(mqrq->bounce_buf);
	mqrq->bounce_buf = NULL;
}

static void mmc_queue_reqs_free_bufs(struct mmc_queue_req *mqrq, int qdepth)
{
	int i;

	for (i = 0; i < qdepth; i++)
		mmc_queue_req_free_bufs(&mqrq[i]);
}

static void mmc_queue_free_mqrqs(struct mmc_queue_req *mqrq, int qdepth)
{
	mmc_queue_reqs_free_bufs(mqrq, qdepth);
	kfree(mqrq);
}

#ifdef CONFIG_MMC_BLOCK_BOUNCE
static int mmc_queue_alloc_bounce_bufs(struct mmc_queue_req *mqrq, int qdepth,
				       unsigned int bouncesz)
{
	int i;

	for (i = 0; i < qdepth; i++) {
		mqrq[i].bounce_buf = kmalloc(bouncesz, GFP_KERNEL);
		if (!mqrq[i].bounce_buf)
			return -ENOMEM;

		mqrq[i].sg = mmc_alloc_sg(1);
		if (!mqrq[i].sg)
			return -ENOMEM;

		mqrq[i].bounce_sg = mmc_alloc_sg(bouncesz / 512);
		if (!mqrq[i].bounce_sg)
			return -ENOMEM;
	}

	return 0;
}

static bool mmc_queue_alloc_bounce(struct mmc_queue_req *mqrq, int qdepth,
				   unsigned int bouncesz)
{
	int ret;

	ret = mmc_queue_alloc_bounce_bufs(mqrq, qdepth, bouncesz);
	if (ret)
		mmc_queue_reqs_free_bufs(mqrq, qdepth);

	return !ret;
}

static unsigned int mmc_queue_calc_bouncesz(struct mmc_host *host)
{
	unsigned int bouncesz = MMC_QUEUE_BOUNCESZ;

	if (host->max_segs != 1)
		return 0;

	if (bouncesz > host->max_req_size)
		bouncesz = host->max_req_size;
	if (bouncesz > host->max_seg_size)
		bouncesz = host->max_seg_size;
	if (bouncesz > host->max_blk_count * 512)
		bouncesz = host->max_blk_count * 512;

	if (bouncesz <= 512)
		return 0;

	return bouncesz;
}
#else
static inline bool mmc_queue_alloc_bounce(struct mmc_queue_req *mqrq,
					  int qdepth, unsigned int bouncesz)
{
	return false;
}

static unsigned int mmc_queue_calc_bouncesz(struct mmc_host *host)
{
	return 0;
}
#endif

static int mmc_queue_alloc_sgs(struct mmc_queue_req *mqrq, int qdepth,
			       int max_segs)
{
	int i;

	for (i = 0; i < qdepth; i++) {
		mqrq[i].sg = mmc_alloc_sg(max_segs);
		if (!mqrq[i].sg)
			return -ENOMEM;
	}

	return 0;
}

void mmc_queue_free_shared_queue(struct mmc_card *card)
{
	if (card->mqrq) {
		mmc_queue_free_mqrqs(card->mqrq, card->qdepth);
		card->mqrq = NULL;
	}
}

static int __mmc_queue_alloc_shared_queue(struct mmc_card *card, int qdepth)
{
	struct mmc_host *host = card->host;
	struct mmc_queue_req *mqrq;
	unsigned int bouncesz;
	int ret = 0;

	if (card->mqrq)
		return -EINVAL;

	mqrq = kcalloc(qdepth, sizeof(*mqrq), GFP_KERNEL);
	if (!mqrq)
		return -ENOMEM;

	card->mqrq = mqrq;
	card->qdepth = qdepth;

	bouncesz = mmc_queue_calc_bouncesz(host);

	if (bouncesz && !mmc_queue_alloc_bounce(mqrq, qdepth, bouncesz)) {
		bouncesz = 0;
		pr_warn("%s: unable to allocate bounce buffers\n",
			mmc_card_name(card));
	}

	card->bouncesz = bouncesz;

	if (!bouncesz) {
		ret = mmc_queue_alloc_sgs(mqrq, qdepth, host->max_segs);
		if (ret)
			goto out_err;
	}

	return ret;

out_err:
	mmc_queue_free_shared_queue(card);
	return ret;
}

int mmc_queue_alloc_shared_queue(struct mmc_card *card)
{
	return __mmc_queue_alloc_shared_queue(card, 2);
}

/**
 * mmc_init_queue - initialise a queue structure.
 * @mq: mmc queue
 * @card: mmc card to attach this queue
 * @lock: queue lock
 * @subname: partition subname
 *
 * Initialise a MMC card request queue.
 */
int mmc_init_queue(struct mmc_queue *mq, struct mmc_card *card,
		   spinlock_t *lock, const char *subname, int area_type)
{
	struct mmc_host *host = card->host;
	u64 limit = BLK_BOUNCE_HIGH;
	int ret = -ENOMEM;
	bool use_cqe = host->cqe_enabled && area_type != MMC_BLK_DATA_AREA_RPMB;

	if (mmc_dev(host)->dma_mask && *mmc_dev(host)->dma_mask)
		limit = (u64)dma_max_pfn(mmc_dev(host)) << PAGE_SHIFT;

	mq->card = card;

	mq->queue = blk_init_queue(use_cqe ?
				   mmc_cqe_request_fn : mmc_request_fn, lock);
	if (!mq->queue)
		return -ENOMEM;

	if (use_cqe) {
		int q_depth = card->ext_csd.cmdq_depth;

		if (q_depth > host->cqe_qdepth)
			q_depth = host->cqe_qdepth;
		if (q_depth > card->qdepth)
			q_depth = card->qdepth;

		ret = blk_queue_init_tags(mq->queue, q_depth, NULL,
					  BLK_TAG_ALLOC_FIFO);
		if (ret)
			goto cleanup_queue;

		blk_queue_softirq_done(mq->queue, mmc_blk_cqe_complete_rq);
		blk_queue_rq_timed_out(mq->queue, mmc_cqe_timed_out);
		blk_queue_rq_timeout(mq->queue, 60 * HZ);

		host->cqe_recovery_notifier = mmc_cqe_recovery_notifier;
	}

	mq->mqrq = card->mqrq;
	mq->qdepth = card->qdepth;
	mq->mqrq_cur = &mq->mqrq[0];
	mq->mqrq_prev = &mq->mqrq[1];
	mq->queue->queuedata = mq;

	blk_queue_prep_rq(mq->queue, mmc_prep_request);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, mq->queue);
	queue_flag_clear_unlocked(QUEUE_FLAG_ADD_RANDOM, mq->queue);
	if (mmc_can_erase(card))
		mmc_queue_setup_discard(mq->queue, card);

	if (card->bouncesz) {
		blk_queue_bounce_limit(mq->queue, BLK_BOUNCE_ANY);
		blk_queue_max_hw_sectors(mq->queue, card->bouncesz / 512);
		blk_queue_max_segments(mq->queue, card->bouncesz / 512);
		blk_queue_max_segment_size(mq->queue, card->bouncesz);
	} else {
		blk_queue_bounce_limit(mq->queue, limit);
		blk_queue_max_hw_sectors(mq->queue,
			min(host->max_blk_count, host->max_req_size / 512));
		blk_queue_max_segments(mq->queue, host->max_segs);
		blk_queue_max_segment_size(mq->queue, host->max_seg_size);
	}

	sema_init(&mq->thread_sem, 1);

	mq->thread = kthread_run(use_cqe ? mmc_cqe_thread : mmc_queue_thread,
				 mq, "mmcqd/%d%s", host->index,
				 subname ? subname : "");
	if (IS_ERR(mq->thread)) {
		ret = PTR_ERR(mq->thread);
		goto cleanup_queue;
	}

	return 0;

cleanup_queue:
	mq->mqrq = NULL;
	blk_cleanup_queue(mq->queue);
	return ret;
}

void mmc_cleanup_queue(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;

	/* Make sure the queue isn't suspended, as that will deadlock */
	mmc_queue_resume(mq);

	/* Then terminate our worker thread */
	kthread_stop(mq->thread);

	/* Empty the queue */
	spin_lock_irqsave(q->queue_lock, flags);
	q->queuedata = NULL;
	blk_start_queue(q);
	spin_unlock_irqrestore(q->queue_lock, flags);

	mq->mqrq = NULL;
	mq->card = NULL;
}
EXPORT_SYMBOL(mmc_cleanup_queue);

/**
 * mmc_queue_suspend - suspend a MMC request queue
 * @mq: MMC queue to suspend
 *
 * Stop the block request queue, and wait for our thread to
 * complete any outstanding requests.  This ensures that we
 * won't suspend while a request is being processed.
 */
void mmc_queue_suspend(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;

	if (!mq->suspended) {
		mq->suspended |= true;

		spin_lock_irqsave(q->queue_lock, flags);
		blk_stop_queue(q);
		spin_unlock_irqrestore(q->queue_lock, flags);

		down(&mq->thread_sem);
	}
}

/**
 * mmc_queue_resume - resume a previously suspended MMC request queue
 * @mq: MMC queue to resume
 */
void mmc_queue_resume(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;

	if (mq->suspended) {
		mq->suspended = false;

		up(&mq->thread_sem);

		spin_lock_irqsave(q->queue_lock, flags);
		blk_start_queue(q);
		spin_unlock_irqrestore(q->queue_lock, flags);
	}
}

/*
 * Prepare the sg list(s) to be handed of to the host driver
 */
unsigned int mmc_queue_map_sg(struct mmc_queue *mq, struct mmc_queue_req *mqrq)
{
	unsigned int sg_len;
	size_t buflen;
	struct scatterlist *sg;
	int i;

	if (!mqrq->bounce_buf)
		return blk_rq_map_sg(mq->queue, mqrq->req, mqrq->sg);

	sg_len = blk_rq_map_sg(mq->queue, mqrq->req, mqrq->bounce_sg);

	mqrq->bounce_sg_len = sg_len;

	buflen = 0;
	for_each_sg(mqrq->bounce_sg, sg, sg_len, i)
		buflen += sg->length;

	sg_init_one(mqrq->sg, mqrq->bounce_buf, buflen);

	return 1;
}

/*
 * If writing, bounce the data to the buffer before the request
 * is sent to the host driver
 */
void mmc_queue_bounce_pre(struct mmc_queue_req *mqrq)
{
	if (!mqrq->bounce_buf)
		return;

	if (rq_data_dir(mqrq->req) != WRITE)
		return;

	sg_copy_to_buffer(mqrq->bounce_sg, mqrq->bounce_sg_len,
		mqrq->bounce_buf, mqrq->sg[0].length);
}

/*
 * If reading, bounce the data from the buffer after the request
 * has been handled by the host driver
 */
void mmc_queue_bounce_post(struct mmc_queue_req *mqrq)
{
	if (!mqrq->bounce_buf)
		return;

	if (rq_data_dir(mqrq->req) != READ)
		return;

	sg_copy_from_buffer(mqrq->bounce_sg, mqrq->bounce_sg_len,
		mqrq->bounce_buf, mqrq->sg[0].length);
}
