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
 */
struct rpmh_request {
	struct tcs_request msg;
	struct tcs_cmd cmd[MAX_RPMH_PAYLOAD];
	struct completion *completion;
	const struct device *dev;
	int err;
};

/**
 * struct rpmh_ctrlr: our representation of the controller
 *
 * @drv: the controller instance
 * @cache: the list of cached requests
 * @cache_lock: synchronize access to the cache data
 * @dirty: was the cache updated since flush
 */
struct rpmh_ctrlr {
	struct rsc_drv *drv;
	struct list_head cache;
	spinlock_t cache_lock;
	bool dirty;
};

static struct rpmh_ctrlr rpmh_rsc[RPMH_MAX_CTRLR];
static DEFINE_SPINLOCK(rpmh_rsc_lock);

static struct rpmh_ctrlr *get_rpmh_ctrlr(const struct device *dev)
{
	int i;
	struct rsc_drv *drv = dev_get_drvdata(dev->parent);
	unsigned long flags;

	if (!drv)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < RPMH_MAX_CTRLR; i++)
		if (rpmh_rsc[i].drv == drv)
			return &rpmh_rsc[i];

	spin_lock_irqsave(&rpmh_rsc_lock, flags);
	for (i = 0; i < RPMH_MAX_CTRLR; i++) {
		if (!rpmh_rsc[i].drv) {
			rpmh_rsc[i].drv = drv;
			spin_lock_init(&rpmh_rsc[i].cache_lock);
			INIT_LIST_HEAD(&rpmh_rsc[i].cache);
			break;
		}
	}
	spin_unlock_irqrestore(&rpmh_rsc_lock, flags);

	if (i < RPMH_MAX_CTRLR)
		return &rpmh_rsc[i];

	return ERR_PTR(-ENOMEM);
}

void rpmh_tx_done(const struct tcs_request *msg, int r)
{
	struct rpmh_request *rpm_msg = container_of(msg, struct rpmh_request,
						    msg);
	struct completion *compl = rpm_msg->completion;

	rpm_msg->err = r;

	if (r)
		dev_err(rpm_msg->dev, "RPMH TX fail in msg addr=%#x, err=%d\n",
			rpm_msg->msg.cmds[0].addr, r);

	/* Signal the blocking thread we are done */
	if (compl)
		complete(compl);
}
EXPORT_SYMBOL(rpmh_tx_done);

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
EXPORT_SYMBOL(rpmh_write);

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
EXPORT_SYMBOL(rpmh_flush);

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

	ctrlr->dirty = true;

	do {
		ret = rpmh_rsc_invalidate(ctrlr->drv);
	} while (ret == -EAGAIN);

	return ret;
}
EXPORT_SYMBOL(rpmh_invalidate);
