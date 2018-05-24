// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 */

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <soc/qcom/rpmh.h>

#include "rpmh-internal.h"

#define RPMH_TIMEOUT_MS			msecs_to_jiffies(10000)
#define RPMH_MAX_REQ_IN_BATCH		10
#define RPMH_MAX_BATCH_CACHE		(2 * RPMH_MAX_REQ_IN_BATCH)

#define DEFINE_RPMH_MSG_ONSTACK(dev, s, q, name)	\
	struct rpmh_request name = {			\
		.msg = {				\
			.state = s,			\
			.cmds = name.cmd,		\
			.num_cmds = 0,			\
			.wait_for_compl = true,		\
		},					\
		.cmd = { { 0 } },			\
		.completion = q,			\
		.dev = dev,				\
		.needs_free = false,				\
		.wait_count = NULL,			\
	}

/**
 * struct cache_req: the request object for caching
 *
 * @addr: the address of the resource
 * @sleep_val: the sleep vote
 * @wake_val: the wake vote
 * @list: linked list obj
 */
struct cache_req {
	u32 addr;
	u32 sleep_val;
	u32 wake_val;
	struct list_head list;
};

/**
 * struct rpmh_request: the message to be sent to rpmh-rsc
 *
 * @msg: the request
 * @cmd: the payload that will be part of the @msg
 * @completion: triggered when request is done
 * @dev: the device making the request
 * @err: err return from the controller
 * @needs_free: check to free dynamically allocated request object
 * @wait_count: count of waiters for this completion
 */
struct rpmh_request {
	struct tcs_request msg;
	struct tcs_cmd cmd[MAX_RPMH_PAYLOAD];
	struct completion *completion;
	const struct device *dev;
	int err;
	bool needs_free;
	atomic_t *wait_count;
};

/**
 * struct rpmh_ctrlr: our representation of the controller
 *
 * @drv: the controller instance
 * @cache: the list of cached requests
 * @cache_lock: synchronize access to the cache data
 * @dirty: was the cache updated since flush
 * @batch_cache: Cache sleep and wake requests sent as batch
 */
struct rpmh_ctrlr {
	struct rsc_drv *drv;
	struct list_head cache;
	spinlock_t cache_lock;
	bool dirty;
	const struct rpmh_request *batch_cache[RPMH_MAX_BATCH_CACHE];
};

static struct rpmh_ctrlr rpmh_rsc[RPMH_MAX_CTRLR];
static DEFINE_SPINLOCK(rpmh_rsc_lock);

static struct rpmh_ctrlr *get_rpmh_ctrlr(const struct device *dev)
{
	int i;
	struct rsc_drv *p, *drv = dev_get_drvdata(dev->parent);
	struct rpmh_ctrlr *ctrlr = ERR_PTR(-EINVAL);
	unsigned long flags;

	if (!drv)
		return ctrlr;

	for (i = 0; i < RPMH_MAX_CTRLR; i++) {
		if (rpmh_rsc[i].drv == drv) {
			ctrlr = &rpmh_rsc[i];
			return ctrlr;
		}
	}

	spin_lock_irqsave(&rpmh_rsc_lock, flags);
	list_for_each_entry(p, &rsc_drv_list, list) {
		if (drv == p) {
			for (i = 0; i < RPMH_MAX_CTRLR; i++) {
				if (!rpmh_rsc[i].drv)
					break;
			}
			if (i == RPMH_MAX_CTRLR) {
				ctrlr = ERR_PTR(-ENOMEM);
				break;
			}
			rpmh_rsc[i].drv = drv;
			spin_lock_init(&rpmh_rsc[i].cache_lock);
			INIT_LIST_HEAD(&rpmh_rsc[i].cache);
			ctrlr = &rpmh_rsc[i];
			break;
		}
	}
	spin_unlock_irqrestore(&rpmh_rsc_lock, flags);

	return ctrlr;
}

void rpmh_tx_done(const struct tcs_request *msg, int r)
{
	struct rpmh_request *rpm_msg = container_of(msg, struct rpmh_request,
						    msg);
	struct completion *compl = rpm_msg->completion;
	atomic_t *wc = rpm_msg->wait_count;

	rpm_msg->err = r;

	if (r)
		dev_err(rpm_msg->dev, "RPMH TX fail in msg addr=%#x, err=%d\n",
			rpm_msg->msg.cmds[0].addr, r);

	if (!compl)
		goto exit;

	if (wc && !atomic_dec_and_test(wc))
		goto exit;

	/* Signal the blocking thread we are done */
	complete(compl);

exit:
	if (rpm_msg->needs_free)
		kfree(rpm_msg);
}

static struct cache_req *__find_req(struct rpmh_ctrlr *ctrlr, u32 addr)
{
	struct cache_req *p, *req = NULL;

	list_for_each_entry(p, &ctrlr->cache, list) {
		if (p->addr == addr) {
			req = p;
			break;
		}
	}

	return req;
}

static struct cache_req *cache_rpm_request(struct rpmh_ctrlr *ctrlr,
					   enum rpmh_state state,
					   struct tcs_cmd *cmd)
{
	struct cache_req *req;
	unsigned long flags;

	spin_lock_irqsave(&ctrlr->cache_lock, flags);
	req = __find_req(ctrlr, cmd->addr);
	if (req)
		goto existing;

	req = kzalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		req = ERR_PTR(-ENOMEM);
		goto unlock;
	}

	req->addr = cmd->addr;
	req->sleep_val = req->wake_val = UINT_MAX;
	INIT_LIST_HEAD(&req->list);
	list_add_tail(&req->list, &ctrlr->cache);

existing:
	switch (state) {
	case RPMH_ACTIVE_ONLY_STATE:
		if (req->sleep_val != UINT_MAX)
			req->wake_val = cmd->data;
		break;
	case RPMH_WAKE_ONLY_STATE:
		req->wake_val = cmd->data;
		break;
	case RPMH_SLEEP_STATE:
		req->sleep_val = cmd->data;
		break;
	default:
		break;
	};

	ctrlr->dirty = true;
unlock:
	spin_unlock_irqrestore(&ctrlr->cache_lock, flags);

	return req;
}

/**
 * __rpmh_write: Cache and send the RPMH request
 *
 * @dev: The device making the request
 * @state: Active/Sleep request type
 * @rpm_msg: The data that needs to be sent (cmds).
 *
 * Cache the RPMH request and send if the state is ACTIVE_ONLY.
 * SLEEP/WAKE_ONLY requests are not sent to the controller at
 * this time. Use rpmh_flush() to send them to the controller.
 */
static int __rpmh_write(const struct device *dev, enum rpmh_state state,
			struct rpmh_request *rpm_msg)
{
	struct rpmh_ctrlr *ctrlr = get_rpmh_ctrlr(dev);
	int ret = -EINVAL;
	struct cache_req *req;
	int i;

	if (IS_ERR(ctrlr))
		return PTR_ERR(ctrlr);

	rpm_msg->msg.state = state;

	/* Cache the request in our store and link the payload */
	for (i = 0; i < rpm_msg->msg.num_cmds; i++) {
		req = cache_rpm_request(ctrlr, state, &rpm_msg->msg.cmds[i]);
		if (IS_ERR(req))
			return PTR_ERR(req);
	}

	rpm_msg->msg.state = state;

	if (state == RPMH_ACTIVE_ONLY_STATE) {
		WARN_ON(irqs_disabled());
		ret = rpmh_rsc_send_data(ctrlr->drv, &rpm_msg->msg);
	} else {
		ret = rpmh_rsc_write_ctrl_data(ctrlr->drv, &rpm_msg->msg);
		/* Clean up our call by spoofing tx_done */
		rpmh_tx_done(&rpm_msg->msg, ret);
	}

	return ret;
}

static struct rpmh_request *__get_rpmh_msg_async(enum rpmh_state state,
						 const struct tcs_cmd *cmd,
						 u32 n)
{
	struct rpmh_request *req;

	if (!cmd || !n || n > MAX_RPMH_PAYLOAD)
		return ERR_PTR(-EINVAL);

	req = kzalloc(sizeof(*req), GFP_ATOMIC);
	if (!req)
		return ERR_PTR(-ENOMEM);

	memcpy(req->cmd, cmd, n * sizeof(*cmd));

	req->msg.state = state;
	req->msg.cmds = req->cmd;
	req->msg.num_cmds = n;
	req->needs_free = true;

	return req;
}

/**
 * rpmh_write_async: Write a set of RPMH commands
 *
 * @dev: The device making the request
 * @state: Active/sleep set
 * @cmd: The payload data
 * @n: The number of elements in payload
 *
 * Write a set of RPMH commands, the order of commands is maintained
 * and will be sent as a single shot.
 */
int rpmh_write_async(const struct device *dev, enum rpmh_state state,
		     const struct tcs_cmd *cmd, u32 n)
{
	struct rpmh_request *rpm_msg;

	rpm_msg = __get_rpmh_msg_async(state, cmd, n);
	if (IS_ERR(rpm_msg))
		return PTR_ERR(rpm_msg);

	return __rpmh_write(dev, state, rpm_msg);
}

/**
 * rpmh_write: Write a set of RPMH commands and block until response
 *
 * @rc: The RPMH handle got from rpmh_get_client
 * @state: Active/sleep set
 * @cmd: The payload data
 * @n: The number of elements in @cmd
 *
 * May sleep. Do not call from atomic contexts.
 */
int rpmh_write(const struct device *dev, enum rpmh_state state,
	       const struct tcs_cmd *cmd, u32 n)
{
	DECLARE_COMPLETION_ONSTACK(compl);
	DEFINE_RPMH_MSG_ONSTACK(dev, state, &compl, rpm_msg);
	int ret;

	if (!cmd || !n || n > MAX_RPMH_PAYLOAD)
		return -EINVAL;

	memcpy(rpm_msg.cmd, cmd, n * sizeof(*cmd));
	rpm_msg.msg.num_cmds = n;

	ret = __rpmh_write(dev, state, &rpm_msg);
	if (ret)
		return ret;

	ret = wait_for_completion_timeout(&compl, RPMH_TIMEOUT_MS);
	WARN_ON(!ret);
	return (ret > 0) ? 0 : -ETIMEDOUT;
}

static int cache_batch(struct rpmh_ctrlr *ctrlr,
		       struct rpmh_request **rpm_msg, int count)
{
	unsigned long flags;
	int ret = 0;
	int index = 0;
	int i;

	spin_lock_irqsave(&ctrlr->cache_lock, flags);
	while (index < RPMH_MAX_BATCH_CACHE && ctrlr->batch_cache[index])
		index++;
	if (index + count >= RPMH_MAX_BATCH_CACHE) {
		ret = -ENOMEM;
		goto exit;
	}

	for (i = 0; i < count; i++)
		ctrlr->batch_cache[index + i] = rpm_msg[i];
exit:
	spin_unlock_irqrestore(&ctrlr->cache_lock, flags);

	return ret;
}

static int flush_batch(struct rpmh_ctrlr *ctrlr)
{
	const struct rpmh_request *rpm_msg;
	unsigned long flags;
	int ret = 0;
	int i;

	/* Send Sleep/Wake requests to the controller, expect no response */
	spin_lock_irqsave(&ctrlr->cache_lock, flags);
	for (i = 0; i < RPMH_MAX_BATCH_CACHE && ctrlr->batch_cache[i]; i++) {
		rpm_msg = ctrlr->batch_cache[i];
		ret = rpmh_rsc_write_ctrl_data(ctrlr->drv, &rpm_msg->msg);
		if (ret)
			break;
	}
	spin_unlock_irqrestore(&ctrlr->cache_lock, flags);

	return ret;
}

static void invalidate_batch(struct rpmh_ctrlr *ctrlr)
{
	unsigned long flags;
	int index = 0;

	spin_lock_irqsave(&ctrlr->cache_lock, flags);
	while (index < RPMH_MAX_BATCH_CACHE && ctrlr->batch_cache[index]) {
		if (ctrlr->batch_cache[index]->needs_free)
			kfree(ctrlr->batch_cache[index]);
		ctrlr->batch_cache[index] = NULL;
		index++;
	}
	spin_unlock_irqrestore(&ctrlr->cache_lock, flags);
}

/**
 * rpmh_write_batch: Write multiple sets of RPMH commands and wait for the
 * batch to finish.
 *
 * @dev: the device making the request
 * @state: Active/sleep set
 * @cmd: The payload data
 * @n: The array of count of elements in each batch, 0 terminated.
 *
 * Write a request to the RSC controller without caching. If the request
 * state is ACTIVE, then the requests are treated as completion request
 * and sent to the controller immediately. The function waits until all the
 * commands are complete. If the request was to SLEEP or WAKE_ONLY, then the
 * request is sent as fire-n-forget and no ack is expected.
 *
 * May sleep. Do not call from atomic contexts for ACTIVE_ONLY requests.
 */
int rpmh_write_batch(const struct device *dev, enum rpmh_state state,
		     const struct tcs_cmd *cmd, u32 *n)
{
	struct rpmh_request *rpm_msg[RPMH_MAX_REQ_IN_BATCH] = { NULL };
	DECLARE_COMPLETION_ONSTACK(compl);
	atomic_t wait_count = ATOMIC_INIT(0);
	struct rpmh_ctrlr *ctrlr = get_rpmh_ctrlr(dev);
	int count = 0;
	int ret, i, j;

	if (IS_ERR(ctrlr) || !cmd || !n)
		return -EINVAL;

	while (n[count++] > 0)
		;
	count--;
	if (!count || count > RPMH_MAX_REQ_IN_BATCH)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		rpm_msg[i] = __get_rpmh_msg_async(state, cmd, n[i]);
		if (IS_ERR(rpm_msg[i])) {
			ret = PTR_ERR(rpm_msg[i]);
			for (j = i-1; j >= 0; j--) {
				if (rpm_msg[j]->needs_free)
					kfree(rpm_msg[j]);
			}
			return ret;
		}
		cmd += n[i];
	}

	if (state != RPMH_ACTIVE_ONLY_STATE)
		return cache_batch(ctrlr, rpm_msg, count);

	atomic_set(&wait_count, count);

	for (i = 0; i < count; i++) {
		rpm_msg[i]->completion = &compl;
		rpm_msg[i]->wait_count = &wait_count;
		ret = rpmh_rsc_send_data(ctrlr->drv, &rpm_msg[i]->msg);
		if (ret) {
			int j;

			pr_err("Error(%d) sending RPMH message addr=%#x\n",
			       ret, rpm_msg[i]->msg.cmds[0].addr);
			for (j = i; j < count; j++)
				rpmh_tx_done(&rpm_msg[j]->msg, ret);
			break;
		}
	}

	ret = wait_for_completion_timeout(&compl, RPMH_TIMEOUT_MS);
	return (ret > 0) ? 0 : -ETIMEDOUT;

}

static int is_req_valid(struct cache_req *req)
{
	return (req->sleep_val != UINT_MAX &&
		req->wake_val != UINT_MAX &&
		req->sleep_val != req->wake_val);
}

static int send_single(const struct device *dev, enum rpmh_state state,
		       u32 addr, u32 data)
{
	DEFINE_RPMH_MSG_ONSTACK(dev, state, NULL, rpm_msg);
	struct rpmh_ctrlr *ctrlr = get_rpmh_ctrlr(dev);

	if (IS_ERR(ctrlr))
		return PTR_ERR(ctrlr);

	/* Wake sets are always complete and sleep sets are not */
	rpm_msg.msg.wait_for_compl = (state == RPMH_WAKE_ONLY_STATE);
	rpm_msg.cmd[0].addr = addr;
	rpm_msg.cmd[0].data = data;
	rpm_msg.msg.num_cmds = 1;

	return rpmh_rsc_write_ctrl_data(ctrlr->drv, &rpm_msg.msg);
}

/**
 * rpmh_flush: Flushes the buffered active and sleep sets to TCS
 *
 * @dev: The device making the request
 *
 * Return: -EBUSY if the controller is busy, probably waiting on a response
 * to a RPMH request sent earlier.
 *
 * This function is always called from the sleep code from the last CPU
 * that is powering down the entire system. Since no other RPMH API would be
 * executing at this time, it is safe to run lockless.
 */
int rpmh_flush(const struct device *dev)
{
	struct cache_req *p;
	struct rpmh_ctrlr *ctrlr = get_rpmh_ctrlr(dev);
	int ret;

	if (IS_ERR(ctrlr))
		return PTR_ERR(ctrlr);

	if (!ctrlr->dirty) {
		pr_debug("Skipping flush, TCS has latest data.\n");
		return 0;
	}

	/* First flush the cached batch requests */
	ret = flush_batch(ctrlr);
	if (ret)
		return ret;

	/*
	 * Nobody else should be calling this function other than system PM,
	 * hence we can run without locks.
	 */
	list_for_each_entry(p, &ctrlr->cache, list) {
		if (!is_req_valid(p)) {
			pr_debug("%s: skipping RPMH req: a:%#x s:%#x w:%#x",
				 __func__, p->addr, p->sleep_val, p->wake_val);
			continue;
		}
		ret = send_single(dev, RPMH_SLEEP_STATE, p->addr, p->sleep_val);
		if (ret)
			return ret;
		ret = send_single(dev, RPMH_WAKE_ONLY_STATE,
				  p->addr, p->wake_val);
		if (ret)
			return ret;
	}

	ctrlr->dirty = false;

	return 0;
}

/**
 * rpmh_invalidate: Invalidate all sleep and active sets
 * sets.
 *
 * @dev: The device making the request
 *
 * Invalidate the sleep and active values in the TCS blocks.
 */
int rpmh_invalidate(const struct device *dev)
{
	struct rpmh_ctrlr *ctrlr = get_rpmh_ctrlr(dev);
	int ret;

	if (IS_ERR(ctrlr))
		return PTR_ERR(ctrlr);

	invalidate_batch(ctrlr);
	ctrlr->dirty = true;

	do {
		ret = rpmh_rsc_invalidate(ctrlr->drv);
	} while (ret == -EAGAIN);

	return ret;
}
