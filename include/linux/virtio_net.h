#ifndef _LINUX_VIRTIO_NET_H
#define _LINUX_VIRTIO_NET_H

#include <linux/if_vlan.h>
#include <uapi/linux/virtio_net.h>

static inline int virtio_net_hdr_to_skb(struct sk_buff *skb,
					const struct virtio_net_hdr *hdr,
					bool little_endian)
{
	u16 start = __virtio16_to_cpu(little_endian, hdr->csum_start);

	if (hdr->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) {
		u16 off = __virtio16_to_cpu(little_endian, hdr->csum_offset);

		if (!skb_partial_csum_set(skb, start, off))
			return -EINVAL;
	}

	if (hdr->gso_type != VIRTIO_NET_HDR_GSO_NONE) {
		unsigned short gso_type = 0;

		switch (hdr->gso_type & ~VIRTIO_NET_HDR_GSO_FLAGS) {
		case VIRTIO_NET_HDR_GSO_TCPV4:
			gso_type = SKB_GSO_TCPV4;
			break;
		case VIRTIO_NET_HDR_GSO_TCPV6:
			gso_type = SKB_GSO_TCPV6;
			break;
		case VIRTIO_NET_HDR_GSO_UDP:
			gso_type = SKB_GSO_UDP;
			break;
		default:
			return -EINVAL;
		}

		if (hdr->gso_type & VIRTIO_NET_HDR_GSO_ECN)
			gso_type |= SKB_GSO_TCP_ECN;
		if (hdr->gso_type & VIRTIO_NET_HDR_GSO_UDP_TUNNEL)
			gso_type |= SKB_GSO_UDP_TUNNEL;
		if (hdr->gso_type & VIRTIO_NET_HDR_GSO_UDP_TUNNEL_CSUM)
			gso_type |= SKB_GSO_UDP_TUNNEL_CSUM;
		if (hdr->gso_type & VIRTIO_NET_HDR_GSO_TUNNEL_REMCSUM) {
			gso_type |= SKB_GSO_TUNNEL_REMCSUM;
			skb->remcsum_offload = true;
		}
		if (gso_type & (SKB_GSO_UDP_TUNNEL | SKB_GSO_UDP_TUNNEL_CSUM)) {
			u16 hdr_len = __virtio16_to_cpu(little_endian,
							hdr->hdr_len);
			skb->encapsulation = 1;
			skb_set_inner_mac_header(skb, hdr_len);
			skb_set_inner_network_header(skb, hdr_len + ETH_HLEN);
			/* XXX: What if start is not set? */
			skb_set_inner_transport_header(skb, start);
		}

		if (hdr->gso_size == 0)
			return -EINVAL;
		skb_shinfo(skb)->gso_size = __virtio16_to_cpu(little_endian,
							      hdr->gso_size);
		skb_shinfo(skb)->gso_type = gso_type;

		/* Header must be checked, and gso_segs computed. */
		skb_shinfo(skb)->gso_type |= SKB_GSO_DODGY;
		skb_shinfo(skb)->gso_segs = 0;
	}

	return 0;
}

static inline int virtio_net_hdr_from_skb(const struct sk_buff *skb,
					  struct virtio_net_hdr *hdr,
					  bool little_endian)
{
	memset(hdr, 0, sizeof(*hdr));

	if (skb_is_gso(skb)) {
		struct skb_shared_info *sinfo = skb_shinfo(skb);

		/* This is a hint as to how much should be linear. */
		u16 hdr_len = skb_headlen(skb);

		hdr->gso_size = __cpu_to_virtio16(little_endian,
						  sinfo->gso_size);
		if (sinfo->gso_type & SKB_GSO_TCPV4)
			hdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
		else if (sinfo->gso_type & SKB_GSO_TCPV6)
			hdr->gso_type = VIRTIO_NET_HDR_GSO_TCPV6;
		else if (sinfo->gso_type & SKB_GSO_UDP)
			hdr->gso_type = VIRTIO_NET_HDR_GSO_UDP;
		else
			return -EINVAL;
		if (sinfo->gso_type & SKB_GSO_TCP_ECN)
			hdr->gso_type |= VIRTIO_NET_HDR_GSO_ECN;
		if (sinfo->gso_type & SKB_GSO_UDP_TUNNEL)
			hdr->gso_type |= VIRTIO_NET_HDR_GSO_UDP_TUNNEL;
		if (sinfo->gso_type & SKB_GSO_UDP_TUNNEL_CSUM)
			hdr->gso_type |= VIRTIO_NET_HDR_GSO_UDP_TUNNEL_CSUM;
		if (sinfo->gso_type & SKB_GSO_TUNNEL_REMCSUM)
			hdr->gso_type |= VIRTIO_NET_HDR_GSO_TUNNEL_REMCSUM;

		if (sinfo->gso_type &
		    (SKB_GSO_UDP_TUNNEL | SKB_GSO_UDP_TUNNEL_CSUM))
			/* For encapsulated packets 'hdr_len' is the offset to
			 * the beginning of the inner packet.  This way the
			 * encapsulation can remain ignorant of the size of the
			 * UDP tunnel header.
			 */
			hdr_len = skb_inner_mac_offset(skb);
		hdr->hdr_len = __cpu_to_virtio16(little_endian, hdr_len);
	} else
		hdr->gso_type = VIRTIO_NET_HDR_GSO_NONE;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		hdr->flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
		if (skb_vlan_tag_present(skb))
			hdr->csum_start = __cpu_to_virtio16(little_endian,
				skb_checksum_start_offset(skb) + VLAN_HLEN);
		else
			hdr->csum_start = __cpu_to_virtio16(little_endian,
				skb_checksum_start_offset(skb));
		hdr->csum_offset = __cpu_to_virtio16(little_endian,
				skb->csum_offset);
	} else if (skb->ip_summed == CHECKSUM_UNNECESSARY) {
		hdr->flags = VIRTIO_NET_HDR_F_DATA_VALID;
	} /* else everything is zero */

	return 0;
}

#endif /* _LINUX_VIRTIO_BYTEORDER */
