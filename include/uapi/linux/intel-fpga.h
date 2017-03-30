/*
 * Header File for Intel FPGA User API
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * Authors:
 *   Kang Luwei <luwei.kang@intel.com>
 *   Zhang Yi <yi.z.zhang@intel.com>
 *   Wu Hao <hao.wu@intel.com>
 *   Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *
 * This work is licensed under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license. See the
 * LICENSE.BSD file under drivers/fpga/intel for the BSD license and see
 * the COPYING file in the top-level directory for the GPLv2 license.
 */

#ifndef _UAPI_LINUX_INTEL_FPGA_H
#define _UAPI_LINUX_INTEL_FPGA_H

#include <linux/types.h>

#define FPGA_API_VERSION 0

/*
 * The IOCTL interface for Intel FPGA is designed for extensibility by
 * embedding the structure length (argsz) and flags into structures passed
 * between kernel and userspace. This design referenced the VFIO IOCTL
 * interface (include/uapi/linux/vfio.h).
 */

#define FPGA_MAGIC 0xB6

#define FPGA_BASE 0
#define FME_BASE 0x80

/**
 * FPGA_GET_API_VERSION - _IO(FPGA_MAGIC, FPGA_BASE + 0)
 *
 * Report the version of the driver API.
 * Return: Driver API Version.
 */

#define FPGA_GET_API_VERSION	_IO(FPGA_MAGIC, FPGA_BASE + 0)

/**
 * FPGA_CHECK_EXTENSION - _IO(FPGA_MAGIC, FPGA_BASE + 1)
 *
 * Check whether an extension is supported.
 * Return: 0 if not supported, otherwise the extension is supported.
 */

#define FPGA_CHECK_EXTENSION	_IO(FPGA_MAGIC, FPGA_BASE + 1)

/* IOCTLs for FME file descriptor */

/**
 * FPGA_FME_PORT_PR - _IOWR(FPGA_MAGIC, FME_BASE + 0, struct fpga_fme_port_pr)
 *
 * Driver does Partial Reconfiguration based on Port ID and Buffer (Image)
 * provided by caller.
 * Return: 0 on success, -errno on failure.
 * If FPGA_FME_PORT_PR returns -EIO, that indicates the HW has detected
 * some errors during PR, under this case, the user can fetch HW error code
 * from fpga_fme_port_pr.status. Each bit on the error code is used as the
 * index for the array created by DEFINE_FPGA_PR_ERR_MSG().
 * Otherwise, it is always zero.
 */

#define DEFINE_FPGA_PR_ERR_MSG(_name_)			\
static const char * const _name_[] = {			\
	"PR operation error detected",			\
	"PR CRC error detected",			\
	"PR incompatiable bitstream error detected",	\
	"PR IP protocol error detected",		\
	"PR FIFO overflow error detected",		\
	"Reserved",					\
	"PR secure load error detected",		\
}

#define PR_MAX_ERR_NUM	7

struct fpga_fme_port_pr {
	/* Input */
	__u32 argsz;		/* Structure length */
	__u32 flags;		/* Zero for now */
	__u32 port_id;
	__u32 buffer_size;
	__u64 buffer_address;	/* Userspace address to the buffer for PR */
	/* Output */
	__u64 status;		/* HW error code if ioctl returns -EIO */
};

#define FPGA_FME_PORT_PR	_IO(FPGA_MAGIC, FME_BASE + 0)

#endif /* _UAPI_INTEL_FPGA_H */
