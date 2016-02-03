/*
 * f_subset.c -- "CDC Subset" Ethernet link function driver
 *
 * Copyright (C) 2003-2005,2008 David Brownell
 * Copyright (C) 2008 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/etherdevice.h>

#include "u_ether.h"
#include "u_ether_configfs.h"
#include "u_gether.h"

/*
 * This function packages a simple "CDC Subset" Ethernet port with no real
 * control mechanisms; just raw data transfer over two bulk endpoints.
 * The data transfer model is exactly that of CDC Ethernet, which is
 * why we call it the "CDC Subset".
 *
 * Because it's not standardized, this has some interoperability issues.
 * They mostly relate to driver binding, since the data transfer model is
 * so simple (CDC Ethernet).  The original versions of this protocol used
 * specific product/vendor IDs:  byteswapped IDs for Digital Equipment's
 * SA-1100 "Itsy" board, which could run Linux 2.4 kernels and supported
 * daughtercards with USB peripheral connectors.  (It was used more often
 * with other boards, using the Itsy identifiers.)  Linux hosts recognized
 * this with CONFIG_USB_ARMLINUX; these devices have only one configuration
 * and one interface.
 *
 * At some point, MCCI defined a (nonconformant) CDC MDLM variant called
 * "SAFE", which happens to have a mode which is identical to the "CDC
 * Subset" in terms of data transfer and lack of control model.  This was
 * adopted by later Sharp Zaurus models, and by some other software which
 * Linux hosts recognize with CONFIG_USB_NET_ZAURUS.
 *
 * Because Microsoft's RNDIS drivers are far from robust, we added a few
 * descriptors to the CDC Subset code, making this code look like a SAFE
 * implementation.  This lets you use MCCI's host side MS-Windows drivers
 * if you get fed up with RNDIS.  It also makes it easier for composite
 * drivers to work, since they can use class based binding instead of
 * caring about specific product and vendor IDs.
 */

struct f_gether {
	struct gether			port;

	char				ethaddr[14];
};

static inline struct f_gether *func_to_geth(struct usb_function *f)
{
	return container_of(f, struct f_gether, port.func);
}

/*-------------------------------------------------------------------------*/

/*
 * "Simple" CDC-subset option is a simple vendor-neutral model that most
 * full speed controllers can handle:  one interface, two bulk endpoints.
 * To assist host side drivers, we fancy it up a bit, and add descriptors so
 * some host side drivers will understand it as a "SAFE" variant.
 *
 * "SAFE" loosely follows CDC WMC MDLM, violating the spec in various ways.
 * Data endpoints live in the control interface, there's no data interface.
 * And it's not used to talk to a cell phone radio.
 */

/* interface descriptor: */

static struct usb_interface_descriptor subset_data_intf = {
	.bLength =		sizeof subset_data_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	/* .bInterfaceNumber = DYNAMIC */
	.bAlternateSetting =	0,
	.bNumEndpoints =	2,
	.bInterfaceClass =      USB_CLASS_COMM,
	.bInterfaceSubClass =	USB_CDC_SUBCLASS_MDLM,
	.bInterfaceProtocol =	0,
	/* .iInterface = DYNAMIC */
};

static struct usb_cdc_header_desc mdlm_header_desc = {
	.bLength =		sizeof mdlm_header_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_HEADER_TYPE,

	.bcdCDC =		cpu_to_le16(0x0110),
};

static struct usb_cdc_mdlm_desc mdlm_desc = {
	.bLength =		sizeof mdlm_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_MDLM_TYPE,

	.bcdVersion =		cpu_to_le16(0x0100),
	.bGUID = {
		0x5d, 0x34, 0xcf, 0x66, 0x11, 0x18, 0x11, 0xd6,
		0xa2, 0x1a, 0x00, 0x01, 0x02, 0xca, 0x9a, 0x7f,
	},
};

/* since "usb_cdc_mdlm_detail_desc" is a variable length structure, we
 * can't really use its struct.  All we do here is say that we're using
 * the submode of "SAFE" which directly matches the CDC Subset.
 */
static u8 mdlm_detail_desc[] = {
	6,
	USB_DT_CS_INTERFACE,
	USB_CDC_MDLM_DETAIL_TYPE,

	0,	/* "SAFE" */
	0,	/* network control capabilities (none) */
	0,	/* network data capabilities ("raw" encapsulation) */
};

static struct usb_cdc_ether_desc ether_desc = {
	.bLength =		sizeof ether_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_ETHERNET_TYPE,

	/* this descriptor actually adds value, surprise! */
	/* .iMACAddress = DYNAMIC */
	.bmEthernetStatistics =	cpu_to_le32(0), /* no statistics */
	.wMaxSegmentSize =	cpu_to_le16(ETH_FRAME_LEN),
	.wNumberMCFilters =	cpu_to_le16(0),
	.bNumberPowerFilters =	0,
};

/* full speed support: */

static struct usb_endpoint_descriptor fs_subset_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor fs_subset_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

/* high speed support: */

static struct usb_endpoint_descriptor hs_subset_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(512),
};

static struct usb_endpoint_descriptor hs_subset_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(512),
};

/* super speed support: */

static struct usb_endpoint_descriptor ss_subset_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(1024),
};

static struct usb_endpoint_descriptor ss_subset_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor ss_subset_bulk_comp_desc = {
	.bLength =		sizeof ss_subset_bulk_comp_desc,
	.bDescriptorType =	USB_DT_SS_ENDPOINT_COMP,

	/* the following 2 values can be tweaked if necessary */
	/* .bMaxBurst =		0, */
	/* .bmAttributes =	0, */
};

USB_COMPOSITE_ENDPOINT(ep_in, &fs_subset_in_desc, &hs_subset_in_desc,
		&ss_subset_in_desc, &ss_subset_bulk_comp_desc);
USB_COMPOSITE_ENDPOINT(ep_out, &fs_subset_out_desc, &hs_subset_out_desc,
		&ss_subset_out_desc, &ss_subset_bulk_comp_desc);

USB_COMPOSITE_ALTSETTING(intf0alt0, &subset_data_intf, &ep_in, &ep_out);

USB_COMPOSITE_INTERFACE(intf0, &intf0alt0);

USB_COMPOSITE_DESCRIPTORS(subset_descs, &intf0);

/* string descriptors: */

static struct usb_string geth_string_defs[] = {
	[0].s = "CDC Ethernet Subset/SAFE",
	[1].s = "",
	{  } /* end of list */
};

static struct usb_gadget_strings geth_string_table = {
	.language =		0x0409,	/* en-us */
	.strings =		geth_string_defs,
};

static struct usb_gadget_strings *geth_strings[] = {
	&geth_string_table,
	NULL,
};

/*-------------------------------------------------------------------------*/

static int geth_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_gether		*geth = func_to_geth(f);
	struct usb_composite_dev *cdev = f->config->cdev;
	struct net_device	*net;

	DBG(cdev, "init + activate cdc subset\n");

	geth->port.in_ep = usb_function_get_ep(f, intf, 0);
	if (!geth->port.in_ep)
		return -ENODEV;
	geth->port.out_ep = usb_function_get_ep(f, intf, 1);
	if (!geth->port.out_ep)
		return -ENODEV;

	net = gether_connect(&geth->port);
	return PTR_ERR_OR_ZERO(net);
}

static void geth_clear_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_gether	*geth = func_to_geth(f);
	struct usb_composite_dev *cdev = f->config->cdev;

	DBG(cdev, "net deactivated\n");
	gether_disconnect(&geth->port);
}

/*-------------------------------------------------------------------------*/

/* serial function driver setup/binding */

static int geth_prep_descs(struct usb_function *f)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_string	*us;

	us = usb_gstrings_attach(cdev, geth_strings,
				 ARRAY_SIZE(geth_string_defs));
	if (IS_ERR(us))
		return PTR_ERR(us);

	subset_data_intf.iInterface = us[0].id;
	ether_desc.iMACAddress = us[1].id;

	return usb_function_set_descs(f, &subset_descs);
}

static int geth_prep_vendor_descs(struct usb_function *f)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	int			status;

	struct f_gether_opts	*gether_opts;

	gether_opts = container_of(f->fi, struct f_gether_opts, func_inst);

	/*
	 * in drivers/usb/gadget/configfs.c:configfs_composite_bind()
	 * configurations are bound in sequence with list_for_each_entry,
	 * in each configuration its functions are bound in sequence
	 * with list_for_each_entry, so we assume no race condition
	 * with regard to gether_opts->bound access
	 */
	if (!gether_opts->bound) {
		mutex_lock(&gether_opts->lock);
		gether_set_gadget(gether_opts->net, cdev->gadget);
		status = gether_register_netdev(gether_opts->net);
		mutex_unlock(&gether_opts->lock);
		if (status)
			return status;
		gether_opts->bound = true;
	}

	subset_data_intf.bInterfaceNumber = usb_get_interface_id(f, 0);

	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&mdlm_header_desc);
	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&mdlm_desc);
	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&mdlm_detail_desc);
	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&ether_desc);

	return status;
}

static inline struct f_gether_opts *to_f_gether_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_gether_opts,
			    func_inst.group);
}

/* f_gether_item_ops */
USB_ETHERNET_CONFIGFS_ITEM(gether);

/* f_gether_opts_dev_addr */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_DEV_ADDR(gether);

/* f_gether_opts_host_addr */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_HOST_ADDR(gether);

/* f_gether_opts_qmult */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_QMULT(gether);

/* f_gether_opts_ifname */
USB_ETHERNET_CONFIGFS_ITEM_ATTR_IFNAME(gether);

static struct configfs_attribute *gether_attrs[] = {
	&gether_opts_attr_dev_addr,
	&gether_opts_attr_host_addr,
	&gether_opts_attr_qmult,
	&gether_opts_attr_ifname,
	NULL,
};

static struct config_item_type gether_func_type = {
	.ct_item_ops	= &gether_item_ops,
	.ct_attrs	= gether_attrs,
	.ct_owner	= THIS_MODULE,
};

static void geth_free_inst(struct usb_function_instance *f)
{
	struct f_gether_opts *opts;

	opts = container_of(f, struct f_gether_opts, func_inst);
	if (opts->bound)
		gether_cleanup(netdev_priv(opts->net));
	else
		free_netdev(opts->net);
	kfree(opts);
}

static struct usb_function_instance *geth_alloc_inst(void)
{
	struct f_gether_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);
	mutex_init(&opts->lock);
	opts->func_inst.free_func_inst = geth_free_inst;
	opts->net = gether_setup_default();
	if (IS_ERR(opts->net)) {
		struct net_device *net = opts->net;
		kfree(opts);
		return ERR_CAST(net);
	}

	config_group_init_type_name(&opts->func_inst.group, "",
				    &gether_func_type);

	return &opts->func_inst;
}

static void geth_free(struct usb_function *f)
{
	struct f_gether *eth;

	eth = func_to_geth(f);
	kfree(eth);
}

static struct usb_function *geth_alloc(struct usb_function_instance *fi)
{
	struct f_gether	*geth;
	struct f_gether_opts *opts;
	int status;

	/* allocate and initialize one new instance */
	geth = kzalloc(sizeof(*geth), GFP_KERNEL);
	if (!geth)
		return ERR_PTR(-ENOMEM);

	opts = container_of(fi, struct f_gether_opts, func_inst);

	mutex_lock(&opts->lock);
	opts->refcnt++;
	/* export host's Ethernet address in CDC format */
	status = gether_get_host_addr_cdc(opts->net, geth->ethaddr,
					  sizeof(geth->ethaddr));
	if (status < 12) {
		kfree(geth);
		mutex_unlock(&opts->lock);
		return ERR_PTR(-EINVAL);
	}
	geth_string_defs[1].s = geth->ethaddr;

	geth->port.ioport = netdev_priv(opts->net);
	mutex_unlock(&opts->lock);
	geth->port.cdc_filter = DEFAULT_FILTER;

	geth->port.func.name = "cdc_subset";
	geth->port.func.prep_descs = geth_prep_descs;
	geth->port.func.prep_vendor_descs = geth_prep_vendor_descs;
	geth->port.func.set_alt = geth_set_alt;
	geth->port.func.clear_alt = geth_clear_alt;
	geth->port.func.free_func = geth_free;

	return &geth->port.func;
}

DECLARE_USB_FUNCTION_INIT(geth, geth_alloc_inst, geth_alloc);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Brownell");
