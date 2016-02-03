/*
 * f_obex.c -- USB CDC OBEX function driver
 *
 * Copyright (C) 2008 Nokia Corporation
 * Contact: Felipe Balbi <felipe.balbi@nokia.com>
 *
 * Based on f_acm.c by Al Borchers and David Brownell.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/* #define VERBOSE_DEBUG */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>

#include "u_serial.h"


/*
 * This CDC OBEX function support just packages a TTY-ish byte stream.
 * A user mode server will put it into "raw" mode and handle all the
 * relevant protocol details ... this is just a kernel passthrough.
 * When possible, we prevent gadget enumeration until that server is
 * ready to handle the commands.
 */

struct f_obex {
	struct gserial			port;
	u8				ctrl_id;
	u8				data_id;
	u8				port_num;
};

static inline struct f_obex *func_to_obex(struct usb_function *f)
{
	return container_of(f, struct f_obex, port.func);
}

static inline struct f_obex *port_to_obex(struct gserial *p)
{
	return container_of(p, struct f_obex, port);
}

/*-------------------------------------------------------------------------*/

#define OBEX_CTRL_IDX	0
#define OBEX_DATA_IDX	1

static struct usb_string obex_string_defs[] = {
	[OBEX_CTRL_IDX].s	= "CDC Object Exchange (OBEX)",
	[OBEX_DATA_IDX].s	= "CDC OBEX Data",
	{  },	/* end of list */
};

static struct usb_gadget_strings obex_string_table = {
	.language		= 0x0409,	/* en-US */
	.strings		= obex_string_defs,
};

static struct usb_gadget_strings *obex_strings[] = {
	&obex_string_table,
	NULL,
};

/*-------------------------------------------------------------------------*/

static struct usb_interface_descriptor obex_control_intf = {
	.bLength		= sizeof(obex_control_intf),
	.bDescriptorType	= USB_DT_INTERFACE,
	.bInterfaceNumber	= 0,

	.bAlternateSetting	= 0,
	.bNumEndpoints		= 0,
	.bInterfaceClass	= USB_CLASS_COMM,
	.bInterfaceSubClass	= USB_CDC_SUBCLASS_OBEX,
};

static struct usb_interface_descriptor obex_data_nop_intf = {
	.bLength		= sizeof(obex_data_nop_intf),
	.bDescriptorType	= USB_DT_INTERFACE,
	.bInterfaceNumber	= 1,

	.bAlternateSetting	= 0,
	.bNumEndpoints		= 0,
	.bInterfaceClass	= USB_CLASS_CDC_DATA,
};

static struct usb_interface_descriptor obex_data_intf = {
	.bLength		= sizeof(obex_data_intf),
	.bDescriptorType	= USB_DT_INTERFACE,
	.bInterfaceNumber	= 2,

	.bAlternateSetting	= 1,
	.bNumEndpoints		= 2,
	.bInterfaceClass	= USB_CLASS_CDC_DATA,
};

static struct usb_cdc_header_desc obex_cdc_header_desc = {
	.bLength		= sizeof(obex_cdc_header_desc),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= USB_CDC_HEADER_TYPE,
	.bcdCDC			= cpu_to_le16(0x0120),
};

static struct usb_cdc_union_desc obex_cdc_union_desc = {
	.bLength		= sizeof(obex_cdc_union_desc),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= USB_CDC_UNION_TYPE,
	.bMasterInterface0	= 1,
	.bSlaveInterface0	= 2,
};

static struct usb_cdc_obex_desc obex_desc = {
	.bLength		= sizeof(obex_desc),
	.bDescriptorType	= USB_DT_CS_INTERFACE,
	.bDescriptorSubType	= USB_CDC_OBEX_TYPE,
	.bcdVersion		= cpu_to_le16(0x0100),
};

/* High-Speed Support */

static struct usb_endpoint_descriptor obex_hs_ep_out_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,

	.bEndpointAddress	= USB_DIR_OUT,
	.bmAttributes		= USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize		= cpu_to_le16(512),
};

static struct usb_endpoint_descriptor obex_hs_ep_in_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,

	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize		= cpu_to_le16(512),
};

/* Full-Speed Support */

static struct usb_endpoint_descriptor obex_fs_ep_in_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,

	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor obex_fs_ep_out_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,

	.bEndpointAddress	= USB_DIR_OUT,
	.bmAttributes		= USB_ENDPOINT_XFER_BULK,
};

USB_COMPOSITE_ENDPOINT(ep_in, &obex_fs_ep_in_desc,
		&obex_hs_ep_in_desc, NULL, NULL);
USB_COMPOSITE_ENDPOINT(ep_out, &obex_fs_ep_out_desc,
		&obex_hs_ep_out_desc, NULL, NULL);

USB_COMPOSITE_ALTSETTING(intf0alt0, &obex_control_intf);
USB_COMPOSITE_ALTSETTING(intf1alt0, &obex_data_nop_intf);
USB_COMPOSITE_ALTSETTING(intf1alt1, &obex_data_intf, &ep_in, &ep_out);

USB_COMPOSITE_INTERFACE(intf0, &intf0alt0);
USB_COMPOSITE_INTERFACE(intf1, &intf1alt0, &intf1alt1);

USB_COMPOSITE_DESCRIPTORS(obex_descs, &intf0, &intf1);

/*-------------------------------------------------------------------------*/

static int obex_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_obex		*obex = func_to_obex(f);
	struct usb_composite_dev *cdev = f->config->cdev;

	if (intf == 0) {
		/* NOP */
		dev_dbg(&cdev->gadget->dev,
			"reset obex ttyGS%d control\n", obex->port_num);
	} else if (intf == 1 && alt == 1) {
		dev_dbg(&cdev->gadget->dev,
				"activate obex ttyGS%d\n", obex->port_num);

		obex->port.in = usb_function_get_ep(f, intf, 0);
		if (!obex->port.in)
			return -ENODEV;
		obex->port.out = usb_function_get_ep(f, intf, 1);
		if (!obex->port.out)
			return -ENODEV;

		gserial_connect(&obex->port, obex->port_num);
	}

	return 0;
}

static void obex_clear_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_obex		*obex = func_to_obex(f);

	if (intf == 1 && alt == 1)
		gserial_disconnect(&obex->port);
}

/*-------------------------------------------------------------------------*/

static void obex_connect(struct gserial *g)
{
	struct f_obex		*obex = port_to_obex(g);
	struct usb_composite_dev *cdev = g->func.config->cdev;
	int			status;

	status = usb_function_activate(&g->func);
	if (status)
		dev_dbg(&cdev->gadget->dev,
			"obex ttyGS%d function activate --> %d\n",
			obex->port_num, status);
}

static void obex_disconnect(struct gserial *g)
{
	struct f_obex		*obex = port_to_obex(g);
	struct usb_composite_dev *cdev = g->func.config->cdev;
	int			status;

	status = usb_function_deactivate(&g->func);
	if (status)
		dev_dbg(&cdev->gadget->dev,
			"obex ttyGS%d function deactivate --> %d\n",
			obex->port_num, status);
}

/*-------------------------------------------------------------------------*/

/* Some controllers can't support CDC OBEX ... */
static inline bool can_support_obex(struct usb_configuration *c)
{
	/* Since the first interface is a NOP, we can ignore the
	 * issue of multi-interface support on most controllers.
	 *
	 * Altsettings are mandatory, however...
	 */
	if (!gadget_is_altset_supported(c->cdev->gadget))
		return false;

	/* everything else is *probably* fine ... */
	return true;
}

static int obex_prep_descs(struct usb_function *f)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_string	*us;

	if (!can_support_obex(f->config))
		return -EINVAL;

	us = usb_gstrings_attach(cdev, obex_strings,
				 ARRAY_SIZE(obex_string_defs));
	if (IS_ERR(us))
		return PTR_ERR(us);
	obex_control_intf.iInterface = us[OBEX_CTRL_IDX].id;
	obex_data_nop_intf.iInterface = us[OBEX_DATA_IDX].id;
	obex_data_intf.iInterface = us[OBEX_DATA_IDX].id;

	return usb_function_set_descs(f, &obex_descs);
}

static int obex_prep_vendor_descs(struct usb_function *f)
{
	struct f_obex		*obex = func_to_obex(f);
	int			intf0_id, intf1_id;

	intf0_id = usb_get_interface_id(f, 0);
	intf1_id = usb_get_interface_id(f, 1);

	obex->ctrl_id = intf0_id;
	obex->data_id = intf1_id;

	obex_cdc_union_desc.bMasterInterface0 = intf0_id;
	obex_cdc_union_desc.bSlaveInterface0 = intf1_id;

	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&obex_cdc_header_desc);
	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&obex_desc);
	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&obex_cdc_union_desc);

	return 0;
}

static inline struct f_serial_opts *to_f_serial_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_serial_opts,
			    func_inst.group);
}

static void obex_attr_release(struct config_item *item)
{
	struct f_serial_opts *opts = to_f_serial_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations obex_item_ops = {
	.release	= obex_attr_release,
};

static ssize_t f_obex_port_num_show(struct config_item *item, char *page)
{
	return sprintf(page, "%u\n", to_f_serial_opts(item)->port_num);
}

CONFIGFS_ATTR_RO(f_obex_, port_num);

static struct configfs_attribute *acm_attrs[] = {
	&f_obex_attr_port_num,
	NULL,
};

static struct config_item_type obex_func_type = {
	.ct_item_ops	= &obex_item_ops,
	.ct_attrs	= acm_attrs,
	.ct_owner	= THIS_MODULE,
};

static void obex_free_inst(struct usb_function_instance *f)
{
	struct f_serial_opts *opts;

	opts = container_of(f, struct f_serial_opts, func_inst);
	gserial_free_line(opts->port_num);
	kfree(opts);
}

static struct usb_function_instance *obex_alloc_inst(void)
{
	struct f_serial_opts *opts;
	int ret;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = obex_free_inst;
	ret = gserial_alloc_line(&opts->port_num);
	if (ret) {
		kfree(opts);
		return ERR_PTR(ret);
	}
	config_group_init_type_name(&opts->func_inst.group, "",
				    &obex_func_type);

	return &opts->func_inst;
}

static void obex_free(struct usb_function *f)
{
	struct f_obex *obex;

	obex = func_to_obex(f);
	kfree(obex);
}

static struct usb_function *obex_alloc(struct usb_function_instance *fi)
{
	struct f_obex	*obex;
	struct f_serial_opts *opts;

	/* allocate and initialize one new instance */
	obex = kzalloc(sizeof(*obex), GFP_KERNEL);
	if (!obex)
		return ERR_PTR(-ENOMEM);

	opts = container_of(fi, struct f_serial_opts, func_inst);

	obex->port_num = opts->port_num;

	obex->port.connect = obex_connect;
	obex->port.disconnect = obex_disconnect;

	obex->port.func.name = "obex";
	/* descriptors are per-instance copies */
	obex->port.func.prep_descs = obex_prep_descs;
	obex->port.func.prep_vendor_descs = obex_prep_vendor_descs;
	obex->port.func.set_alt = obex_set_alt;
	obex->port.func.clear_alt = obex_clear_alt;
	obex->port.func.free_func = obex_free;
	obex->port.func.bind_deactivated = true;

	return &obex->port.func;
}

DECLARE_USB_FUNCTION_INIT(obex, obex_alloc_inst, obex_alloc);
MODULE_AUTHOR("Felipe Balbi");
MODULE_LICENSE("GPL");
