/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __LINUX_PROCFD_H
#define __LINUX_PROCFD_H

#include <linux/ioctl.h>

/* Returns a file descriptor that refers to a struct pid */
#define PROC_FD_SIGNAL		_IOW('p', 1, __s32)

#endif /* __LINUX_PROCFD_H */

