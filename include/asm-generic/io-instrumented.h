/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_GENERIC_IO_INSTRUMENTED_H
#define _ASM_GENERIC_IO_INSTRUMENTED_H

#include <linux/dynamic_debug.h>

#define __raw_write(v, a, _t) ({			\
	volatile void __iomem *_a = (a);		\
	dynamic_rtb("LOGK_WRITE", (void __force *)(_a));\
	arch_raw_write##_t((v), _a);			\
	})

#define __raw_writeb(v, a)	__raw_write((v), a, b)
#define __raw_writew(v, a)	__raw_write((v), a, w)
#define __raw_writel(v, a)	__raw_write((v), a, l)
#define __raw_writeq(v, a)	__raw_write((v), a, q)

#define __raw_read(a, _l, _t)    ({					\
	_t __a;								\
	const volatile void __iomem *_a = (const volatile void __iomem *)(a);\
	dynamic_rtb("LOGK_READ", (void __force *)(_a));			\
	__a = arch_raw_read##_l(_a);					\
	__a;								\
	})

#define __raw_readb(a)	__raw_read((a), b, u8)
#define __raw_readw(a)	__raw_read((a), w, u16)
#define __raw_readl(a)	__raw_read((a), l, u32)
#define __raw_readq(a)	__raw_read((a), q, u64)

#endif
