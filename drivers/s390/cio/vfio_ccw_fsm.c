// SPDX-License-Identifier: GPL-2.0
/*
 * Finite state machine for vfio-ccw device handling
 *
 * Copyright IBM Corp. 2017
 * Copyright Red Hat, Inc. 2018
 *
 * Author(s): Dong Jia Shi <bjsdjshi@linux.vnet.ibm.com>
 *            Cornelia Huck <cohuck@redhat.com>
 */

#include <linux/vfio.h>
#include <linux/mdev.h>

#include "ioasm.h"
#include "vfio_ccw_private.h"

#define CREATE_TRACE_POINTS
#include "vfio_ccw_trace.h"

static int fsm_io_helper(struct vfio_ccw_private *private)
{
	struct subchannel *sch;
	union orb *orb;
	int ccode;
	__u8 lpm;
	unsigned long flags;
	int ret;

	sch = private->sch;

	spin_lock_irqsave(sch->lock, flags);
	private->state = VFIO_CCW_STATE_BUSY;

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
	spin_unlock_irqrestore(sch->lock, flags);
	return ret;
}

static int fsm_do_halt(struct vfio_ccw_private *private)
{
	struct subchannel *sch;
	unsigned long flags;
	int ccode;
	int ret;

	sch = private->sch;

	spin_lock_irqsave(sch->lock, flags);
	private->state = VFIO_CCW_STATE_BUSY;

	/* Issue "Halt Subchannel" */
	ccode = hsch(sch->schid);

	switch (ccode) {
	case 0:
		/*
		 * Initialize device status information
		 */
		sch->schib.scsw.cmd.actl |= SCSW_ACTL_HALT_PEND;
		ret = 0;
		break;
	case 1:		/* Status pending */
	case 2:		/* Busy */
		ret = -EBUSY;
		break;
	case 3:		/* Device not operational */
	{
		ret = -ENODEV;
		break;
	}
	default:
		ret = ccode;
	}
	spin_unlock_irqrestore(sch->lock, flags);
	return ret;
}

static int fsm_do_clear(struct vfio_ccw_private *private)
{
	struct subchannel *sch;
	unsigned long flags;
	int ccode;
	int ret;

	sch = private->sch;

	spin_lock_irqsave(sch->lock, flags);
	private->state = VFIO_CCW_STATE_BUSY;

	/* Issue "Clear Subchannel" */
	ccode = csch(sch->schid);

	switch (ccode) {
	case 0:
		/*
		 * Initialize device status information
		 */
		sch->schib.scsw.cmd.actl = SCSW_ACTL_CLEAR_PEND;
		/* TODO: check what else we might need to clear */
		ret = 0;
		break;
	case 3:		/* Device not operational */
	{
		ret = -ENODEV;
		break;
	}
	default:
		ret = ccode;
	}
	spin_unlock_irqrestore(sch->lock, flags);
	return ret;
}

static void fsm_notoper(struct vfio_ccw_private *private,
			enum vfio_ccw_event event)
{
	struct subchannel *sch = private->sch;

	/*
	 * TODO:
	 * Probably we should send the machine check to the guest.
	 */
	css_sched_sch_todo(sch, SCH_TODO_UNREG);
	private->state = VFIO_CCW_STATE_NOT_OPER;
}

/*
 * No operation action.
 */
static void fsm_nop(struct vfio_ccw_private *private,
		    enum vfio_ccw_event event)
{
}

static void fsm_io_error(struct vfio_ccw_private *private,
			 enum vfio_ccw_event event)
{
	pr_err("vfio-ccw: FSM: I/O request from state:%d\n", private->state);
	private->io_region->ret_code = -EIO;
}

static void fsm_io_busy(struct vfio_ccw_private *private,
			enum vfio_ccw_event event)
{
	private->io_region->ret_code = -EBUSY;
}

static void fsm_async_error(struct vfio_ccw_private *private,
			    enum vfio_ccw_event event)
{
	pr_err("vfio-ccw: FSM: halt/clear request from state:%d\n",
	       private->state);
	private->cmd_region->ret_code = -EIO;
}

static void fsm_async_busy(struct vfio_ccw_private *private,
			   enum vfio_ccw_event event)
{
	private->cmd_region->ret_code = -EBUSY;
}

static void fsm_disabled_irq(struct vfio_ccw_private *private,
			     enum vfio_ccw_event event)
{
	struct subchannel *sch = private->sch;

	/*
	 * An interrupt in a disabled state means a previous disable was not
	 * successful - should not happen, but we try to disable again.
	 */
	cio_disable_subchannel(sch);
}
inline struct subchannel_id get_schid(struct vfio_ccw_private *p)
{
	return p->sch->schid;
}

/*
 * Deal with the ccw command request from the userspace.
 */
static void fsm_io_request(struct vfio_ccw_private *private,
			   enum vfio_ccw_event event)
{
	union orb *orb;
	union scsw *scsw = &private->scsw;
	struct ccw_io_region *io_region = private->io_region;
	struct mdev_device *mdev = private->mdev;
	char *errstr = "request";

	private->state = VFIO_CCW_STATE_BOXED;

	memcpy(scsw, io_region->scsw_area, sizeof(*scsw));

	if (scsw->cmd.fctl & SCSW_FCTL_START_FUNC) {
		orb = (union orb *)io_region->orb_area;

		/* Don't try to build a cp if transport mode is specified. */
		if (orb->tm.b) {
			io_region->ret_code = -EOPNOTSUPP;
			errstr = "transport mode";
			goto err_out;
		}
		io_region->ret_code = cp_init(&private->cp, mdev_dev(mdev),
					      orb);
		if (io_region->ret_code) {
			errstr = "cp init";
			goto err_out;
		}

		io_region->ret_code = cp_prefetch(&private->cp);
		if (io_region->ret_code) {
			errstr = "cp prefetch";
			cp_free(&private->cp);
			goto err_out;
		}

		/* Start channel program and wait for I/O interrupt. */
		io_region->ret_code = fsm_io_helper(private);
		if (io_region->ret_code) {
			errstr = "cp fsm_io_helper";
			cp_free(&private->cp);
			goto err_out;
		}
		return;
	} else if (scsw->cmd.fctl & SCSW_FCTL_HALT_FUNC) {
		/* halt is handled via the async cmd region */
		io_region->ret_code = -EOPNOTSUPP;
		goto err_out;
	} else if (scsw->cmd.fctl & SCSW_FCTL_CLEAR_FUNC) {
		/* clear is handled via the async cmd region */
		io_region->ret_code = -EOPNOTSUPP;
		goto err_out;
	}

err_out:
	private->state = VFIO_CCW_STATE_IDLE;
	trace_vfio_ccw_io_fctl(scsw->cmd.fctl, get_schid(private),
			       io_region->ret_code, errstr);
}

/*
 * Deal with a halt request from userspace.
 */
static void fsm_halt_request(struct vfio_ccw_private *private,
			     enum vfio_ccw_event event)
{
	struct ccw_cmd_region *cmd_region = private->cmd_region;
	int state = private->state;

	private->state = VFIO_CCW_STATE_BOXED;

	if (cmd_region->command != VFIO_CCW_ASYNC_CMD_HSCH) {
		/* should not happen? */
		cmd_region->ret_code = -EINVAL;
		goto err_out;
	}

	cmd_region->ret_code = fsm_do_halt(private);
	if (cmd_region->ret_code)
		goto err_out;

	return;

err_out:
	private->state = state;
}

/*
 * Deal with a clear request from userspace.
 */
static void fsm_clear_request(struct vfio_ccw_private *private,
			      enum vfio_ccw_event event)
{
	struct ccw_cmd_region *cmd_region = private->cmd_region;
	int state = private->state;

	private->state = VFIO_CCW_STATE_BOXED;

	if (cmd_region->command != VFIO_CCW_ASYNC_CMD_CSCH) {
		/* should not happen? */
		cmd_region->ret_code = -EINVAL;
		goto err_out;
	}

	cmd_region->ret_code = fsm_do_clear(private);
	if (cmd_region->ret_code)
		goto err_out;

	return;

err_out:
	private->state = state;
}
/*
 * Got an interrupt for a normal io (state busy).
 */
static void fsm_irq(struct vfio_ccw_private *private,
		    enum vfio_ccw_event event)
{
	struct irb *irb = this_cpu_ptr(&cio_irb);

	memcpy(&private->irb, irb, sizeof(*irb));

	queue_work(vfio_ccw_work_q, &private->io_work);

	if (private->completion)
		complete(private->completion);
}

/*
 * Device statemachine
 */
fsm_func_t *vfio_ccw_jumptable[NR_VFIO_CCW_STATES][NR_VFIO_CCW_EVENTS] = {
	[VFIO_CCW_STATE_NOT_OPER] = {
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_nop,
		[VFIO_CCW_EVENT_IO_REQ]		= fsm_io_error,
		[VFIO_CCW_EVENT_HALT_REQ]	= fsm_async_error,
		[VFIO_CCW_EVENT_CLEAR_REQ]	= fsm_async_error,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_disabled_irq,
	},
	[VFIO_CCW_STATE_STANDBY] = {
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_notoper,
		[VFIO_CCW_EVENT_IO_REQ]		= fsm_io_error,
		[VFIO_CCW_EVENT_HALT_REQ]	= fsm_async_error,
		[VFIO_CCW_EVENT_CLEAR_REQ]	= fsm_async_error,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_irq,
	},
	[VFIO_CCW_STATE_IDLE] = {
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_notoper,
		[VFIO_CCW_EVENT_IO_REQ]		= fsm_io_request,
		[VFIO_CCW_EVENT_HALT_REQ]	= fsm_halt_request,
		[VFIO_CCW_EVENT_CLEAR_REQ]	= fsm_clear_request,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_irq,
	},
	[VFIO_CCW_STATE_BOXED] = {
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_notoper,
		[VFIO_CCW_EVENT_IO_REQ]		= fsm_io_busy,
		[VFIO_CCW_EVENT_HALT_REQ]	= fsm_async_busy,
		[VFIO_CCW_EVENT_CLEAR_REQ]	= fsm_async_busy,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_irq,
	},
	[VFIO_CCW_STATE_BUSY] = {
		[VFIO_CCW_EVENT_NOT_OPER]	= fsm_notoper,
		[VFIO_CCW_EVENT_IO_REQ]		= fsm_io_busy,
		[VFIO_CCW_EVENT_HALT_REQ]	= fsm_halt_request,
		[VFIO_CCW_EVENT_CLEAR_REQ]	= fsm_clear_request,
		[VFIO_CCW_EVENT_INTERRUPT]	= fsm_irq,
	},
};
