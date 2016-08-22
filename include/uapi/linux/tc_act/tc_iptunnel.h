/*
 * Copyright (c) 2016, Amir Vadai <amir@vadai.me>
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_TC_IPTUNNEL_H
#define __LINUX_TC_IPTUNNEL_H

#include <linux/pkt_cls.h>

#define TCA_ACT_IPTUNNEL 17

#define TCA_IPTUNNEL_ACT_ENCAP	1
#define TCA_IPTUNNEL_ACT_DECAP	2

struct tc_iptunnel {
	tc_gen;
	int t_action;
};

enum {
	TCA_IPTUNNEL_UNSPEC,
	TCA_IPTUNNEL_TM,
	TCA_IPTUNNEL_PARMS,
	TCA_IPTUNNEL_ENC_IPV4_SRC,	/* be32 */
	TCA_IPTUNNEL_ENC_IPV4_DST,	/* be32 */
	TCA_IPTUNNEL_ENC_KEY_ID,	/* be64 */
	TCA_IPTUNNEL_PAD,
	__TCA_IPTUNNEL_MAX,
};

#define TCA_IPTUNNEL_MAX (__TCA_IPTUNNEL_MAX - 1)

#endif

