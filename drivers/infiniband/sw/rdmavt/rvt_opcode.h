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

#ifndef RVT_OPCODE_H
#define RVT_OPCODE_H

/*
 * contains header bit mask definitions and header lengths
 * declaration of the rvt_opcode_info struct and
 * rvt_wr_opcode_info struct
 */

enum rvt_wr_mask {
	WR_INLINE_MASK			= BIT(0),
	WR_ATOMIC_MASK			= BIT(1),
	WR_SEND_MASK			= BIT(2),
	WR_READ_MASK			= BIT(3),
	WR_WRITE_MASK			= BIT(4),
	WR_LOCAL_MASK			= BIT(5),

	WR_READ_OR_WRITE_MASK		= WR_READ_MASK | WR_WRITE_MASK,
	WR_READ_WRITE_OR_SEND_MASK	= WR_READ_OR_WRITE_MASK | WR_SEND_MASK,
	WR_WRITE_OR_SEND_MASK		= WR_WRITE_MASK | WR_SEND_MASK,
	WR_ATOMIC_OR_READ_MASK		= WR_ATOMIC_MASK | WR_READ_MASK,
};

#define WR_MAX_QPT		(8)

struct rvt_wr_opcode_info {
	char			*name;
	enum rvt_wr_mask	mask[WR_MAX_QPT];
};

enum rvt_hdr_type {
	RVT_LRH,
	RVT_GRH,
	RVT_BTH,
	RVT_RETH,
	RVT_AETH,
	RVT_ATMETH,
	RVT_ATMACK,
	RVT_IETH,
	RVT_RDETH,
	RVT_DETH,
	RVT_IMMDT,
	RVT_PAYLOAD,
	NUM_HDR_TYPES
};

enum rvt_hdr_mask {
	RVT_LRH_MASK		= BIT(RVT_LRH),
	RVT_GRH_MASK		= BIT(RVT_GRH),
	RVT_BTH_MASK		= BIT(RVT_BTH),
	RVT_IMMDT_MASK		= BIT(RVT_IMMDT),
	RVT_RETH_MASK		= BIT(RVT_RETH),
	RVT_AETH_MASK		= BIT(RVT_AETH),
	RVT_ATMETH_MASK		= BIT(RVT_ATMETH),
	RVT_ATMACK_MASK		= BIT(RVT_ATMACK),
	RVT_IETH_MASK		= BIT(RVT_IETH),
	RVT_RDETH_MASK		= BIT(RVT_RDETH),
	RVT_DETH_MASK		= BIT(RVT_DETH),
	RVT_PAYLOAD_MASK	= BIT(RVT_PAYLOAD),

	RVT_REQ_MASK		= BIT(NUM_HDR_TYPES + 0),
	RVT_ACK_MASK		= BIT(NUM_HDR_TYPES + 1),
	RVT_SEND_MASK		= BIT(NUM_HDR_TYPES + 2),
	RVT_WRITE_MASK		= BIT(NUM_HDR_TYPES + 3),
	RVT_READ_MASK		= BIT(NUM_HDR_TYPES + 4),
	RVT_ATOMIC_MASK		= BIT(NUM_HDR_TYPES + 5),

	RVT_RWR_MASK		= BIT(NUM_HDR_TYPES + 6),
	RVT_COMP_MASK		= BIT(NUM_HDR_TYPES + 7),

	RVT_START_MASK		= BIT(NUM_HDR_TYPES + 8),
	RVT_MIDDLE_MASK		= BIT(NUM_HDR_TYPES + 9),
	RVT_END_MASK		= BIT(NUM_HDR_TYPES + 10),

	RVT_LOOPBACK_MASK	= BIT(NUM_HDR_TYPES + 12),

	RVT_READ_OR_ATOMIC	= (RVT_READ_MASK | RVT_ATOMIC_MASK),
	RVT_WRITE_OR_SEND	= (RVT_WRITE_MASK | RVT_SEND_MASK),
};

extern struct rvt_wr_opcode_info rvt_wr_opcode_info[];

#define OPCODE_NONE		(-1)
#define RVT_NUM_OPCODE		256

struct rvt_opcode_info {
	char			*name;
	enum rvt_hdr_mask	mask;
	int			length;
	int			offset[NUM_HDR_TYPES];
};

extern struct rvt_opcode_info rvt_opcode[RVT_NUM_OPCODE];

#endif /* RVT_OPCODE_H */
