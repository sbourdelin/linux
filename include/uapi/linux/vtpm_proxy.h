/*
 * Definitions for the VTPM proxy driver
 * Copyright (c) 2015, 2016, IBM Corporation
 * Copyright (C) 2016 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef _UAPI_LINUX_VTPM_PROXY_H
#define _UAPI_LINUX_VTPM_PROXY_H

#include <linux/types.h>
#include <linux/ioctl.h>

/**
 * enum vtpm_proxy_flags - flags for the proxy TPM
 * @VTPM_PROXY_FLAG_TPM2:	the proxy TPM uses TPM 2.0 protocol
 * @VTPM_PROXY_PREPEND_LOCALITY:locality byte prepended on each command
 */
enum vtpm_proxy_flags {
	VTPM_PROXY_FLAG_TPM2			= 1,
	VTPM_PROXY_FLAG_PREPEND_LOCALITY	= 2,
};

/**
 * struct vtpm_proxy_new_dev - parameter structure for the
 *                             %VTPM_PROXY_IOC_NEW_DEV ioctl
 * @flags:	flags for the proxy TPM
 * @tpm_num:	index of the TPM device
 * @fd:		the file descriptor used by the proxy TPM
 * @major:	the major number of the TPM device
 * @minor:	the minor number of the TPM device
 */
struct vtpm_proxy_new_dev {
	__u32 flags;         /* input */
	__u32 tpm_num;       /* output */
	__u32 fd;            /* output */
	__u32 major;         /* output */
	__u32 minor;         /* output */
};

/**
 * struct vtpm_proxy_supt_flags - parameter structure for the
 *                                %VTPM_PROXY_IOC_GET_SUPT_FLAGS ioctl
 * @flags: flags supported by the vtpm proxy driver
 */
struct vtpm_proxy_supt_flags {
	__u32 flags;         /* output */
};

#define VTPM_PROXY_IOC_NEW_DEV	_IOWR(0xa1, 0x00, struct vtpm_proxy_new_dev)
#define VTPM_PROXY_IOC_GET_SUPT_FLAGS \
				_IOR(0xa1, 0x01, struct vtpm_proxy_supt_flags)

#endif /* _UAPI_LINUX_VTPM_PROXY_H */
