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

struct xdp_ring_req {
	__u32   mr_fd;      /* FD of packet buffer area registered
			     * with XDP_MEM_REG
			     */
	__u32   desc_nr;    /* Number of descriptors in ring */
};

/* Pgoff for mmaping the rings */
#define XDP_PGOFF_RX_RING 0
#define XDP_PGOFF_TX_RING 0x80000000

/* XDP user space ring structure */
#define XDP_DESC_KERNEL 0x0080 /* The descriptor is owned by the kernel */
#define XDP_PKT_CONT    1      /* The packet continues in the next descriptor */

struct xdp_desc {
	__u32 idx;
	__u32 len;
	__u16 offset;
	__u8  error; /* an errno */
	__u8  flags;
	__u8  padding[4];
};

struct xdp_queue {
	struct xdp_desc *ring;

	__u32 avail_idx;
	__u32 last_used_idx;
	__u32 num_free;
	__u32 ring_mask;
};

#endif /* _LINUX_IF_XDP_H */
