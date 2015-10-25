/*
 * Freescale Management Complex (MC) ioclt interface
 *
 * Copyright (C) 2014 Freescale Semiconductor, Inc.
 * Author: Lijun Pan <Lijun.Pan@freescale.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#ifndef _FSL_MC_IOCTL_H_
#define _FSL_MC_IOCTL_H_

#include <linux/ioctl.h>

#define RESTOOL_IOCTL_TYPE   'R'

#define RESTOOL_DPRC_SYNC \
	_IO(RESTOOL_IOCTL_TYPE, 0x2)

#define RESTOOL_SEND_MC_COMMAND \
	_IOWR(RESTOOL_IOCTL_TYPE, 0x4, struct mc_command)

#endif /* _FSL_MC_IOCTL_H_ */
