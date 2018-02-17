/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 */


#ifndef __RPM_INTERNAL_H__
#define __RPM_INTERNAL_H__

#include <soc/qcom/tcs.h>

#define TCS_TYPE_NR			4
#define MAX_CMDS_PER_TCS		16
#define MAX_TCS_PER_TYPE		3
#define MAX_TCS_NR			(MAX_TCS_PER_TYPE * TCS_TYPE_NR)
#define MAX_TCS_SLOTS			(MAX_CMDS_PER_TCS * MAX_TCS_PER_TYPE)

struct rsc_drv;

/**
 * tcs_response: Response object for a request
 *
 * @drv: the controller
 * @msg: the request for this response
 * @m: the tcs identifier
 * @err: error reported in the response
 * @list: link list object.
 */
struct tcs_response {
	struct rsc_drv *drv;
	struct tcs_request *msg;
	u32 m;
	int err;
	struct list_head list;
};

/**
 * tcs_group: group of TCSes for a request state
 *
 * @type: type of the TCS in this group - active, sleep, wake
 * @tcs_mask: mask of the TCSes relative to all the TCSes in the RSC
 * @tcs_offset: start of the TCS group relative to the TCSes in the RSC
 * @num_tcs: number of TCSes in this type
 * @ncpt: number of commands in each TCS
 * @tcs_lock: lock for synchronizing this TCS writes
 * @responses: response objects for requests sent from each TCS
 * @cmd_addr: flattened cache of cmds in sleep/wake TCS
 * @slots: indicates which of @cmd_addr are occupied
 */
struct tcs_group {
	struct rsc_drv *drv;
	int type;
	u32 tcs_mask;
	u32 tcs_offset;
	int num_tcs;
	int ncpt;
	spinlock_t tcs_lock;
	struct tcs_response *responses[MAX_TCS_PER_TYPE];
	u32 *cmd_addr;
	DECLARE_BITMAP(slots, MAX_TCS_SLOTS);

};

/**
 * rsc_drv: the RSC controller
 *
 * @name: controller identifier
 * @tcs_base: start address of the TCS registers in this controller
 * @drv_id: instance id in the controller (DRV)
 * @num_tcs: number of TCSes in this DRV
 * @tasklet: handle responses, off-load work from IRQ handler
 * @response_pending: list of responses that needs to be sent to caller
 * @tcs: TCS groups
 * @tcs_in_use: s/w state of the TCS
 * @drv_lock: synchronize state of the controller
 */
struct rsc_drv {
	const char *name;
	void __iomem *tcs_base;
	int drv_id;
	int num_tcs;
	struct tasklet_struct tasklet;
	struct list_head response_pending;
	struct tcs_group tcs[TCS_TYPE_NR];
	atomic_t tcs_in_use[MAX_TCS_NR];
	spinlock_t drv_lock;
};


int rpmh_rsc_send_data(struct rsc_drv *drv, struct tcs_request *msg);
int rpmh_rsc_write_ctrl_data(struct rsc_drv *drv, struct tcs_request *msg);
int rpmh_rsc_invalidate(struct rsc_drv *drv);

void rpmh_tx_done(struct tcs_request *msg, int r);

#endif /* __RPM_INTERNAL_H__ */
