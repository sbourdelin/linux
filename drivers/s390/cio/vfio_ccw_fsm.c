// SPDX-License-Identifier: GPL-2.0
/*
 * Finite state machine for vfio-ccw device handling
 *
 * Copyright IBM Corp. 2017
 *
 * Author(s): Dong Jia Shi <bjsdjshi@linux.vnet.ibm.com>
 */

#include <linux/vfio.h>
#include <linux/mdev.h>
#include <asm/isc.h>

#include "ioasm.h"
#include "vfio_ccw_private.h"

static int fsm_io_helper(struct vfio_ccw_private *private)
{
	struct subchannel *sch;
	union orb *orb;
	int ccode;
	__u8 lpm;
	int ret;

	sch = private->sch;

	orb = cp_get_orb(&private->cp, (u32)(addr_t)sch, sch->lpm);

	/* Issue "Start Subchannel" */
	ccode = ssch(sch->schid, orb);

	switch (ccode) {
	case 0:
		/*
		 * Initialize device status information
		 */
		sch->schib.scsw.cmd.actl |= SCSW_ACTL_START_PEND;
		ret = 0;
		break;
	case 1:		/* Status pending */
	case 2:		/* Busy */
		ret = -EBUSY;
		break;
	case 3:		/* Device/path not operational */
	{
		lpm = orb->cmd.lpm;
		if (lpm != 0)
			sch->lpm &= ~lpm;
		else
			sch->lpm = 0;

		if (cio_update_schib(sch))
			ret = -ENODEV;
		else
			ret = sch->lpm ? -EACCES : -ENODEV;
		break;
	}
	default:
		ret = ccode;
	}
	return ret;
}

static int fsm_notoper(struct vfio_ccw_private *private)
{
	struct subchannel *sch = private->sch;

	/*
	 * TODO:
	 * Probably we should send the machine check to the guest.
	 */
	css_sched_sch_todo(sch, SCH_TODO_UNREG);
	return VFIO_CCW_STATE_NOT_OPER;
}

static int fsm_online(struct vfio_ccw_private *private)
{
	struct subchannel *sch = private->sch;
	int ret = VFIO_CCW_STATE_IDLE;

	spin_lock_irq(sch->lock);
	if (cio_enable_subchannel(sch, (u32)(unsigned long)sch))
		ret = VFIO_CCW_STATE_NOT_OPER;
	spin_unlock_irq(sch->lock);

	return ret;
}
static int fsm_offline(struct vfio_ccw_private *private)
{
	struct subchannel *sch = private->sch;
	int ret = VFIO_CCW_STATE_STANDBY;

	spin_lock_irq(sch->lock);
	if (cio_disable_subchannel(sch))
		ret = VFIO_CCW_STATE_NOT_OPER;
	spin_unlock_irq(sch->lock);
	if (private->completion)
		complete(private->completion);

	return ret;
}
static int fsm_quiescing(struct vfio_ccw_private *private)
{
	struct subchannel *sch = private->sch;
	int ret = VFIO_CCW_STATE_STANDBY;
	int iretry = 255;

	spin_lock_irq(sch->lock);
	ret = cio_cancel_halt_clear(sch, &iretry);
	if (ret == -EBUSY)
		ret = VFIO_CCW_STATE_QUIESCING;
	else if (private->completion)
		complete(private->completion);
	spin_unlock_irq(sch->lock);
	return ret;
}
static int fsm_quiescing_done(struct vfio_ccw_private *private)
{
	if (private->completion)
		complete(private->completion);
	return VFIO_CCW_STATE_STANDBY;
}
/*
 * No operation action.
 */
static int fsm_nop(struct vfio_ccw_private *private)
{
	return private->state;
}

static int fsm_io_error(struct vfio_ccw_private *private)
{
	pr_err("vfio-ccw: FSM: I/O request from state:%d\n", private->state);
	private->io_region.ret_code = -EIO;
	return private->state;
}

static int fsm_io_busy(struct vfio_ccw_private *private)
{
	private->io_region.ret_code = -EBUSY;
	return private->state;
}

static int fsm_disabled_irq(struct vfio_ccw_private *private)
{
	struct subchannel *sch = private->sch;

	/*
	 * An interrupt in a disabled state means a previous disable was not
	 * successful - should not happen, but we try to disable again.
	 */
	cio_disable_subchannel(sch);
	return private->state;
}

/*
 * Deal with the ccw command request from the userspace.
 */
static int fsm_io_request(struct vfio_ccw_private *private)
{
	union orb *orb;
	struct ccw_io_region *io_region = &private->io_region;
	struct mdev_device *mdev = private->mdev;

	private->state = VFIO_CCW_STATE_BOXED;

	orb = (union orb *)io_region->orb_area;

	io_region->ret_code = cp_init(&private->cp, mdev_dev(mdev), orb);
	if (io_region->ret_code)
		goto err_out;

	io_region->ret_code = cp_prefetch(&private->cp);
	if (io_region->ret_code) {
		cp_free(&private->cp);
		goto err_out;
	}

	io_region->ret_code = fsm_io_helper(private);
	if (io_region->ret_code) {
		cp_free(&private->cp);
		goto err_out;
	}
	return VFIO_CCW_STATE_BUSY;

err_out:
	return VFIO_CCW_STATE_IDLE;
}

/*
 * Got an interrupt for a normal io (state busy).
 */
static int fsm_irq(struct vfio_ccw_private *private)
{
	struct irb *irb = &private->irb;

	if (scsw_is_solicited(&irb->scsw)) {
		cp_update_scsw(&private->cp, &irb->scsw);
		cp_free(&private->cp);
	}
	memcpy(private->io_region.irb_area, irb, sizeof(*irb));

	if (private->io_trigger)
		eventfd_signal(private->io_trigger, 1);
	return VFIO_CCW_STATE_IDLE;
}

/*
 * Got a sub-channel event .
 */
static int fsm_sch_event(struct vfio_ccw_private *private)
{
	unsigned long flags;
	int ret = private->state;
	struct subchannel *sch = private->sch;

	spin_lock_irqsave(sch->lock, flags);
	if (cio_update_schib(sch))
		ret = VFIO_CCW_STATE_NOT_OPER;
	spin_unlock_irqrestore(sch->lock, flags);

	return ret;
}

static int fsm_init(struct vfio_ccw_private *private)
{
	struct subchannel *sch = private->sch;

	sch->isc = VFIO_CCW_ISC;

	return VFIO_CCW_STATE_STANDBY;
}


/*
 * Device statemachine
 */
fsm_func_t *vfio_ccw_jumptable[NR_VFIO_CCW_STATES][NR_VFIO_CCW_EVENTS] = {
	[VFIO_CCW_STATE_NOT_OPER] = {
		[VFIO_CCW_EVENT_INIT]		= fsm_init,
		[VFIO_CCW_EVENT_ONLINE]		= fsm_nop,
		[VFIO_CCW_EVENT_OFFLINE]	= fsm_nop,
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_nop,
		[VFIO_CCW_EVENT_SSCH_REQ]	= fsm_nop,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_nop,
		[VFIO_CCW_EVENT_SCHIB_CHANGED]	= fsm_nop,
	},
	[VFIO_CCW_STATE_STANDBY] = {
		[VFIO_CCW_EVENT_INIT]		= fsm_nop,
		[VFIO_CCW_EVENT_ONLINE]		= fsm_online,
		[VFIO_CCW_EVENT_OFFLINE]	= fsm_offline,
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_notoper,
		[VFIO_CCW_EVENT_SSCH_REQ]	= fsm_io_error,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_disabled_irq,
		[VFIO_CCW_EVENT_SCHIB_CHANGED]	= fsm_sch_event,
	},
	[VFIO_CCW_STATE_IDLE] = {
		[VFIO_CCW_EVENT_INIT]		= fsm_nop,
		[VFIO_CCW_EVENT_ONLINE]		= fsm_nop,
		[VFIO_CCW_EVENT_OFFLINE]	= fsm_offline,
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_notoper,
		[VFIO_CCW_EVENT_SSCH_REQ]	= fsm_io_request,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_irq,
		[VFIO_CCW_EVENT_SCHIB_CHANGED]	= fsm_sch_event,
	},
	[VFIO_CCW_STATE_BOXED] = {
		[VFIO_CCW_EVENT_INIT]		= fsm_nop,
		[VFIO_CCW_EVENT_ONLINE]		= fsm_nop,
		[VFIO_CCW_EVENT_OFFLINE]	= fsm_quiescing,
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_notoper,
		[VFIO_CCW_EVENT_SSCH_REQ]	= fsm_io_busy,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_irq,
		[VFIO_CCW_EVENT_SCHIB_CHANGED]	= fsm_sch_event,
	},
	[VFIO_CCW_STATE_BUSY] = {
		[VFIO_CCW_EVENT_INIT]		= fsm_nop,
		[VFIO_CCW_EVENT_ONLINE]		= fsm_nop,
		[VFIO_CCW_EVENT_OFFLINE]	= fsm_quiescing,
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_notoper,
		[VFIO_CCW_EVENT_SSCH_REQ]	= fsm_io_busy,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_irq,
		[VFIO_CCW_EVENT_SCHIB_CHANGED]	= fsm_sch_event,
	},
	[VFIO_CCW_STATE_QUIESCING] = {
		[VFIO_CCW_EVENT_INIT]		= fsm_nop,
		[VFIO_CCW_EVENT_ONLINE]		= fsm_nop,
		[VFIO_CCW_EVENT_OFFLINE]	= fsm_nop,
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_notoper,
		[VFIO_CCW_EVENT_SSCH_REQ]	= fsm_io_busy,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_quiescing_done,
		[VFIO_CCW_EVENT_SCHIB_CHANGED]	= fsm_sch_event,
	},
};
