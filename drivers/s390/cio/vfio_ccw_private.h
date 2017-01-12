/*
 * Private stuff for vfio_ccw driver
 *
 * Copyright IBM Corp. 2017
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 * Author(s): Dong Jia Shi <bjsdjshi@linux.vnet.ibm.com>
 *            Xiao Feng Ren <renxiaof@linux.vnet.ibm.com>
 */

#ifndef _VFIO_CCW_PRIVATE_H_
#define _VFIO_CCW_PRIVATE_H_

#include <linux/completion.h>
#include <linux/eventfd.h>
#include <asm/vfio_ccw.h>

#include "css.h"
#include "vfio_ccw_cp.h"

/**
 * struct vfio_ccw_private
 * @sch: pointor to the subchannel
 * @completion: synchronization helper of the I/O completion
 * @mdev: pointor to the mediated device
 * @nb: notifier for vfio events
 * @io_region: MMIO region to input/output I/O arguments/results
 * @wait_q: wait for interrupt
 * @intparm: record current interrupt parameter, used for wait interrupt
 * @cp: ccw program for the current I/O operation
 * @irb: irb info received from interrupt
 * @scsw: scsw info
 * @io_trigger: eventfd ctx for signaling userspace I/O results
 */
struct vfio_ccw_private {
	struct subchannel	*sch;
	struct completion	*completion;
	struct mdev_device	*mdev;
	struct notifier_block	nb;
	struct ccw_io_region	io_region;

	wait_queue_head_t	wait_q;
	u32			intparm;
	struct ccwprogram	cp;
	struct irb		irb;
	union scsw		scsw;

	struct eventfd_ctx	*io_trigger;
} __aligned(8);

extern int vfio_ccw_mdev_reg(struct subchannel *sch);
extern void vfio_ccw_mdev_unreg(struct subchannel *sch);

extern int vfio_ccw_sch_quiesce(struct subchannel *sch);
extern int vfio_ccw_sch_cmd_request(struct vfio_ccw_private *private);

#endif
