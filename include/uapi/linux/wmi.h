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

#include <linux/ioctl.h>
#include <linux/types.h>

/* WMI bus will filter all WMI vendor driver requests through this IOC */
#define WMI_IOC 'W'

/* All ioctl requests through WMI should declare their size followed by
 * relevant data objects
 */
struct wmi_ioctl_buffer {
	__u64	length;
	__u8	data[];
};

/* This structure may be modified by the firmware when we enter
 * system management mode through SMM, hence the volatiles
 */
struct calling_interface_buffer {
	__u16 class;
	__u16 select;
	volatile __u32 input[4];
	volatile __u32 output[4];
} __packed;

struct dell_wmi_extensions {
	__u32 argattrib;
	__u32 blength;
	__u8 data[];
} __packed;

struct dell_wmi_smbios_buffer {
	__u64 length;
	struct calling_interface_buffer std;
	struct dell_wmi_extensions	ext;
} __packed;

/* Dell SMBIOS calling IOCTL command used by dell-smbios-wmi */
#define DELL_WMI_SMBIOS_CMD	_IOWR(WMI_IOC, 0, struct dell_wmi_smbios_buffer)

#endif
