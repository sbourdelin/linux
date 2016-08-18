#ifndef _NET_SECURE_SEQ
#define _NET_SECURE_SEQ

#include <linux/types.h>

struct secure_tcp_seq {
	u32 seq;
	u32 tsoff;
};

u32 secure_ipv4_port_ephemeral(__be32 saddr, __be32 daddr, __be16 dport);
u32 secure_ipv6_port_ephemeral(const __be32 *saddr, const __be32 *daddr,
			       __be16 dport);
struct secure_tcp_seq secure_tcp_sequence_number(__be32 saddr, __be32 daddr,
						 __be16 sport, __be16 dport);
struct secure_tcp_seq secure_tcpv6_sequence_number(const __be32 *saddr, const __be32 *daddr,
						    __be16 sport, __be16 dport);
u64 secure_dccp_sequence_number(__be32 saddr, __be32 daddr,
				__be16 sport, __be16 dport);
u64 secure_dccpv6_sequence_number(__be32 *saddr, __be32 *daddr,
				  __be16 sport, __be16 dport);

#endif /* _NET_SECURE_SEQ */
