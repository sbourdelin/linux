// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS DRD Driver.
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak <pawell@cadence.com
 *
 */
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/usb/otg.h>

#include "gadget.h"
#include "drd.h"
#include "core.h"

static int cdns3_drd_switch_gadget(struct cdns3 *cdns, int on);
static int cdns3_drd_switch_host(struct cdns3 *cdns, int on);

/**
 * cdns3_set_mode - change mode of OTG Core
 * @cdns: pointer to context structure
 * @mode: selected mode from cdns_role
 */
void cdns3_set_mode(struct cdns3 *cdns, enum usb_dr_mode mode)
{
	u32 reg;

	cdns->current_dr_mode = mode;

	switch (mode) {
	case USB_DR_MODE_PERIPHERAL:
		dev_info(cdns->dev, "Set controller to Gadget mode\n");
		cdns3_drd_switch_gadget(cdns, 1);
		break;
	case USB_DR_MODE_HOST:
		dev_info(cdns->dev, "Set controller to Host mode\n");
		cdns3_drd_switch_host(cdns, 1);
		break;
	case USB_DR_MODE_OTG:
		dev_info(cdns->dev, "Set controller to OTG mode\n");
		reg = readl(&cdns->otg_regs->override);
		reg |= OVERRIDE_IDPULLUP;
		writel(reg, &cdns->otg_regs->override);

		/*
		 * Hardware specification says: "ID_VALUE must be valid within
		 * 50ms after idpullup is set to '1" so driver must wait
		 * 50ms before reading this pin.
		 */
		usleep_range(50000, 60000);
		break;
	default:
		cdns->current_dr_mode = USB_DR_MODE_UNKNOWN;
		dev_err(cdns->dev, "Unsupported mode of operation %d\n", mode);
		return;
	}
}

int cdns3_get_id(struct cdns3 *cdns)
{
	int id;

	id = readl(&cdns->otg_regs->sts) & OTGSTS_ID_VALUE;
	dev_dbg(cdns->dev, "OTG ID: %d", id);
	return  id;
}

int cdns3_is_host(struct cdns3 *cdns)
{
	if (cdns->current_dr_mode == USB_DR_MODE_HOST)
		return 1;
	else if (!cdns3_get_id(cdns))
		return 1;

	return 0;
}

int cdns3_is_device(struct cdns3 *cdns)
{
	if (cdns->current_dr_mode == USB_DR_MODE_PERIPHERAL)
		return 1;
	else if (cdns->current_dr_mode == USB_DR_MODE_OTG)
		if (cdns3_get_id(cdns))
			return 1;

	return 0;
}

/**
 * cdns3_otg_disable_irq - Disable all OTG interrupts
 * @cdns: Pointer to controller context structure
 */
static void cdns3_otg_disable_irq(struct cdns3 *cdns)
{
	writel(0, &cdns->otg_regs->ien);
}

/**
 * cdns3_otg_enable_irq - enable id and sess_valid interrupts
 * @cdns: Pointer to controller context structure
 */
static void cdns3_otg_enable_irq(struct cdns3 *cdns)
{
	writel(OTGIEN_ID_CHANGE_INT | OTGIEN_VBUSVALID_RISE_INT |
	       OTGIEN_VBUSVALID_FALL_INT, &cdns->otg_regs->ien);
}

/**
 * cdns3_drd_switch_host - start/stop host
 * @cdns: Pointer to controller context structure
 * @on: 1 for start, 0 for stop
 *
 * Returns 0 on success otherwise negative errno
 */
static int cdns3_drd_switch_host(struct cdns3 *cdns, int on)
{
	int ret;
	u32 reg = OTGCMD_OTG_DIS;

	/* switch OTG core */
	if (on) {
		writel(OTGCMD_HOST_BUS_REQ | reg, &cdns->otg_regs->cmd);

		dev_dbg(cdns->dev, "Waiting for Host mode is turned on\n");
		ret = cdns3_handshake(&cdns->otg_regs->sts, OTGSTS_XHCI_READY,
				      OTGSTS_XHCI_READY, 100000);

		if (ret)
			return ret;
	} else {
		usleep_range(30, 40);
		writel(OTGCMD_HOST_BUS_DROP | OTGCMD_DEV_BUS_DROP |
		       OTGCMD_DEV_POWER_OFF | OTGCMD_HOST_POWER_OFF,
		       &cdns->otg_regs->cmd);
	}

	return 0;
}

/**
 * cdns3_drd_switch_gadget - start/stop gadget
 * @cdns: Pointer to controller context structure
 * @on: 1 for start, 0 for stop
 *
 * Returns 0 on success otherwise negative errno
 */
static int cdns3_drd_switch_gadget(struct cdns3 *cdns, int on)
{
	int ret;
	u32 reg = OTGCMD_OTG_DIS;

	/* switch OTG core */
	if (on) {
		writel(OTGCMD_DEV_BUS_REQ | reg, &cdns->otg_regs->cmd);

		dev_dbg(cdns->dev, "Waiting for Device mode is turned on\n");

		ret = cdns3_handshake(&cdns->otg_regs->sts, OTGSTS_DEV_READY,
				      OTGSTS_DEV_READY, 100000);

		if (ret)
			return ret;
	} else {
		/*
		 * driver should wait at least 10us after disabling Device
		 * before turning-off Device (DEV_BUS_DROP)
		 */
		usleep_range(20, 30);
		writel(OTGCMD_HOST_BUS_DROP | OTGCMD_DEV_BUS_DROP |
		       OTGCMD_DEV_POWER_OFF | OTGCMD_HOST_POWER_OFF,
		       &cdns->otg_regs->cmd);
	}

	return 0;
}

/**
 * cdns3_init_otg_mode - initialize drd controller
 * @cdns: Pointer to controller context structure
 *
 * Returns 0 on success otherwise negative errno
 */
static void cdns3_init_otg_mode(struct cdns3 *cdns)
{
	cdns3_otg_disable_irq(cdns);
	/* clear all interrupts */
	writel(~0, &cdns->otg_regs->ivect);

	cdns3_set_mode(cdns, USB_DR_MODE_OTG);

	if (cdns3_is_host(cdns))
		cdns3_drd_switch_host(cdns, 1);
	else
		cdns3_drd_switch_gadget(cdns, 1);

	cdns3_otg_enable_irq(cdns);
}

/**
 * cdns3_drd_update_mode - initialize mode of operation
 * @cdns: Pointer to controller context structure
 *
 * Returns 0 on success otherwise negative errno
 */
int cdns3_drd_update_mode(struct cdns3 *cdns)
{
	int ret = 0;

	if (cdns->desired_dr_mode == cdns->current_dr_mode)
		return ret;

	cdns3_drd_switch_gadget(cdns, 0);
	cdns3_drd_switch_host(cdns, 0);

	switch (cdns->desired_dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
		cdns3_set_mode(cdns, USB_DR_MODE_PERIPHERAL);
		break;
	case USB_DR_MODE_HOST:
		cdns3_set_mode(cdns, USB_DR_MODE_HOST);
		break;
	case USB_DR_MODE_OTG:
		cdns3_init_otg_mode(cdns);
		break;
	default:
		dev_err(cdns->dev, "Unsupported mode of operation %d\n",
			cdns->dr_mode);
		return -EINVAL;
	}

	return ret;
}

/**
 * cdns3_drd_irq - interrupt handler for OTG events
 *
 * @irq: irq number for cdns3 core device
 * @data: structure of cdns3
 *
 * Returns IRQ_HANDLED or IRQ_NONE
 */
static irqreturn_t cdns3_drd_irq(int irq, void *data)
{
	irqreturn_t ret = IRQ_NONE;
	struct cdns3 *cdns = data;
	u32 reg;

	if (cdns->dr_mode != USB_DR_MODE_OTG)
		return ret;

	reg = readl(&cdns->otg_regs->ivect);
	if (!reg)
		return ret;

	if (reg & OTGIEN_ID_CHANGE_INT) {
		dev_dbg(cdns->dev, "OTG IRQ: new ID: %d\n",
			cdns3_get_id(cdns));

		queue_work(system_freezable_wq, &cdns->role_switch_wq);

		ret = IRQ_HANDLED;
	}

	writel(~0, &cdns->otg_regs->ivect);
	return ret;
}

int cdns3_drd_init(struct cdns3 *cdns)
{
	int ret = 0;
	u32 state;

	state = OTGSTS_STRAP(readl(&cdns->otg_regs->sts));

	/* Update dr_mode according to STRAP configuration. */
	cdns->dr_mode = USB_DR_MODE_OTG;
	if (state == OTGSTS_STRAP_HOST) {
		dev_info(cdns->dev, "Controller strapped to HOST\n");
		cdns->dr_mode = USB_DR_MODE_HOST;
	} else if (state == OTGSTS_STRAP_GADGET) {
		dev_info(cdns->dev, "Controller strapped to PERIPHERAL\n");
		cdns->dr_mode = USB_DR_MODE_PERIPHERAL;
	}

	cdns->desired_dr_mode = cdns->dr_mode;
	cdns->current_dr_mode = USB_DR_MODE_UNKNOWN;

	ret = devm_request_irq(cdns->dev, cdns->irq, cdns3_drd_irq, IRQF_SHARED,
			       dev_name(cdns->dev), cdns);

	if (ret)
		return ret;

	state = readl(&cdns->otg_regs->sts);
	if (OTGSTS_OTG_NRDY(state) != 0) {
		dev_err(cdns->dev, "Cadence USB3 OTG device not ready\n");
		return -ENODEV;
	}

	ret = cdns3_drd_update_mode(cdns);

	dev_info(cdns->dev, "Controller Device ID: %08lx, Revision ID: %08lx\n",
		 CDNS_RID(readl(&cdns->otg_regs->rid)),
		 CDNS_DID(readl(&cdns->otg_regs->did)));

	return ret;
}

int cdns3_drd_exit(struct cdns3 *cdns)
{
	return cdns3_drd_switch_host(cdns, 0);
}
