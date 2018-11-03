// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS DRD Driver - gadget side.
 *
 * Copyright (C) 2018 Cadence Design Systems.
 * Copyright (C) 2017 NXP
 *
 * Authors: Pawel Jez <pjez@cadence.com>,
 *          Pawel Laszczak <pawell@cadence.com>
 *	    Peter Chen <peter.chen@nxp.com>
 */

#include <linux/dma-mapping.h>
#include <linux/usb/gadget.h>

#include "core.h"
#include "gadget-export.h"
#include "gadget.h"

/**
 * cdns3_set_register_bit - set bit in given register.
 * @ptr: address of device controller register to be read and changed
 * @mask: bits requested to set
 */
void cdns3_set_register_bit(void __iomem *ptr, u32 mask)
{
	mask = readl(ptr) | mask;
	writel(mask, ptr);
}

/**
 * select_ep - selects endpoint
 * @priv_dev:  extended gadget object
 * @ep: endpoint address
 */
void cdns3_select_ep(struct cdns3_device *priv_dev, u32 ep)
{
	if (priv_dev->selected_ep == ep)
		return;

	dev_dbg(&priv_dev->dev, "Ep sel: 0x%02x\n", ep);
	priv_dev->selected_ep = ep;
	writel(ep, &priv_dev->regs->ep_sel);
	/* memory barrier for selecting endpoint. */
	wmb();
}

/**
 * cdns3_irq_handler - irq line interrupt handler
 * @cdns: cdns3 instance
 *
 * Returns IRQ_HANDLED when interrupt raised by USBSS_DEV,
 * IRQ_NONE when interrupt raised by other device connected
 * to the irq line
 */
static irqreturn_t cdns3_irq_handler_thread(struct cdns3 *cdns)
{
	irqreturn_t ret = IRQ_NONE;
	//TODO: implements this function
	return ret;
}

static void cdns3_gadget_config(struct cdns3_device *priv_dev)
{
	struct cdns3_usb_regs __iomem *regs = priv_dev->regs;

	cdns3_ep0_config(priv_dev);

	/* enable interrupts for endpoint 0 (in and out) */
	writel(EP_IEN_EP_OUT0 | EP_IEN_EP_IN0, &regs->ep_ien);

	/* enable generic interrupt*/
	writel(USB_IEN_INIT, &regs->usb_ien);
	writel(USB_CONF_CLK2OFFDS | USB_CONF_L1DS, &regs->usb_conf);
	writel(USB_CONF_U1DS | USB_CONF_U2DS, &regs->usb_conf);
	writel(USB_CONF_DMULT, &regs->usb_conf);
	writel(USB_CONF_DEVEN, &regs->usb_conf);
}

/**
 * cdns3_init_ep Initializes software endpoints of gadget
 * @cdns3: extended gadget object
 *
 * Returns 0 on success, error code elsewhere
 */
static int cdns3_init_ep(struct cdns3_device *priv_dev)
{
	u32 ep_enabled_reg, iso_ep_reg;
	struct cdns3_endpoint *priv_ep;
	int found_endpoints = 0;
	int ep_dir, ep_number;
	u32 ep_mask;
	int i;

	/* Read it from USB_CAP3 to USB_CAP5 */
	ep_enabled_reg = readl(&priv_dev->regs->usb_cap3);
	iso_ep_reg = readl(&priv_dev->regs->usb_cap4);

	dev_dbg(&priv_dev->dev, "Initializing non-zero endpoints\n");

	for (i = 0; i < USB_SS_ENDPOINTS_MAX_COUNT; i++) {
		ep_number = (i / 2) + 1;
		ep_dir = i % 2;
		ep_mask = BIT((16 * ep_dir) + ep_number);

		if (!(ep_enabled_reg & ep_mask))
			continue;

		priv_ep = devm_kzalloc(&priv_dev->dev, sizeof(*priv_ep),
				       GFP_KERNEL);
		if (!priv_ep)
			return -ENOMEM;

		/* set parent of endpoint object */
		priv_ep->cdns3_dev = priv_dev;
		priv_dev->eps[found_endpoints++] = priv_ep;

		snprintf(priv_ep->name, sizeof(priv_ep->name), "ep%d%s",
			 ep_number, !!ep_dir ? "in" : "out");
		priv_ep->endpoint.name = priv_ep->name;

		usb_ep_set_maxpacket_limit(&priv_ep->endpoint,
					   ENDPOINT_MAX_PACKET_LIMIT);
		priv_ep->endpoint.max_streams = ENDPOINT_MAX_STREAMS;
		//TODO: Add implementation of cdns3_gadget_ep_ops
		//priv_ep->endpoint.ops = &cdns3_gadget_ep_ops;
		if (ep_dir)
			priv_ep->endpoint.caps.dir_in = 1;
		else
			priv_ep->endpoint.caps.dir_out = 1;

		if (iso_ep_reg & ep_mask)
			priv_ep->endpoint.caps.type_iso = 1;

		priv_ep->endpoint.caps.type_bulk = 1;
		priv_ep->endpoint.caps.type_int = 1;
		priv_ep->endpoint.maxburst = CDNS3_EP_BUF_SIZE - 1;

		dev_info(&priv_dev->dev, "Initialized  %s support: %s %s\n",
			 priv_ep->name,
			 priv_ep->endpoint.caps.type_bulk ? "BULK, INT" : "",
			 priv_ep->endpoint.caps.type_iso ? "ISO" : "");

		list_add_tail(&priv_ep->endpoint.ep_list,
			      &priv_dev->gadget.ep_list);
		INIT_LIST_HEAD(&priv_ep->request_list);
		INIT_LIST_HEAD(&priv_ep->ep_match_pending_list);
	}

	priv_dev->ep_nums = found_endpoints;
	return 0;
}

static void cdns3_gadget_release(struct device *dev)
{
	struct cdns3_device *priv_dev;

	priv_dev = container_of(dev, struct cdns3_device, dev);
	kfree(priv_dev);
}

static int __cdns3_gadget_init(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;
	struct device *dev;
	int ret;

	priv_dev = kzalloc(sizeof(*priv_dev), GFP_KERNEL);
	if (!priv_dev)
		return -ENOMEM;

	dev = &priv_dev->dev;
	dev->release = cdns3_gadget_release;
	dev->parent = cdns->dev;
	dev_set_name(dev, "gadget-cdns3");
	cdns->gadget_dev = dev;

	priv_dev->sysdev = cdns->dev;
	ret = device_register(dev);
	if (ret)
		goto err1;

	priv_dev->regs = cdns->dev_regs;

	/* fill gadget fields */
	priv_dev->gadget.max_speed = USB_SPEED_SUPER;
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;
	//TODO: Add implementation of cdns3_gadget_ops
	//priv_dev->gadget.ops = &cdns3_gadget_ops;
	priv_dev->gadget.name = "usb-ss-gadget";
	priv_dev->gadget.sg_supported = 1;
	priv_dev->is_connected = 0;

	spin_lock_init(&priv_dev->lock);

	priv_dev->in_standby_mode = 1;

	/* initialize endpoint container */
	INIT_LIST_HEAD(&priv_dev->gadget.ep_list);
	INIT_LIST_HEAD(&priv_dev->ep_match_list);

	ret = cdns3_init_ep0(priv_dev);
	if (ret) {
		dev_err(dev, "Failed to create endpoint 0\n");
		ret = -ENOMEM;
		goto err2;
	}

	ret = cdns3_init_ep(priv_dev);
	if (ret) {
		dev_err(dev, "Failed to create non zero endpoints\n");
		ret = -ENOMEM;
		goto err2;
	}

	/* allocate memory for default endpoint TRB */
	priv_dev->trb_ep0 = dma_alloc_coherent(priv_dev->sysdev, 24,
					       &priv_dev->trb_ep0_dma, GFP_DMA);
	if (!priv_dev->trb_ep0) {
		dev_err(dev, "Failed to allocate memory for ep0 TRB\n");
		ret = -ENOMEM;
		goto err2;
	}

	/* allocate memory for setup packet buffer */
	priv_dev->setup = dma_alloc_coherent(priv_dev->sysdev, 8,
					     &priv_dev->setup_dma, GFP_DMA);
	if (!priv_dev->setup) {
		dev_err(dev, "Failed to allocate memory for SETUP buffer\n");
		ret = -ENOMEM;
		goto err3;
	}

	dev_dbg(dev, "Device Controller version: %08x\n",
		readl(&priv_dev->regs->usb_cap6));
	dev_dbg(dev, "USB Capabilities:: %08x\n",
		readl(&priv_dev->regs->usb_cap1));
	dev_dbg(dev, "On-Chip memory cnfiguration: %08x\n",
		readl(&priv_dev->regs->usb_cap2));

	/* add USB gadget device */
	ret = usb_add_gadget_udc(&priv_dev->dev, &priv_dev->gadget);
	if (ret < 0) {
		dev_err(dev, "Failed to register USB device controller\n");
		goto err4;
	}

	priv_dev->zlp_buf = kzalloc(ENDPOINT_ZLP_BUF_SIZE, GFP_KERNEL);
	if (!priv_dev->zlp_buf) {
		ret = -ENOMEM;
		goto err4;
	}

	return 0;
err4:
	dma_free_coherent(priv_dev->sysdev, 8, priv_dev->setup,
			  priv_dev->setup_dma);
err3:
	dma_free_coherent(priv_dev->sysdev, 20, priv_dev->trb_ep0,
			  priv_dev->trb_ep0_dma);
err2:
	device_del(dev);
err1:
	put_device(dev);
	cdns->gadget_dev = NULL;
	return ret;
}

/**
 * cdns3_gadget_remove: parent must call this to remove UDC
 *
 * cdns: cdns3 instance
 */
void cdns3_gadget_remove(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;

	if (!cdns->roles[CDNS3_ROLE_GADGET])
		return;

	priv_dev = container_of(cdns->gadget_dev, struct cdns3_device, dev);
	usb_del_gadget_udc(&priv_dev->gadget);
	dma_free_coherent(priv_dev->sysdev, 8, priv_dev->setup,
			  priv_dev->setup_dma);
	dma_free_coherent(priv_dev->sysdev, 20, priv_dev->trb_ep0,
			  priv_dev->trb_ep0_dma);
	device_unregister(cdns->gadget_dev);
	cdns->gadget_dev = NULL;
	kfree(priv_dev->zlp_buf);
}

static int cdns3_gadget_start(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev = container_of(cdns->gadget_dev,
			struct cdns3_device, dev);
	unsigned long flags;

	pm_runtime_get_sync(cdns->dev);
	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->start_gadget = 1;

	if (!priv_dev->gadget_driver) {
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		return 0;
	}

	cdns3_gadget_config(priv_dev);
	priv_dev->in_standby_mode = 0;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

static void __cdns3_gadget_stop(struct cdns3 *cdns)
{
	struct cdns3_device *priv_dev;
	unsigned long flags;

	priv_dev = container_of(cdns->gadget_dev, struct cdns3_device, dev);

	if (priv_dev->gadget_driver)
		priv_dev->gadget_driver->disconnect(&priv_dev->gadget);

	usb_gadget_disconnect(&priv_dev->gadget);
	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->gadget.speed = USB_SPEED_UNKNOWN;

	/* disable interrupt for device */
	writel(0, &priv_dev->regs->usb_ien);
	writel(USB_CONF_DEVDS, &priv_dev->regs->usb_conf);
	priv_dev->start_gadget = 0;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
}

static void cdns3_gadget_stop(struct cdns3 *cdns)
{
	if (cdns->role == CDNS3_ROLE_GADGET)
		__cdns3_gadget_stop(cdns);

	pm_runtime_mark_last_busy(cdns->dev);
	pm_runtime_put_autosuspend(cdns->dev);
}

static int cdns3_gadget_suspend(struct cdns3 *cdns, bool do_wakeup)
{
	__cdns3_gadget_stop(cdns);
	return 0;
}

static int cdns3_gadget_resume(struct cdns3 *cdns, bool hibernated)
{
	struct cdns3_device *priv_dev;
	unsigned long flags;

	priv_dev = container_of(cdns->gadget_dev, struct cdns3_device, dev);
	spin_lock_irqsave(&priv_dev->lock, flags);
	priv_dev->start_gadget = 1;
	if (!priv_dev->gadget_driver) {
		spin_unlock_irqrestore(&priv_dev->lock, flags);
		return 0;
	}

	cdns3_gadget_config(priv_dev);
	priv_dev->in_standby_mode = 0;
	spin_unlock_irqrestore(&priv_dev->lock, flags);
	return 0;
}

/**
 * cdns3_gadget_init - initialize device structure
 *
 * cdns: cdns3 instance
 *
 * This function initializes the gadget.
 */
int cdns3_gadget_init(struct cdns3 *cdns)
{
	struct cdns3_role_driver *rdrv;

	rdrv = devm_kzalloc(cdns->dev, sizeof(*rdrv), GFP_KERNEL);
	if (!rdrv)
		return -ENOMEM;

	rdrv->start	= cdns3_gadget_start;
	rdrv->stop	= cdns3_gadget_stop;
	rdrv->suspend	= cdns3_gadget_suspend;
	rdrv->resume	= cdns3_gadget_resume;
	rdrv->irq	= cdns3_irq_handler_thread;
	rdrv->name	= "gadget";
	cdns->roles[CDNS3_ROLE_GADGET] = rdrv;
	return __cdns3_gadget_init(cdns);
}
