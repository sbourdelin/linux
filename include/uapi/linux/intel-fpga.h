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

#define FPGA_API_VERSION 0

/*
 * The IOCTL interface for Intel FPGA is designed for extensibility by
 * embedding the structure length (argsz) and flags into structures passed
 * between kernel and userspace. This design referenced the VFIO IOCTL
 * interface (include/uapi/linux/vfio.h).
 */

#define FPGA_MAGIC 0xB6

#define FPGA_BASE 0

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

#endif /* _UAPI_INTEL_FPGA_H */
