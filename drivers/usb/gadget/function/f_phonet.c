/*
 * f_phonet.c -- USB CDC Phonet function
 *
 * Copyright (C) 2007-2008 Nokia Corporation. All rights reserved.
 *
 * Author: Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>

#include <linux/netdevice.h>
#include <linux/if_ether.h>
#include <linux/if_phonet.h>
#include <linux/if_arp.h>

#include <linux/usb/ch9.h>
#include <linux/usb/cdc.h>
#include <linux/usb/composite.h>

#include "u_phonet.h"
#include "u_ether.h"

#define PN_MEDIA_USB	0x1B
#define MAXPACKET	512
#if (PAGE_SIZE % MAXPACKET)
#error MAXPACKET must divide PAGE_SIZE!
#endif

/*-------------------------------------------------------------------------*/

struct phonet_port {
	struct f_phonet			*usb;
	spinlock_t			lock;
};

struct f_phonet {
	struct usb_function		function;
	struct {
		struct sk_buff		*skb;
		spinlock_t		lock;
	} rx;
	struct net_device		*dev;
	struct usb_ep			*in_ep, *out_ep;

	struct usb_request		*in_req;
	struct usb_request		*out_reqv[0];
};

static int phonet_rxq_size = 17;

static inline struct f_phonet *func_to_pn(struct usb_function *f)
{
	return container_of(f, struct f_phonet, function);
}

/*-------------------------------------------------------------------------*/

#define USB_CDC_SUBCLASS_PHONET	0xfe
#define USB_CDC_PHONET_TYPE	0xab

static struct usb_interface_descriptor
pn_control_intf_desc = {
	.bLength =		sizeof pn_control_intf_desc,
	.bDescriptorType =	USB_DT_INTERFACE,

	/* .bInterfaceNumber =	DYNAMIC, */
	.bInterfaceClass =	USB_CLASS_COMM,
	.bInterfaceSubClass =	USB_CDC_SUBCLASS_PHONET,
};

static const struct usb_cdc_header_desc
pn_header_desc = {
	.bLength =		sizeof pn_header_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_HEADER_TYPE,
	.bcdCDC =		cpu_to_le16(0x0110),
};

static const struct usb_cdc_header_desc
pn_phonet_desc = {
	.bLength =		sizeof pn_phonet_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_PHONET_TYPE,
	.bcdCDC =		cpu_to_le16(0x1505), /* ??? */
};

static struct usb_cdc_union_desc
pn_union_desc = {
	.bLength =		sizeof pn_union_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	USB_CDC_UNION_TYPE,

	/* .bMasterInterface0 =	DYNAMIC, */
	/* .bSlaveInterface0 =	DYNAMIC, */
};

static struct usb_interface_descriptor
pn_data_nop_intf_desc = {
	.bLength =		sizeof pn_data_nop_intf_desc,
	.bDescriptorType =	USB_DT_INTERFACE,

	/* .bInterfaceNumber =	DYNAMIC, */
	.bAlternateSetting =	0,
	.bNumEndpoints =	0,
	.bInterfaceClass =	USB_CLASS_CDC_DATA,
};

static struct usb_interface_descriptor
pn_data_intf_desc = {
	.bLength =		sizeof pn_data_intf_desc,
	.bDescriptorType =	USB_DT_INTERFACE,

	/* .bInterfaceNumber =	DYNAMIC, */
	.bAlternateSetting =	1,
	.bNumEndpoints =	2,
	.bInterfaceClass =	USB_CLASS_CDC_DATA,
};

static struct usb_endpoint_descriptor
pn_fs_sink_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor
pn_hs_sink_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(MAXPACKET),
};

static struct usb_endpoint_descriptor
pn_fs_source_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor
pn_hs_source_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	cpu_to_le16(512),
};

USB_COMPOSITE_ENDPOINT(ep_sink, &pn_fs_sink_desc,
		&pn_hs_sink_desc, NULL, NULL);
USB_COMPOSITE_ENDPOINT(ep_source, &pn_fs_source_desc,
		&pn_hs_source_desc, NULL, NULL);

USB_COMPOSITE_ALTSETTING(intf0alt0, &pn_control_intf_desc);
USB_COMPOSITE_ALTSETTING(intf1alt0, &pn_data_nop_intf_desc);
USB_COMPOSITE_ALTSETTING(intf1alt1, &pn_data_intf_desc, &ep_sink, &ep_source);

USB_COMPOSITE_INTERFACE(intf0, &intf0alt0);
USB_COMPOSITE_INTERFACE(intf1, &intf1alt0, &intf1alt1);

USB_COMPOSITE_DESCRIPTORS(phonet_descs, &intf0, &intf1);

/*-------------------------------------------------------------------------*/

static int pn_net_open(struct net_device *dev)
{
	netif_wake_queue(dev);
	return 0;
}

static int pn_net_close(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static void pn_tx_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_phonet *fp = ep->driver_data;
	struct net_device *dev = fp->dev;
	struct sk_buff *skb = req->context;

	switch (req->status) {
	case 0:
		dev->stats.tx_packets++;
		dev->stats.tx_bytes += skb->len;
		break;

	case -ESHUTDOWN: /* disconnected */
	case -ECONNRESET: /* disabled */
		dev->stats.tx_aborted_errors++;
	default:
		dev->stats.tx_errors++;
	}

	dev_kfree_skb_any(skb);
	netif_wake_queue(dev);
}

static int pn_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct phonet_port *port = netdev_priv(dev);
	struct f_phonet *fp;
	struct usb_request *req;
	unsigned long flags;

	if (skb->protocol != htons(ETH_P_PHONET))
		goto out;

	spin_lock_irqsave(&port->lock, flags);
	fp = port->usb;
	if (unlikely(!fp)) /* race with carrier loss */
		goto out_unlock;

	req = fp->in_req;
	req->buf = skb->data;
	req->length = skb->len;
	req->complete = pn_tx_complete;
	req->zero = 1;
	req->context = skb;

	if (unlikely(usb_ep_queue(fp->in_ep, req, GFP_ATOMIC)))
		goto out_unlock;

	netif_stop_queue(dev);
	skb = NULL;

out_unlock:
	spin_unlock_irqrestore(&port->lock, flags);
out:
	if (unlikely(skb)) {
		dev_kfree_skb(skb);
		dev->stats.tx_dropped++;
	}
	return NETDEV_TX_OK;
}

static int pn_net_mtu(struct net_device *dev, int new_mtu)
{
	if ((new_mtu < PHONET_MIN_MTU) || (new_mtu > PHONET_MAX_MTU))
		return -EINVAL;
	dev->mtu = new_mtu;
	return 0;
}

static const struct net_device_ops pn_netdev_ops = {
	.ndo_open	= pn_net_open,
	.ndo_stop	= pn_net_close,
	.ndo_start_xmit	= pn_net_xmit,
	.ndo_change_mtu	= pn_net_mtu,
};

static void pn_net_setup(struct net_device *dev)
{
	dev->features		= 0;
	dev->type		= ARPHRD_PHONET;
	dev->flags		= IFF_POINTOPOINT | IFF_NOARP;
	dev->mtu		= PHONET_DEV_MTU;
	dev->hard_header_len	= 1;
	dev->dev_addr[0]	= PN_MEDIA_USB;
	dev->addr_len		= 1;
	dev->tx_queue_len	= 1;

	dev->netdev_ops		= &pn_netdev_ops;
	dev->destructor		= free_netdev;
	dev->header_ops		= &phonet_header_ops;
}

/*-------------------------------------------------------------------------*/

/*
 * Queue buffer for data from the host
 */
static int
pn_rx_submit(struct f_phonet *fp, struct usb_request *req, gfp_t gfp_flags)
{
	struct page *page;
	int err;

	page = __dev_alloc_page(gfp_flags | __GFP_NOMEMALLOC);
	if (!page)
		return -ENOMEM;

	req->buf = page_address(page);
	req->length = PAGE_SIZE;
	req->context = page;

	err = usb_ep_queue(fp->out_ep, req, gfp_flags);
	if (unlikely(err))
		put_page(page);
	return err;
}

static void pn_rx_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct f_phonet *fp = ep->driver_data;
	struct net_device *dev = fp->dev;
	struct page *page = req->context;
	struct sk_buff *skb;
	unsigned long flags;
	int status = req->status;

	switch (status) {
	case 0:
		spin_lock_irqsave(&fp->rx.lock, flags);
		skb = fp->rx.skb;
		if (!skb)
			skb = fp->rx.skb = netdev_alloc_skb(dev, 12);
		if (req->actual < req->length) /* Last fragment */
			fp->rx.skb = NULL;
		spin_unlock_irqrestore(&fp->rx.lock, flags);

		if (unlikely(!skb))
			break;

		if (skb->len == 0) { /* First fragment */
			skb->protocol = htons(ETH_P_PHONET);
			skb_reset_mac_header(skb);
			/* Can't use pskb_pull() on page in IRQ */
			memcpy(skb_put(skb, 1), page_address(page), 1);
		}

		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags, page,
				skb->len <= 1, req->actual, PAGE_SIZE);
		page = NULL;

		if (req->actual < req->length) { /* Last fragment */
			skb->dev = dev;
			dev->stats.rx_packets++;
			dev->stats.rx_bytes += skb->len;

			netif_rx(skb);
		}
		break;

	/* Do not resubmit in these cases: */
	case -ESHUTDOWN: /* disconnect */
	case -ECONNABORTED: /* hw reset */
	case -ECONNRESET: /* dequeued (unlink or netif down) */
		req = NULL;
		break;

	/* Do resubmit in these cases: */
	case -EOVERFLOW: /* request buffer overflow */
		dev->stats.rx_over_errors++;
	default:
		dev->stats.rx_errors++;
		break;
	}

	if (page)
		put_page(page);
	if (req)
		pn_rx_submit(fp, req, GFP_ATOMIC);
}

/*-------------------------------------------------------------------------*/

static void __pn_reset(struct usb_function *f)
{
	struct f_phonet *fp = func_to_pn(f);
	struct net_device *dev = fp->dev;
	struct phonet_port *port = netdev_priv(dev);

	netif_carrier_off(dev);
	port->usb = NULL;

	if (fp->rx.skb) {
		dev_kfree_skb_irq(fp->rx.skb);
		fp->rx.skb = NULL;
	}
}

static int pn_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_phonet *fp = func_to_pn(f);
	int status, i;

	if (intf == 0) {
		struct net_device *dev = fp->dev;
		struct phonet_port *port = netdev_priv(dev);

		/* data intf (0: inactive, 1: active) */
		spin_lock(&port->lock);

		if (fp->in_ep->enabled)
			__pn_reset(f);

		if (alt == 1) {
			int i;

			fp->out_ep = usb_function_get_ep(f, intf, 0);
			if (!fp->out_ep)
				return -ENODEV;
			fp->in_ep = usb_function_get_ep(f, intf, 1);
			if (!fp->out_ep)
				return -ENODEV;

			port->usb = fp;
			fp->out_ep->driver_data = fp;
			fp->in_ep->driver_data = fp;

			/* Incoming USB requests */
			status = -ENOMEM;
			for (i = 0; i < phonet_rxq_size; i++) {
				struct usb_request *req;

				req = usb_ep_alloc_request(fp->out_ep, GFP_KERNEL);
				if (!req)
					goto err_req;

				req->complete = pn_rx_complete;
				fp->out_reqv[i] = req;
			}

			/* Outgoing USB requests */
			fp->in_req = usb_ep_alloc_request(fp->in_ep, GFP_KERNEL);
			if (!fp->in_req)
				goto err_req;

			netif_carrier_on(dev);
			for (i = 0; i < phonet_rxq_size; i++)
				pn_rx_submit(fp, fp->out_reqv[i], GFP_ATOMIC);
		}
		spin_unlock(&port->lock);
	}

	return 0;

err_req:
	for (i = 0; i < phonet_rxq_size && fp->out_reqv[i]; i++)
		usb_ep_free_request(fp->out_ep, fp->out_reqv[i]);
	return status;
}

static void pn_clear_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct f_phonet *fp = func_to_pn(f);
	struct phonet_port *port = netdev_priv(fp->dev);
	unsigned long flags;
	int i;

	/* remain disabled until set_alt */
	spin_lock_irqsave(&port->lock, flags);
	__pn_reset(f);
	spin_unlock_irqrestore(&port->lock, flags);

	/* We are already disconnected */
	if (fp->in_req)
		usb_ep_free_request(fp->in_ep, fp->in_req);
	for (i = 0; i < phonet_rxq_size; i++)
		if (fp->out_reqv[i])
			usb_ep_free_request(fp->out_ep, fp->out_reqv[i]);
}

/*-------------------------------------------------------------------------*/

static int pn_prep_descs(struct usb_function *f)
{
	return usb_function_set_descs(f, &phonet_descs);
}

static int pn_prep_vendor_descs(struct usb_function *f)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	int status, intf0_id, intf1_id;

	struct f_phonet_opts *phonet_opts;

	phonet_opts = container_of(f->fi, struct f_phonet_opts, func_inst);

	/*
	 * in drivers/usb/gadget/configfs.c:configfs_composite_bind()
	 * configurations are bound in sequence with list_for_each_entry,
	 * in each configuration its functions are bound in sequence
	 * with list_for_each_entry, so we assume no race condition
	 * with regard to phonet_opts->bound access
	 */
	if (!phonet_opts->bound) {
		gphonet_set_gadget(phonet_opts->net, cdev->gadget);
		status = gphonet_register_netdev(phonet_opts->net);
		if (status)
			return status;
		phonet_opts->bound = true;
	}

	intf0_id = usb_get_interface_id(f, 0);
	intf1_id = usb_get_interface_id(f, 1);

	pn_union_desc.bMasterInterface0 = intf0_id;
	pn_union_desc.bSlaveInterface0 = intf1_id;

	pn_data_intf_desc.bInterfaceNumber = intf1_id;

	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&pn_header_desc);
	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&pn_phonet_desc);
	usb_altset_add_vendor_desc(f, 0, 0,
			(struct usb_descriptor_header *)&pn_union_desc);

	return 0;
}

static inline struct f_phonet_opts *to_f_phonet_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_phonet_opts,
			func_inst.group);
}

static void phonet_attr_release(struct config_item *item)
{
	struct f_phonet_opts *opts = to_f_phonet_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations phonet_item_ops = {
	.release		= phonet_attr_release,
};

static ssize_t f_phonet_ifname_show(struct config_item *item, char *page)
{
	return gether_get_ifname(to_f_phonet_opts(item)->net, page, PAGE_SIZE);
}

CONFIGFS_ATTR_RO(f_phonet_, ifname);

static struct configfs_attribute *phonet_attrs[] = {
	&f_phonet_attr_ifname,
	NULL,
};

static struct config_item_type phonet_func_type = {
	.ct_item_ops	= &phonet_item_ops,
	.ct_attrs	= phonet_attrs,
	.ct_owner	= THIS_MODULE,
};

static void phonet_free_inst(struct usb_function_instance *f)
{
	struct f_phonet_opts *opts;

	opts = container_of(f, struct f_phonet_opts, func_inst);
	if (opts->bound)
		gphonet_cleanup(opts->net);
	else
		free_netdev(opts->net);
	kfree(opts);
}

static struct usb_function_instance *phonet_alloc_inst(void)
{
	struct f_phonet_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	opts->func_inst.free_func_inst = phonet_free_inst;
	opts->net = gphonet_setup_default();
	if (IS_ERR(opts->net)) {
		struct net_device *net = opts->net;
		kfree(opts);
		return ERR_CAST(net);
	}

	config_group_init_type_name(&opts->func_inst.group, "",
			&phonet_func_type);

	return &opts->func_inst;
}

static void phonet_free(struct usb_function *f)
{
	struct f_phonet *phonet;

	phonet = func_to_pn(f);
	kfree(phonet);
}

static struct usb_function *phonet_alloc(struct usb_function_instance *fi)
{
	struct f_phonet *fp;
	struct f_phonet_opts *opts;
	int size;

	size = sizeof(*fp) + (phonet_rxq_size * sizeof(struct usb_request *));
	fp = kzalloc(size, GFP_KERNEL);
	if (!fp)
		return ERR_PTR(-ENOMEM);

	opts = container_of(fi, struct f_phonet_opts, func_inst);

	fp->dev = opts->net;
	fp->function.name = "phonet";
	fp->function.prep_descs = pn_prep_descs;
	fp->function.prep_vendor_descs = pn_prep_vendor_descs;
	fp->function.set_alt = pn_set_alt;
	fp->function.clear_alt = pn_clear_alt;
	fp->function.free_func = phonet_free;
	spin_lock_init(&fp->rx.lock);

	return &fp->function;
}

struct net_device *gphonet_setup_default(void)
{
	struct net_device *dev;
	struct phonet_port *port;

	/* Create net device */
	dev = alloc_netdev(sizeof(*port), "upnlink%d", NET_NAME_UNKNOWN,
			   pn_net_setup);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	port = netdev_priv(dev);
	spin_lock_init(&port->lock);
	netif_carrier_off(dev);

	return dev;
}

void gphonet_set_gadget(struct net_device *net, struct usb_gadget *g)
{
	SET_NETDEV_DEV(net, &g->dev);
}

int gphonet_register_netdev(struct net_device *net)
{
	int status;

	status = register_netdev(net);
	if (status)
		free_netdev(net);

	return status;
}

void gphonet_cleanup(struct net_device *dev)
{
	unregister_netdev(dev);
}

DECLARE_USB_FUNCTION_INIT(phonet, phonet_alloc_inst, phonet_alloc);
MODULE_AUTHOR("Rémi Denis-Courmont");
MODULE_LICENSE("GPL");
