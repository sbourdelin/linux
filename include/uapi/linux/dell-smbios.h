/*
 *  User API for WMI methods for use with dell-smbios
 *
 *  Copyright (c) 2017 Dell Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#ifndef _UAPI_DELL_SMBIOS_H_
#define _UAPI_DELL_SMBIOS_H_

#include <linux/ioctl.h>
#include <linux/wmi.h>

/* This structure may be modified by the firmware when we enter
 * system management mode through SMM, hence the volatiles
 */
struct calling_interface_buffer {
	__u16 class;
	__u16 select;
	volatile __u32 input[4];
	volatile __u32 output[4];
} __packed;

struct wmi_extensions {
	__u32 argattrib;
	__u32 blength;
	__u8 data[];
} __packed;

struct wmi_smbios_buffer {
	__u64 length;
	struct calling_interface_buffer std;
	struct wmi_extensions		ext;
} __packed;

/* SMBIOS calling IOCTL command */
#define DELL_WMI_SMBIOS_CMD	WMI_IOWR(0, struct wmi_smbios_buffer)

#endif /* _UAPI_DELL_SMBIOS_H_ */
