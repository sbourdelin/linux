#ifndef _LINUX_KCOV_IOCTLS_H
#define _LINUX_KCOV_IOCTLS_H

#include <linux/types.h>

struct kcov_init_trace {
	unsigned long	flags;		/* In: reserved, must be 0. */
	unsigned long	size;		/* In: trace buffer size. */
	unsigned long	version;	/* Out: trace format, currently 1. */
	/*
	 * Output.
	 * pc_size can be 4 or 8. If pc_size = 4 on a 64-bit arch,
	 * returned PCs are compressed by subtracting pc_base and then
	 * truncating to 4 bytes.
	 */
	unsigned long	pc_size;
	unsigned long	pc_base;
};

#define KCOV_INIT_TRACE			_IOWR('c', 1, struct kcov_init_trace)
#define KCOV_ENABLE			_IO('c', 100)
#define KCOV_DISABLE			_IO('c', 101)

#endif /* _LINUX_KCOV_IOCTLS_H */
