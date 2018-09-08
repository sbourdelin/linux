/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_IO_INSTRUMENTED_H
#define _ASM_GENERIC_IO_INSTRUMENTED_H

#if defined(CONFIG_TRACING_EVENTS_IO)
#include <linux/tracepoint-defs.h>

extern struct tracepoint __tracepoint_io_write;
extern struct tracepoint __tracepoint_io_read;
#define io_tracepoint_active(t) static_key_false(&(t).key)
extern void do_trace_io_write(const char *type, void *addr);
extern void do_trace_io_read(const char *type, void *addr);
#else
#define io_tracepoint_active(t) false
static inline void do_trace_io_write(const char *type, void *addr) {}
static inline void do_trace_io_read(const char *type, void *addr) {}
#endif /* CONFIG_TRACING_EVENTS_IO */

#define __raw_write(v, a, _l)	({						\
	volatile void __iomem *_a = (a);					\
	if (io_tracepoint_active(__tracepoint_io_write))			\
		do_trace_io_write(__stringify(write##_l), (void __force *)(_a));\
	arch_raw_write##_l((v), _a);						\
	})

#define __raw_writeb(v, a)	__raw_write((v), a, b)
#define __raw_writew(v, a)	__raw_write((v), a, w)
#define __raw_writel(v, a)	__raw_write((v), a, l)
#define __raw_writeq(v, a)	__raw_write((v), a, q)

#define __raw_read(a, _l, _t)    ({						\
	_t __a;									\
	const volatile void __iomem *_a = (a);					\
	if (io_tracepoint_active(__tracepoint_io_read))				\
		do_trace_io_read(__stringify(read##_l), (void __force *)(_a));	\
	__a = arch_raw_read##_l(_a);						\
	__a;									\
	})

#define __raw_readb(a)	__raw_read((a), b, u8)
#define __raw_readw(a)	__raw_read((a), w, u16)
#define __raw_readl(a)	__raw_read((a), l, u32)
#define __raw_readq(a)	__raw_read((a), q, u64)

#endif /* _ASM_GENERIC_IO_INSTRUMENTED_H */
