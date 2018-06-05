/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * cros_ec_dev - Expose the Chrome OS Embedded Controller to userspace
 *
 * Copyright (C) 2014 Google, Inc.
 */

#ifndef _CROS_EC_DEV_H_
#define _CROS_EC_DEV_H_

#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/mfd/cros_ec.h>

#define CROS_EC_DEV_VERSION "1.0.0"

/*
 * @offset: within EC_LPC_ADDR_MEMMAP region
 * @bytes: number of bytes to read. zero means "read a string" (including '\0')
 *         (at most only EC_MEMMAP_SIZE bytes can be read)
 * @buffer: where to store the result
 * ioctl returns the number of bytes read, negative on error
 */
struct cros_ec_readmem {
	uint32_t offset;
	uint32_t bytes;
	uint8_t buffer[EC_MEMMAP_SIZE];
};

#define CROS_EC_DEV_IOC       0xEC
#define CROS_EC_DEV_IOCXCMD   _IOWR(CROS_EC_DEV_IOC, 0, struct cros_ec_command)
#define CROS_EC_DEV_IOCRDMEM  _IOWR(CROS_EC_DEV_IOC, 1, struct cros_ec_readmem)

/* Lightbar utilities */
extern bool ec_has_lightbar(struct cros_ec_dev *ec);
extern int lb_manual_suspend_ctrl(struct cros_ec_dev *ec, uint8_t enable);
extern int lb_suspend(struct cros_ec_dev *ec);
extern int lb_resume(struct cros_ec_dev *ec);

#endif /* _CROS_EC_DEV_H_ */
