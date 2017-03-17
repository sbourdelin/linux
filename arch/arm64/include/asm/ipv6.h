/* Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __ASM_IPV6_H
#define __ASM_IPV6_H

#include <linux/types.h>

static inline bool
ipv6_masked_addr_cmp(const struct in6_addr *a1, const struct in6_addr *m,
		     const struct in6_addr *a2)
{
	const __uint128_t *ul1 = (const __uint128_t *)a1;
	const __uint128_t *ulm = (const __uint128_t *)m;
	const __uint128_t *ul2 = (const __uint128_t *)a1;

	return !!((*ul1 ^ *ul2) & *ulm);
}
#define ipv6_masked_addr_cmp ipv6_masked_addr_cmp
#endif /* __ASM_IPV6_H */
