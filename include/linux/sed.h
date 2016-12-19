/*
 * Self-Encrypting Drive interface - sed.h
 *
 * Copyright © 2016 Intel Corporation
 *
 * Authors:
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 *    Scott  Bauer      <scott.bauer@intel.com>
 *
 * This code is the generic layer to interface with self-encrypting
 * drives. Specific command sets should advertise support to sed uapi
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#ifndef LINUX_SED_H
#define LINUX_SED_H

#include <linux/blkdev.h>
#include <uapi/linux/sed.h>

/*
 * These constant values come from:
 * TCG Storage Architecture Core Spec v2.01 r1
 * Section: 3.3 Interface Communications
 */
enum {
	TCG_SECP_00 = 0,
	TCG_SECP_01,
};

/**
 * struct sed_context - SED Security context for a device
 * @ops:The Trusted Send/Recv functions.
 * @sec_data:Opaque pointer that will be passed to the send/recv fn.
 *Drivers can use this to pass necessary data required for
 *Their implementation of send/recv.
 * @dev:Currently an Opal Dev structure. In the future can be other types
 *Of security structures.
 */
struct sed_context {
	const struct sec_ops *ops;
	void *sec_data;
	void *dev;
};

/*
 * sec_ops - transport specific Trusted Send/Receive functions
* See SPC-4 for specific definitions
 *
 * @sec_send: sends the payload to the trusted peripheral
 *     spsp: Security Protocol Specific
 *     secp: Security Protocol
 *     buf: Payload
 *     len: Payload length
 * @recv: Receives a payload from the trusted peripheral
 *     spsp: Security Protocol Specific
 *     secp: Security Protocol
 *     buf: Payload
 *     len: Payload length
 */
struct sec_ops {
	int (*sec_send)(void *ctrl_data, u16 spsp, u8 secp, void *buf, size_t len);
	int (*sec_recv)(void *ctrl_data, u16 spsp, u8 secp, void *buf, size_t len);
};
int fdev_sed_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);

#endif /* LINUX_SED_H */
