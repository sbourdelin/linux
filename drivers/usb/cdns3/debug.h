/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cadence USBSS DRD Driver.
 * Debug header file.
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak <pawell@cadence.com>
 */
#ifndef __LINUX_CDNS3_DEBUG
#define __LINUX_CDNS3_DEBUG
#include "gadget.h"

static inline void cdns3_decode_get_status(u8 bRequestType, u16 wIndex,
					   u16 wLength, char *str)
{
	switch (bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_INTERFACE:
		sprintf(str, "Get Interface Status Intf = %d, L: = %d",
			wIndex, wLength);
		break;
	case USB_RECIP_ENDPOINT:
		sprintf(str, "Get Endpoint Status ep%d%s",
			wIndex & ~USB_DIR_IN,
			wIndex & USB_DIR_IN ? "in" : "out");
		break;
	}
}

static inline const char *cdns3_decode_device_feature(u16 wValue)
{
	switch (wValue) {
	case USB_DEVICE_SELF_POWERED:
		return "Self Powered";
	case USB_DEVICE_REMOTE_WAKEUP:
		return "Remote Wakeup";
	case USB_DEVICE_TEST_MODE:
		return "Test Mode";
	case USB_DEVICE_U1_ENABLE:
		return "U1 Enable";
	case USB_DEVICE_U2_ENABLE:
		return "U2 Enable";
	case USB_DEVICE_LTM_ENABLE:
		return "LTM Enable";
	default:
		return "UNKNOWN";
	}
}

static inline const char *cdns3_decode_test_mode(u16 wIndex)
{
	switch (wIndex) {
	case TEST_J:
		return ": TEST_J";
	case TEST_K:
		return ": TEST_K";
	case TEST_SE0_NAK:
		return ": TEST_SE0_NAK";
	case TEST_PACKET:
		return ": TEST_PACKET";
	case TEST_FORCE_EN:
		return ": TEST_FORCE_EN";
	default:
		return ": UNKNOWN";
	}
}

static inline void cdns3_decode_set_clear_feature(u8 bRequestType, u8 bRequest,
						  u16 wValue, u16 wIndex,
						  char *str)
{
	switch (bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_DEVICE:
		sprintf(str, "%s Device Feature(%s%s)",
			bRequest == USB_REQ_CLEAR_FEATURE ? "Clear" : "Set",
			cdns3_decode_device_feature(wValue),
			wValue == USB_DEVICE_TEST_MODE ?
			cdns3_decode_test_mode(wIndex) : "");
		break;
	case USB_RECIP_INTERFACE:
		sprintf(str, "%s Interface Feature(%s)",
			bRequest == USB_REQ_CLEAR_FEATURE ? "Clear" : "Set",
			wIndex == USB_INTRF_FUNC_SUSPEND ?
			"Function Suspend" : "UNKNOWN");
		break;
	case USB_RECIP_ENDPOINT:
		sprintf(str, "%s Endpoint Feature(%s ep%d%s)",
			bRequest == USB_REQ_CLEAR_FEATURE ? "Clear" : "Set",
			    wIndex == USB_ENDPOINT_HALT ? "Halt" : "UNKNOWN",
			wIndex & ~USB_DIR_IN,
			wIndex & USB_DIR_IN ? "in" : "out");
		break;
	}
}

static inline const char *cdns3_decode_descriptor(u16 wValue)
{
	switch (wValue >> 8) {
	case USB_DT_DEVICE:
		return "Device";
	case USB_DT_CONFIG:
		return "Configuration";
	case USB_DT_STRING:
		return "String";
	case USB_DT_INTERFACE:
		return "Interface";
	case USB_DT_ENDPOINT:
		return "Endpoint";
	case USB_DT_DEVICE_QUALIFIER:
		return "Device Qualifier";
	case USB_DT_OTHER_SPEED_CONFIG:
		return "Other Speed Config";
	case USB_DT_INTERFACE_POWER:
		return "Interface Power";
	case USB_DT_OTG:
		return "OTG";
	case USB_DT_DEBUG:
		return "Debug";
	case USB_DT_INTERFACE_ASSOCIATION:
		return "Interface Association";
	case USB_DT_BOS:
		return "BOS";
	case USB_DT_DEVICE_CAPABILITY:
		return "Device Capability";
	case USB_DT_SS_ENDPOINT_COMP:
		return "SS Endpoint Companion";
	case USB_DT_SSP_ISOC_ENDPOINT_COMP:
		return "SSP Isochronous Endpoint Companion";
	default:
		return "UNKNOWN";
	}
}

/**
 * cdns3_decode_ctrl - returns a string represetion of ctrl request
 */
static inline const char *cdns3_decode_ctrl(char *str, u8 bRequestType,
					    u8 bRequest, u16 wValue,
					    u16 wIndex, u16 wLength)
{
	switch (bRequest) {
	case USB_REQ_GET_STATUS:
		cdns3_decode_get_status(bRequestType, wIndex,
					wLength, str);
		break;
	case USB_REQ_CLEAR_FEATURE:
	case USB_REQ_SET_FEATURE:
		cdns3_decode_set_clear_feature(bRequestType, bRequest,
					       wValue, wIndex, str);
		break;
	case USB_REQ_SET_ADDRESS:
		sprintf(str, "Set Address Addr: %02x", wValue);
		break;
	case USB_REQ_GET_DESCRIPTOR:
		sprintf(str, "GET %s Descriptor I: %d, L: %d",
			cdns3_decode_descriptor(wValue),
			wValue & 0xff, wLength);
		break;
	case USB_REQ_SET_DESCRIPTOR:
		sprintf(str, "SET %s Descriptor I: %d, L: %d",
			cdns3_decode_descriptor(wValue),
			wValue & 0xff, wLength);
		break;
	case USB_REQ_GET_CONFIGURATION:
		sprintf(str, "Get Configuration L: %d", wLength);
		break;
	case USB_REQ_SET_CONFIGURATION:
		sprintf(str, "Set Configuration Config: %d ", wValue);
		break;
	case USB_REQ_GET_INTERFACE:
		sprintf(str, "Get Interface Intf: %d, L: %d", wIndex, wLength);
		break;
	case USB_REQ_SET_INTERFACE:
		sprintf(str, "Set Interface Intf: %d, Alt: %d", wIndex, wValue);
		break;
	case USB_REQ_SYNCH_FRAME:
		sprintf(str, "Synch Frame Ep: %d, L: %d", wIndex, wLength);
		break;
	case USB_REQ_SET_SEL:
		sprintf(str, "Set SEL L: %d", wLength);
		break;
	case USB_REQ_SET_ISOCH_DELAY:
		sprintf(str, "Set Isochronous Delay Delay: %d ns", wValue);
		break;
	default:
		sprintf(str,
			"SETUP BRT: %02x BR: %02x V: %04x I: %04x L: %04x\n",
			bRequestType, bRequest,
			wValue, wIndex, wLength);
	}

	return str;
}

static inline char *cdns3_decode_usb_irq(struct cdns3_device *priv_dev,
					 u32 usb_ists)
{
	static char str[256];
	int ret;

	ret = sprintf(str, "IRQ %08x = ", usb_ists);

	if (usb_ists & (USB_ISTS_CON2I | USB_ISTS_CONI)) {
		u32 speed = cdns3_get_speed(priv_dev);

		ret += sprintf(str + ret, "Connection %s\n",
			       usb_speed_string(speed));
	}
	if (usb_ists & USB_ISTS_CON2I || usb_ists & USB_ISTS_CONI)
		ret += sprintf(str + ret, "Disconnection ");
	if (usb_ists & USB_ISTS_L2ENTI)
		ret += sprintf(str + ret, "suspended ");

	if (usb_ists & USB_ISTS_L2EXTI)
		ret += sprintf(str + ret, "L2 exit ");
	if (usb_ists & USB_ISTS_U3EXTI)
		ret += sprintf(str + ret, "U3 exit ");
	if (usb_ists & USB_ISTS_UWRESI)
		ret += sprintf(str + ret, "Warm Reset ");
	if (usb_ists & USB_ISTS_UHRESI)
		ret += sprintf(str + ret, "Hot Reset ");
	if (usb_ists & USB_ISTS_U2RESI)
		ret += sprintf(str + ret, "Reset");

	return str;
}

static inline  char *cdns3_decode_ep_irq(u32 ep_sts, const char *ep_name)
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

static inline char *cdns3_decode_epx_irq(struct cdns3_endpoint *priv_ep)
{
	struct cdns3_device *priv_dev = priv_ep->cdns3_dev;

	return cdns3_decode_ep_irq(readl(&priv_dev->regs->ep_sts),
				   priv_ep->name);
}

static inline char *cdns3_decode_ep0_irq(struct cdns3_device *priv_dev)
{
	if (priv_dev->ep0_data_dir)
		return cdns3_decode_ep_irq(readl(&priv_dev->regs->ep_sts),
					   "ep0IN");
	else
		return cdns3_decode_ep_irq(readl(&priv_dev->regs->ep_sts),
					   "ep0OUT");
}

/**
 * Debug a transfer ring.
 *
 * Prints out all TRBs in the endpoint ring, even those after the Link TRB.
 *.
 */
static inline char *cdns3_dbg_ring(struct cdns3_endpoint *priv_ep,
				   int free_trbs, u8 pcs, u8 ccs,
				   int enqueue, int dequeue,
				   struct cdns3_trb *ring, char *str)
{
	u64 addr = priv_ep->trb_pool_dma;
	struct cdns3_trb *trb;
	int ret = 0;
	int i;

	trb = &ring[priv_ep->dequeue];
	ret += sprintf(str + ret, "\n\t\tRing contents for %s:", priv_ep->name);

	ret += sprintf(str + ret,
		       "\n\t\tRing deq index: %d, trb: %p (virt), 0x%llx (dma)\n",
		       dequeue, trb,
		       (unsigned long long)cdns3_trb_virt_to_dma(priv_ep, trb));

	trb = &ring[priv_ep->enqueue];
	ret += sprintf(str + ret,
		       "\t\tRing enq index: %d, trb: %p (virt), 0x%llx (dma)\n",
		       enqueue, trb,
		       (unsigned long long)cdns3_trb_virt_to_dma(priv_ep, trb));

	ret += sprintf(str + ret,
		       "\t\tfree trbs: %d, CCS=%d, PCS=%d\n", free_trbs, ccs,
		       pcs);

	if (TRBS_PER_SEGMENT > 64) {
		sprintf(str + ret, "\t\tTo big transfer ring %d\n",
			TRBS_PER_SEGMENT);
		return str;
	}

	for (i = 0; i < TRBS_PER_SEGMENT; ++i) {
		trb = &ring[i];
		ret += sprintf(str + ret,
			"\t\t@%016llx %08x %08x %08x\n", addr,
			le32_to_cpu(trb->buffer),
			le32_to_cpu(trb->length),
			le32_to_cpu(trb->control));
		addr += sizeof(*trb);
	}

	return str;
}

#ifdef CONFIG_DEBUG_FS
void cdns3_debugfs_init(struct cdns3 *cdns);
void cdns3_debugfs_exit(struct cdns3 *cdns);
#else
void cdns3_debugfs_init(struct cdns3 *cdns);
{  }
void cdns3_debugfs_exit(struct cdns3 *cdns);
{  }
#endif

#endif /*__LINUX_CDNS3_DEBUG*/
