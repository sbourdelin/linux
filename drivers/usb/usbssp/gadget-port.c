// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Some code borrowed from the Linux XHCI driver.
 */

#include <linux/slab.h>
#include <asm/unaligned.h>

#include "gadget-trace.h"
#include "gadget.h"

unsigned int usbssp_port_speed(unsigned int port_status)
{
	/*Detect gadget speed*/
	if (DEV_SUPERSPEEDPLUS(port_status))
		return USB_SPEED_SUPER_PLUS;
	else if (DEV_SUPERSPEED(port_status))
		return USB_SPEED_SUPER;
	else if (DEV_HIGHSPEED(port_status))
		return USB_SPEED_HIGH;
	else if (DEV_FULLSPEED(port_status))
		return USB_SPEED_FULL;
	else if (DEV_LOWSPEED(port_status))
		return USB_SPEED_LOW;

	/*if device is detached  then speed will be USB_SPEED_UNKNOWN*/
	return USB_SPEED_UNKNOWN;
}

/*
 * These bits are Read Only (RO) and should be saved and written to the
 * registers: 0, 3, 10:13, 30
 * connect status, over-current status and port speed.
 * connect status and port speed are also sticky - meaning they're in
 * the AUX well and they aren't changed by a hot and warm.
 */
#define	USBSSP_PORT_RO	(PORT_CONNECT | PORT_OC | DEV_SPEED_MASK)

/*
 * These bits are RW; writing a 0 clears the bit, writing a 1 sets the bit:
 * bits 5:8, 9, 14:15, 25:27
 * link state, port power, port indicator state, "wake on" enable state
 */
#define USBSSP_PORT_RWS	(PORT_PLS_MASK | PORT_POWER | PORT_WKCONN_E | \
			PORT_WKDISC_E | PORT_WKOC_E)

/*
 * Given a port state, this function returns a value that would result in the
 * port being in the same state, if the value was written to the port status
 * control register.
 * Save Read Only (RO) bits and save read/write bits where
 * writing a 0 clears the bit and writing a 1 sets the bit (RWS).
 * For all other types (RW1S, RW1CS, RW, and RZ), writing a '0' has no effect.
 */
u32 usbssp_port_state_to_neutral(u32 state)
{
	/* Save read-only status and port state */
	return (state & USBSSP_PORT_RO) | (state & USBSSP_PORT_RWS);
}

/*
 * Stop device
 * It issues stop endpoint command for EP 0 to 30. And wait the last command
 * to complete.
 */
int usbssp_stop_device(struct usbssp_udc *usbssp_data, int suspend)
{
	struct usbssp_device *priv_dev;
	struct usbssp_ep_ctx *ep_ctx;
	int ret = 0;
	int i;

	ret = 0;
	priv_dev = &usbssp_data->devs;

	trace_usbssp_stop_device(priv_dev);

	if (usbssp_data->gadget.state < USB_STATE_ADDRESS) {
		usbssp_dbg(usbssp_data,
			"Device is not yet in  USB_STATE_ADDRESS state\n");
		goto stop_ep0;
	}

	for (i = LAST_EP_INDEX; i > 0; i--) {
		if (priv_dev->eps[i].ring && priv_dev->eps[i].ring->dequeue) {
			struct usbssp_command *command;

			if (priv_dev->eps[i].ep_state & EP_HALTED) {
				usbssp_dbg(usbssp_data,
					"ep_index %d is in halted state "
					"- ep state: %x\n",
					i, priv_dev->eps[i].ep_state);
				usbssp_halt_endpoint(usbssp_data,
						&priv_dev->eps[i], 0);
			}

			ep_ctx = usbssp_get_ep_ctx(usbssp_data,
					priv_dev->out_ctx, i);

			/* Check ep is running, required by AMD SNPS 3.1 xHC */
			if (GET_EP_CTX_STATE(ep_ctx) != EP_STATE_RUNNING) {
				usbssp_dbg(usbssp_data,
					"ep_index %d is already stopped.\n", i);
				continue;
			}

			if (priv_dev->eps[i].ep_state & EP_STOP_CMD_PENDING) {
				usbssp_dbg(usbssp_data,
					"Stop endpoint command is pending "
					"for ep_index %d.\n", i);
				continue;
			}

			/*device was disconnected so endpoint should be disabled
			 * and transfer ring stopped.
			 */
			priv_dev->eps[i].ep_state |= EP_STOP_CMD_PENDING |
					USBSSP_EP_DISABLE_PENDING;

			command = usbssp_alloc_command(usbssp_data, false,
					GFP_ATOMIC);
			if (!command)
				return -ENOMEM;

			ret = usbssp_queue_stop_endpoint(usbssp_data,
					command, i, suspend);
			if (ret) {
				usbssp_free_command(usbssp_data, command);
				return ret;
			}
		}
	}

stop_ep0:
	if (priv_dev->eps[0].ep_state & EP_HALTED) {
		usbssp_dbg(usbssp_data,
			"ep_index 0 is in halted state - ep state: %x\n",
			priv_dev->eps[i].ep_state);
		ret = usbssp_halt_endpoint(usbssp_data, &priv_dev->eps[0], 0);
	} else {
		/*device was disconnected so endpoint should be disabled
		 * and transfer ring stopped.
		 */
		priv_dev->eps[0].ep_state &= ~USBSSP_EP_ENABLED;
		ret = usbssp_cmd_stop_ep(usbssp_data, &usbssp_data->gadget,
				&priv_dev->eps[0]);
	}

	return ret;
}

__le32 __iomem *usbssp_get_port_io_addr(struct usbssp_udc *usbssp_data)
{
	if (usbssp_data->port_major_revision == 0x03)
		return usbssp_data->usb3_ports;
	else
		return usbssp_data->usb2_ports;
}


void usbssp_set_link_state(struct usbssp_udc *usbssp_data,
			   __le32 __iomem *port_regs,
			   u32 link_state)
{
	u32 temp;

	temp = readl(port_regs);
	temp = usbssp_port_state_to_neutral(temp);
	temp &= ~PORT_PLS_MASK;
	temp |= PORT_LINK_STROBE | link_state;
	writel(temp, port_regs);
}

/* Test and clear port RWC bit */
void usbssp_test_and_clear_bit(struct usbssp_udc *usbssp_data,
			       __le32 __iomem *port_regs,
			       u32 port_bit)
{
	u32 temp;

	temp = readl(port_regs);
	if (temp & port_bit) {
		temp = usbssp_port_state_to_neutral(temp);
		temp |= port_bit;
		writel(temp, port_regs);
	}
}

static void usbssp_set_port_power(struct usbssp_udc *usbssp_data,
				  bool on, unsigned long *flags)
{
	__le32 __iomem *addr;
	u32 temp;

	addr = usbssp_get_port_io_addr(usbssp_data);
	temp = readl(addr);
	temp = usbssp_port_state_to_neutral(temp);
	if (on) {
		/* Power on */
		writel(temp | PORT_POWER, addr);
		temp = readl(addr);
		usbssp_dbg(usbssp_data,
			"set port power, actual port status  = 0x%x\n",
			temp);
	} else {
		/* Power off */
		writel(temp & ~PORT_POWER, addr);
		usbssp_dbg(usbssp_data,
			"clear port power, actual port status  = 0x%x\n",
			 temp);
	}
}

static void usbssp_port_set_test_mode(struct usbssp_udc *usbssp_data,
	u16 test_mode)
{
	u32 temp;
	__le32 __iomem *addr;

	/* USBSSP only supports test mode for usb2 ports, */
	addr = usbssp_get_port_io_addr(usbssp_data);
	temp = readl(addr + PORTPMSC);
	temp |= test_mode << PORT_TEST_MODE_SHIFT;
	writel(temp, addr + PORTPMSC);
	usbssp_data->test_mode = test_mode;
	if (test_mode == TEST_FORCE_EN)
		usbssp_start(usbssp_data);
}

int usbssp_enter_test_mode(struct usbssp_udc *usbssp_data,
			   u16 test_mode, unsigned long *flags)
{
	int retval;

	retval = usbssp_disable_slot(usbssp_data);
	if (retval) {
		usbssp_err(usbssp_data,
			"Failed to disable slot %d, %d. Enter test mode anyway\n",
			usbssp_data->slot_id, retval);
		return retval;
	}
	/* Put port to the Disable state by clear PP */
	usbssp_set_port_power(usbssp_data, false, flags);

	/* Stop the controller */
	retval = usbssp_halt(usbssp_data);
	if (retval)
		return retval;

	/* Disable runtime PM for test mode */
	pm_runtime_forbid(usbssp_data->dev);
	/* Set PORTPMSC.PTC field to enter selected test mode */
	/* Port is selected by wIndex. port_id = wIndex + 1 */
	usbssp_dbg(usbssp_data, "Enter Test Mode: _id=%d\n",
			test_mode);
	usbssp_port_set_test_mode(usbssp_data, test_mode);

	return retval;
}

int usbssp_exit_test_mode(struct usbssp_udc *usbssp_data)
{
	int retval;

	if (!usbssp_data->test_mode) {
		usbssp_err(usbssp_data, "Not in test mode, do nothing.\n");
		return 0;
	}
	if (usbssp_data->test_mode == TEST_FORCE_EN &&
		!(usbssp_data->usbssp_state & USBSSP_STATE_HALTED)) {
		retval = usbssp_halt(usbssp_data);
		if (retval)
			return retval;
	}
	pm_runtime_allow(usbssp_data->dev);
	usbssp_data->test_mode = 0;
	return usbssp_reset(usbssp_data);
}

