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

/**
 * cdns3_set_mode - change mode of OTG Core
 * @cdns: pointer to context structure
 * @mode: selected mode from cdns_role
 */
void cdns3_set_mode(struct cdns3 *cdns, u32 mode)
{
	u32 reg;

	switch (mode) {
	case CDNS3_ROLE_GADGET:
		dev_info(cdns->dev, "Set controller to Gadget mode\n");
		writel(OTGCMD_DEV_BUS_REQ | OTGCMD_OTG_DIS,
		       &cdns->otg_regs->cmd);
		break;
	case CDNS3_ROLE_HOST:
		dev_info(cdns->dev, "Set controller to Host mode\n");
		writel(OTGCMD_HOST_BUS_REQ | OTGCMD_OTG_DIS,
		       &cdns->otg_regs->cmd);
		break;
	case CDNS3_ROLE_OTG:
		dev_info(cdns->dev, "Set controller to OTG mode\n");
		reg = readl(&cdns->otg_regs->ctrl1);
		reg |= OTGCTRL1_IDPULLUP;
		writel(reg, &cdns->otg_regs->ctrl1);

		/* wait until valid ID (ID_VALUE) can be sampled (50ms). */
		mdelay(50);
		break;
	default:
		dev_err(cdns->dev, "Unsupported mode of operation %d\n", mode);
		return;
	}
}

static int cdns3_otg_get_id(struct cdns3 *cdns)
{
	int id;

	id = readl(&cdns->otg_regs->sts) & OTGSTS_ID_VALUE;
	dev_dbg(cdns->dev, "OTG ID: %d", id);
	return id;
}

int cdns3_is_host(struct cdns3 *cdns)
{
	if (cdns->dr_mode == USB_DR_MODE_HOST)
		return 1;

	return 0;
}

int cdns3_is_device(struct cdns3 *cdns)
{
	if (cdns->dr_mode == USB_DR_MODE_PERIPHERAL)
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
 * cdns3_drd_init - initialize drd controller
 * @cdns: Pointer to controller context structure
 *
 * Returns 0 on success otherwise negative errno
 */
static void cdns3_drd_init(struct cdns3 *cdns)
{
	cdns3_otg_disable_irq(cdns);
	/* clear all interrupts */
	writel(~0, &cdns->otg_regs->ivect);

	cdns3_set_mode(cdns, CDNS3_ROLE_OTG);

	cdns3_otg_enable_irq(cdns);
}

/**
 * cdns3_core_init_mode - initialize mode of operation
 * @cdns: Pointer to controller context structure
 *
 * Returns 0 on success otherwise negative errno
 */
static int cdns3_core_init_mode(struct cdns3 *cdns)
{
	int ret = 0;

	switch (cdns->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
		cdns3_set_mode(cdns, CDNS3_ROLE_GADGET);
		break;
	case USB_DR_MODE_HOST:
		cdns3_set_mode(cdns, CDNS3_ROLE_HOST);
		break;
	case USB_DR_MODE_OTG:
		cdns3_drd_init(cdns);
		break;
	default:
		dev_err(cdns->dev, "Unsupported mode of operation %d\n",
			cdns->dr_mode);
		return -EINVAL;
	}

	return ret;
}

irqreturn_t cdns3_drd_irq(struct cdns3 *cdns)
{
	irqreturn_t ret = IRQ_NONE;
	u32 reg;

	if (cdns->dr_mode != USB_DR_MODE_OTG)
		return ret;

	reg = readl(&cdns->otg_regs->ivect);
	if (!reg)
		return ret;

	if (reg & OTGIEN_ID_CHANGE_INT) {
		int id = cdns3_otg_get_id(cdns);

		dev_dbg(cdns->dev, "OTG IRQ: new ID: %d\n",
			cdns3_otg_get_id(cdns));

		if (id)
			cdns->desired_role = CDNS3_ROLE_GADGET;
		else
			cdns->desired_role = CDNS3_ROLE_GADGET;

		queue_work(system_freezable_wq, &cdns->role_switch_wq);

		ret = IRQ_HANDLED;
	}

	writel(~0, &cdns->otg_regs->ivect);
	return IRQ_HANDLED;
}

int cdns3_drd_probe(struct cdns3 *cdns)
{
	enum usb_dr_mode dr_mode;
	int ret = 0;
	u32 state;

	state = OTGSTS_STRAP(readl(&cdns->otg_regs->sts));

	dr_mode = cdns->dr_mode;
	if (state == OTGSTS_STRAP_HOST) {
		dev_info(cdns->dev, "Controller strapped to HOST\n");
		dr_mode = USB_DR_MODE_HOST;
		if (cdns->dr_mode != USB_DR_MODE_HOST &&
		    cdns->dr_mode != USB_DR_MODE_OTG)
			ret = -EINVAL;
	} else if (state == OTGSTS_STRAP_GADGET) {
		dev_info(cdns->dev, "Controller strapped to HOST\n");
		dr_mode = USB_DR_MODE_PERIPHERAL;
		if (cdns->dr_mode != USB_DR_MODE_PERIPHERAL &&
		    cdns->dr_mode != USB_DR_MODE_OTG)
			ret = -EINVAL;
	}

	if (ret) {
		dev_err(cdns->dev, "Incorrect DRD configuration\n");
		return ret;
	}

	//Updating DR mode according to strap.
	cdns->dr_mode = dr_mode;

	dev_info(cdns->dev, "Controller Device ID: %08lx, Revision ID: %08lx\n",
		 CDNS_RID(readl(&cdns->otg_regs->rid)),
		 CDNS_DID(readl(&cdns->otg_regs->did)));

	state = readl(&cdns->otg_regs->sts);
	if (OTGSTS_OTG_NRDY(state) != 0) {
		dev_err(cdns->dev, "Cadence USB3 OTG device not ready\n");
		return -ENODEV;
	}

	ret = cdns3_core_init_mode(cdns);

	return ret;
}
