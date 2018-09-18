// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/libgcc.h>

extern unsigned long long __udivmoddi4(unsigned long long u,
				       unsigned long long v,
				       unsigned long long *rp);

unsigned long long __umoddi3(unsigned long long u, unsigned long long v)
{
	unsigned long long w;
	(void)__udivmoddi4(u, v, &w);
	return w;
}
EXPORT_SYMBOL(__umoddi3);
