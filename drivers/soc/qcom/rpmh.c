// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 */

#include <linux/atomic.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <soc/qcom/rpmh.h>

#include "rpmh-internal.h"

#define RPMH_MAX_MBOXES			2
#define RPMH_TIMEOUT			(10 * HZ)

#define DEFINE_RPMH_MSG_ONSTACK(rc, s, q, c, name)	\
	struct rpmh_request name = {			\
		.msg = {				\
			.state = s,			\
			.payload = name.cmd,		\
			.num_payload = 0,		\
			.is_complete = true,		\
		},					\
		.cmd = { { 0 } },			\
		.completion = q,			\
		.wait_count = c,			\
		.rc = rc,				\
	}

/**
 * rpmh_request: the message to be sent to rpmh-rsc
 *
 * @msg: the request
 * @cmd: the payload that will be part of the @msg
 * @completion: triggered when request is done
 * @wait_count: count of waiters for this completion
 * @err: err return from the controller
 */
struct rpmh_request {
	struct tcs_request msg;
	struct tcs_cmd cmd[MAX_RPMH_PAYLOAD];
	struct completion *completion;
	atomic_t *wait_count;
	struct rpmh_client *rc;
	int err;
};

/**
 * rpmh_ctrlr: our representation of the controller
 *
 * @drv: the controller instance
 */
struct rpmh_ctrlr {
	struct rsc_drv *drv;
};

/**
 * rpmh_client: the client object
 *
 * @dev: the platform device that is the owner
 * @ctrlr: the controller associated with this client.
 */
struct rpmh_client {
	struct device *dev;
	struct rpmh_ctrlr *ctrlr;
};

static struct rpmh_ctrlr rpmh_rsc[RPMH_MAX_MBOXES];
static DEFINE_MUTEX(rpmh_ctrlr_mutex);

void rpmh_tx_done(struct tcs_request *msg, int r)
{
	struct rpmh_request *rpm_msg = container_of(msg,
						   struct rpmh_request, msg);
	atomic_t *wc = rpm_msg->wait_count;
	struct completion *compl = rpm_msg->completion;

	rpm_msg->err = r;

	if (r)
		dev_err(rpm_msg->rc->dev,
		       "RPMH TX fail in msg addr 0x%x, err=%d\n",
		       rpm_msg->msg.payload[0].addr, r);

	/* Signal the blocking thread we are done */
	if (wc && atomic_dec_and_test(wc) && compl)
		complete(compl);
}
EXPORT_SYMBOL(rpmh_tx_done);

/**
 * wait_for_tx_done: Wait until the response is received.
 *
 * @rc: The RPMH client
 * @compl: The completion object
 * @addr: An addr that we sent in that request
 * @data: The data for the address in that request
 *
 */
static int wait_for_tx_done(struct rpmh_client *rc,
			   struct completion *compl, u32 addr, u32 data)
{
	int ret;

	ret = wait_for_completion_timeout(compl, RPMH_TIMEOUT);
	if (ret)
		dev_dbg(rc->dev,
		       "RPMH response received addr=0x%x data=0x%x\n",
		       addr, data);
	else
		dev_err(rc->dev,
			"RPMH response timeout addr=0x%x data=0x%x\n",
			addr, data);

	return (ret > 0) ? 0 : -ETIMEDOUT;
}

/**
 * __rpmh_write: send the RPMH request
 *
 * @rc: The RPMH client
 * @state: Active/Sleep request type
 * @rpm_msg: The data that needs to be sent (payload).
 */
static int __rpmh_write(struct rpmh_client *rc, enum rpmh_state state,
		       struct rpmh_request *rpm_msg)
{
	int ret = -EFAULT;

	rpm_msg->msg.state = state;

	if (state == RPMH_ACTIVE_ONLY_STATE) {
		WARN_ON(irqs_disabled());
		ret = rpmh_rsc_send_data(rc->ctrlr->drv, &rpm_msg->msg);
		if (!ret)
			dev_dbg(rc->dev,
			       "RPMH request sent addr=0x%x, data=0x%x\n",
			       rpm_msg->msg.payload[0].addr,
			       rpm_msg->msg.payload[0].data);
		else
			dev_warn(rc->dev,
				"Error in RPMH request addr=0x%x, data=0x%x\n",
				rpm_msg->msg.payload[0].addr,
				rpm_msg->msg.payload[0].data);
	}

	return ret;
}

/**
 * rpmh_write: Write a set of RPMH commands and block until response
 *
 * @rc: The RPMh handle got from rpmh_get_dev_channel
 * @state: Active/sleep set
 * @cmd: The payload data
 * @n: The number of elements in payload
 *
 * May sleep. Do not call from atomic contexts.
 */
int rpmh_write(struct rpmh_client *rc, enum rpmh_state state,
	      struct tcs_cmd *cmd, int n)
{
	DECLARE_COMPLETION_ONSTACK(compl);
	atomic_t wait_count = ATOMIC_INIT(1);
	DEFINE_RPMH_MSG_ONSTACK(rc, state, &compl, &wait_count, rpm_msg);
	int ret;

	if (IS_ERR_OR_NULL(rc) || !cmd || n <= 0 || n > MAX_RPMH_PAYLOAD)
		return -EINVAL;

	might_sleep();

	memcpy(rpm_msg.cmd, cmd, n * sizeof(*cmd));
	rpm_msg.msg.num_payload = n;

	ret = __rpmh_write(rc, state, &rpm_msg);
	if (ret)
		return ret;

	return wait_for_tx_done(rc, &compl, cmd[0].addr, cmd[0].data);
}
EXPORT_SYMBOL(rpmh_write);

static struct rpmh_ctrlr *get_rpmh_ctrlr(struct platform_device *pdev)
{
	int i;
	struct rsc_drv *drv = dev_get_drvdata(pdev->dev.parent);
	struct rpmh_ctrlr *ctrlr = ERR_PTR(-EFAULT);

	if (!drv)
		return ctrlr;

	mutex_lock(&rpmh_ctrlr_mutex);
	for (i = 0; i < RPMH_MAX_MBOXES; i++) {
		if (rpmh_rsc[i].drv == drv) {
			ctrlr = &rpmh_rsc[i];
			goto unlock;
		}
	}

	for (i = 0; i < RPMH_MAX_MBOXES; i++) {
		if (rpmh_rsc[i].drv == NULL) {
			ctrlr = &rpmh_rsc[i];
			ctrlr->drv = drv;
			break;
		}
	}
	WARN_ON(i == RPMH_MAX_MBOXES);
unlock:
	mutex_unlock(&rpmh_ctrlr_mutex);
	return ctrlr;
}

/**
 * rpmh_get_client: Get the RPMh handle
 *
 * @pdev: the platform device which needs to communicate with RPM
 * accelerators
 * May sleep.
 */
struct rpmh_client *rpmh_get_client(struct platform_device *pdev)
{
	struct rpmh_client *rc;

	rc = kzalloc(sizeof(*rc), GFP_KERNEL);
	if (!rc)
		return ERR_PTR(-ENOMEM);

	rc->dev = &pdev->dev;
	rc->ctrlr = get_rpmh_ctrlr(pdev);
	if (IS_ERR(rc->ctrlr)) {
		kfree(rc);
		return ERR_PTR(-EFAULT);
	}

	return rc;
}
EXPORT_SYMBOL(rpmh_get_client);

/**
 * rpmh_release: Release the RPMH client
 *
 * @rc: The RPMh handle to be freed.
 */
void rpmh_release(struct rpmh_client *rc)
{
	kfree(rc);
}
EXPORT_SYMBOL(rpmh_release);
