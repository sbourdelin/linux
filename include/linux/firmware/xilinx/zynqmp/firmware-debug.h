/*
 * Xilinx Zynq MPSoC Firmware layer
 *
 *  Copyright (C) 2014-2017 Xilinx
 *
 *  Michal Simek <michal.simek@xilinx.com>
 *  Davorin Mista <davorin.mista@aggios.com>
 *  Jolly Shah <jollys@xilinx.com>
 *  Rajan Vaja <rajanv@xilinx.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#ifndef __SOC_ZYNQMP_FIRMWARE_DEBUG_H__
#define __SOC_ZYNQMP_FIRMWARE_DEBUG_H__

#include <linux/firmware/xilinx/zynqmp/firmware.h>

int zynqmp_pm_self_suspend(const u32 node,
			   const u32 latency,
			   const u32 state);
int zynqmp_pm_abort_suspend(const enum zynqmp_pm_abort_reason reason);
int zynqmp_pm_register_notifier(const u32 node, const u32 event,
				const u32 wake, const u32 enable);

#if IS_REACHABLE(CONFIG_ZYNQMP_FIRMWARE_DEBUG)
int zynqmp_pm_api_debugfs_init(void);
#else
static inline int zynqmp_pm_api_debugfs_init(void) { return 0; }
#endif

#endif /* __SOC_ZYNQMP_FIRMWARE_DEBUG_H__ */
