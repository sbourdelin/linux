/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2018 Archronix Corp. All Rights Reserved.
 *
 */

#ifndef _UAPILINUX_PROFINET_H_
#define _UAPILINUX_PROFINET_H_

#include <asm/types.h>
#include <linux/hms-common.h>

#define PROFI_CFG_STRLEN	64

struct ProfinetConfig {
	struct {
		/* addresses IN NETWORK ORDER! */
		__u32 ip_addr;
		__u32 subnet_msk;
		__u32 gateway_addr;
		__u8  is_valid:1;
	} eth;
	struct {
		__u16 vendorid, deviceid;
		__u8  is_valid:1;
	} dev_id;
	struct {
		char name[PROFI_CFG_STRLEN];
		__u8 is_valid:1;
	} station_name;
	struct {
		char name[PROFI_CFG_STRLEN];
		__u8 is_valid:1;
	} station_type;
	struct {
		__u8 addr[6];
		__u8 is_valid:1;
	} mac_addr;
	struct {
		char hostname[PROFI_CFG_STRLEN];
		char domainname[PROFI_CFG_STRLEN];
		__u8 is_valid:1;
	} host_domain;
	struct {
		__u8 enable:1;
		__u8 is_valid:1;
	} hicp;
	struct {
		__u8 enable:1;
		__u8 is_valid:1;
	} web_server;
	struct {
		__u8 disable:1;
	} ftp_server;
	struct {
		__u8 enable:1;
	} global_admin_mode;
	struct {
		__u8 disable:1;
	} vfs;
	struct {
		/* one of HMS_SMA_CLEAR/FREEZE/SET */
		int action;
		__u8 is_valid:1;
	} stop_mode;
	struct {
		char description[PROFI_CFG_STRLEN];
		__u8 is_valid:1;
	} snmp_system_descr;
	struct {
		char description[PROFI_CFG_STRLEN];
		__u8 is_valid:1;
	} snmp_iface_descr;
	struct {
		char description[PROFI_CFG_STRLEN];
		__u8 is_valid:1;
	} mib2_system_descr;
	struct {
		char contact[PROFI_CFG_STRLEN];
		__u8 is_valid:1;
	} mib2_system_contact;
	struct {
		char location[PROFI_CFG_STRLEN];
		__u8 is_valid:1;
	} mib2_system_location;
	/*
	 * use non-volatile defaults for any properties not specified.
	 * when in doubt, keep this OFF.
	 */
	__u8 use_nv_defaults:1;
};

#define PROFINET_IOC_MAGIC 'l'

/*
 * Configures profinet according to the ProfinetConfig structure, and
 * switches the card on if it was previously off.
 */
#define PROFINET_IOCSETCONFIG   _IOW(PROFINET_IOC_MAGIC, 0x80,\
						struct ProfinetConfig)

#endif /* _UAPILINUX_PROFINET_H_ */
