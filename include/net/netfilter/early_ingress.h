#ifndef _NF_EARLY_INGRESS_H_
#define _NF_EARLY_INGRESS_H_

#include <net/protocol.h>

struct sk_buff *nft_skb_segment(struct sk_buff *head_skb);
struct sk_buff **nft_udp_gro_receive(struct sk_buff **head,
				     struct sk_buff *skb);
struct sk_buff **nft_tcp_gro_receive(struct sk_buff **head,
				     struct sk_buff *skb);
struct sk_buff *nft_esp_gso_segment(struct sk_buff *skb,
				    netdev_features_t features);

int nf_hook_early_ingress(struct sk_buff *skb);

void nf_early_ingress_ip_enable(void);
void nf_early_ingress_ip_disable(void);
void nf_early_ingress_ip6_enable(void);
void nf_early_ingress_ip6_disable(void);

void nf_early_ingress_enable(void);
void nf_early_ingress_disable(void);

#endif
