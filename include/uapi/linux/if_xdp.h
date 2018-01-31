/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * if_xdp: XDP socket user-space interface
 *
 * Copyright(c) 2017 Intel Corporation.
 *
 * Author(s): Björn Töpel <bjorn.topel@intel.com>
 *	      Magnus Karlsson <magnus.karlsson@intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#ifndef _LINUX_IF_XDP_H
#define _LINUX_IF_XDP_H

#include <linux/types.h>

struct sockaddr_xdp {
	__u16	sxdp_family;
	__u32	sxdp_ifindex;
	__u32	sxdp_queue_id;
};

/* XDP socket options */
#define XDP_MEM_REG	1
#define XDP_RX_RING	2
#define XDP_TX_RING	3

struct xdp_mr_req {
	__u64	addr;           /* Start of packet data area */
	__u64	len;            /* Length of packet data area */
	__u32	frame_size;     /* Frame size */
	__u32	data_headroom;  /* Frame head room */
};

#endif /* _LINUX_IF_XDP_H */
