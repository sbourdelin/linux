/*
 * Header file for Intel FPGA Management Engine (FME) Driver
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * Authors:
 *   Kang Luwei <luwei.kang@intel.com>
 *   Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *   Joseph Grecco <joe.grecco@intel.com>
 *   Enno Luebbers <enno.luebbers@intel.com>
 *   Tim Whisonant <tim.whisonant@intel.com>
 *   Ananda Ravuri <ananda.ravuri@intel.com>
 *   Henry Mitchel <henry.mitchel@intel.com>
 *
 * This work is licensed under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license. See the
 * LICENSE.BSD file under this directory for the BSD license and see
 * the COPYING file in the top-level directory for the GPLv2 license.
 */

#ifndef __INTEL_FME_H
#define __INTEL_FME_H

struct fpga_fme {
	u8  port_id;
	u64 pr_err;
	struct feature_platform_data *pdata;
};

extern struct feature_ops pr_mgmt_ops;

#endif
