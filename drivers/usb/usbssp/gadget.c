// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Some code borrowed from the Linux XHCI driver.
 */

#include <linux/pci.h>
#include <linux/irq.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/dmi.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>

#include "gadget-trace.h"
#include "gadget-debugfs.h"
#include "gadget.h"

u64 usbssp_get_hw_deq(struct usbssp_udc *usbssp_data,
		      struct usbssp_device *dev,
		      unsigned int ep_index, unsigned int stream_id);

void usbssp_bottom_irq(struct work_struct *work)
{
	struct usbssp_udc *usbssp_data = container_of(work, struct usbssp_udc,
						bottom_irq);

	usbssp_dbg(usbssp_data, "===== Bottom IRQ handler start ====\n");

	if (usbssp_data->usbssp_state & USBSSP_STATE_DYING) {
		usbssp_err(usbssp_data, "Device controller dying\n");
		return;
	}

	mutex_lock(&usbssp_data->mutex);
	spin_lock_irqsave(&usbssp_data->irq_thread_lock,
			usbssp_data->irq_thread_flag);

	if (usbssp_data->defered_event & EVENT_DEV_DISCONECTED) {
		usbssp_dbg(usbssp_data, "Disconnecting device sequence\n");
		usbssp_data->defered_event &= ~EVENT_DEV_DISCONECTED;
		usbssp_data->usbssp_state |= USBSSP_STATE_DISCONNECT_PENDING;
		usbssp_stop_device(usbssp_data, 0);

	//time needed for disconnect
	usbssp_gadget_disconnect_interrupt(usbssp_data);
	usbssp_data->gadget.speed = USB_SPEED_UNKNOWN;
	usb_gadget_set_state(&usbssp_data->gadget, USB_STATE_NOTATTACHED);

	usbssp_dbg(usbssp_data, "Wait for disconnect\n");

	spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,
				usbssp_data->irq_thread_flag);
	/*fixme: should be replaced by wait_for_completion*/
	msleep(200);
	spin_lock_irqsave(&usbssp_data->irq_thread_lock,
			usbssp_data->irq_thread_flag);
	}

	if (usbssp_data->defered_event & EVENT_DEV_CONNECTED) {
		usbssp_dbg(usbssp_data, "Connecting device sequence\n");
	if (usbssp_data->usbssp_state & USBSSP_STATE_DISCONNECT_PENDING) {
		usbssp_free_dev(usbssp_data);
		usbssp_data->usbssp_state &= ~USBSSP_STATE_DISCONNECT_PENDING;
	}

	usbssp_data->defered_event &= ~EVENT_DEV_CONNECTED;
		usbssp_alloc_dev(usbssp_data);
	}

	if (usbssp_data->defered_event & EVENT_USB_RESET) {
		__le32 __iomem *port_regs;
		u32 temp;

		usbssp_dbg(usbssp_data, "Beginning USB reset device sequence\n");

		/*Reset Device Command*/
		usbssp_data->defered_event &= ~EVENT_USB_RESET;
		usbssp_reset_device(usbssp_data);
		usbssp_data->devs.eps[0].ep_state |= USBSSP_EP_ENABLED;
		usbssp_data->defered_event &= ~EVENT_DEV_CONNECTED;

		usbssp_enable_device(usbssp_data);
		if ((usbssp_data->gadget.speed == USB_SPEED_SUPER) ||
		    (usbssp_data->gadget.speed == USB_SPEED_SUPER_PLUS)) {
			usbssp_dbg(usbssp_data, "Set U1/U2 enable\n");
			port_regs = usbssp_get_port_io_addr(usbssp_data);
			temp = readl(port_regs+PORTPMSC);
			temp &= ~(PORT_U1_TIMEOUT_MASK | PORT_U2_TIMEOUT_MASK);
			temp |= PORT_U1_TIMEOUT(1) | PORT_U2_TIMEOUT(1);
			writel(temp, port_regs+PORTPMSC);
		}
	}

	/*handle setup packet*/
	if (usbssp_data->defered_event & EVENT_SETUP_PACKET) {
		usbssp_dbg(usbssp_data, "Beginning handling SETUP packet\n");
		usbssp_data->defered_event &= ~EVENT_SETUP_PACKET;
		usbssp_setup_analyze(usbssp_data);
	}

	spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,
				usbssp_data->irq_thread_flag);
	mutex_unlock(&usbssp_data->mutex);
	usbssp_dbg(usbssp_data, "===== Bottom IRQ handler end ====\n");
}

/*
 * usbssp_handshake - spin reading dc until handshake completes or fails
 * @ptr: address of dc register to be read
 * @mask: bits to look at in result of read
 * @done: value of those bits when handshake succeeds
 * @usec: timeout in microseconds
 *
 * Returns negative errno, or zero on success
 *
 * Success happens when the "mask" bits have the specified value (hardware
 * handshake done). There are two failure modes: "usec" have passed (major
 * hardware flakeout), or the register reads as all-ones (hardware removed).
 */
int usbssp_handshake(void __iomem *ptr, u32 mask, u32 done, int usec)
{
	u32	result;

	do {
		result = readl(ptr);
		if (result == ~(u32)0)	/* card removed */
			return -ENODEV;
		result &= mask;
		if (result == done)
			return 0;
		udelay(1);
		usec--;
	} while (usec > 0);
	return -ETIMEDOUT;
}

/*
 * Disable interrupts and begin the DC halting process.
 */
void usbssp_quiesce(struct usbssp_udc *usbssp_data)
{
	u32 halted;
	u32 cmd;
	u32 mask;

	mask = ~(USBSSP_IRQS);

	halted = readl(&usbssp_data->op_regs->status) & STS_HALT;
	if (!halted)
		mask &= ~CMD_RUN;

	cmd = readl(&usbssp_data->op_regs->command);
	cmd &= mask;
	writel(cmd, &usbssp_data->op_regs->command);
}

/*
 * Force DC into halt state.
 *
 * Disable any IRQs and clear the run/stop bit.
 * USBSSP will complete any current and actively pipelined transactions, and
 * should halt within 16 ms of the run/stop bit being cleared.
 * Read DC Halted bit in the status register to see when the DC is finished.
 */
int usbssp_halt(struct usbssp_udc *usbssp_data)
{
	int ret;

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			 "// Halt the USBSSP");
	usbssp_quiesce(usbssp_data);

	ret = usbssp_handshake(&usbssp_data->op_regs->status,
		STS_HALT, STS_HALT, USBSSP_MAX_HALT_USEC);

	if (!ret) {
		usbssp_warn(usbssp_data, "Device halt failed, %d\n", ret);
		return ret;
	}

	usbssp_data->usbssp_state |= USBSSP_STATE_HALTED;
	usbssp_data->cmd_ring_state = CMD_RING_STATE_STOPPED;
	return ret;
}

/*
 * Set the run bit and wait for the device to be running.
 */
int usbssp_start(struct usbssp_udc *usbssp_data)
{
	u32 temp;
	int ret;

	temp = readl(&usbssp_data->op_regs->command);
	temp |= (CMD_RUN | CMD_DEVEN);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// Turn on USBSSP, cmd = 0x%x.", temp);
	writel(temp, &usbssp_data->op_regs->command);

	/*
	 * Wait for the HCHalted Staus bit to be 0 to indicate the device is
	 * running.
	 */
	ret = usbssp_handshake(&usbssp_data->op_regs->status,
			STS_HALT, 0, USBSSP_MAX_HALT_USEC);

	if (ret == -ETIMEDOUT)
		usbssp_err(usbssp_data, "Device took too long to start, waited %u microseconds.\n",
			USBSSP_MAX_HALT_USEC);
	if (!ret)
		/* clear state flags. Including dying, halted or removing */
		usbssp_data->usbssp_state = 0;

	return ret;
}

/*
 * Reset a halted DC.
 *
 * This resets pipelines, timers, counters, state machines, etc.
 * Transactions will be terminated immediately, and operational registers
 * will be set to their defaults.
 */
int usbssp_reset(struct usbssp_udc *usbssp_data)
{
	u32 command;
	u32 state;
	int ret;

	state = readl(&usbssp_data->op_regs->status);

	if (state == ~(u32)0) {
		usbssp_warn(usbssp_data, "Device not accessible, reset failed.\n");
		return -ENODEV;
	}

	if ((state & STS_HALT) == 0) {
		usbssp_warn(usbssp_data, "DC not halted, aborting reset.\n");
		return 0;
	}

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init, "// Reset the DC");
	command = readl(&usbssp_data->op_regs->command);
	command |= CMD_RESET;
	writel(command, &usbssp_data->op_regs->command);

	ret = usbssp_handshake(&usbssp_data->op_regs->command,
			CMD_RESET, 0, 10 * 1000 * 1000);

	if (ret)
		return ret;
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
		"Wait for controller to be ready for doorbell rings");
	/*
	 * USBSSP cannot write to any doorbells or operational registers other
	 * than status until the "Controller Not Ready" flag is cleared.
	 */
	ret = usbssp_handshake(&usbssp_data->op_regs->status,
			STS_CNR, 0, 10 * 1000 * 1000);

	return ret;
}

static inline int usbssp_try_enable_msi(struct usbssp_udc *usbssp_data)
{
	usbssp_data->msi_enabled = 1;
	return 0;
}

static inline void usbssp_cleanup_msix(struct usbssp_udc *usbssp_data)
{
	/*TODO*/
}

static inline void usbssp_msix_sync_irqs(struct usbssp_udc *usbssp_data)
{
	/*TODO*/
}

/*
 * Initialize memory for gadget driver and USBSSP (one-time init).
 *
 * Program the PAGESIZE register, initialize the device context array, create
 * device contexts, set up a command ring segment (or two?), create event
 * ring (one for now).
 */
int usbssp_init(struct usbssp_udc *usbssp_data)
{
	int retval = 0;

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init, "usbssp_init");

	spin_lock_init(&usbssp_data->lock);
	spin_lock_init(&usbssp_data->irq_thread_lock);
	retval = usbssp_mem_init(usbssp_data, GFP_KERNEL);

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Finished usbssp_init");
	return retval;
}

/*-------------------------------------------------------------------------*/
static int usbssp_run_finished(struct usbssp_udc *usbssp_data)
{
	if (usbssp_start(usbssp_data)) {
		usbssp_halt(usbssp_data);
		return -ENODEV;
	}
//	usbssp_data->usbssp_state = USBSSP_STATE_RUNNING;
	usbssp_data->cmd_ring_state = CMD_RING_STATE_RUNNING;

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Finished usbssp_run for USB3 device");
	return 0;
}

/*
 * Start the USBSSP after it was halted.
 *
 * This function is called by the usbssp_gadget_start function when the
 * gadget driver is started. Its opposite is usbssp_stop().
 *
 * usbssp_init() must be called once before this function can be called.
 * Reset the USBSSP, enable device slot contexts, program DCBAAP, and
 * set command ring pointer and event ring pointer.
 */
int usbssp_run(struct usbssp_udc *usbssp_data)
{
	u32 temp;
	u64 temp_64;
	int ret;
	__le32 __iomem	*portsc;
	u32 portsc_val = 0;
	int i = 0;

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init, "usbssp_run");

	ret = usbssp_try_enable_msi(usbssp_data);
	if (ret)
		return ret;

	temp_64 = usbssp_read_64(usbssp_data,
				 &usbssp_data->ir_set->erst_dequeue);
	temp_64 &= ~ERST_PTR_MASK;
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"ERST deq = 64'h%0lx",
			(unsigned long int) temp_64);

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
		"// Set the interrupt modulation register");
	temp = readl(&usbssp_data->ir_set->irq_control);
	temp &= ~ER_IRQ_INTERVAL_MASK;
	temp |= (usbssp_data->imod_interval / 250) & ER_IRQ_INTERVAL_MASK;
	writel(temp, &usbssp_data->ir_set->irq_control);

	/******************************************/
	//enable USB2 port
	for (i = 0; i < usbssp_data->num_usb2_ports; i++) {
		portsc = usbssp_data->usb2_ports + PORTSC;
		portsc_val = readl(portsc) & ~PORT_PLS_MASK;
		portsc_val = portsc_val | (5 << 5) | PORT_LINK_STROBE;
		writel(portsc_val, portsc);
	}

	//enable USB3.0 port
	for (i = 0; i < usbssp_data->num_usb3_ports; i++) {
		portsc = usbssp_data->usb3_ports + PORTSC;
		portsc_val = readl(portsc) & ~PORT_PLS_MASK;
		portsc_val = portsc_val | (5 << 5) | PORT_LINK_STROBE;
		writel(portsc_val, portsc);
	}

	if (usbssp_start(usbssp_data)) {
		usbssp_halt(usbssp_data);
		return -ENODEV;
	}

	/* Set the USBSSP state before we enable the irqs */
	temp = readl(&usbssp_data->op_regs->command);
	temp |= (CMD_EIE);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"// Enable interrupts, cmd = 0x%x.", temp);
	writel(temp, &usbssp_data->op_regs->command);

	temp = readl(&usbssp_data->ir_set->irq_pending);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
		"// Enabling event ring interrupter %p by writing 0x%x to irq_pending",
		usbssp_data->ir_set, (unsigned int) ER_IRQ_ENABLE(temp));
	writel(ER_IRQ_ENABLE(temp), &usbssp_data->ir_set->irq_pending);

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"Finished usbssp_run for USBSSP controller");

	usbssp_data->cmd_ring_state = CMD_RING_STATE_RUNNING;

//	usbssp_debugfs_init(usbssp_data);

	return 0;
}

/*
 * Stop USBSSP controller.
 *
 * This function is called by the gadget core when the USBSSP driver is removed.
 * Its opposite is usbssp_run().
 *
 * Disable device contexts, disable IRQs, and quiesce the DC.
 * Reset the DC, finish any completed transactions, and cleanup memory.
 */
void usbssp_stop(struct usbssp_udc *usbssp_data)
{
	u32 temp;

	spin_lock_irq(&usbssp_data->lock);
	usbssp_data->usbssp_state |= USBSSP_STATE_HALTED;
	usbssp_data->cmd_ring_state = CMD_RING_STATE_STOPPED;
	usbssp_halt(usbssp_data);
	usbssp_reset(usbssp_data);
	spin_unlock_irq(&usbssp_data->lock);
	usbssp_cleanup_msix(usbssp_data);

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			 "// Disabling event ring interrupts");
	temp = readl(&usbssp_data->op_regs->status);
	writel((temp & ~0x1fff) | STS_EINT, &usbssp_data->op_regs->status);
	temp = readl(&usbssp_data->ir_set->irq_pending);
	writel(ER_IRQ_DISABLE(temp), &usbssp_data->ir_set->irq_pending);
	usbssp_print_ir_set(usbssp_data, 0);

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"cleaning up memory");
	usbssp_mem_cleanup(usbssp_data);
//	usbssp_debugfs_exit(usbssp_data);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_init,
			"usbssp_stop completed - status = %x",
			readl(&usbssp_data->op_regs->status));
}

#ifdef CONFIG_PM
/*
 * Stop DC (not bus-specific)
 *
 * This is called when the machine transition into S3/S4 mode.
 *
 */
int usbssp_suspend(struct usbssp_udc *usbssp_data, bool do_wakeup)
{
	/*TODO*/
	return -ENOSYS;
}

/*
 * start DC (not bus-specific)
 *
 * This is called when the machine transition from S3/S4 mode.
 *
 */
int usbssp_resume(struct usbssp_udc *usbssp_data, bool hibernated)
{
	/*TODO*/
	return -ENOSYS;
}

#endif	/* CONFIG_PM */

/**
 * usbssp_get_endpoint_index - Find the index for an endpoint given its
 * descriptor.Use the return value to right shift 1 for the bitmask.
 *
 * Index = (epnum * 2) + direction - 1,
 * where direction = 0 for OUT, 1 for IN.
 * For control endpoints, the IN index is used (OUT index is unused), so
 * index = (epnum * 2) + direction - 1 = (epnum * 2) + 1 - 1 = (epnum * 2)
 */
unsigned int usbssp_get_endpoint_index(
		const struct usb_endpoint_descriptor *desc)
{
	unsigned int index;

	if (usb_endpoint_xfer_control(desc)) {
		index = (unsigned int) (usb_endpoint_num(desc)*2);
	} else {
		index = (unsigned int) (usb_endpoint_num(desc)*2) +
			(usb_endpoint_dir_in(desc) ? 1 : 0) - 1;
	}
	return index;
}

/* The reverse operation to usbssp_get_endpoint_index.
 * Calculate the USB endpoint address from the USBSSP endpoint index.
 */
unsigned int usbssp_get_endpoint_address(unsigned int ep_index)
{
	unsigned int number = DIV_ROUND_UP(ep_index, 2);
	unsigned int direction = ep_index % 2 ? USB_DIR_OUT : USB_DIR_IN;

	return direction | number;
}

/* Find the flag for this endpoint (for use in the control context). Use the
 * endpoint index to create a bitmask. The slot context is bit 0, endpoint 0 is
 * bit 1, etc.
 */
unsigned int usbssp_get_endpoint_flag(
			const struct usb_endpoint_descriptor *desc)
{
	return 1 << (usbssp_get_endpoint_index(desc) + 1);
}

/* Find the flag for this endpoint (for use in the control context). Use the
 * endpoint index to create a bitmask. The slot context is bit 0, endpoint 0 is
 * bit 1, etc.
 */
unsigned int usbssp_get_endpoint_flag_from_index(unsigned int ep_index)
{
	return 1 << (ep_index + 1);
}

/* Compute the last valid endpoint context index. Basically, this is the
 * endpoint index plus one. For slot contexts with more than valid endpoint,
 * we find the most significant bit set in the added contexts flags.
 * e.g. ep 1 IN (with epnum 0x81) => added_ctxs = 0b1000
 * fls(0b1000) = 4, but the endpoint context index is 3, so subtract one.
 */
unsigned int usbssp_last_valid_endpoint(u32 added_ctxs)
{
	return fls(added_ctxs) - 1;
}

/* Returns 1 if the arguments are OK;
 * returns -EINVAL for NULL pointers.
 */

static int usbssp_check_args(struct usbssp_udc *usbssp_data,
			     struct usbssp_ep *ep, int check_ep,
			     bool check_dev_priv, const char *func)
{
	struct usbssp_device	*dev_priv;

	if (!usbssp_data || (check_ep && !ep)) {
		pr_debug("USBSSP %s called with invalid args\n", func);
		return -EINVAL;
	}

	if (check_dev_priv)
		dev_priv = &usbssp_data->devs;

	if (usbssp_data->usbssp_state & USBSSP_STATE_HALTED)
		return -ENODEV;

	return 1;
}

static int usbssp_configure_endpoint(struct usbssp_udc *usbssp_data,
				     struct usb_gadget *g,
				     struct usbssp_command *command,
				     bool ctx_change,
				     bool must_succeed);

int usbssp_enqueue(struct usbssp_ep *dep, struct usbssp_request *req_priv)
{
	int ret = 0;
	unsigned int ep_index;
	unsigned int ep_state;
	const struct usb_endpoint_descriptor *desc;
	struct usbssp_udc *usbssp_data = dep->usbssp_data;
	int num_tds;

	if (usbssp_check_args(usbssp_data, dep, true, true, __func__) <= 0)
		return -EINVAL;

	if (!dep->endpoint.desc) {
		usbssp_err(usbssp_data, "%s: can't queue to disabled endpoint\n",
			dep->name);
		return -ESHUTDOWN;
	}

	if (WARN(req_priv->dep != dep, "request %p belongs to '%s'\n",
	    &req_priv->request, req_priv->dep->name)) {
		usbssp_err(usbssp_data, "%s: reequest %p belongs to '%s'\n",
			dep->name, &req_priv->request, req_priv->dep->name);
		return -EINVAL;
	}

	if (!list_empty(&dep->pending_list) && req_priv->epnum == 0) {
		usbssp_warn(usbssp_data,
			"Ep0 has incomplete previous transfer'\n");
		return -EBUSY;
	}

	//pm_runtime_get(usbssp_data->dev);
	req_priv->request.actual = 0;
	req_priv->request.status = -EINPROGRESS;
	req_priv->direction = dep->direction;
	req_priv->epnum = dep->number;

	desc = req_priv->dep->endpoint.desc;
	ep_index = usbssp_get_endpoint_index(desc);
	ep_state = usbssp_data->devs.eps[ep_index].ep_state;
	req_priv->sg = req_priv->request.sg;

	req_priv->num_pending_sgs = req_priv->request.num_mapped_sgs;
	usbssp_info(usbssp_data, "SG list addr: %p with %d elements.\n",
			req_priv->sg, req_priv->num_pending_sgs);

	list_add_tail(&req_priv->list, &dep->pending_list);

	if (req_priv->num_pending_sgs > 0)
		num_tds = req_priv->num_pending_sgs;
	else
		num_tds = 1;

	if (req_priv->request.zero && req_priv->request.length &&
	   (req_priv->request.length & (dep->endpoint.maxpacket == 0))) {
		num_tds++;
	}

	ret = usb_gadget_map_request_by_dev(usbssp_data->dev,
				&req_priv->request,
				dep->direction);

	if (ret) {
		usbssp_err(usbssp_data, "Can't map request to DMA\n");
		goto req_del;
	}

	/*allocating memory for transfer descriptors*/
	req_priv->td = kzalloc(num_tds * sizeof(struct usbssp_td), GFP_ATOMIC);

	if (!req_priv->td) {
		ret = -ENOMEM;
		goto free_priv;
	}

	if (ep_state & (EP_GETTING_STREAMS | EP_GETTING_NO_STREAMS)) {
		usbssp_warn(usbssp_data, "WARN: Can't enqueue USB Request, "
				"ep in streams transition state %x\n",
				ep_state);
		ret = -EINVAL;
		goto free_priv;
	}

	req_priv->num_tds = num_tds;
	req_priv->num_tds_done = 0;
	trace_usbssp_request_enqueue(&req_priv->request);

	switch (usb_endpoint_type(desc)) {
	case USB_ENDPOINT_XFER_CONTROL:
		ret = usbssp_queue_ctrl_tx(usbssp_data, GFP_ATOMIC, req_priv,
				ep_index);
		break;
	case USB_ENDPOINT_XFER_BULK:
		ret = usbssp_queue_bulk_tx(usbssp_data, GFP_ATOMIC, req_priv,
				ep_index);
		break;
	case USB_ENDPOINT_XFER_INT:
		ret = usbssp_queue_intr_tx(usbssp_data, GFP_ATOMIC, req_priv,
				ep_index);
		break;
	case USB_ENDPOINT_XFER_ISOC:
		ret = usbssp_queue_isoc_tx_prepare(usbssp_data, GFP_ATOMIC,
				req_priv, ep_index);
	}

	if (ret < 0) {
free_priv:
		usb_gadget_unmap_request_by_dev(usbssp_data->dev,
				&req_priv->request, dep->direction);
		usbssp_request_free_priv(req_priv);

req_del:
		list_del(&req_priv->list);
	}
	return ret;
}

/*
 * Remove the request's TD from the endpoint ring. This may cause the DC to stop
 * USB transfers, potentially stopping in the middle of a TRB buffer. The DC
 * should pick up where it left off in the TD, unless a Set Transfer Ring
 * Dequeue Pointer is issued.
 *
 * The TRBs that make up the buffers for the canceled request will be "removed"
 * from the ring. Since the ring is a contiguous structure, they can't be
 * physically removed. Instead, there are two options:
 *
 *  1) If the DC is in the middle of processing the request to be canceled, we
 *     simply move the ring's dequeue pointer past those TRBs using the Set
 *    Transfer Ring Dequeue Pointer command. This will be the common case,
 *     when drivers timeout on the last submitted request and attempt to cancel.
 *
 *  2) If the DC is in the middle of a different TD, we turn the TRBs into a
 *     series of 1-TRB transfer no-op TDs. (No-ops shouldn't be chained.) The
 *     DC will need to invalidate the any TRBs it has cached after the stop
 *     endpoint command.
 *
 *  3) The TD may have completed by the time the Stop Endpoint Command
 *     completes, so software needs to handle that case too.
 *
 * This function should protect against the TD enqueueing code ringing the
 * doorbell while this code is waiting for a Stop Endpoint command to complete.
 *
 */
int usbssp_dequeue(struct usbssp_ep *ep_priv, struct usbssp_request *req_priv)
{
	int ret = 0, i;
	struct usbssp_udc *usbssp_data;
	unsigned int ep_index;
	struct usbssp_ring *ep_ring;
	struct usbssp_device *priv_dev;
	struct usbssp_ep_ctx *ep_ctx;

	usbssp_data = ep_priv->usbssp_data;
	trace_usbssp_request_dequeue(&req_priv->request);

	priv_dev = &usbssp_data->devs;
	ep_index = usbssp_get_endpoint_index(req_priv->dep->endpoint.desc);
	ep_priv = &usbssp_data->devs.eps[ep_index];
	ep_ring = usbssp_request_to_transfer_ring(usbssp_data, req_priv);

	if (!ep_ring)
		goto err_giveback;

	i = req_priv->num_tds_done;

	if (i < req_priv->num_tds)
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_cancel_request,
			"Cancel request %p, dev %s, ep 0x%x, "
			"starting at offset 0x%llx",
			&req_priv->request, usbssp_data->gadget.name,
			req_priv->dep->endpoint.desc->bEndpointAddress,
			(unsigned long long) usbssp_trb_virt_to_dma(
				req_priv->td[i].start_seg,
				req_priv->td[i].first_trb));

	/* Queue a stop endpoint command, but only if it is
	 * in EP_STATE_RUNNING state.
	 */
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, priv_dev->out_ctx, ep_index);
	if (GET_EP_CTX_STATE(ep_ctx) == EP_STATE_RUNNING) {
		ret = usbssp_cmd_stop_ep(usbssp_data, &usbssp_data->gadget,
				ep_priv);
		if (ret)
			return ret;
	}

	usbssp_remove_request(usbssp_data, req_priv, ep_index);
	return ret;

err_giveback:
	usbssp_giveback_request_in_irq(usbssp_data, req_priv->td, -ESHUTDOWN);
	return ret;
}

/* Drop an endpoint from a new bandwidth configuration for this device.
 * Only one call to this function is allowed per endpoint before
 * check_bandwidth() or reset_bandwidth() must be called.
 * A call to usbssp_drop_endpoint() followed by a call to usbssp_add_endpoint()
 * will add the endpoint to the schedule with possibly new parameters
 * denoted by a different endpoint descriptor in usbssp_ep.
 * A call to usbssp_add_endpoint() followed by a call to
 * usbsssp_drop_endpoint() is not allowed.
 */
int usbssp_drop_endpoint(struct usbssp_udc *usbssp_data, struct usb_gadget *g,
		struct usbssp_ep *dep)
{
	struct usbssp_container_ctx *in_ctx, *out_ctx;
	struct usbssp_input_control_ctx *ctrl_ctx;
	unsigned int ep_index;
	struct usbssp_ep_ctx *ep_ctx;
	u32 drop_flag;
	u32 new_add_flags, new_drop_flags;
	int ret;

	ret = usbssp_check_args(usbssp_data, dep, 1, true, __func__);
	if (ret <= 0)
		return ret;

	if (usbssp_data->usbssp_state & USBSSP_STATE_DYING)
		return -ENODEV;

	drop_flag = usbssp_get_endpoint_flag(dep->endpoint.desc);
	if (drop_flag == SLOT_FLAG || drop_flag == EP0_FLAG) {
		usbssp_dbg(usbssp_data, "USBSSP %s - can't drop slot or ep 0 %#x\n",
				__func__, drop_flag);
		return 0;
	}

	in_ctx = usbssp_data->devs.in_ctx;
	out_ctx = usbssp_data->devs.out_ctx;
	ctrl_ctx = usbssp_get_input_control_ctx(in_ctx);
	if (!ctrl_ctx) {
		usbssp_warn(usbssp_data, "%s: Could not get input context, bad type.\n",
				__func__);
		return 0;
	}

	ep_index = usbssp_get_endpoint_index(dep->endpoint.desc);
	ep_ctx = usbssp_get_ep_ctx(usbssp_data, out_ctx, ep_index);
	/* If the controller already knows the endpoint is disabled,
	 * or the USBSSP driver has noted it is disabled, ignore this request
	 */
	if ((GET_EP_CTX_STATE(ep_ctx) == EP_STATE_DISABLED) ||
	    le32_to_cpu(ctrl_ctx->drop_flags) &
	    usbssp_get_endpoint_flag(dep->endpoint.desc)) {
		/* Do not warn when called after a usb_device_reset */
		if (usbssp_data->devs.eps[ep_index].ring != NULL)
			usbssp_warn(usbssp_data, "USBSSP %s called with disabled ep %p\n",
					__func__, dep);
		return 0;
	}

	ctrl_ctx->drop_flags |= cpu_to_le32(drop_flag);
	new_drop_flags = le32_to_cpu(ctrl_ctx->drop_flags);

	ctrl_ctx->add_flags &= cpu_to_le32(~drop_flag);
	new_add_flags = le32_to_cpu(ctrl_ctx->add_flags);

//	usbssp_debugfs_remove_endpoint(usbssp_data, &usbssp_data->devs,
//				       ep_index);

	usbssp_endpoint_zero(usbssp_data, &usbssp_data->devs, dep);

	usbssp_dbg(usbssp_data, "drop ep 0x%x, new drop flags = %#x, new add flags = %#x\n",
			(unsigned int) dep->endpoint.desc->bEndpointAddress,
			(unsigned int) new_drop_flags,
			(unsigned int) new_add_flags);
	return 0;
}

/* Add an endpoint to a new possible bandwidth configuration for this device.
 * Only one call to this function is allowed per endpoint before
 * check_bandwidth() or reset_bandwidth() must be called.
 * A call to usbssp_drop_endpoint() followed by a call to
 * usbssp_add_endpoint() will add the endpoint to the schedule with possibly
 * new parameters denoted by different endpoint descriptor in usbssp_ep.
 * A call to usbssp_add_endpoint() followed by a call to usbssp_drop_endpoint()
 * is not allowed.
 *
 */
int usbssp_add_endpoint(struct usbssp_udc *usbssp_data, struct usbssp_ep *dep)
{
	const struct usb_endpoint_descriptor *desc = dep->endpoint.desc;
	struct usbssp_container_ctx *in_ctx;
	unsigned int ep_index;
	struct usbssp_input_control_ctx *ctrl_ctx;
	u32 added_ctxs;
	u32 new_add_flags, new_drop_flags;
	struct usbssp_device *dev_priv;
	int ret = 0;

	ret = usbssp_check_args(usbssp_data, dep, 1, true, __func__);
	if (ret <= 0)
		return ret;

	if (usbssp_data->usbssp_state & USBSSP_STATE_DYING)
		return -ENODEV;

	added_ctxs = usbssp_get_endpoint_flag(desc);
	if (added_ctxs == SLOT_FLAG || added_ctxs == EP0_FLAG) {
		usbssp_dbg(usbssp_data, "USBSSP %s - can't add slot or ep 0 %#x\n",
				__func__, added_ctxs);
		return 0;
	}

	dev_priv = &usbssp_data->devs;
	in_ctx = dev_priv->in_ctx;
	ctrl_ctx = usbssp_get_input_control_ctx(in_ctx);
	if (!ctrl_ctx) {
		usbssp_warn(usbssp_data, "%s: Could not get input context, bad type.\n",
				__func__);
		return 0;
	}

	ep_index = usbssp_get_endpoint_index(desc);
	/* If this endpoint is already in use, and the upper layers are trying
	 * to add it again without dropping it, reject the addition.
	 */
	if (dev_priv->eps[ep_index].ring &&
	    !(le32_to_cpu(ctrl_ctx->drop_flags) & added_ctxs)) {
		usbssp_warn(usbssp_data,
			"Trying to add endpoint 0x%x without dropping it.\n",
			(unsigned int) desc->bEndpointAddress);
		return -EINVAL;
	}

	/* If already noted the endpoint is enabled,
	 * ignore this request.
	 */
	if (le32_to_cpu(ctrl_ctx->add_flags) & added_ctxs) {
		usbssp_warn(usbssp_data, "USBSSP %s called with enabled ep %p\n",
				__func__, dep);
		return 0;
	}

	if (usbssp_endpoint_init(usbssp_data, dev_priv, dep, GFP_ATOMIC) < 0) {
		usbssp_dbg(usbssp_data, "%s - could not initialize ep %#x\n",
				__func__, desc->bEndpointAddress);
		return -ENOMEM;
	}

	ctrl_ctx->add_flags |= cpu_to_le32(added_ctxs);
	new_add_flags = le32_to_cpu(ctrl_ctx->add_flags);
	new_drop_flags = le32_to_cpu(ctrl_ctx->drop_flags);

//	usbssp_debugfs_create_endpoint(usbssp_data,
//		&usbssp_data->devs, ep_index);
	usbssp_dbg(usbssp_data,
		"add ep 0x%x, new drop flags = %#x, new add flags = %#x\n",
		(unsigned int) desc->bEndpointAddress,
		(unsigned int) new_drop_flags,
		(unsigned int) new_add_flags);
	return 0;
}

static void usbssp_zero_in_ctx(struct usbssp_udc *usbssp_data,
			       struct usbssp_device *dev_priv)
{
	struct usbssp_input_control_ctx *ctrl_ctx;
	struct usbssp_ep_ctx *ep_ctx;
	struct usbssp_slot_ctx *slot_ctx;
	int i;

	ctrl_ctx = usbssp_get_input_control_ctx(dev_priv->in_ctx);
	if (!ctrl_ctx) {
		usbssp_warn(usbssp_data,
			"%s: Could not get input context, bad type.\n",
			__func__);
		return;
	}

	/* When a device's add flag and drop flag are zero, any subsequent
	 * configure endpoint command will leave that endpoint's state
	 * untouched. Make sure we don't leave any old state in the input
	 * endpoint contexts.
	 */
	ctrl_ctx->drop_flags = 0;
	ctrl_ctx->add_flags = 0;
	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev_priv->in_ctx);
	slot_ctx->dev_info &= cpu_to_le32(~LAST_CTX_MASK);
	/* Endpoint 0 is always valid */
	slot_ctx->dev_info |= cpu_to_le32(LAST_CTX(1));
	for (i = 1; i < 31; ++i) {
		ep_ctx = usbssp_get_ep_ctx(usbssp_data, dev_priv->in_ctx, i);
		ep_ctx->ep_info = 0;
		ep_ctx->ep_info2 = 0;
		ep_ctx->deq = 0;
		ep_ctx->tx_info = 0;
	}
}

static int usbssp_configure_endpoint_result(struct usbssp_udc *usbssp_data,
					    struct usb_gadget *g,
					    u32 *cmd_status)
{
	int ret;

	switch (*cmd_status) {
	case COMP_COMMAND_ABORTED:
	case COMP_COMMAND_RING_STOPPED:
		usbssp_warn(usbssp_data,
			"Timeout while waiting for configure endpoint command\n");
		ret = -ETIME;
		break;
	case COMP_RESOURCE_ERROR:
		dev_warn(&g->dev,
			"Not enough device controller resources for new device state.\n");
		ret = -ENOMEM;
		break;
	case COMP_TRB_ERROR:
		/* the gadget driver set up something wrong */
		dev_warn(&g->dev, "ERROR: Endpoint drop flag = 0, "
			"add flag = 1, and endpoint is not disabled.\n");
		ret = -EINVAL;
		break;
	case COMP_INCOMPATIBLE_DEVICE_ERROR:
		dev_warn(&g->dev,
			"ERROR: Incompatible device for endpoint configure command.\n");
		ret = -ENODEV;
		break;
	case COMP_SUCCESS:
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_context_change,
			"Successful Endpoint Configure command");
		ret = 0;
		break;
	default:
		usbssp_err(usbssp_data,
			"ERROR: unexpected command completion code 0x%x.\n",
			*cmd_status);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int usbssp_evaluate_context_result(struct usbssp_udc *usbssp_data,
		struct usb_gadget *g, u32 *cmd_status)
{
	int ret;

	switch (*cmd_status) {
	case COMP_COMMAND_ABORTED:
	case COMP_COMMAND_RING_STOPPED:
		usbssp_warn(usbssp_data,
			"Timeout while waiting for evaluate context command\n");
		ret = -ETIME;
		break;
	case COMP_PARAMETER_ERROR:
		dev_warn(&g->dev,
			"WARN: USBSSP driver setup invalid evaluate context command.\n");
		ret = -EINVAL;
		break;
	case COMP_SLOT_NOT_ENABLED_ERROR:
		dev_warn(&g->dev,
			"WARN: slot not enabled for evaluate context command.\n");
		ret = -EINVAL;
		break;
	case COMP_CONTEXT_STATE_ERROR:
		dev_warn(&g->dev,
			"WARN: invalid context state for evaluate context command.\n");
		ret = -EINVAL;
		break;
	case COMP_INCOMPATIBLE_DEVICE_ERROR:
		dev_warn(&g->dev,
			"ERROR: Incompatible device for evaluate context command.\n");
		ret = -ENODEV;
		break;
	case COMP_MAX_EXIT_LATENCY_TOO_LARGE_ERROR:
		/* Max Exit Latency too large error */
		dev_warn(&g->dev, "WARN: Max Exit Latency too large\n");
		ret = -EINVAL;
		break;
	case COMP_SUCCESS:
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_context_change,
			"Successful evaluate context command");
		ret = 0;
		break;
	default:
		usbssp_err(usbssp_data,
			"ERROR: unexpected command completion code 0x%x.\n",
			*cmd_status);
		ret = -EINVAL;
		break;
	}
	return ret;
}

/* Issue a configure endpoint command or evaluate context command
 * and wait for it to finish.
 */
static int usbssp_configure_endpoint(struct usbssp_udc *usbssp_data,
				     struct usb_gadget *g,
				     struct usbssp_command *command,
				     bool ctx_change, bool must_succeed)
{
	int ret;
	struct usbssp_input_control_ctx *ctrl_ctx;
	struct usbssp_device *dev_priv;
	struct usbssp_slot_ctx *slot_ctx;

	if (!command)
		return -EINVAL;

	if (usbssp_data->usbssp_state & USBSSP_STATE_DYING)
		return -ESHUTDOWN;

	dev_priv = &usbssp_data->devs;
	ctrl_ctx = usbssp_get_input_control_ctx(command->in_ctx);
	if (!ctrl_ctx) {
		usbssp_warn(usbssp_data,
			"%s: Could not get input context, bad type.\n",
			__func__);
		return -ENOMEM;
	}

	slot_ctx = usbssp_get_slot_ctx(usbssp_data, command->in_ctx);
	trace_usbssp_configure_endpoint(slot_ctx);

	if (!ctx_change) {
		ret = usbssp_queue_configure_endpoint(usbssp_data, command,
				command->in_ctx->dma, must_succeed);
	} else {
		ret = usbssp_queue_evaluate_context(usbssp_data, command,
				command->in_ctx->dma, must_succeed);
	}

	if (ret < 0) {
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_context_change,
			"FIXME allocate a new ring segment");
		return -ENOMEM;
	}

	usbssp_ring_cmd_db(usbssp_data);

	spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,
			usbssp_data->irq_thread_flag);

	/*Waiting for handling Endpoint Configure command */
	while (!command->status)
		udelay(100);

	spin_lock_irqsave(&usbssp_data->irq_thread_lock,
			usbssp_data->irq_thread_flag);

	if (!ctx_change)
		ret = usbssp_configure_endpoint_result(usbssp_data, g,
			&command->status);
	else
		ret = usbssp_evaluate_context_result(usbssp_data, g,
			&command->status);
	return ret;
}

static void usbssp_check_bw_drop_ep_streams(struct usbssp_udc *usbssp_data,
	struct usbssp_device *vdev, int i)
{
	struct usbssp_ep *ep = &vdev->eps[i];

	if (ep->ep_state & EP_HAS_STREAMS) {
		usbssp_warn(usbssp_data,
			"WARN: endpoint 0x%02x has streams on set_interface, freeing streams.\n",
			 usbssp_get_endpoint_address(i));
		usbssp_free_stream_info(usbssp_data, ep->stream_info);
		ep->stream_info = NULL;
		ep->ep_state &= ~EP_HAS_STREAMS;
	}
}

int usbssp_halt_endpoint(struct usbssp_udc *usbssp_data, struct usbssp_ep *dep,
			 int value)
{
	int ret = 1;
	struct usbssp_device *dev_priv;
	struct usbssp_command *command;
	unsigned int ep_index;
	int interrupt_disabled_locally = 0;

	ret = usbssp_check_args(usbssp_data, NULL, 0, true, __func__);
	if (ret <= 0)
		return ret;

	if ((usbssp_data->usbssp_state & USBSSP_STATE_DYING) ||
	    (usbssp_data->usbssp_state & USBSSP_STATE_REMOVING))
		return -ENODEV;

	dev_priv = &usbssp_data->devs;
	ep_index = usbssp_get_endpoint_index(dep->endpoint.desc);

	command = usbssp_alloc_command(usbssp_data, true, GFP_ATOMIC);

	if (!command)
		return -ENOMEM;

	if (value) {
		dep->ep_state |= EP_HALTED;

		ret = usbssp_cmd_stop_ep(usbssp_data,
				&usbssp_data->gadget, dep);
		if (ret < 0) {
			usbssp_err(usbssp_data,
					"Command Stop Endpoint failed 1\n");
			return ret;
		}

		ret = usbssp_queue_halt_endpoint(usbssp_data, command,
				ep_index);

		if (ret < 0) {
			usbssp_err(usbssp_data,
					"Command Halt Endpoint failed\n");
			goto command_cleanup;
		}

		usbssp_ring_cmd_db(usbssp_data);
		/*wait for ep*/
		if (irqs_disabled()) {
			spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,
					usbssp_data->irq_thread_flag);
			interrupt_disabled_locally = 1;
		} else {
			spin_unlock(&usbssp_data->irq_thread_lock);
		}

		/* Wait for last stop endpoint command to finish */
		wait_for_completion(command->completion);

		if (interrupt_disabled_locally)
			spin_lock_irqsave(&usbssp_data->irq_thread_lock,
					usbssp_data->irq_thread_flag);
		else
			spin_lock(&usbssp_data->irq_thread_lock);


	} else {
		struct usbssp_td *td;

		/* Issue a reset endpoint command to clear the device side
		 * halt, followed by a set dequeue command to move the
		 * dequeue pointer past the TD.
		 */
		td = list_first_entry(&dep->ring->td_list, struct usbssp_td,
				td_list);

		usbssp_cleanup_halted_endpoint(usbssp_data, ep_index,
				dep->ring->stream_id, td, EP_HARD_RESET);

		goto command_cleanup;
	}

	ret = command->status;

	switch (ret) {
	case COMP_COMMAND_ABORTED:
	case COMP_COMMAND_RING_STOPPED:
		usbssp_warn(usbssp_data,
				"Timeout waiting for Halt Endpoint command\n");
		ret = -ETIME;
		goto command_cleanup;
	case COMP_SUCCESS:
		usbssp_dbg(usbssp_data, "Successful Halt Endpoint command.\n");
		break;
	default:
		if (usbssp_is_vendor_info_code(usbssp_data, ret))
			break;
		usbssp_warn(usbssp_data, "Unknown completion code %u for "
				"Halt Endpoint command.\n", ret);
		ret = -EINVAL;
		goto command_cleanup;
	}

command_cleanup:
	kfree(command->completion);
	kfree(command);
	return ret;
}

/* Called after one or more calls to usbssp_add_endpoint() or
 * usbssp_drop_endpoint(). If this call fails, the driver is expected
 * to call usbssp_reset_bandwidth().
 *
 */
int usbssp_check_bandwidth(struct usbssp_udc *usbssp_data, struct usb_gadget *g)
{
	int i;
	int ret = 0;
	struct usbssp_device *dev_priv;
	struct usbssp_input_control_ctx *ctrl_ctx;
	struct usbssp_slot_ctx *slot_ctx;
	struct usbssp_command *command;

	ret = usbssp_check_args(usbssp_data, NULL, 0, true, __func__);
	if (ret <= 0)
		return ret;

	if ((usbssp_data->usbssp_state & USBSSP_STATE_DYING) ||
	    (usbssp_data->usbssp_state & USBSSP_STATE_REMOVING))
		return -ENODEV;

	dev_priv = &usbssp_data->devs;

	command = usbssp_alloc_command(usbssp_data, true, GFP_ATOMIC);
	if (!command)
		return -ENOMEM;

	command->in_ctx = dev_priv->in_ctx;

	ctrl_ctx = usbssp_get_input_control_ctx(command->in_ctx);
	if (!ctrl_ctx) {
		usbssp_warn(usbssp_data,
			"%s: Could not get input context, bad type.\n",
			__func__);
		ret = -ENOMEM;
		goto command_cleanup;
	}
	ctrl_ctx->add_flags |= cpu_to_le32(SLOT_FLAG);
	ctrl_ctx->add_flags &= cpu_to_le32(~EP0_FLAG);
	ctrl_ctx->drop_flags &= cpu_to_le32(~(SLOT_FLAG | EP0_FLAG));

	/* Don't issue the command if there's no endpoints to update.*/
	if (ctrl_ctx->add_flags == cpu_to_le32(SLOT_FLAG) &&
	    ctrl_ctx->drop_flags == 0) {
		ret = 0;
		goto command_cleanup;
	}

	/* Fix up Context Entries field. Minimum value is EP0 == BIT(1). */
	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev_priv->in_ctx);
	for (i = 31; i >= 1; i--) {
		__le32 le32 = cpu_to_le32(BIT(i));

		if ((dev_priv->eps[i-1].ring && !(ctrl_ctx->drop_flags & le32))
		    || (ctrl_ctx->add_flags & le32) || i == 1) {
			slot_ctx->dev_info &= cpu_to_le32(~LAST_CTX_MASK);
			slot_ctx->dev_info |= cpu_to_le32(LAST_CTX(i));
			break;
		}
	}

	usbssp_dbg(usbssp_data, "New Input Control Context:\n");
	usbssp_dbg_ctx(usbssp_data, dev_priv->in_ctx,
			LAST_CTX_TO_EP_NUM(le32_to_cpu(slot_ctx->dev_info)));

	ret = usbssp_configure_endpoint(usbssp_data, g, command,
			false, false);

	if (ret)
		/* Caller should call reset_bandwidth() */
		goto command_cleanup;

	usbssp_dbg(usbssp_data, "Output CTX after successful config ep cmd:\n");
	usbssp_dbg_ctx(usbssp_data, dev_priv->out_ctx,
			LAST_CTX_TO_EP_NUM(le32_to_cpu(slot_ctx->dev_info)));

	/* Free any rings that were dropped, but not changed. */
	for (i = 1; i < 31; ++i) {
		if ((le32_to_cpu(ctrl_ctx->drop_flags) & (1 << (i + 1))) &&
		    !(le32_to_cpu(ctrl_ctx->add_flags) & (1 << (i + 1)))) {
			usbssp_free_endpoint_ring(usbssp_data, dev_priv, i);
			usbssp_check_bw_drop_ep_streams(usbssp_data,
					dev_priv, i);
		}
	}

	usbssp_zero_in_ctx(usbssp_data, dev_priv);
	/*
	 * Install any rings for completely new endpoints or changed endpoints,
	 * and free any old rings from changed endpoints.
	 */
	for (i = 1; i < 31; ++i) {
		if (!dev_priv->eps[i].new_ring)
			continue;
		/* Only free the old ring if it exists.
		 * It may not if this is the first add of an endpoint.
		 */
		if (dev_priv->eps[i].ring)
			usbssp_free_endpoint_ring(usbssp_data, dev_priv, i);

		usbssp_check_bw_drop_ep_streams(usbssp_data, dev_priv, i);
		dev_priv->eps[i].ring = dev_priv->eps[i].new_ring;
		dev_priv->eps[i].new_ring = NULL;
	}
command_cleanup:
	kfree(command->completion);
	kfree(command);
	return ret;
}

void usbssp_reset_bandwidth(struct usbssp_udc *usbssp_data,
			    struct usb_gadget *g)
{
	struct usbssp_device *dev_priv;
	int i, ret;

	ret = usbssp_check_args(usbssp_data, NULL, 0, true, __func__);
	if (ret <= 0)
		return;

	dev_priv = &usbssp_data->devs;
	/* Free any rings allocated for added endpoints */
	for (i = 0; i < 31; ++i) {
		if (dev_priv->eps[i].new_ring) {
			usbssp_debugfs_remove_endpoint(usbssp_data,
					dev_priv, i);
			usbssp_ring_free(usbssp_data,
					dev_priv->eps[i].new_ring);
			dev_priv->eps[i].new_ring = NULL;
		}
	}
	usbssp_zero_in_ctx(usbssp_data, dev_priv);
}

void usbssp_cleanup_stalled_ring(struct usbssp_udc *usbssp_data,
				 unsigned int ep_index,
				 unsigned int stream_id,
				 struct usbssp_td *td)
{
	struct usbssp_dequeue_state deq_state;
	struct usbssp_ep *ep_priv;

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_reset_ep,
			"Cleaning up stalled endpoint ring");
	ep_priv = &usbssp_data->devs.eps[ep_index];

	/* We need to move the HW's dequeue pointer past this TD,
	 * or it will attempt to resend it on the next doorbell ring.
	 */
	usbssp_find_new_dequeue_state(usbssp_data,
			ep_index, stream_id, td, &deq_state);

	if (!deq_state.new_deq_ptr || !deq_state.new_deq_seg)
		return;

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_reset_ep,
			"Queueing new dequeue state");
	usbssp_queue_new_dequeue_state(usbssp_data,
			ep_index, &deq_state);
}

/*
 * This submits a Reset Device Command, which will set the device state to 0,
 * set the device address to 0, and disable all the endpoints except the default
 * control endpoint. The USB core should come back and call
 * usbssp_address_device(), and then re-set up the configuration.
 *
 * Wait for the Reset Device command to finish. Remove all structures
 * associated with the endpoints that were disabled. Clear the input device
 * structure? Reset the control endpoint 0 max packet size?
 */
int usbssp_reset_device(struct usbssp_udc *usbssp_data)
{
	struct usbssp_device *dev_priv;
	struct usbssp_command *reset_device_cmd;
	int last_freed_endpoint = 0;
	struct usbssp_slot_ctx *slot_ctx;
	int slot_state;
	int ret = 0;

	ret = usbssp_check_args(usbssp_data, NULL, 0, false, __func__);
	if (ret <= 0)
		return ret;

	dev_priv = &usbssp_data->devs;

	/* If device is not setup, there is no point in resetting it */
	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev_priv->out_ctx);
	slot_state = GET_SLOT_STATE(le32_to_cpu(slot_ctx->dev_state));
	pr_info("usbssp_reset_deviceslot_stated\n");
	if (slot_state == SLOT_STATE_DISABLED ||
	    slot_state == SLOT_STATE_ENABLED ||
	    slot_state == SLOT_STATE_DEFAULT) {
		usbssp_dbg(usbssp_data,
			"Slot in DISABLED/ENABLED state - reset not allowed\n");
		return 0;
	}

	trace_usbssp_reset_device(slot_ctx);

	usbssp_dbg(usbssp_data, "Resetting device with slot ID %u\n",
			usbssp_data->slot_id);
	/* Allocate the command structure that holds the struct completion.
	 */
	reset_device_cmd = usbssp_alloc_command(usbssp_data, true, GFP_ATOMIC);

	if (!reset_device_cmd) {
		usbssp_dbg(usbssp_data,
			"Couldn't allocate command structure.\n");
		return -ENOMEM;
	}

	/* Attempt to submit the Reset Device command to the command ring */
	ret = usbssp_queue_reset_device(usbssp_data, reset_device_cmd);
	if (ret) {
		usbssp_dbg(usbssp_data,
			"FIXME: allocate a command ring segment\n");
		goto command_cleanup;
	}
	usbssp_ring_cmd_db(usbssp_data);

	spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,
			usbssp_data->irq_thread_flag);

	/* Wait for the Reset Device command to finish */
	wait_for_completion(reset_device_cmd->completion);
	spin_lock_irqsave(&usbssp_data->irq_thread_lock,
			usbssp_data->irq_thread_flag);

	/* The Reset Device command can't fail, according to spec,
	 * unless we tried to reset a slot ID that wasn't enabled,
	 * or the device wasn't in the addressed or configured state.
	 */
	ret = reset_device_cmd->status;
	switch (ret) {
	case COMP_COMMAND_ABORTED:
	case COMP_COMMAND_RING_STOPPED:
		usbssp_warn(usbssp_data,
			"Timeout waiting for reset device command\n");
		ret = -ETIME;
		goto command_cleanup;
	case COMP_SLOT_NOT_ENABLED_ERROR: /*completion code for bad slot ID */
	case COMP_CONTEXT_STATE_ERROR: /* completion code for same thing */
		usbssp_dbg(usbssp_data,
			"Can't reset device (slot ID %u) in %s state\n",
			usbssp_data->slot_id,
			usbssp_get_slot_state(usbssp_data,
					dev_priv->out_ctx));
		usbssp_dbg(usbssp_data, "Not freeing device rings.\n");
		ret = 0;
		goto command_cleanup;
	case COMP_SUCCESS:
		usbssp_dbg(usbssp_data, "Successful reset device command.\n");
		break;
	default:
		usbssp_warn(usbssp_data, "Unknown completion code %u for "
			"reset device command.\n", ret);
		ret = -EINVAL;
		goto command_cleanup;
	}

	usbssp_dbg(usbssp_data,
		"Output context after successful reset device cmd:\n");
	usbssp_dbg_ctx(usbssp_data, dev_priv->out_ctx, last_freed_endpoint);
	ret = 0;

command_cleanup:
	usbssp_free_command(usbssp_data, reset_device_cmd);
	return ret;
}

/*
 * At this point, the struct usb_device is about to go away, the device has
 * disconnected, and all traffic has been stopped and the endpoints have been
 * disabled. Free any DC data structures associated with that device.
 */
void usbssp_free_dev(struct usbssp_udc *usbssp_data)
{
	struct usbssp_device *priv_dev;
	int i, ret;
	struct usbssp_slot_ctx *slot_ctx;

	priv_dev = &usbssp_data->devs;
	slot_ctx = usbssp_get_slot_ctx(usbssp_data, priv_dev->out_ctx);
	trace_usbssp_free_dev(slot_ctx);

	for (i = 0; i < 31; ++i)
		priv_dev->eps[i].ep_state &= ~EP_STOP_CMD_PENDING;

//	usbssp_debugfs_remove_slot(usbssp_data, usbssp_data->slot_id);
	ret = usbssp_disable_slot(usbssp_data);
	if (ret)
		usbssp_free_priv_device(usbssp_data);
}

int usbssp_disable_slot(struct usbssp_udc *usbssp_data)
{
	struct usbssp_command *command;
	u32 state;
	int ret = 0;

	command = usbssp_alloc_command(usbssp_data, false, GFP_ATOMIC);
	if (!command)
		return -ENOMEM;

	/* Don't disable the slot if the device controller is dead. */
	state = readl(&usbssp_data->op_regs->status);
	if (state == 0xffffffff ||
	    (usbssp_data->usbssp_state & USBSSP_STATE_DYING) ||
	    (usbssp_data->usbssp_state & USBSSP_STATE_HALTED)) {
		kfree(command);
		return -ENODEV;
	}

	ret = usbssp_queue_slot_control(usbssp_data, command, TRB_DISABLE_SLOT);
	if (ret) {
		kfree(command);
		return ret;
	}
	usbssp_ring_cmd_db(usbssp_data);
	return ret;
}

/*
 * Returns 0 if the DC n out of device slots, the Enable Slot command
 * timed out, or allocating memory failed. Returns 1 on success.
 */
int usbssp_alloc_dev(struct usbssp_udc *usbssp_data)
{
	int ret, slot_id;
	struct usbssp_command *command;
	struct usbssp_slot_ctx *slot_ctx;

	command = usbssp_alloc_command(usbssp_data, true, GFP_ATOMIC);

	if (!command)
		return -ENOMEM;

	ret = usbssp_queue_slot_control(usbssp_data, command, TRB_ENABLE_SLOT);

	if (ret) {
		usbssp_free_command(usbssp_data, command);
		return ret;
	}

	usbssp_ring_cmd_db(usbssp_data);
	spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,
				usbssp_data->irq_thread_flag);
	wait_for_completion(command->completion);
	spin_lock_irqsave(&usbssp_data->irq_thread_lock,
			usbssp_data->irq_thread_flag);

	slot_id = usbssp_data->slot_id;

	if (!slot_id || command->status != COMP_SUCCESS) {
		usbssp_err(usbssp_data,
			"Error while assigning device slot ID\n");
		usbssp_free_command(usbssp_data, command);
		return 0;
	}

	usbssp_free_command(usbssp_data, command);

	if (!usbssp_alloc_priv_device(usbssp_data, GFP_ATOMIC)) {
		usbssp_warn(usbssp_data,
			"Could not allocate usbssp_device data structures\n");
		goto disable_slot;
	}

	slot_ctx = usbssp_get_slot_ctx(usbssp_data, usbssp_data->devs.out_ctx);
	trace_usbssp_alloc_dev(slot_ctx);

//	usbssp_debugfs_create_slot(usbssp_data, slot_id);

	return 1;

disable_slot:
	ret = usbssp_disable_slot(usbssp_data);
	if (ret)
		usbssp_free_priv_device(usbssp_data);

	return 0;
}

/*
 * Issue an Address Device command
 */
static int usbssp_setup_device(struct usbssp_udc *usbssp_data,
			       enum usbssp_setup_dev setup)
{
	const char *act = setup == SETUP_CONTEXT_ONLY ? "context" : "address";
	struct usbssp_device *dev_priv;
	int ret = 0;
	struct usbssp_slot_ctx *slot_ctx;
	struct usbssp_input_control_ctx *ctrl_ctx;
	u64 temp_64;
	struct usbssp_command *command = NULL;
	int dev_state = 0;
	int slot_id = usbssp_data->slot_id;

	if (usbssp_data->usbssp_state) {/* dying, removing or halted */
		ret = -ESHUTDOWN;
		goto out;
	}

	if (!slot_id) {
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_address,
				"Bad Slot ID %d", slot_id);
		ret = -EINVAL;
		goto out;
	}

	dev_priv = &usbssp_data->devs;

	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev_priv->out_ctx);
	trace_usbssp_setup_device_slot(slot_ctx);

	dev_state = GET_SLOT_STATE(le32_to_cpu(slot_ctx->dev_state));

	if (setup == SETUP_CONTEXT_ONLY) {
		if (dev_state == SLOT_STATE_DEFAULT) {
			usbssp_dbg(usbssp_data,
				"Slot already in default state\n");
			goto out;
		}
	}

	command = usbssp_alloc_command(usbssp_data, true, GFP_ATOMIC);
	if (!command) {
		ret = -ENOMEM;
		goto out;
	}

	command->in_ctx = dev_priv->in_ctx;

	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev_priv->in_ctx);
	ctrl_ctx = usbssp_get_input_control_ctx(dev_priv->in_ctx);

	if (!ctrl_ctx) {
		usbssp_warn(usbssp_data,
			"%s: Could not get input context, bad type.\n",
			__func__);
		ret = -EINVAL;
		goto out;
	}

	/*
	 * If this is the first Set Address (BSR=0) or driver trays
	 * transition to Default (BSR=1) since device plug-in or
	 * priv device reallocation after a resume with an USBSSP power loss,
	 * then set up the slot context or update device address in slot
	 * context.
	 */
	if (!slot_ctx->dev_info || dev_state == SLOT_STATE_DEFAULT)
		usbssp_setup_addressable_priv_dev(usbssp_data);

	if (dev_state == SLOT_STATE_DEFAULT)
		usbssp_copy_ep0_dequeue_into_input_ctx(usbssp_data);

	ctrl_ctx->add_flags = cpu_to_le32(SLOT_FLAG | EP0_FLAG);
	ctrl_ctx->drop_flags = 0;

	usbssp_dbg(usbssp_data, "Slot ID %d Input Context:\n", slot_id);
	usbssp_dbg_ctx(usbssp_data, dev_priv->in_ctx, 2);
	trace_usbssp_address_ctx(usbssp_data, dev_priv->in_ctx,
			le32_to_cpu(slot_ctx->dev_info) >> 27);

	ret = usbssp_queue_address_device(usbssp_data, command,
			dev_priv->in_ctx->dma, setup);

	if (ret) {
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_address,
				"Prabably command ring segment is full");
		goto out;
	}

	usbssp_ring_cmd_db(usbssp_data);

	spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,
			usbssp_data->irq_thread_flag);
	wait_for_completion(command->completion);
	spin_lock_irqsave(&usbssp_data->irq_thread_lock,
			usbssp_data->irq_thread_flag);

	switch (command->status) {
	case COMP_COMMAND_ABORTED:
	case COMP_COMMAND_RING_STOPPED:
		usbssp_warn(usbssp_data,
			"Timeout while waiting for setup device command\n");
		ret = -ETIME;
		break;
	case COMP_CONTEXT_STATE_ERROR:
	case COMP_SLOT_NOT_ENABLED_ERROR:
		usbssp_err(usbssp_data,
			"Setup ERROR: setup %s command for slot %d.\n",
			act, slot_id);
		ret = -EINVAL;
		break;
	case COMP_INCOMPATIBLE_DEVICE_ERROR:
		dev_warn(usbssp_data->dev,
			"ERROR: Incompatible device for setup %s command\n",
			act);
		ret = -ENODEV;
		break;
	case COMP_SUCCESS:
		usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_address,
				"Successful setup %s command", act);
		break;
	default:
		usbssp_err(usbssp_data,
			"ERROR: unexpected setup %s command completion code 0x%x.\n",
			 act, command->status);
		usbssp_dbg(usbssp_data, "Slot ID %d Output Context:\n",
				slot_id);
		usbssp_dbg_ctx(usbssp_data, dev_priv->out_ctx, 2);
		trace_usbssp_address_ctx(usbssp_data, dev_priv->out_ctx, 1);
		ret = -EINVAL;
		break;
	}

	if (ret)
		goto out;

	temp_64 = usbssp_read_64(usbssp_data, &usbssp_data->op_regs->dcbaa_ptr);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_address,
			"Op regs DCBAA ptr = %#016llx", temp_64);
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_address,
		"Slot ID %d dcbaa entry @%p = %#016llx",
		slot_id, &usbssp_data->dcbaa->dev_context_ptrs[slot_id],
		(unsigned long long)
		le64_to_cpu(usbssp_data->dcbaa->dev_context_ptrs[slot_id]));
	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_address,
			"Output Context DMA address = %#08llx",
			(unsigned long long)dev_priv->out_ctx->dma);

	trace_usbssp_address_ctx(usbssp_data, dev_priv->in_ctx,
				le32_to_cpu(slot_ctx->dev_info) >> 27);
	usbssp_dbg(usbssp_data, "Slot ID %d Output Context:\n", slot_id);
	usbssp_dbg_ctx(usbssp_data, dev_priv->out_ctx, 2);

	slot_ctx = usbssp_get_slot_ctx(usbssp_data, dev_priv->out_ctx);
	trace_usbssp_address_ctx(usbssp_data, dev_priv->out_ctx,
				le32_to_cpu(slot_ctx->dev_info) >> 27);
	/* Zero the input context control for later use */
	ctrl_ctx->add_flags = 0;
	ctrl_ctx->drop_flags = 0;

	usbssp_dbg_trace(usbssp_data, trace_usbssp_dbg_address,
			"Internal device address = %d",
			le32_to_cpu(slot_ctx->dev_state) & DEV_ADDR_MASK);

	if (setup == SETUP_CONTEXT_ADDRESS)
		usbssp_status_stage(usbssp_data);
out:
	if (command) {
		kfree(command->completion);
		kfree(command);
	}
	return ret;
}

int usbssp_address_device(struct usbssp_udc *usbssp_data)
{
	return usbssp_setup_device(usbssp_data, SETUP_CONTEXT_ADDRESS);
}

int usbssp_enable_device(struct usbssp_udc *usbssp_data)
{
	return usbssp_setup_device(usbssp_data, SETUP_CONTEXT_ONLY);
}

int usbssp_set_usb2_hardware_lpm(struct usbssp_udc *usbssp_data,
		struct usb_request *req, int enable)
{
	__le32 __iomem	*pm_addr;
	u32		pm_val, field;
	int		besl;

	struct usb_ext_cap_descriptor *usb_ext = req->buf + USB_DT_BOS_SIZE;

	if (usbssp_data->port_major_revision >= 3 ||
	   !usbssp_data->hw_lpm_support
	   /*|| !usbssp_data->gadget->lpm_capable*/)
		return -EPERM;

	if (usb_ext->bDescriptorType != USB_DT_DEVICE_CAPABILITY ||
	    usb_ext->bDevCapabilityType != USB_CAP_TYPE_EXT) {
		return -EPERM;
	}
	pm_addr = usbssp_data->usb2_ports + PORTPMSC;
	pm_val = readl(pm_addr);
	field = le32_to_cpu(usb_ext->bmAttributes);

	//workaround for LPM - will be removed in feature.
	field &= ~(USB_BESL_SUPPORT | USB_LPM_SUPPORT);
	usb_ext->bmAttributes = field;

	usbssp_dbg(usbssp_data, "%s port %d USB2 hardware LPM\n",
		enable ? "enable" : "disable", usbssp_data->devs.port_num);

	if (enable) {
		/* if device doesn't have a preferred BESL value use a
		 * default one . See USBSSP_DEFAULT_BESL definition in gadget.h
		 */
		if ((field & USB_BESL_SUPPORT) &&
		    (field & USB_BESL_BASELINE_VALID))
			besl = USB_GET_BESL_BASELINE(field);
		else
			besl = USBSSP_DEFAULT_BESL;

		pm_val &= ~(PORT_BESL_MASK | PORT_HLE_MASK);
		pm_val |= PORT_RBESL(besl) | PORT_HLE | 3 /*L1S set to 3*/;
		pr_err("usbssp_set_usb2_hardware_lpm7 %08x\n", pm_val);
		writel(pm_val, pm_addr);
		/* flush write */
		readl(pm_addr);
	} else {
		pm_val &= ~(PORT_HLE | PORT_BESL_MASK | PORT_L1S_MASK);
		pm_val |= PORT_L1S_HLE0_STALL;
		writel(pm_val, pm_addr);
	}
	return 0;
}

int usbssp_get_frame(struct usbssp_udc *usbssp_data)
{
	return readl(&usbssp_data->run_regs->microframe_index) >> 3;
}

int usbssp_gen_setup(struct usbssp_udc *usbssp_data)
{
	int	retval;

	mutex_init(&usbssp_data->mutex);

	usbssp_data->cap_regs = usbssp_data->regs;
	usbssp_data->op_regs = usbssp_data->regs +
		HC_LENGTH(readl(&usbssp_data->cap_regs->hc_capbase));

	usbssp_data->run_regs = usbssp_data->regs +
		(readl(&usbssp_data->cap_regs->run_regs_off) & RTSOFF_MASK);
	/* Cache read-only capability registers */
	usbssp_data->hcs_params1 = readl(&usbssp_data->cap_regs->hcs_params1);
	usbssp_data->hcs_params2 = readl(&usbssp_data->cap_regs->hcs_params2);
	usbssp_data->hcs_params3 = readl(&usbssp_data->cap_regs->hcs_params3);
	usbssp_data->hcc_params = readl(&usbssp_data->cap_regs->hc_capbase);
	usbssp_data->hci_version = HC_VERSION(usbssp_data->hcc_params);
	usbssp_data->hcc_params = readl(&usbssp_data->cap_regs->hcc_params);
	usbssp_data->hcc_params2 = readl(&usbssp_data->cap_regs->hcc_params2);
	usbssp_print_registers(usbssp_data);

	/* Make sure the Device Controller is halted. */
	retval = usbssp_halt(usbssp_data);
	if (retval)
		return retval;

	usbssp_dbg(usbssp_data, "Resetting Device Controller\n");
	/* Reset the internal DC memory state and registers. */
	retval = usbssp_reset(usbssp_data);
	if (retval)
		return retval;
	usbssp_dbg(usbssp_data, "Reset complete\n");

	/* Set dma_mask and coherent_dma_mask to 64-bits,
	 * if USBSSP supports 64-bit addressing
	 */
	if (HCC_64BIT_ADDR(usbssp_data->hcc_params) &&
	    !dma_set_mask(usbssp_data->dev, DMA_BIT_MASK(64))) {
		usbssp_dbg(usbssp_data, "Enabling 64-bit DMA addresses.\n");
		dma_set_coherent_mask(usbssp_data->dev, DMA_BIT_MASK(64));
	} else {
		/*
		 * This is to avoid error in cases where a 32-bit USB
		 * controller is used on a 64-bit capable system.
		 */
		retval = dma_set_mask(usbssp_data->dev, DMA_BIT_MASK(32));
		if (retval)
			return retval;
		usbssp_dbg(usbssp_data, "Enabling 32-bit DMA addresses.\n");
		dma_set_coherent_mask(usbssp_data->dev, DMA_BIT_MASK(32));
	}

	usbssp_dbg(usbssp_data, "Calling USBSSP init\n");
	/* Initialize USBSSP controller data structures. */
	retval = usbssp_init(usbssp_data);
	if (retval)
		return retval;
	usbssp_dbg(usbssp_data, "Called USBSSPinit\n");

	usbssp_info(usbssp_data, "USBSSP params 0x%08x USBSSP version 0x%x\n",
		usbssp_data->hcc_params, usbssp_data->hci_version);

	return 0;
}

/*
 * gadget-if.c file is part of gadget.c file and implements interface
 * for gadget driver
 */
#include "gadget-if.c"

int usbssp_gadget_init(struct usbssp_udc *usbssp_data)
{
	int ret;

	/*
	 * Check the compiler generated sizes of structures that must be laid
	 * out in specific ways for hardware access.
	 */
	BUILD_BUG_ON(sizeof(struct usbssp_doorbell_array) != 2*32/8);
	BUILD_BUG_ON(sizeof(struct usbssp_slot_ctx) != 8*32/8);
	BUILD_BUG_ON(sizeof(struct usbssp_ep_ctx) != 8*32/8);
	/* usbssp_device has eight fields, and also
	 * embeds one usbssp_slot_ctx and 31 usbssp_ep_ctx
	 */
	BUILD_BUG_ON(sizeof(struct usbssp_stream_ctx) != 4*32/8);
	BUILD_BUG_ON(sizeof(union usbssp_trb) != 4*32/8);
	BUILD_BUG_ON(sizeof(struct usbssp_erst_entry) != 4*32/8);
	BUILD_BUG_ON(sizeof(struct usbssp_cap_regs) != 8*32/8);
	BUILD_BUG_ON(sizeof(struct usbssp_intr_reg) != 8*32/8);
	/* usbssp_run_regs has eight fields and embeds 128 usbssp_intr_regs */
	BUILD_BUG_ON(sizeof(struct usbssp_run_regs) != (8+8*128)*32/8);

	/* fill gadget fields */
	usbssp_data->gadget.ops = &usbssp_gadget_ops;
	usbssp_data->gadget.name = "usbssp-gadget";
	usbssp_data->gadget.max_speed = USB_SPEED_SUPER_PLUS;
	usbssp_data->gadget.speed = USB_SPEED_UNKNOWN;
	usbssp_data->gadget.sg_supported = true;
//	usbssp_data->gadget.lpm_capable = 1;

	usbssp_data->setup_buf = kzalloc(USBSSP_EP0_SETUP_SIZE, GFP_KERNEL);
	if (!usbssp_data->setup_buf)
		return -ENOMEM;

//	usbssp_debugfs_create_root();

	/*USBSSP support not aligned buffer but this option
	 * improve performance of this controller.
	 */
	usbssp_data->gadget.quirk_ep_out_aligned_size = true;
	ret = usbssp_gen_setup(usbssp_data);
	if (ret < 0) {
		usbssp_err(usbssp_data,
				"Generic initialization failed with error code%d\n",
				ret);
		goto err3;
	}

	ret = usbssp_gadget_init_endpoint(usbssp_data);
	if (ret < 0) {
		usbssp_err(usbssp_data, "failed to initialize endpoints\n");
		goto err1;
	}

	ret = usb_add_gadget_udc(usbssp_data->dev, &usbssp_data->gadget);

	if (ret) {
		usbssp_err(usbssp_data, "failed to register udc\n");
		goto err2;
	}

	return ret;
err2:
	usbssp_gadget_free_endpoint(usbssp_data);
err1:
	usbssp_halt(usbssp_data);
	usbssp_reset(usbssp_data);
	usbssp_mem_cleanup(usbssp_data);
err3:
	usbssp_debugfs_remove_root();
	return ret;
}

int usbssp_gadget_exit(struct usbssp_udc *usbssp_data)
{
	int ret = 0;

	usb_del_gadget_udc(&usbssp_data->gadget);
	usbssp_gadget_free_endpoint(usbssp_data);
	usbssp_stop(usbssp_data);
	usbssp_debugfs_remove_root();
	return ret;
}

/**NOP command - for testing purpose*/
int usbssp_nop_test(struct usbssp_udc *usbssp_data)
{
	struct usbssp_command *nop_cmd;
	int ret = 0;

	ret = usbssp_check_args(usbssp_data, NULL, 0, false, __func__);
	if (ret <= 0)
		return ret;

	usbssp_dbg(usbssp_data, "Test: NOP command\n");

	/* Allocate the command structure that holds the struct completion.
	 */
	nop_cmd = usbssp_alloc_command(usbssp_data, true, GFP_NOIO);
	if (!nop_cmd) {
		usbssp_dbg(usbssp_data,
			"Couldn't allocate command structure.\n");
		return -ENOMEM;
	}

	ret = usbssp_queue_nop(usbssp_data, nop_cmd);
	if (ret)
		goto command_cleanup;

	usbssp_ring_cmd_db(usbssp_data);
	spin_unlock_irqrestore(&usbssp_data->irq_thread_lock,
		usbssp_data->irq_thread_flag);

	/* Wait for the Reset Device command to finish */
	wait_for_completion(nop_cmd->completion);
	spin_lock_irqsave(&usbssp_data->irq_thread_lock,
		usbssp_data->irq_thread_flag);

	/* The NOP command can't fail*/
	ret = nop_cmd->status;
	switch (ret) {
	case COMP_SUCCESS:
		usbssp_dbg(usbssp_data, "Successful NOP command.\n");
		break;
	default:
		usbssp_warn(usbssp_data,
			"Unknown completion code %u for NOP command.\n", ret);
		ret = -EINVAL;
		goto command_cleanup;
	}

	ret = 0;

command_cleanup:
	usbssp_free_command(usbssp_data, nop_cmd);
	return ret;
}
