/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef _IP6T_SEG6_H
#define _IP6T_SEG6_H

#include <linux/types.h>

/* SEG6 action options */
enum ip6t_seg6_action {
	IP6T_SEG6_GO_NEXT,
	IP6T_SEG6_SKIP_NEXT,
	IP6T_SEG6_GO_LAST,
	IP6T_SEG6_BSID,
};

struct ip6t_seg6_info {
	__u32			action; /* SEG6 action */
	struct	in6_addr	bsid;	/* SRv6 Bind SID */
	unsigned int		tbl;	/* Routing table of bsid */
};

#endif /*_IP6T_SEG6_H*/
