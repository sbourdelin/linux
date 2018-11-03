// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS DRD Driver.
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak <pawell@cadence.com>
 */

#include "gadget.h"

static inline char *cdns3_decode_ep_irq(u32 ep_sts, const char *ep_name)
{
	static char str[256];
	int ret;

	ret = sprintf(str, "IRQ for %s: %08x ", ep_name, ep_sts);

	if (ep_sts & EP_STS_SETUP)
		ret += sprintf(str + ret, "SETUP ");
	if (ep_sts & EP_STS_IOC)
		ret += sprintf(str + ret, "IOC ");
	if (ep_sts & EP_STS_ISP)
		ret += sprintf(str + ret, "ISP ");
	if (ep_sts & EP_STS_DESCMIS)
		ret += sprintf(str + ret, "DESCMIS ");
	if (ep_sts & EP_STS_STREAMR)
		ret += sprintf(str + ret, "STREAMR ");
	if (ep_sts & EP_STS_MD_EXIT)
		ret += sprintf(str + ret, "MD_EXIT ");
	if (ep_sts & EP_STS_TRBERR)
		ret += sprintf(str + ret, "TRBERR ");
	if (ep_sts & EP_STS_NRDY)
		ret += sprintf(str + ret, "NRDY ");
	if (ep_sts & EP_STS_PRIME)
		ret += sprintf(str + ret, "PRIME ");
	if (ep_sts & EP_STS_SIDERR)
		ret += sprintf(str + ret, "SIDERRT ");
	if (ep_sts & EP_STS_OUTSMM)
		ret += sprintf(str + ret, "OUTSMM ");
	if (ep_sts & EP_STS_ISOERR)
		ret += sprintf(str + ret, "ISOERR ");
	if (ep_sts & EP_STS_IOT)
		ret += sprintf(str + ret, "IOT ");

	return str;
}

char *cdns3_decode_epx_irq(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	return cdns3_decode_ep_irq(readl(&priv_dev->regs->ep_sts),
				   priv_ep->name);
}

char *cdns3_decode_ep0_irq(struct cdns3_device *priv_dev, int dir)
{
	if (dir)
		return cdns3_decode_ep_irq(readl(&priv_dev->regs->ep_sts),
					   "ep0IN");
	else
		return cdns3_decode_ep_irq(readl(&priv_dev->regs->ep_sts),
					   "ep0OUT");
}

void cdns3_dbg_setup(struct cdns3_device *priv_dev)
{
	struct usb_ctrlrequest *setup = priv_dev->setup;

	dev_dbg(&priv_dev->dev,
		"SETUP BRT: %02x BR: %02x V: %04x I: %04x L: %04x\n",
		setup->bRequestType,
		setup->bRequest,
		le16_to_cpu(setup->wValue),
		le16_to_cpu(setup->wIndex),
		le16_to_cpu(setup->wLength));
}

/**
 * Debug a transfer ring.
 *
 * Prints out all TRBs in the endpoint ring, even those after the Link TRB.
 *.
 */
void cdns3_dbg_ring(struct cdns3_device *priv_dev,
		    struct cdns3_endpoint *priv_ep)
{
	u64 addr = priv_ep->trb_pool_dma;
	struct cdns3_trb *trb;
	int i;

	for (i = 0; i < TRBS_PER_SEGMENT; ++i) {
		trb = &priv_ep->trb_pool[i];
		dev_dbg(&priv_dev->dev, "@%016llx %08x %08x %08x\n", addr,
			le32_to_cpu(trb->buffer),
			le32_to_cpu(trb->length),
			le32_to_cpu(trb->control));
		addr += sizeof(*trb);
	}
}

void cdns3_dbg_ring_ptrs(struct cdns3_device *priv_dev,
			 struct cdns3_endpoint *priv_ep)
{
	struct cdns3_trb *trb;

	trb = &priv_ep->trb_pool[priv_ep->dequeue];
	dev_dbg(&priv_dev->dev,
		"Ring deq index: %d, trb: %p (virt), 0x%llx (dma)\n",
		priv_ep->dequeue, trb,
		cdns3_trb_virt_to_dma(priv_ep, trb));

	trb = &priv_ep->trb_pool[priv_ep->enqueue];
	dev_dbg(&priv_dev->dev,
		"Ring enq index: %d, trb: %p (virt), 0x%llx (dma)\n",
		priv_ep->enqueue, trb,
		cdns3_trb_virt_to_dma(priv_ep, trb));
}

void cdns3_dbg_ep_rings(struct cdns3_device *priv_dev,
			struct cdns3_endpoint *priv_ep)
{
	dev_dbg(&priv_dev->dev, "Endpoint ring %s:\n", priv_ep->name);

	cdns3_dbg_ring_ptrs(priv_dev, priv_ep);
	cdns3_dbg_ring(priv_dev, priv_ep);
}
