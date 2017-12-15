// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2015-2017, Intel Corporation.

#ifndef _UAPI_LINUX_KCS_BMC_H
#define _UAPI_LINUX_KCS_BMC_H

#include <linux/ioctl.h>

#define __KCS_BMC_IOCTL_MAGIC      'K'
#define KCS_BMC_IOCTL_SMS_ATN      _IOW(__KCS_BMC_IOCTL_MAGIC, 1, unsigned long)
#define KCS_BMC_IOCTL_FORCE_ABORT  _IO(__KCS_BMC_IOCTL_MAGIC, 2)

#endif /* _UAPI_LINUX_KCS_BMC_H */
