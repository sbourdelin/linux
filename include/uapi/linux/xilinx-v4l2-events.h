/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Xilinx V4L2 Events
 *
 * Copyright (C) 2019 Xilinx, Inc.
 *
 * Contacts: Vishal Sagar <vishal.sagar@xilinx.com>
 *
 */

#ifndef __UAPI_XILINX_V4L2_EVENTS_H__
#define __UAPI_XILINX_V4L2_EVENTS_H__

#include <linux/videodev2.h>

/* Xilinx CSI2 Receiver events */
#define V4L2_EVENT_XLNXCSIRX_CLASS	(V4L2_EVENT_PRIVATE_START | 0x100)
/* Short packet received */
#define V4L2_EVENT_XLNXCSIRX_SPKT	(V4L2_EVENT_XLNXCSIRX_CLASS | 0x1)
/* Short packet FIFO overflow */
#define V4L2_EVENT_XLNXCSIRX_SPKT_OVF	(V4L2_EVENT_XLNXCSIRX_CLASS | 0x2)
/* Stream Line Buffer full */
#define V4L2_EVENT_XLNXCSIRX_SLBF	(V4L2_EVENT_XLNXCSIRX_CLASS | 0x3)

#endif /* __UAPI_XILINX_V4L2_EVENTS_H__ */
