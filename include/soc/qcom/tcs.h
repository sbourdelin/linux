/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 */

#ifndef __SOC_QCOM_TCS_H__
#define __SOC_QCOM_TCS_H__

#define MAX_RPMH_PAYLOAD	16

/**
 * rpmh_state: state for the request
 *
 * RPMH_WAKE_ONLY_STATE:   Resume resource state to the value previously
 *                         requested before the processor was powered down.
 * RPMH_SLEEP_STATE:       State of the resource when the processor subsystem
 *                         is powered down. There is no client using the
 *                         resource actively.
 * RPMH_ACTIVE_ONLY_STATE: Active or AMC mode requests. Resource state
 *                         is aggregated immediately.
 */
enum rpmh_state {
	RPMH_SLEEP_STATE,
	RPMH_WAKE_ONLY_STATE,
	RPMH_ACTIVE_ONLY_STATE,
};

/**
 * tcs_cmd: an individual request to RPMH.
 *
 * @addr:     the address of the resource slv_id:18:16 | offset:0:15
 * @data:     the resource state request
 * @complete: wait for this request to be complete before sending the next
 */
struct tcs_cmd {
	u32 addr;
	u32 data;
	bool complete;
};

/**
 * tcs_request: A set of tcs_cmds sent togther in a TCS
 *
 * @state:       state for the request.
 * @is_complete: expect a response from the h/w accelerator
 * @num_payload: the number of tcs_cmds in thsi payload
 * @payload:     an array of tcs_cmds
 */
struct tcs_request {
	enum rpmh_state state;
	bool is_complete;
	u32 num_payload;
	struct tcs_cmd *payload;
};

#endif /* __SOC_QCOM_TCS_H__ */
