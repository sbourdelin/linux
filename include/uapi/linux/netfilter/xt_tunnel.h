/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _XT_TUNNEL_H
#define _XT_TUNNEL_H

#include <linux/types.h>

struct xt_tunnel_mtinfo {
	__u32 key, mask;
	__u8 invert;
};

#endif /*_XT_TUNNEL_H*/
