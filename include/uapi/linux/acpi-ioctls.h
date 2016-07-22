/*
 * ACPI IOCTL collections
 *
 * Copyright (C) 2016, Intel Corporation
 * Authors: Lv Zheng <lv.zheng@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _UAPI_LINUX_ACPI_IOCTLS_H
#define _UAPI_LINUX_ACPI_IOCTLS_H

#include <linux/ioctl.h>

#define ACPI_IOCTL_IDENT		'a'

#define ACPI_IOCTL_DEBUGGER_FLUSH	_IO(ACPI_IOCTL_IDENT, 0x80)

#endif /* _UAPI_LINUX_ACPI_IOCTLS_H */
