/*
 * Copyright (c) 2015 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
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
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
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

#ifndef RVT_PARAM_H
#define RVT_PARAM_H

#include "rvt_hdr.h"

static inline enum ib_mtu rvt_mtu_int_to_enum(int mtu)
{
	if (mtu < 256)
		return 0;
	else if (mtu < 512)
		return IB_MTU_256;
	else if (mtu < 1024)
		return IB_MTU_512;
	else if (mtu < 2048)
		return IB_MTU_1024;
	else if (mtu < 4096)
		return IB_MTU_2048;
	else
		return IB_MTU_4096;
}

/* Find the IB mtu for a given network MTU. */
static inline enum ib_mtu eth_mtu_int_to_enum(int mtu)
{
	mtu -= RVT_MAX_HDR_LENGTH;

	return rvt_mtu_int_to_enum(mtu);
}

/* default/initial rvt device parameter settings */
enum rvt_device_param {
	RVT_FW_VER			= 0,
	RVT_MAX_MR_SIZE			= -1ull,
	RVT_PAGE_SIZE_CAP		= 0xfffff000,
	RVT_VENDOR_ID			= 0,
	RVT_VENDOR_PART_ID		= 0,
	RVT_HW_VER			= 0,
	RVT_MAX_QP			= 0x10000,
	RVT_MAX_QP_WR			= 0x4000,
	RVT_MAX_INLINE_DATA		= 400,
	RVT_DEVICE_CAP_FLAGS		= IB_DEVICE_BAD_PKEY_CNTR
					| IB_DEVICE_BAD_QKEY_CNTR
					| IB_DEVICE_AUTO_PATH_MIG
					| IB_DEVICE_CHANGE_PHY_PORT
					| IB_DEVICE_UD_AV_PORT_ENFORCE
					| IB_DEVICE_PORT_ACTIVE_EVENT
					| IB_DEVICE_SYS_IMAGE_GUID
					| IB_DEVICE_RC_RNR_NAK_GEN
					| IB_DEVICE_SRQ_RESIZE,
	RVT_MAX_SGE			= 27,
	RVT_MAX_SGE_RD			= 0,
	RVT_MAX_CQ			= 16384,
	RVT_MAX_LOG_CQE			= 13,
	RVT_MAX_MR			= 2 * 1024,
	RVT_MAX_PD			= 0x7ffc,
	RVT_MAX_QP_RD_ATOM		= 128,
	RVT_MAX_EE_RD_ATOM		= 0,
	RVT_MAX_RES_RD_ATOM		= 0x3f000,
	RVT_MAX_QP_INIT_RD_ATOM		= 128,
	RVT_MAX_EE_INIT_RD_ATOM		= 0,
	RVT_ATOMIC_CAP			= 1,
	RVT_MAX_EE			= 0,
	RVT_MAX_RDD			= 0,
	RVT_MAX_MW			= 0,
	RVT_MAX_RAW_IPV6_QP		= 0,
	RVT_MAX_RAW_ETHY_QP		= 0,
	RVT_MAX_MCAST_GRP		= 8192,
	RVT_MAX_MCAST_QP_ATTACH		= 56,
	RVT_MAX_TOT_MCAST_QP_ATTACH	= 0x70000,
	RVT_MAX_AH			= 100,
	RVT_MAX_FMR			= 2 * 1024,
	RVT_MAX_MAP_PER_FMR		= 100,
	RVT_MAX_SRQ			= 960,
	RVT_MAX_SRQ_WR			= 0x4000,
	RVT_MIN_SRQ_WR			= 1,
	RVT_MAX_SRQ_SGE			= 27,
	RVT_MIN_SRQ_SGE			= 1,
	RVT_MAX_FMR_PAGE_LIST_LEN	= 0,
	RVT_MAX_PKEYS			= 64,
	RVT_LOCAL_CA_ACK_DELAY		= 15,

	RVT_MAX_UCONTEXT		= 512,

	RVT_NUM_PORT			= 1,
	RVT_NUM_COMP_VECTORS		= 1,

	RVT_MIN_QP_INDEX		= 16,
	RVT_MAX_QP_INDEX		= 0x00020000,

	RVT_MIN_SRQ_INDEX		= 0x00020001,
	RVT_MAX_SRQ_INDEX		= 0x00040000,

	RVT_MIN_MR_INDEX		= 0x00000001,
	RVT_MAX_MR_INDEX		= 0x00020000,
	RVT_MIN_FMR_INDEX		= 0x00020001,
	RVT_MAX_FMR_INDEX		= 0x00040000,
	RVT_MIN_MW_INDEX		= 0x00040001,
	RVT_MAX_MW_INDEX		= 0x00060000,
	RVT_MAX_PKT_PER_ACK		= 64,

	/* PSN window in RC, to prevent mixing new packets PSN with
	 * old ones. According to IB SPEC this number is half of
	 * the PSN range (2^24).
	 */
	RVT_MAX_UNACKED_PSNS		= 0x800000,

	/* Max inflight SKBs per queue pair */
	RVT_INFLIGHT_SKBS_PER_QP_HIGH	= 64,
	RVT_INFLIGHT_SKBS_PER_QP_LOW	= 16,

	/* Delay before calling arbiter timer */
	RVT_NSEC_ARB_TIMER_DELAY	= 200,
};

/* default/initial rvt port parameters */
enum rvt_port_param {
	RVT_PORT_STATE			= IB_PORT_DOWN,
	RVT_PORT_MAX_MTU		= IB_MTU_4096,
	RVT_PORT_ACTIVE_MTU		= IB_MTU_256,
	RVT_PORT_GID_TBL_LEN		= 32,
	RVT_PORT_PORT_CAP_FLAGS		= RDMA_CORE_CAP_PROT_ROCE_UDP_ENCAP,
	RVT_PORT_MAX_MSG_SZ		= 0x800000,
	RVT_PORT_BAD_PKEY_CNTR		= 0,
	RVT_PORT_QKEY_VIOL_CNTR		= 0,
	RVT_PORT_LID			= 0,
	RVT_PORT_SM_LID			= 0,
	RVT_PORT_SM_SL			= 0,
	RVT_PORT_LMC			= 0,
	RVT_PORT_MAX_VL_NUM		= 1,
	RVT_PORT_SUBNET_TIMEOUT		= 0,
	RVT_PORT_INIT_TYPE_REPLY	= 0,
	RVT_PORT_ACTIVE_WIDTH		= IB_WIDTH_1X,
	RVT_PORT_ACTIVE_SPEED		= 1,
	RVT_PORT_PKEY_TBL_LEN		= 64,
	RVT_PORT_PHYS_STATE		= 2,
	RVT_PORT_SUBNET_PREFIX		= 0xfe80000000000000ULL,
};

/* default/initial port info parameters */
enum rvt_port_info_param {
	RVT_PORT_INFO_VL_CAP		= 4,	/* 1-8 */
	RVT_PORT_INFO_MTU_CAP		= 5,	/* 4096 */
	RVT_PORT_INFO_OPER_VL		= 1,	/* 1 */
};

#endif /* RVT_PARAM_H */
