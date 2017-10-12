/*
 * include/uapi/linux/devlink.h - Network physical device Netlink interface
 * Copyright (c) 2016 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016 Jiri Pirko <jiri@mellanox.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _UAPI_LINUX_DEVLINK_H_
#define _UAPI_LINUX_DEVLINK_H_

#define DEVLINK_GENL_NAME "devlink"
#define DEVLINK_GENL_VERSION 0x1
#define DEVLINK_GENL_MCGRP_CONFIG_NAME "config"

enum devlink_command {
	/* don't change the order or add anything between, this is ABI! */
	DEVLINK_CMD_UNSPEC,

	DEVLINK_CMD_GET,		/* can dump */
	DEVLINK_CMD_SET,
	DEVLINK_CMD_NEW,
	DEVLINK_CMD_DEL,

	DEVLINK_CMD_PORT_GET,		/* can dump */
	DEVLINK_CMD_PORT_SET,
	DEVLINK_CMD_PORT_NEW,
	DEVLINK_CMD_PORT_DEL,

	DEVLINK_CMD_PORT_SPLIT,
	DEVLINK_CMD_PORT_UNSPLIT,

	DEVLINK_CMD_SB_GET,		/* can dump */
	DEVLINK_CMD_SB_SET,
	DEVLINK_CMD_SB_NEW,
	DEVLINK_CMD_SB_DEL,

	DEVLINK_CMD_SB_POOL_GET,	/* can dump */
	DEVLINK_CMD_SB_POOL_SET,
	DEVLINK_CMD_SB_POOL_NEW,
	DEVLINK_CMD_SB_POOL_DEL,

	DEVLINK_CMD_SB_PORT_POOL_GET,	/* can dump */
	DEVLINK_CMD_SB_PORT_POOL_SET,
	DEVLINK_CMD_SB_PORT_POOL_NEW,
	DEVLINK_CMD_SB_PORT_POOL_DEL,

	DEVLINK_CMD_SB_TC_POOL_BIND_GET,	/* can dump */
	DEVLINK_CMD_SB_TC_POOL_BIND_SET,
	DEVLINK_CMD_SB_TC_POOL_BIND_NEW,
	DEVLINK_CMD_SB_TC_POOL_BIND_DEL,

	/* Shared buffer occupancy monitoring commands */
	DEVLINK_CMD_SB_OCC_SNAPSHOT,
	DEVLINK_CMD_SB_OCC_MAX_CLEAR,

	DEVLINK_CMD_ESWITCH_GET,
#define DEVLINK_CMD_ESWITCH_MODE_GET /* obsolete, never use this! */ \
	DEVLINK_CMD_ESWITCH_GET

	DEVLINK_CMD_ESWITCH_SET,
#define DEVLINK_CMD_ESWITCH_MODE_SET /* obsolete, never use this! */ \
	DEVLINK_CMD_ESWITCH_SET

	DEVLINK_CMD_DPIPE_TABLE_GET,
	DEVLINK_CMD_DPIPE_ENTRIES_GET,
	DEVLINK_CMD_DPIPE_HEADERS_GET,
	DEVLINK_CMD_DPIPE_TABLE_COUNTERS_SET,

	DEVLINK_CMD_CONFIG_GET,
	DEVLINK_CMD_CONFIG_SET,

	/* add new commands above here */
	__DEVLINK_CMD_MAX,
	DEVLINK_CMD_MAX = __DEVLINK_CMD_MAX - 1
};

enum devlink_port_type {
	DEVLINK_PORT_TYPE_NOTSET,
	DEVLINK_PORT_TYPE_AUTO,
	DEVLINK_PORT_TYPE_ETH,
	DEVLINK_PORT_TYPE_IB,
};

enum devlink_sb_pool_type {
	DEVLINK_SB_POOL_TYPE_INGRESS,
	DEVLINK_SB_POOL_TYPE_EGRESS,
};

/* static threshold - limiting the maximum number of bytes.
 * dynamic threshold - limiting the maximum number of bytes
 *   based on the currently available free space in the shared buffer pool.
 *   In this mode, the maximum quota is calculated based
 *   on the following formula:
 *     max_quota = alpha / (1 + alpha) * Free_Buffer
 *   While Free_Buffer is the amount of none-occupied buffer associated to
 *   the relevant pool.
 *   The value range which can be passed is 0-20 and serves
 *   for computation of alpha by following formula:
 *     alpha = 2 ^ (passed_value - 10)
 */

enum devlink_sb_threshold_type {
	DEVLINK_SB_THRESHOLD_TYPE_STATIC,
	DEVLINK_SB_THRESHOLD_TYPE_DYNAMIC,
};

#define DEVLINK_SB_THRESHOLD_TO_ALPHA_MAX 20

enum devlink_eswitch_mode {
	DEVLINK_ESWITCH_MODE_LEGACY,
	DEVLINK_ESWITCH_MODE_SWITCHDEV,
};

enum devlink_eswitch_inline_mode {
	DEVLINK_ESWITCH_INLINE_MODE_NONE,
	DEVLINK_ESWITCH_INLINE_MODE_LINK,
	DEVLINK_ESWITCH_INLINE_MODE_NETWORK,
	DEVLINK_ESWITCH_INLINE_MODE_TRANSPORT,
};

enum devlink_eswitch_encap_mode {
	DEVLINK_ESWITCH_ENCAP_MODE_NONE,
	DEVLINK_ESWITCH_ENCAP_MODE_BASIC,
};

enum devlink_dcbx_mode {
	DEVLINK_DCBX_MODE_DISABLED,
	DEVLINK_DCBX_MODE_IEEE,
	DEVLINK_DCBX_MODE_CEE,
	DEVLINK_DCBX_MODE_IEEE_CEE,
};

enum devlink_multifunc_mode {
	DEVLINK_MULTIFUNC_MODE_ALLOWED,		/* Ext switch activates MF */
	DEVLINK_MULTIFUNC_MODE_FORCE_SINGFUNC,
	DEVLINK_MULTIFUNC_MODE_NPAR10,		/* NPAR 1.0 */
	DEVLINK_MULTIFUNC_MODE_NPAR15,		/* NPAR 1.5 */
	DEVLINK_MULTIFUNC_MODE_NPAR20,		/* NPAR 2.0 */
};

enum devlink_autoneg_protocol {
	DEVLINK_AUTONEG_PROTOCOL_IEEE8023BY_BAM,
	DEVLINK_AUTONEG_PROTOCOL_IEEE8023BY_CONSORTIUM,
	DEVLINK_AUTONEG_PROTOCOL_IEEE8023BY,
	DEVLINK_AUTONEG_PROTOCOL_BAM,		/* Broadcom Autoneg Mode */
	DEVLINK_AUTONEG_PROTOCOL_CONSORTIUM,	/* Consortium Autoneg Mode */
};

enum devlink_pre_os_link_speed {
	DEVLINK_PRE_OS_LINK_SPEED_AUTONEG,
	DEVLINK_PRE_OS_LINK_SPEED_1G,
	DEVLINK_PRE_OS_LINK_SPEED_10G,
	DEVLINK_PRE_OS_LINK_SPEED_25G,
	DEVLINK_PRE_OS_LINK_SPEED_40G,
	DEVLINK_PRE_OS_LINK_SPEED_50G,
	DEVLINK_PRE_OS_LINK_SPEED_100G,
	DEVLINK_PRE_OS_LINK_SPEED_5G = 0xe,
	DEVLINK_PRE_OS_LINK_SPEED_100M = 0xf,
};

enum devlink_mba_boot_type {
	DEVLINK_MBA_BOOT_TYPE_AUTO_DETECT,
	DEVLINK_MBA_BOOT_TYPE_BBS,		/* BIOS Boot Specification */
	DEVLINK_MBA_BOOT_TYPE_INTR18,		/* Hook interrupt 0x18 */
	DEVLINK_MBA_BOOT_TYPE_INTR19,		/* Hook interrupt 0x19 */
};

enum devlink_mba_setup_hot_key {
	DEVLINK_MBA_SETUP_HOT_KEY_CTRL_S,
	DEVLINK_MBA_SETUP_HOT_KEY_CTRL_B,
};

enum devlink_mba_boot_protocol {
	DEVLINK_MBA_BOOT_PROTOCOL_PXE,
	DEVLINK_MBA_BOOT_PROTOCOL_ISCSI,
	DEVLINK_MBA_BOOT_PROTOCOL_NONE = 0x7,
};

enum devlink_mba_link_speed {
	DEVLINK_MBA_LINK_SPEED_AUTONEG,
	DEVLINK_MBA_LINK_SPEED_1G,
	DEVLINK_MBA_LINK_SPEED_10G,
	DEVLINK_MBA_LINK_SPEED_25G,
	DEVLINK_MBA_LINK_SPEED_40G,
	DEVLINK_MBA_LINK_SPEED_50G,
};

enum devlink_attr {
	/* don't change the order or add anything between, this is ABI! */
	DEVLINK_ATTR_UNSPEC,

	/* bus name + dev name together are a handle for devlink entity */
	DEVLINK_ATTR_BUS_NAME,			/* string */
	DEVLINK_ATTR_DEV_NAME,			/* string */

	DEVLINK_ATTR_PORT_INDEX,		/* u32 */
	DEVLINK_ATTR_PORT_TYPE,			/* u16 */
	DEVLINK_ATTR_PORT_DESIRED_TYPE,		/* u16 */
	DEVLINK_ATTR_PORT_NETDEV_IFINDEX,	/* u32 */
	DEVLINK_ATTR_PORT_NETDEV_NAME,		/* string */
	DEVLINK_ATTR_PORT_IBDEV_NAME,		/* string */
	DEVLINK_ATTR_PORT_SPLIT_COUNT,		/* u32 */
	DEVLINK_ATTR_PORT_SPLIT_GROUP,		/* u32 */
	DEVLINK_ATTR_SB_INDEX,			/* u32 */
	DEVLINK_ATTR_SB_SIZE,			/* u32 */
	DEVLINK_ATTR_SB_INGRESS_POOL_COUNT,	/* u16 */
	DEVLINK_ATTR_SB_EGRESS_POOL_COUNT,	/* u16 */
	DEVLINK_ATTR_SB_INGRESS_TC_COUNT,	/* u16 */
	DEVLINK_ATTR_SB_EGRESS_TC_COUNT,	/* u16 */
	DEVLINK_ATTR_SB_POOL_INDEX,		/* u16 */
	DEVLINK_ATTR_SB_POOL_TYPE,		/* u8 */
	DEVLINK_ATTR_SB_POOL_SIZE,		/* u32 */
	DEVLINK_ATTR_SB_POOL_THRESHOLD_TYPE,	/* u8 */
	DEVLINK_ATTR_SB_THRESHOLD,		/* u32 */
	DEVLINK_ATTR_SB_TC_INDEX,		/* u16 */
	DEVLINK_ATTR_SB_OCC_CUR,		/* u32 */
	DEVLINK_ATTR_SB_OCC_MAX,		/* u32 */
	DEVLINK_ATTR_ESWITCH_MODE,		/* u16 */
	DEVLINK_ATTR_ESWITCH_INLINE_MODE,	/* u8 */

	DEVLINK_ATTR_DPIPE_TABLES,		/* nested */
	DEVLINK_ATTR_DPIPE_TABLE,		/* nested */
	DEVLINK_ATTR_DPIPE_TABLE_NAME,		/* string */
	DEVLINK_ATTR_DPIPE_TABLE_SIZE,		/* u64 */
	DEVLINK_ATTR_DPIPE_TABLE_MATCHES,	/* nested */
	DEVLINK_ATTR_DPIPE_TABLE_ACTIONS,	/* nested */
	DEVLINK_ATTR_DPIPE_TABLE_COUNTERS_ENABLED,	/* u8 */

	DEVLINK_ATTR_DPIPE_ENTRIES,		/* nested */
	DEVLINK_ATTR_DPIPE_ENTRY,		/* nested */
	DEVLINK_ATTR_DPIPE_ENTRY_INDEX,		/* u64 */
	DEVLINK_ATTR_DPIPE_ENTRY_MATCH_VALUES,	/* nested */
	DEVLINK_ATTR_DPIPE_ENTRY_ACTION_VALUES,	/* nested */
	DEVLINK_ATTR_DPIPE_ENTRY_COUNTER,	/* u64 */

	DEVLINK_ATTR_DPIPE_MATCH,		/* nested */
	DEVLINK_ATTR_DPIPE_MATCH_VALUE,		/* nested */
	DEVLINK_ATTR_DPIPE_MATCH_TYPE,		/* u32 */

	DEVLINK_ATTR_DPIPE_ACTION,		/* nested */
	DEVLINK_ATTR_DPIPE_ACTION_VALUE,	/* nested */
	DEVLINK_ATTR_DPIPE_ACTION_TYPE,		/* u32 */

	DEVLINK_ATTR_DPIPE_VALUE,
	DEVLINK_ATTR_DPIPE_VALUE_MASK,
	DEVLINK_ATTR_DPIPE_VALUE_MAPPING,	/* u32 */

	DEVLINK_ATTR_DPIPE_HEADERS,		/* nested */
	DEVLINK_ATTR_DPIPE_HEADER,		/* nested */
	DEVLINK_ATTR_DPIPE_HEADER_NAME,		/* string */
	DEVLINK_ATTR_DPIPE_HEADER_ID,		/* u32 */
	DEVLINK_ATTR_DPIPE_HEADER_FIELDS,	/* nested */
	DEVLINK_ATTR_DPIPE_HEADER_GLOBAL,	/* u8 */
	DEVLINK_ATTR_DPIPE_HEADER_INDEX,	/* u32 */

	DEVLINK_ATTR_DPIPE_FIELD,		/* nested */
	DEVLINK_ATTR_DPIPE_FIELD_NAME,		/* string */
	DEVLINK_ATTR_DPIPE_FIELD_ID,		/* u32 */
	DEVLINK_ATTR_DPIPE_FIELD_BITWIDTH,	/* u32 */
	DEVLINK_ATTR_DPIPE_FIELD_MAPPING_TYPE,	/* u32 */

	DEVLINK_ATTR_PAD,

	DEVLINK_ATTR_ESWITCH_ENCAP_MODE,	/* u8 */

	/* Configuration Parameters */
	DEVLINK_ATTR_SRIOV_ENABLED,		/* u8 */
	DEVLINK_ATTR_NUM_VF_PER_PF,		/* u32 */
	DEVLINK_ATTR_MAX_NUM_PF_MSIX_VECT,	/* u32 */
	DEVLINK_ATTR_MSIX_VECTORS_PER_VF,	/* u32 */
	DEVLINK_ATTR_NPAR_NUM_PARTITIONS_PER_PORT,	/* u32 */
	DEVLINK_ATTR_NPAR_BW_IN_PERCENT,	/* u8 */
	DEVLINK_ATTR_NPAR_BW_RESERVATION,	/* u8 */
	DEVLINK_ATTR_NPAR_BW_RESERVATION_VALID,	/* u8 */
	DEVLINK_ATTR_NPAR_BW_LIMIT,		/* u8 */
	DEVLINK_ATTR_NPAR_BW_LIMIT_VALID,	/* u8 */
	DEVLINK_ATTR_DCBX_MODE,			/* u8 */
	DEVLINK_ATTR_RDMA_ENABLED,		/* u8 */
	DEVLINK_ATTR_MULTIFUNC_MODE,		/* u8 */
	DEVLINK_ATTR_SECURE_NIC_ENABLED,	/* u8 */
	DEVLINK_ATTR_IGNORE_ARI_CAPABILITY,	/* u8 */
	DEVLINK_ATTR_LLDP_NEAREST_BRIDGE_ENABLED,	/* u8 */
	DEVLINK_ATTR_LLDP_NEAREST_NONTPMR_BRIDGE_ENABLED,	/* u8 */
	DEVLINK_ATTR_PME_CAPABILITY_ENABLED,	/* u8 */
	DEVLINK_ATTR_MAGIC_PACKET_WOL_ENABLED,	/* u8 */
	DEVLINK_ATTR_EEE_PWR_SAVE_ENABLED,	/* u8 */
	DEVLINK_ATTR_AUTONEG_PROTOCOL,		/* u8 */
	DEVLINK_ATTR_MEDIA_AUTO_DETECT,		/* u8 */
	DEVLINK_ATTR_PHY_SELECT,		/* u8 */
	DEVLINK_ATTR_PRE_OS_LINK_SPEED_D0,	/* u8 */
	DEVLINK_ATTR_PRE_OS_LINK_SPEED_D3,	/* u8 */
	DEVLINK_ATTR_MBA_ENABLED,		/* u8 */
	DEVLINK_ATTR_MBA_BOOT_TYPE,		/* u8 */
	DEVLINK_ATTR_MBA_DELAY_TIME,		/* u32 */
	DEVLINK_ATTR_MBA_SETUP_HOT_KEY,		/* u8 */
	DEVLINK_ATTR_MBA_HIDE_SETUP_PROMPT,	/* u8 */
	DEVLINK_ATTR_MBA_BOOT_RETRY_COUNT,	/* u32 */
	DEVLINK_ATTR_MBA_VLAN_ENABLED,		/* u8 */
	DEVLINK_ATTR_MBA_VLAN_TAG,		/* u16 */
	DEVLINK_ATTR_MBA_BOOT_PROTOCOL,		/* u8 */
	DEVLINK_ATTR_MBA_LINK_SPEED,		/* u8 */

	/* When config doesn't take effect until next reboot (config
	 * just changed NVM which isn't read until boot, for example),
	 * this attribute should be set by the driver.
	 */
	DEVLINK_ATTR_RESTART_REQUIRED,		/* u8 */

	/* add new attributes above here, update the policy in devlink.c */

	__DEVLINK_ATTR_MAX,
	DEVLINK_ATTR_MAX = __DEVLINK_ATTR_MAX - 1
};

/* Mapping between internal resource described by the field and system
 * structure
 */
enum devlink_dpipe_field_mapping_type {
	DEVLINK_DPIPE_FIELD_MAPPING_TYPE_NONE,
	DEVLINK_DPIPE_FIELD_MAPPING_TYPE_IFINDEX,
};

/* Match type - specify the type of the match */
enum devlink_dpipe_match_type {
	DEVLINK_DPIPE_MATCH_TYPE_FIELD_EXACT,
};

/* Action type - specify the action type */
enum devlink_dpipe_action_type {
	DEVLINK_DPIPE_ACTION_TYPE_FIELD_MODIFY,
};

enum devlink_dpipe_field_ethernet_id {
	DEVLINK_DPIPE_FIELD_ETHERNET_DST_MAC,
};

enum devlink_dpipe_field_ipv4_id {
	DEVLINK_DPIPE_FIELD_IPV4_DST_IP,
};

enum devlink_dpipe_field_ipv6_id {
	DEVLINK_DPIPE_FIELD_IPV6_DST_IP,
};

enum devlink_dpipe_header_id {
	DEVLINK_DPIPE_HEADER_ETHERNET,
	DEVLINK_DPIPE_HEADER_IPV4,
	DEVLINK_DPIPE_HEADER_IPV6,
};

#endif /* _UAPI_LINUX_DEVLINK_H_ */
