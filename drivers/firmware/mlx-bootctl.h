/*
 *  Mellanox boot control driver
 *  These definitions are shared with the ATF source code.
 *
 *  Copyright (C) 2017 Mellanox Technologies.  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License v2.0 as published by
 *  the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef __BLUEFIELD_SVC_H__
#define __BLUEFIELD_SVC_H__

/* BlueField-specific SMC function IDs */

/*
 * Request that the on-chip watchdog be enabled, or disabled, after
 * the next chip soft reset. This call does not affect the current
 * status of the on-chip watchdog. If non-zero, the argument
 * specifies the watchdog interval in seconds. If zero, the watchdog
 * will not be enabled after the next soft reset. Non-zero errors are
 * returned as documented below.
 */
#define MLNX_SET_POST_RESET_WDOG	0x82000000

/*
 * Query the status which has been requested for the on-chip watchdog
 * after the next chip soft reset. Returns the interval as set by
 * MLNX_SET_POST_RESET_WDOG.
 */
#define MLNX_GET_POST_RESET_WDOG	0x82000001

/*
 * Request that a specific boot action be taken at the next soft
 * reset. By default, the boot action is set by external chip pins,
 * which are sampled on hard reset. Note that the boot action
 * requested by this call will persist on subsequent resets unless
 * this service, or the MLNX_SET_SECOND_RESET_ACTION service, is
 * invoked. See below for the available MLNX_BOOT_xxx parameter
 * values. Non-zero errors are returned as documented below.
 */
#define MLNX_SET_RESET_ACTION		0x82000002

/*
 * Return the specific boot action which will be taken at the next
 * soft reset. Returns the reset action (see below for the parameter
 * values for MLNX_SET_RESET_ACTION).
 */
#define MLNX_GET_RESET_ACTION		0x82000003

/*
 * Request that a specific boot action be taken at the soft reset
 * after the next soft reset. For a specified valid boot mode, the
 * effect of this call is identical to that of invoking
 * MLNX_SET_RESET_ACTION after the next chip soft reset; in
 * particular, after that reset, the action for the now next reset can
 * be queried with MLNX_GET_RESET_ACTION and modified with
 * MLNX_SET_RESET_ACTION. You may also specify the parameter as
 * MLNX_BOOT_NONE, which is equivalent to specifying that no call to
 * MLNX_SET_RESET_ACTION be taken after the next chip soft reset.
 * This call does not affect the action to be taken at the next soft
 * reset. Non-zero errors are returned as documented below.
 */
#define MLNX_SET_SECOND_RESET_ACTION	0x82000004

/*
 * Return the specific boot action which will be taken at the soft
 * reset after the next soft reset; this will be one of the valid
 * actions for MLNX_SET_SECOND_RESET_ACTION.
 */
#define MLNX_GET_SECOND_RESET_ACTION	0x82000005

/* SMC function IDs for SiP Service queries */
#define MLNX_SIP_SVC_CALL_COUNT		0x8200ff00
#define MLNX_SIP_SVC_UID		0x8200ff01
#define MLNX_SIP_SVC_VERSION		0x8200ff03

/* ARM Standard Service Calls version numbers */
#define MLNX_SVC_VERSION_MAJOR		0x0
#define MLNX_SVC_VERSION_MINOR		0x1

/* Number of svc calls defined. */
#define MLNX_NUM_SVC_CALLS 9

/* Valid reset actions for MLNX_SET_RESET_ACTION. */
#define MLNX_BOOT_EXTERNAL	0 /* Do not boot from eMMC */
#define MLNX_BOOT_EMMC		1 /* Boot from primary eMMC boot partition */
#define MLNX_BOOT_SWAP_EMMC	2 /* Swap eMMC boot partitions and reboot */
#define MLNX_BOOT_EMMC_LEGACY	3 /* Boot from primary eMMC in legacy mode */

/* Additional parameter value to disable the MLNX_SET_SECOND_RESET_ACTION. */
#define MLNX_BOOT_NONE		0x7fffffff /* Don't change next boot action */

/* Error values (non-zero). */
#define SMCCC_INVALID_PARAMETERS	-2

#endif /* __BLUEFIELD_SVC_H__ */
