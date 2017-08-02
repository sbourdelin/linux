/*
 * FPGA Bus Device Framework driver Header
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * This work is licensed under the terms of the GNU GPL version 2. See
 * the COPYING file in the top-level directory.
 */
#ifndef _LINUX_FPGA_DEV_H
#define _LINUX_FPGA_DEV_H

/**
 * struct fpga_dev - fpga bus device structure
 * @name: name of fpga bus device
 * @dev: fpga bus device
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
