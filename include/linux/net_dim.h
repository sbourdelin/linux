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

#ifndef NET_DIM_H
#define NET_DIM_H

#include <linux/module.h>
#include <linux/dim.h>

#define NET_DIM_PARAMS_NUM_PROFILES 5
/* Netdev dynamic interrupt moderation profiles */
#define NET_DIM_DEFAULT_RX_CQ_MODERATION_PKTS_FROM_EQE 256
#define NET_DIM_DEFAULT_TX_CQ_MODERATION_PKTS_FROM_EQE 128
#define NET_DIM_DEF_PROFILE_CQE 1
#define NET_DIM_DEF_PROFILE_EQE 1

/* All profiles sizes must be NET_PARAMS_DIM_NUM_PROFILES */
#define NET_DIM_RX_EQE_PROFILES { \
	{1,   NET_DIM_DEFAULT_RX_CQ_MODERATION_PKTS_FROM_EQE}, \
	{8,   NET_DIM_DEFAULT_RX_CQ_MODERATION_PKTS_FROM_EQE}, \
	{64,  NET_DIM_DEFAULT_RX_CQ_MODERATION_PKTS_FROM_EQE}, \
	{128, NET_DIM_DEFAULT_RX_CQ_MODERATION_PKTS_FROM_EQE}, \
	{256, NET_DIM_DEFAULT_RX_CQ_MODERATION_PKTS_FROM_EQE}, \
}

#define NET_DIM_RX_CQE_PROFILES { \
	{2,  256},             \
	{8,  128},             \
	{16, 64},              \
	{32, 64},              \
	{64, 64}               \
}

#define NET_DIM_TX_EQE_PROFILES { \
	{1,   NET_DIM_DEFAULT_TX_CQ_MODERATION_PKTS_FROM_EQE},  \
	{8,   NET_DIM_DEFAULT_TX_CQ_MODERATION_PKTS_FROM_EQE},  \
	{32,  NET_DIM_DEFAULT_TX_CQ_MODERATION_PKTS_FROM_EQE},  \
	{64,  NET_DIM_DEFAULT_TX_CQ_MODERATION_PKTS_FROM_EQE},  \
	{128, NET_DIM_DEFAULT_TX_CQ_MODERATION_PKTS_FROM_EQE}   \
}

#define NET_DIM_TX_CQE_PROFILES { \
	{5,  128},  \
	{8,  64},  \
	{16, 32},  \
	{32, 32},  \
	{64, 32}   \
}

static const struct dim_cq_moder
rx_profile[DIM_CQ_PERIOD_NUM_MODES][NET_DIM_PARAMS_NUM_PROFILES] = {
	NET_DIM_RX_EQE_PROFILES,
	NET_DIM_RX_CQE_PROFILES,
};

static const struct dim_cq_moder
tx_profile[DIM_CQ_PERIOD_NUM_MODES][NET_DIM_PARAMS_NUM_PROFILES] = {
	NET_DIM_TX_EQE_PROFILES,
	NET_DIM_TX_CQE_PROFILES,
};

struct dim_cq_moder net_dim_get_rx_moderation(u8 cq_period_mode, int ix);

struct dim_cq_moder net_dim_get_def_rx_moderation(u8 cq_period_mode);

struct dim_cq_moder net_dim_get_tx_moderation(u8 cq_period_mode, int ix);

struct dim_cq_moder net_dim_get_def_tx_moderation(u8 cq_period_mode);

void net_dim(struct dim *dim, struct dim_sample end_sample);

#endif /* NET_DIM_H */
