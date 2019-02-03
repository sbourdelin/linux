/*
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
 * Copyright (c) 2017-2018, Broadcom Limited. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef DIM_H
#define DIM_H

#include <linux/module.h>

#define DIM_NEVENTS 64
#define IS_SIGNIFICANT_DIFF(val, ref) \
	(((100UL * abs((val) - (ref))) / (ref)) > 10) /* more than 10% difference */
#define BIT_GAP(bits, end, start) ((((end) - (start)) + BIT_ULL(bits)) & (BIT_ULL(bits) - 1))


struct dim_cq_moder {
	u16 usec;
	u16 pkts;
	u8 cq_period_mode;
};

struct dim_sample {
	ktime_t time;
	u32     pkt_ctr;
	u32     byte_ctr;
	u16     event_ctr;
};

struct dim_stats {
	int ppms; /* packets per msec */
	int bpms; /* bytes per msec */
	int epms; /* events per msec */
};

struct dim { /* Dynamic Interrupt Moderation */
	u8                                      state;
	struct dim_stats                        prev_stats;
	struct dim_sample                       start_sample;
	struct work_struct                      work;
	u8                                      profile_ix;
	u8                                      mode;
	u8                                      tune_state;
	u8                                      steps_right;
	u8                                      steps_left;
	u8                                      tired;
};

enum {
	DIM_CQ_PERIOD_MODE_START_FROM_EQE = 0x0,
	DIM_CQ_PERIOD_MODE_START_FROM_CQE = 0x1,
	DIM_CQ_PERIOD_NUM_MODES
};

enum {
	DIM_START_MEASURE,
	DIM_MEASURE_IN_PROGRESS,
	DIM_APPLY_NEW_PROFILE,
};

enum {
	DIM_PARKING_ON_TOP,
	DIM_PARKING_TIRED,
	DIM_GOING_RIGHT,
	DIM_GOING_LEFT,
};

enum {
	DIM_STATS_WORSE,
	DIM_STATS_SAME,
	DIM_STATS_BETTER,
};

enum {
	DIM_STEPPED,
	DIM_TOO_TIRED,
	DIM_ON_EDGE,
};

bool dim_on_top(struct dim *dim);

void dim_turn(struct dim *dim);

void dim_park_on_top(struct dim *dim);

void dim_park_tired(struct dim *dim);

void dim_create_sample(u16 event_ctr, u64 packets, u64 bytes, struct dim_sample *s);

void dim_calc_stats(struct dim_sample *start, struct dim_sample *end,
		    struct dim_stats *curr_stats);

#endif /* DIM_H */
