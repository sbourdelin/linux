/*
 * f_trace.c -- USB FTrace Export
 *
 * Copyright (C) 2017 Intel Corporation
 * Author: Felipe Balbi <felipe.balbi@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License v2 as
 * published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/trace.h>
#include <linux/usb.h>
#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>
#include <linux/workqueue.h>

struct usb_ftrace {
	struct trace_export ftrace;
	struct usb_function function;
	struct work_struct queue_work;
	spinlock_t lock;

	struct list_head list;
	struct list_head pending;
	struct list_head queued;

	struct usb_ep *in;

	u8 intf_id;
};
#define ftrace_to_trace(f)	(container_of((f), struct usb_ftrace, ftrace))
#define work_to_trace(w)	(container_of((w), struct usb_ftrace, queue_work))
#define to_trace(f)		(container_of((f), struct usb_ftrace, function))

#define FTRACE_REQUEST_QUEUE_LENGTH	250

static inline struct usb_request *next_request(struct list_head *list)
{
	return list_first_entry_or_null(list, struct usb_request, list);
}

struct usb_ftrace_opts {
	struct usb_function_instance func_inst;
};
#define to_opts(fi)	(container_of((fi), struct usb_ftrace_opts, func_inst))

static struct usb_interface_descriptor ftrace_intf_desc = {
	.bLength		= USB_DT_INTERFACE_SIZE,
	.bDescriptorType	= USB_DT_INTERFACE,

	.bAlternateSetting	= 0,
	.bNumEndpoints		= 1,
	.bInterfaceClass	= USB_CLASS_VENDOR_SPEC,
	.bInterfaceSubClass	= USB_SUBCLASS_VENDOR_SPEC,
};

/* Super-Speed Support */
static struct usb_endpoint_descriptor ftrace_ss_in_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,

	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize		= cpu_to_le16(1024),
};

static struct usb_ss_ep_comp_descriptor ftrace_ss_in_comp_desc = {
	.bLength		= USB_DT_SS_EP_COMP_SIZE,
	.bDescriptorType	= USB_DT_SS_ENDPOINT_COMP,

	.bMaxBurst		= 15,
};

static struct usb_descriptor_header *ftrace_ss_function[] = {
	(struct usb_descriptor_header *) &ftrace_intf_desc,
	(struct usb_descriptor_header *) &ftrace_ss_in_desc,
	(struct usb_descriptor_header *) &ftrace_ss_in_comp_desc,
	NULL,
};

/* High-Speed Support */
static struct usb_endpoint_descriptor ftrace_hs_in_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,

	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize		= cpu_to_le16(512),
};

static struct usb_descriptor_header *ftrace_hs_function[] = {
	(struct usb_descriptor_header *) &ftrace_intf_desc,
	(struct usb_descriptor_header *) &ftrace_hs_in_desc,
	NULL,
};

/* Full-Speed Support */
static struct usb_endpoint_descriptor ftrace_fs_in_desc = {
	.bLength		= USB_DT_ENDPOINT_SIZE,
	.bDescriptorType	= USB_DT_ENDPOINT,

	.bEndpointAddress	= USB_DIR_IN,
	.bmAttributes		= USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize		= cpu_to_le16(64),
};

static struct usb_descriptor_header *ftrace_fs_function[] = {
	(struct usb_descriptor_header *) &ftrace_intf_desc,
	(struct usb_descriptor_header *) &ftrace_fs_in_desc,
	NULL,
};

static struct usb_string ftrace_string_defs[] = {
	[0].s = "Linux Ftrace Export",
	{ },
};

static struct usb_gadget_strings ftrace_string_table = {
	.language		= 0x0409, /* en-US */
	.strings		= ftrace_string_defs,
};

static struct usb_gadget_strings *ftrace_strings[] = {
	&ftrace_string_table,
	NULL,
};

/* ------------------------------------------------------------------------ */

static void ftrace_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct usb_ftrace		*trace = req->context;

	kfree(req->buf);
	list_move_tail(&req->list, &trace->list);
}

static void ftrace_queue_work(struct work_struct *work)
{
	struct usb_ftrace		*trace = work_to_trace(work);
	struct usb_request		*req;
	struct usb_request		*tmp;
	struct list_head		local_list;

	spin_lock_irq(&trace->lock);
restart:
	list_replace_init(&trace->pending, &local_list);
	spin_unlock_irq(&trace->lock);

	list_for_each_entry_safe(req, tmp, &local_list, list) {
		int			ret;

		ret = usb_ep_queue(trace->in, req, GFP_KERNEL);
		if (!ret)
			list_move_tail(&req->list, &trace->queued);
	}

	spin_lock_irq(&trace->lock);
	if (!list_empty(&trace->pending))
		goto restart;
	spin_unlock_irq(&trace->lock);
}

static void notrace ftrace_write(struct trace_export *ftrace, const void *buf,
				 unsigned int len)
{
	struct usb_ftrace		*trace = ftrace_to_trace(ftrace);
	struct usb_request		*req = next_request(&trace->list);

	if (!req)
		return;

	if (!trace->in->enabled)
		return;

	req->buf = kmemdup(buf, len, GFP_ATOMIC);
	req->length = len;
	req->context = trace;
	req->complete = ftrace_complete;
	list_move_tail(&req->list, &trace->pending);

	schedule_work(&trace->queue_work);
}

/* ------------------------------------------------------------------------ */

static void ftrace_disable_endpoint(struct usb_ftrace *trace)
{
	if (trace->in->enabled)
		WARN_ON(usb_ep_disable(trace->in));
}

static int ftrace_enable_endpoint(struct usb_ftrace *trace)
{
	if (trace->in->enabled)
		return 0;

	return usb_ep_enable(trace->in);
}

static int ftrace_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct usb_ftrace		*trace = to_trace(f);
	struct usb_composite_dev	*cdev = f->config->cdev;
	int				ret;

	if (alt != 0)
		goto fail;

	if (intf != trace->intf_id)
		goto fail;

	ftrace_disable_endpoint(trace);

	if (!trace->in->desc) {
		ret = config_ep_by_speed(cdev->gadget, f, trace->in);
		if (ret) {
			trace->in->desc = NULL;
			goto fail;
		}
	}

	ret = ftrace_enable_endpoint(trace);
	if (ret)
		goto fail;

	return 0;

fail:
	return -EINVAL;
}

static int ftrace_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_composite_dev	*cdev = c->cdev;
	struct usb_ftrace		*trace = to_trace(f);
	struct usb_string		*us;
	struct usb_ep			*ep;

	int				ret;
	int				i;

	us = usb_gstrings_attach(cdev, ftrace_strings,
				 ARRAY_SIZE(ftrace_string_defs));
	if (IS_ERR(us))
		return PTR_ERR(us);

	ftrace_intf_desc.iInterface = us[0].id;

	ret = usb_interface_id(c, f);
	if (ret < 0)
		goto err0;
	trace->intf_id = ret;
	ftrace_intf_desc.bInterfaceNumber = ret;

	ep = usb_ep_autoconfig(cdev->gadget, &ftrace_fs_in_desc);
	if (!ep)
		goto err0;
	trace->in = ep;

	ftrace_hs_in_desc.bEndpointAddress = ftrace_fs_in_desc.bEndpointAddress;
	ftrace_ss_in_desc.bEndpointAddress = ftrace_fs_in_desc.bEndpointAddress;

	trace->ftrace.write = ftrace_write;

	spin_lock_init(&trace->lock);
	INIT_WORK(&trace->queue_work, ftrace_queue_work);
	INIT_LIST_HEAD(&trace->list);
	INIT_LIST_HEAD(&trace->pending);
	INIT_LIST_HEAD(&trace->queued);

	ret = usb_assign_descriptors(f, ftrace_fs_function, ftrace_hs_function,
				     ftrace_ss_function, NULL);
	if (ret)
		goto err0;

	for (i = 0; i < FTRACE_REQUEST_QUEUE_LENGTH; i++) {
		struct usb_request *req;

		req = usb_ep_alloc_request(trace->in, GFP_KERNEL);
		if (!req)
			goto err1;

		list_add_tail(&req->list, &trace->list);
	}

	ret = register_ftrace_export(&trace->ftrace);
	if (ret)
		goto err1;

	return 0;

err1:
	while (!list_empty(&trace->list)) {
		struct usb_request *req = next_request(&trace->list);

		usb_ep_free_request(trace->in, req);
		list_del(&req->list);
	}

	usb_free_all_descriptors(f);

err0:
	ERROR(cdev, "%s: can't bind --> err %d\n", f->name, ret);

	return ret;
}

static void ftrace_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_ftrace		*trace = to_trace(f);
	struct usb_request		*req;
	struct usb_request		*tmp;

	unregister_ftrace_export(&trace->ftrace);
	cancel_work_sync(&trace->queue_work);
	usb_free_all_descriptors(f);

	list_for_each_entry(req, &trace->queued, list)
		usb_ep_dequeue(trace->in, req);

	list_for_each_entry_safe(req, tmp, &trace->pending, list) {
		usb_ep_free_request(trace->in, req);
		list_del(&req->list);
	}

	list_for_each_entry_safe(req, tmp, &trace->list, list) {
		usb_ep_free_request(trace->in, req);
		list_del(&req->list);
	}
}

static void ftrace_disable(struct usb_function *f)
{
	struct usb_ftrace		*trace = to_trace(f);

	ftrace_disable_endpoint(trace);
}

static void ftrace_free_func(struct usb_function *f)
{
	kfree(to_trace(f));
}

static struct config_item_type ftrace_func_type = {
	.ct_owner		= THIS_MODULE,
};

static void ftrace_free_inst(struct usb_function_instance *fi)
{
	struct usb_ftrace_opts		*opts = to_opts(fi);

	kfree(opts);
}

static struct usb_function_instance *ftrace_alloc_inst(void)
{
	struct usb_ftrace_opts		*opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = ftrace_free_inst;

	config_group_init_type_name(&opts->func_inst.group, "",
				    &ftrace_func_type);

	return &opts->func_inst;
}

static struct usb_function *ftrace_alloc(struct usb_function_instance *fi)
{
	struct usb_ftrace		*trace;

	trace = kzalloc(sizeof(*trace), GFP_KERNEL);
	if (!trace)
		return NULL;

	trace->function.name = "ftrace";
	trace->function.bind = ftrace_bind;
	trace->function.unbind = ftrace_unbind;
	trace->function.set_alt = ftrace_set_alt;
	trace->function.disable = ftrace_disable;
	trace->function.strings = ftrace_strings;
	trace->function.free_func = ftrace_free_func;

	return &trace->function;
}

DECLARE_USB_FUNCTION_INIT(ftrace, ftrace_alloc_inst, ftrace_alloc);
MODULE_AUTHOR("Felipe Balbi <felipe.balbi@linux.intel.com>");
MODULE_LICENSE("GPL v2");
