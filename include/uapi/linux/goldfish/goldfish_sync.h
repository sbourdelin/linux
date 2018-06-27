/*
 * Copyright (C) 2018 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef UAPI_GOLDFISH_SYNC_H
#define UAPI_GOLDFISH_SYNC_H

#include <linux/types.h>

/* GOLDFISH SYNC
 *
 * The Goldfish sync driver is designed to provide a interface
 * between the underlying host's sync device and the kernel's
 * fence sync framework.
 *
 * The purpose of the device/driver is to enable lightweight creation
 * and signaling of timelines and fences in order to synchronize the
 * guest with host-side graphics events.
 */

struct goldfish_sync_ioctl_info {
	__u64 host_glsync_handle_in;
	__u64 host_syncthread_handle_in;
	__s32 fence_fd_out;
};

/* There is an ioctl associated with goldfish sync driver.
 * Make it conflict with ioctls that are not likely to be used
 * in the emulator.
 *
 * '@'	00-0F	linux/radeonfb.h		conflict!
 * '@'	00-0F	drivers/video/aty/aty128fb.c	conflict!
 */
#define GOLDFISH_SYNC_IOC_MAGIC	'@'

#define GOLDFISH_SYNC_IOC_QUEUE_WORK \
	_IOWR(GOLDFISH_SYNC_IOC_MAGIC, 0, struct goldfish_sync_ioctl_info)

#endif /* UAPI_GOLDFISH_SYNC_H */
