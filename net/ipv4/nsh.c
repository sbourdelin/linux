/*
 * Network Service Header (NSH) inserted onto encapsulated packets
 * or frames to realize service function paths.
 * NSH also provides a mechanism for metadata exchange along the
 * instantiated service path.
 *
 * https://tools.ietf.org/html/draft-ietf-sfc-nsh-01
 *
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <net/nsh.h>

static struct list_head nsh_listeners;
static DEFINE_MUTEX(nsh_listener_mutex);
static struct nsh_metadata *decap_ctx_hdrs;
static unsigned char limit_ctx_hdrs = 10;
module_param_named(nsh_hdrs, limit_ctx_hdrs, byte, 0444);
MODULE_PARM_DESC(nsh_hdrs, "Maximum NSH metadata headers per packet");

int nsh_register_listener(struct nsh_listener *listener)
{
	if (listener->max_ctx_hdrs > limit_ctx_hdrs)
		return -ENOMEM;

	mutex_lock(&nsh_listener_mutex);
	list_add(&listener->list, &nsh_listeners);
	mutex_unlock(&nsh_listener_mutex);
	return 0;
}
EXPORT_SYMBOL(nsh_register_listener);

int nsh_unregister_listener(struct nsh_listener *listener)
{
	mutex_lock(&nsh_listener_mutex);
	list_del(&listener->list);
	mutex_unlock(&nsh_listener_mutex);
	return 0;
}
EXPORT_SYMBOL(nsh_unregister_listener);

static int
notify_listeners(struct sk_buff *skb,
		 u32 service_path_id,
		 u8 service_index,
		 u8 next_proto,
		 struct nsh_metadata *ctx_hdrs,
		 unsigned int num_ctx_hdrs)
{
	struct nsh_listener *listener;
	int i, err = 0;

	mutex_lock(&nsh_listener_mutex);
	list_for_each_entry(listener, &nsh_listeners, list) {
		for (i = 0; i < num_ctx_hdrs; i++)
			if (listener->class == ctx_hdrs[i].class) {
				err = listener->notify(skb,
						       service_path_id,
						       service_index,
						       next_proto,
						       ctx_hdrs,
						       num_ctx_hdrs);
				if (err < 0) {
					mutex_unlock(&nsh_listener_mutex);
					return err;
				}
				break;
			}
	}
	mutex_unlock(&nsh_listener_mutex);
	return 0;
}

static int
type_1_decap(struct sk_buff *skb,
	     struct nsh_md_type_1 *md,
	     unsigned int max_ctx_hdrs,
	     struct nsh_metadata *ctx_hdrs,
	     unsigned int *num_ctx_hdrs)
{
	int i;
	u32 *data =  &md->ctx_hdr1;

	if (max_ctx_hdrs == 0)
		return -ENOMEM;

	ctx_hdrs[0].class = NSH_MD_CLASS_TYPE_1;
	ctx_hdrs[0].type = NSH_MD_TYPE_TYPE_1;
	ctx_hdrs[0].len = NSH_MD_LEN_TYPE_1;
	ctx_hdrs[0].data = data;

	for (i = 0; i < NSH_MD_TYPE_1_NUM_HDRS; i++, data++)
		*data = ntohl(*data);

	*num_ctx_hdrs = 1;

	return 0;
}

static int
type_2_decap(struct sk_buff *skb,
	     struct nsh_md_type_2 *md,
	     u8 md_len,
	     unsigned int max_ctx_hdrs,
	     struct nsh_metadata *ctx_hdrs,
	     unsigned int *num_ctx_hdrs)
{
	u32 *data;
	int i = 0, j;

	while (md_len > 0) {
		if (i > max_ctx_hdrs)
			return -ENOMEM;

		ctx_hdrs[i].class = ntohs(md->tlv_class);
		ctx_hdrs[i].type = md->tlv_type;
		if (ctx_hdrs[i].type & NSH_TYPE_CRIT) {
			ctx_hdrs[i].type &= ~NSH_TYPE_CRIT;
			ctx_hdrs[i].crit = 1;
		}
		ctx_hdrs[i].len = md->length;

		data = (u32 *) ++md;
		md_len--;

		ctx_hdrs[i].data = data;

		for (j = 0; j < ctx_hdrs[i].len; j++)
			data[j] = ntohl(data[j]);

		md = (struct nsh_md_type_2 *)&data[j];
		md_len -= j;
		i++;
	}
	*num_ctx_hdrs = i;

	return 0;
}

/* Parse NSH header.
 *
 * No additional memory is allocated. Context header data is pointed
 * to in the buffer payload. Context headers and skb are passed to anyone
 * who has registered interest in the class(es) of metadata received.
 *
 * Returns the total number of 4 byte words in the NSH headers, <0 on failure.
 */
int nsh_decap(struct sk_buff *skb,
	      u32 *spi,
	      u8 *si,
	      u8 *np)
{
	struct nsh_header *nsh = (struct nsh_header *)skb->data;
	struct nsh_base *base = &nsh->base;
	unsigned int max_ctx_hdrs = limit_ctx_hdrs;
	unsigned int num_ctx_hdrs;
	u32 service_path_id;
	u8 service_index;
	u8 next_proto;
	u32 sph;
	u8 md_type;
	u8 hdrlen; /* 4 byte words */
	unsigned int len; /* bytes */
	int err;

	hdrlen = base->length;
	len = hdrlen * sizeof(u32);

	if (unlikely(!pskb_may_pull(skb, len)))
		return -ENOMEM;

	skb_pull_rcsum(skb, len);

	if (((base->base_flags & NSH_BF_VER_MASK) >> 6) != NSH_BF_VER0)
		return -EINVAL;

	next_proto = base->next_proto;

	switch (next_proto) {
	case NSH_NEXT_PROTO_IPv4:
		skb->protocol = htons(ETH_P_IP);
		break;
	case NSH_NEXT_PROTO_IPv6:
		skb->protocol = htons(ETH_P_IPV6);
		break;
	case NSH_NEXT_PROTO_ETH:
		skb->protocol = htons(ETH_P_TEB);
		break;
	default:
		return -EINVAL;
	}

	if (np)
		*np = next_proto;

	md_type = base->md_type;

	switch (md_type) {
	case NSH_MD_TYPE_1:
		if (hdrlen != NSH_LEN_TYPE_1)
			return -EINVAL;
		err = type_1_decap(skb, (struct nsh_md_type_1 *) ++nsh,
				   max_ctx_hdrs, decap_ctx_hdrs, &num_ctx_hdrs);
		break;
	case NSH_MD_TYPE_2:
		if (hdrlen < NSH_LEN_TYPE_2_MIN)
			return -EINVAL;
		err = type_2_decap(skb, (struct nsh_md_type_2 *) ++nsh,
				   hdrlen - NSH_LEN_TYPE_2_MIN,
				   max_ctx_hdrs, decap_ctx_hdrs, &num_ctx_hdrs);
		break;
	default:
		return -EINVAL;
	}

	if (err < 0)
		return err;

	sph = ntohl(nsh->sp_header);
	service_path_id = (sph & NSH_SPI_MASK) >> 8;
	service_index = sph & NSH_SI_MASK;

	if (spi)
		*spi = service_path_id;
	if (si)
		*si = service_index;

	err = notify_listeners(skb, service_path_id,
			       service_index, next_proto,
			       decap_ctx_hdrs, num_ctx_hdrs);
	if (err < 0)
		return err;

	return hdrlen;
}
EXPORT_SYMBOL_GPL(nsh_decap);

static void
type_1_encap(u32 *data_out,
	     struct nsh_metadata *ctx_hdrs)
{
	int i;
	u32 *data_in = (u32 *)ctx_hdrs[0].data;

	for (i = 0; i < NSH_MD_TYPE_1_NUM_HDRS; i++)
		data_out[i] = htonl(data_in[i]);
}

static void
type_2_encap(struct nsh_md_type_2 *md,
	     unsigned int num_ctx_hdrs,
	     struct nsh_metadata *ctx_hdrs)
{
	int i, j;
	u32 *data_in, *data_out;

	for (i = 0; i < num_ctx_hdrs; i++) {
		md->tlv_class = htons(ctx_hdrs[i].class);
		md->tlv_type = ctx_hdrs[i].type;
		if (ctx_hdrs[i].crit)
			md->tlv_type |= NSH_TYPE_CRIT;
		md->length = ctx_hdrs[i].len;

		data_out = (u32 *) ++md;
		data_in = (u32 *)ctx_hdrs[i].data;

		for (j = 0; j < ctx_hdrs[i].len; j++)
			data_out[j] = htonl(data_in[j]);

		md = (struct nsh_md_type_2 *)&data_out[j];
	}
}

/* Add NSH header.
 */
int nsh_encap(struct sk_buff *skb,
	      u32 spi,
	      u8 si,
	      u8 np,
	      unsigned int num_ctx_hdrs,
	      struct nsh_metadata *ctx_hdrs)
{
	bool has_t1 = false, has_t2 = false;
	bool has_crit = false;
	unsigned int headroom = sizeof(struct nsh_header);
	struct nsh_header *nsh;
	struct nsh_base *base;
	int i;
	int err;

	if (np != NSH_NEXT_PROTO_IPv4 &&
	    np != NSH_NEXT_PROTO_IPv6 &&
	    np != NSH_NEXT_PROTO_ETH)
		return -EINVAL;

	if (spi >= NSH_N_SPI)
		return -EINVAL;

	for (i = 0; i < num_ctx_hdrs; i++) {
		if (ctx_hdrs[i].class == NSH_MD_CLASS_TYPE_1) {
			if (num_ctx_hdrs != 1)
				return -EINVAL;
			headroom += NSH_MD_LEN_TYPE_1 * sizeof(u32);
			has_t1 |= true;
		} else {
			headroom += ctx_hdrs[i].len * sizeof(u32) +
				sizeof(struct nsh_md_type_2);
			has_t2 |= true;
			has_crit |= ctx_hdrs[i].type & NSH_TYPE_CRIT;
		}

		if (has_t1 && has_t2)
			return -EINVAL;
	}

	err = skb_cow_head(skb, headroom);
	if (err)
		return err;

	nsh = (struct nsh_header *)__skb_push(skb, headroom);

	base = &nsh->base;
	base->base_flags = has_crit ? NSH_BF_CRIT : 0; /* Ver 0, OAM 0 */
	base->length = headroom / sizeof(u32);
	base->md_type = has_t1 ? NSH_MD_TYPE_1 : NSH_MD_TYPE_2;
	base->next_proto = np;

	nsh->sp_header = htonl((spi << 8) | si);

	if (has_t1)
		type_1_encap((u32 *) ++nsh, ctx_hdrs);
	else
		type_2_encap((struct nsh_md_type_2 *) ++nsh, num_ctx_hdrs,
			     ctx_hdrs);
	return 0;
}
EXPORT_SYMBOL_GPL(nsh_encap);

static int __init nsh_init(void)
{
	INIT_LIST_HEAD(&nsh_listeners);

	decap_ctx_hdrs = kmalloc_array(limit_ctx_hdrs, sizeof(*decap_ctx_hdrs),
				       GFP_KERNEL);
	if (!decap_ctx_hdrs)
		return -ENOMEM;

	return 0;
}

static void __exit nsh_exit(void)
{
	kfree(decap_ctx_hdrs);
}

module_init(nsh_init);
module_exit(nsh_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Brian Russell <brussell@brocade.com>");
MODULE_DESCRIPTION("Network Service Header Encap/Decap");
