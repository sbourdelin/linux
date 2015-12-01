/*
 * xfrm4_mode_transport.c - Transport mode encapsulation for IPv4.
 *
 * Copyright (c) 2004-2006 Herbert Xu <herbert@gondor.apana.org.au>
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/stringify.h>
#include <net/dst.h>
#include <net/ip.h>
#include <net/xfrm.h>

#ifdef XFRM_GSO
/* 
 * when we come here, we have
 * mac_header pointing to start of ether addr. This is also skb->data
 * ip_hdr/network_header pointing to start of IP header (14 bytes after
 *                       mac header. 
 * transport header points at ip_hdr + ihl.
 * Unfortunately, esp_output overloads mac_header to use it as a pointer
 * to the ip_proto field (which will get over-written by IPPROTO_ESP
 * in esp_output).
 * We should really pullup mac and ip header fields and leave some room
 * for the esp header. Actually we should not be doing any move at all.
 * This is a mess.
 */
static int xfrm4_transport_output_gso(struct xfrm_state *x, struct sk_buff *skb)
{
	struct iphdr *iph = ip_hdr(skb);
	int ihl = iph->ihl * 4;
	int iph_off = (unsigned char *)iph - (unsigned char *)skb->data;
	unsigned char *data = skb_mac_header(skb);

	skb->network_header -= x->props.header_len;
	skb->transport_header = skb->network_header + ihl;
	skb->mac_header -= x->props.header_len;

	__skb_pull(skb, ihl + iph_off);
	memmove(skb_mac_header(skb), data, ihl + iph_off);

	/* This is a mess */
	skb->mac_header = skb->network_header +
			  offsetof(struct iphdr, protocol);
	return 0;
}
#endif /* XFRM_GSO */

/* Add encapsulation header.
 *
 * The IP header will be moved forward to make space for the encapsulation
 * header.
 */
static int xfrm4_transport_output(struct xfrm_state *x, struct sk_buff *skb)
{
	struct iphdr *iph = ip_hdr(skb);
	int ihl = iph->ihl * 4;
	int iph_off = (unsigned char *)iph - (unsigned char *)skb->data;

#ifdef XFRM_GSO
	if (skb->recirc)
		return xfrm4_transport_output_gso(x, skb);
#endif /* XFRM_GSO */

	/* move network/ip_hdr back by esp hdr size */
	skb_set_network_header(skb, -x->props.header_len);
	/* make mac_header point to ip_proto field in the
	 * new location of ip_hdr
	 */
	skb->mac_header = skb->network_header +
			  offsetof(struct iphdr, protocol);
	/* make transport_hdr point to tcp payload
	 * in the new location. This is where the esp hdr will go
	 */
	skb->transport_header = skb->network_header + ihl;
	/* move up the skb->data to go past ip hdr to tcp hdr.
	 * This reduces the len by the ip header len */
	__skb_pull(skb, ihl);
	/* copy the ip hdr over to new location */
	memmove(skb_network_header(skb), iph, ihl);
	return 0;
}

/* Remove encapsulation header.
 *
 * The IP header will be moved over the top of the encapsulation header.
 *
 * On entry, skb->h shall point to where the IP header should be and skb->nh
 * shall be set to where the IP header currently is.  skb->data shall point
 * to the start of the payload.
 */
static int xfrm4_transport_input(struct xfrm_state *x, struct sk_buff *skb)
{
	int ihl = skb->data - skb_transport_header(skb);

	if (skb->transport_header != skb->network_header) {
		memmove(skb_transport_header(skb),
			skb_network_header(skb), ihl);
		skb->network_header = skb->transport_header;
	}
	ip_hdr(skb)->tot_len = htons(skb->len + ihl);
	skb_reset_transport_header(skb);
	return 0;
}

static struct xfrm_mode xfrm4_transport_mode = {
	.input = xfrm4_transport_input,
	.output = xfrm4_transport_output,
	.owner = THIS_MODULE,
	.encap = XFRM_MODE_TRANSPORT,
};

static int __init xfrm4_transport_init(void)
{
	return xfrm_register_mode(&xfrm4_transport_mode, AF_INET);
}

static void __exit xfrm4_transport_exit(void)
{
	int err;

	err = xfrm_unregister_mode(&xfrm4_transport_mode, AF_INET);
	BUG_ON(err);
}

module_init(xfrm4_transport_init);
module_exit(xfrm4_transport_exit);
MODULE_LICENSE("GPL");
MODULE_ALIAS_XFRM_MODE(AF_INET, XFRM_MODE_TRANSPORT);
