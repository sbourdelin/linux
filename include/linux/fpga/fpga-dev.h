/*
 * FPGA Device Driver Header
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * This work is licensed under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license. See the
 * LICENSE.BSD file under drivers/fpga/intel for the BSD license and see
 * the COPYING file in the top-level directory for the GPLv2 license.
 *
 */
#ifndef _LINUX_FPGA_DEV_H
#define _LINUX_FPGA_DEV_H

/**
 * struct fpga_dev - fpga device structure
 * @name: name of fpga device
 * @dev: fpga device
 */
struct fpga_dev {
	const char *name;
	struct device dev;
};

#define to_fpga_dev(d) container_of(d, struct fpga_dev, dev)

struct fpga_dev *fpga_dev_create(struct device *parent, const char *name);

static inline void fpga_dev_destroy(struct fpga_dev *fdev)
{
	device_unregister(&fdev->dev);
}

#endif
