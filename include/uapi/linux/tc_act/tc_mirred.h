#ifndef __LINUX_TC_MIR_H
#define __LINUX_TC_MIR_H

#include <linux/types.h>
#include <linux/pkt_cls.h>

#define TCA_ACT_MIRRED 8
#define TCA_EGRESS_REDIR 1  /* packet redirect to EGRESS*/
#define TCA_EGRESS_MIRROR 2 /* mirror packet to EGRESS */
#define TCA_INGRESS_REDIR 3  /* packet redirect to INGRESS*/
#define TCA_INGRESS_MIRROR 4 /* mirror packet to INGRESS */

#define MIRRED_F_TC_MAP		0x1
#define MIRRED_TC_MAP_MAX	0x10
#define MIRRED_TC_MAP_MASK	0xF
                                                                                
struct tc_mirred {
	tc_gen;
	int                     eaction;   /* one of IN/EGRESS_MIRROR/REDIR */
	__u32                   ifindex;  /* ifindex of egress port */
};
                                                                                
enum {
	TCA_MIRRED_UNSPEC,
	TCA_MIRRED_TM,
	TCA_MIRRED_PARMS,
	TCA_MIRRED_PAD,
	TCA_MIRRED_TC_MAP,
	__TCA_MIRRED_MAX
};
#define TCA_MIRRED_MAX (__TCA_MIRRED_MAX - 1)
                                                                                
#endif
