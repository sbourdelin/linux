/*
 *  User API methods for ACPI-WMI mapping driver
 *
 *  Copyright (C) 2017 Dell, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */
#ifndef _UAPI_LINUX_WMI_H
#define _UAPI_LINUX_WMI_H

#define WMI_IOC 'W'
#define WMI_IO(instance)		_IO(WMI_IOC, instance)
#define WMI_IOR(instance, type)		_IOR(WMI_IOC, instance, type)
#define WMI_IOW(instance, type)		_IOW(WMI_IOC, instance, type)
#define WMI_IOWR(instance, type)	_IOWR(WMI_IOC, instance, type)

#endif
