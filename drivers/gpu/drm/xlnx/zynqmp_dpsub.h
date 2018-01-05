/*
 * ZynqMP DPSUB Subsystem Driver
 *
 *  Copyright (C) 2017 - 2018 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyun.kwon@xilinx.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _ZYNQMP_DPSUB_H_
#define _ZYNQMP_DPSUB_H_

struct zynqmp_dpsub {
	struct zynqmp_dp *dp;
	struct zynqmp_disp *disp;
};

#endif /* _ZYNQMP_DPSUB_H_ */
