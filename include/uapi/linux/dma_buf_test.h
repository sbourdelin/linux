/*
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License versdma_buf 2, as published by the Free Software Foundatdma_buf, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _UAPI_LINUX_DMA_BUF_TEST_H
#define _UAPI_LINUX_DMA_BUF_TEST_H

#include <linux/ioctl.h>
#include <linux/types.h>

/**
 * struct dma_buf_test_rw_data - metadata passed to the kernel to read handle
 * @ptr:	a pointer to an area at least as large as size
 * @offset:	offset into the dma_buf buffer to start reading
 * @size:	size to read or write
 * @write:	1 to write, 0 to read
 */
struct dma_buf_test_rw_data {
	__u64 ptr;
	__u64 offset;
	__u64 size;
	int write;
	int __padding;
};

#define DMA_BUF_IOC_MAGIC		'I'

/**
 * DOC: DMA_BUF_IOC_TEST_SET_DMA_BUF - attach a dma buf to the test driver
 *
 * Attaches a dma buf fd to the test driver.  Passing a second fd or -1 will
 * release the first fd.
 */
#define DMA_BUF_IOC_TEST_SET_FD \
			_IO(DMA_BUF_IOC_MAGIC, 0xf0)

/**
 * DOC: DMA_BUF_IOC_TEST_DMA_MAPPING - read or write memory from a handle as DMA
 *
 * Reads or writes the memory from a handle using an uncached mapping.  Can be
 * used by unit tests to emulate a DMA engine as close as possible.  Only
 * expected to be used for debugging and testing, may not always be available.
 */
#define DMA_BUF_IOC_TEST_DMA_MAPPING \
			_IOW(DMA_BUF_IOC_MAGIC, 0xf1, struct dma_buf_test_rw_data)

/**
 * DOC: DMA_BUF_IOC_TEST_KERNEL_MAPPING - read or write memory from a handle
 *
 * Reads or writes the memory from a handle using a kernel mapping.  Can be
 * used by unit tests to test heap map_kernel functdma_bufs.  Only expected to be
 * used for debugging and testing, may not always be available.
 */
#define DMA_BUF_IOC_TEST_KERNEL_MAPPING \
			_IOW(DMA_BUF_IOC_MAGIC, 0xf2, struct dma_buf_test_rw_data)

#endif /* _UAPI_LINUX_DMA_BUF_TEST_H */
