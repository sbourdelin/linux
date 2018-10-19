/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SCSI_GENERIC_H
#define _SCSI_GENERIC_H

#include <linux/compiler.h>

/*
 * History:
 *  Started: Aug 9 by Lawrence Foard (entropy@world.std.com), to allow user
 *  process control of SCSI devices.
 *  Development Sponsored by Killy Corp. NY NY
 *
 * Original driver (sg.h):
 *   Copyright (C) 1992 Lawrence Foard
 * Version 2 and 3 extensions to driver:
 *   Copyright (C) 1998 - 2018 Douglas Gilbert
 *
 *  Version: 3.9.01 (20181016)
 *  This version is for 2.6, 3 and 4 series kernels.
 *
 * Documentation
 * =============
 * A web site for the SG device driver can be found at:
 *   http://sg.danny.cz/sg  [alternatively check the MAINTAINERS file]
 * The documentation for the sg version 3 driver can be found at:
 *   http://sg.danny.cz/sg/p/sg_v3_ho.html
 * Also see: <kernel_source>/Documentation/scsi/scsi-generic.txt
 *
 * For utility and test programs see: http://sg.danny.cz/sg/sg3_utils.html
 */

#ifdef __KERNEL__
extern int sg_big_buff; /* for sysctl */
#endif

#include <uapi/scsi/sg.h>

#ifdef __KERNEL__
#define SG_DEFAULT_TIMEOUT_USER (60*USER_HZ) /* HZ == 'jiffies in 1 second' */
#endif

#undef SG_DEFAULT_TIMEOUT	/* cause define in sg.c */

#endif	/* end of ifndef _SCSI_GENERIC_H guard */
