/* Copyright (c) 2013-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * RMNET Data configuration specification
 */

#ifndef _RMNET_DATA_H_
#define _RMNET_DATA_H_

/* Constants */
#define RMNET_LOCAL_LOGICAL_ENDPOINT -1

#define RMNET_EGRESS_FORMAT__RESERVED__         (1<<0)
#define RMNET_EGRESS_FORMAT_MAP                 (1<<1)
#define RMNET_EGRESS_FORMAT_AGGREGATION         (1<<2)
#define RMNET_EGRESS_FORMAT_MUXING              (1<<3)
#define RMNET_EGRESS_FORMAT_MAP_CKSUMV3         (1<<4)
#define RMNET_EGRESS_FORMAT_MAP_CKSUMV4         (1<<5)

#define RMNET_INGRESS_FIX_ETHERNET              (1<<0)
#define RMNET_INGRESS_FORMAT_MAP                (1<<1)
#define RMNET_INGRESS_FORMAT_DEAGGREGATION      (1<<2)
#define RMNET_INGRESS_FORMAT_DEMUXING           (1<<3)
#define RMNET_INGRESS_FORMAT_MAP_COMMANDS       (1<<4)
#define RMNET_INGRESS_FORMAT_MAP_CKSUMV3        (1<<5)
#define RMNET_INGRESS_FORMAT_MAP_CKSUMV4        (1<<6)

/* Netlink API */
#define RMNET_NETLINK_PROTO 31
#define RMNET_MAX_STR_LEN  16
#define RMNET_NL_DATA_MAX_LEN 64

#define RMNET_NETLINK_MSG_COMMAND    0
#define RMNET_NETLINK_MSG_RETURNCODE 1
#define RMNET_NETLINK_MSG_RETURNDATA 2

struct rmnet_nl_msg_s {
	uint16_t reserved;
	uint16_t message_type;
	uint16_t reserved2:14;
	uint16_t crd:2;
	union {
		uint16_t arg_length;
		uint16_t return_code;
	};
	union {
		uint8_t data[RMNET_NL_DATA_MAX_LEN];
		struct {
			uint8_t  dev[RMNET_MAX_STR_LEN];
			uint32_t flags;
			uint16_t agg_size;
			uint16_t agg_count;
			uint8_t  tail_spacing;
		} data_format;
		struct {
			uint8_t dev[RMNET_MAX_STR_LEN];
			int32_t ep_id;
			uint8_t operating_mode;
			uint8_t next_dev[RMNET_MAX_STR_LEN];
		} local_ep_config;
		struct {
			uint32_t id;
			uint8_t  vnd_name[RMNET_MAX_STR_LEN];
		} vnd;
		struct {
			uint32_t id;
			uint32_t map_flow_id;
			uint32_t tc_flow_id;
		} flow_control;
	};
};

enum rmnet_netlink_message_types_e {
	/* RMNET_NETLINK_ASSOCIATE_NETWORK_DEVICE - Register RMNET data driver
	 *                                          on a particular device.
	 * Args: char[] dev_name: Null terminated ASCII string, max length: 15
	 * Returns: status code
	 */
	RMNET_NETLINK_ASSOCIATE_NETWORK_DEVICE,

	/* RMNET_NETLINK_UNASSOCIATE_NETWORK_DEVICE - Unregister RMNET data
	 *                                            driver on a particular
	 *                                            device.
	 * Args: char[] dev_name: Null terminated ASCII string, max length: 15
	 * Returns: status code
	 */
	RMNET_NETLINK_UNASSOCIATE_NETWORK_DEVICE,

	/* RMNET_NETLINK_GET_NETWORK_DEVICE_ASSOCIATED - Get if RMNET data
	 *                                            driver is registered on a
	 *                                            particular device.
	 * Args: char[] dev_name: Null terminated ASCII string, max length: 15
	 * Returns: 1 if registered, 0 if not
	 */
	RMNET_NETLINK_GET_NETWORK_DEVICE_ASSOCIATED,

	/* RMNET_NETLINK_SET_LINK_EGRESS_DATA_FORMAT - Sets the egress data
	 *                                             format for a particular
	 *                                             link.
	 * Args: uint32_t egress_flags
	 *       char[] dev_name: Null terminated ASCII string, max length: 15
	 * Returns: status code
	 */
	RMNET_NETLINK_SET_LINK_EGRESS_DATA_FORMAT,

	/* RMNET_NETLINK_GET_LINK_EGRESS_DATA_FORMAT - Gets the egress data
	 *                                             format for a particular
	 *                                             link.
	 * Args: char[] dev_name: Null terminated ASCII string, max length: 15
	 * Returns: 4-bytes data: uint32_t egress_flags
	 */
	RMNET_NETLINK_GET_LINK_EGRESS_DATA_FORMAT,

	/* RMNET_NETLINK_SET_LINK_INGRESS_DATA_FORMAT - Sets the ingress data
	 *                                              format for a particular
	 *                                              link.
	 * Args: uint32_t ingress_flags
	 *       char[] dev_name: Null terminated ASCII string, max length: 15
	 * Returns: status code
	 */
	RMNET_NETLINK_SET_LINK_INGRESS_DATA_FORMAT,

	/* RMNET_NETLINK_GET_LINK_INGRESS_DATA_FORMAT - Gets the ingress data
	 *                                              format for a particular
	 *                                              link.
	 * Args: char[] dev_name: Null terminated ASCII string, max length: 15
	 * Returns: 4-bytes data: uint32_t ingress_flags
	 */
	RMNET_NETLINK_GET_LINK_INGRESS_DATA_FORMAT,

	/* RMNET_NETLINK_SET_LOGICAL_EP_CONFIG - Sets the logical endpoint
	 *                                       configuration for a particular
	 *                                       link.
	 * Args: char[] dev_name: Null terminated ASCII string, max length: 15
	 *     int32_t logical_ep_id, valid values are -1 through 31
	 *     uint8_t rmnet_mode: one of none, vnd, bridged
	 *     char[] egress_dev_name: Egress device if operating in bridge mode
	 * Returns: status code
	 */
	RMNET_NETLINK_SET_LOGICAL_EP_CONFIG,

	/* RMNET_NETLINK_UNSET_LOGICAL_EP_CONFIG - Un-sets the logical endpoint
	 *                                       configuration for a particular
	 *                                       link.
	 * Args: char[] dev_name: Null terminated ASCII string, max length: 15
	 *       int32_t logical_ep_id, valid values are -1 through 31
	 * Returns: status code
	 */
	RMNET_NETLINK_UNSET_LOGICAL_EP_CONFIG,

	/* RMNET_NETLINK_GET_LOGICAL_EP_CONFIG - Gets the logical endpoint
	 *                                       configuration for a particular
	 *                                       link.
	 * Args: char[] dev_name: Null terminated ASCII string, max length: 15
	 *        int32_t logical_ep_id, valid values are -1 through 31
	 * Returns: uint8_t rmnet_mode: one of none, vnd, bridged
	 * char[] egress_dev_name: Egress device
	 */
	RMNET_NETLINK_GET_LOGICAL_EP_CONFIG,

	/* RMNET_NETLINK_NEW_VND - Creates a new virtual network device node
	 * Args: int32_t node number
	 * Returns: status code
	 */
	RMNET_NETLINK_NEW_VND,

	/* RMNET_NETLINK_NEW_VND_WITH_PREFIX - Creates a new virtual network
	 *                                     device node with the specified
	 *                                     prefix for the device name
	 * Args: int32_t node number
	 *       char[] vnd_name - Use as prefix
	 * Returns: status code
	 */
	RMNET_NETLINK_NEW_VND_WITH_PREFIX,

	/* RMNET_NETLINK_GET_VND_NAME - Gets the string name of a VND from ID
	 * Args: int32_t node number
	 * Returns: char[] vnd_name
	 */
	RMNET_NETLINK_GET_VND_NAME,

	/* RMNET_NETLINK_FREE_VND - Removes virtual network device node
	 * Args: int32_t node number
	 * Returns: status code
	 */
	RMNET_NETLINK_FREE_VND,

	/* RMNET_NETLINK_ADD_VND_TC_FLOW - Add flow control handle on VND
	 * Args: int32_t node number
	 *       uint32_t MAP Flow Handle
	 *       uint32_t TC Flow Handle
	 * Returns: status code
	 */
	RMNET_NETLINK_ADD_VND_TC_FLOW,

	/* RMNET_NETLINK_DEL_VND_TC_FLOW - Removes flow control handle on VND
	 * Args: int32_t node number
	 *       uint32_t MAP Flow Handle
	 * Returns: status code
	 */
	RMNET_NETLINK_DEL_VND_TC_FLOW
};

enum rmnet_config_endpoint_modes_e {
	/* Pass the frame up the stack with no modifications to skb->dev      */
	RMNET_EPMODE_NONE,
	/* Replace skb->dev to a virtual rmnet device and pass up the stack   */
	RMNET_EPMODE_VND,
	/* Pass the frame directly to another device with dev_queue_xmit().   */
	RMNET_EPMODE_BRIDGE,
	/* Must be the last item in the list                                  */
	RMNET_EPMODE_LENGTH
};

enum rmnet_config_return_codes_e {
	RMNET_CONFIG_OK,
	RMNET_CONFIG_UNKNOWN_MESSAGE,
	RMNET_CONFIG_UNKNOWN_ERROR,
	RMNET_CONFIG_NOMEM,
	RMNET_CONFIG_DEVICE_IN_USE,
	RMNET_CONFIG_INVALID_REQUEST,
	RMNET_CONFIG_NO_SUCH_DEVICE,
	RMNET_CONFIG_BAD_ARGUMENTS,
	RMNET_CONFIG_BAD_EGRESS_DEVICE,
	RMNET_CONFIG_TC_HANDLE_FULL
};


struct rmnet_map_header_s {
#ifndef RMNET_USE_BIG_ENDIAN_STRUCTS
	uint8_t  pad_len:6;
	uint8_t  reserved_bit:1;
	uint8_t  cd_bit:1;
#else
	uint8_t  cd_bit:1;
	uint8_t  reserved_bit:1;
	uint8_t  pad_len:6;
#endif /* RMNET_USE_BIG_ENDIAN_STRUCTS */
	uint8_t  mux_id;
	uint16_t pkt_len;
}  __aligned(1);

#define RMNET_MAP_GET_MUX_ID(Y) (((struct rmnet_map_header_s *)Y->data)->mux_id)
#define RMNET_MAP_GET_CD_BIT(Y) (((struct rmnet_map_header_s *)Y->data)->cd_bit)
#define RMNET_MAP_GET_PAD(Y) (((struct rmnet_map_header_s *)Y->data)->pad_len)
#define RMNET_MAP_GET_CMD_START(Y) ((struct rmnet_map_control_command_s *) \
				  (Y->data + sizeof(struct rmnet_map_header_s)))
#define RMNET_MAP_GET_LENGTH(Y) (ntohs( \
			       ((struct rmnet_map_header_s *)Y->data)->pkt_len))

#define RMNET_IP_VER_MASK 0xF0
#define RMNET_IPV4        0x40
#define RMNET_IPV6        0x60


/* Bitmap macros for RmNET driver operation mode. */
#define RMNET_MODE_NONE     (0x00)
#define RMNET_MODE_LLP_ETH  (0x01)
#define RMNET_MODE_LLP_IP   (0x02)
#define RMNET_MODE_QOS      (0x04)
#define RMNET_MODE_MASK     (RMNET_MODE_LLP_ETH | \
			     RMNET_MODE_LLP_IP  | \
			     RMNET_MODE_QOS)

#define RMNET_IS_MODE_QOS(mode)  \
	((mode & RMNET_MODE_QOS) == RMNET_MODE_QOS)
#define RMNET_IS_MODE_IP(mode)   \
	((mode & RMNET_MODE_LLP_IP) == RMNET_MODE_LLP_IP)

/* IOCTL command enum
 * Values chosen to not conflict with other drivers in the ecosystem */
enum rmnet_ioctl_cmds_e {
	RMNET_IOCTL_SET_LLP_ETHERNET = 0x000089F1, /* Set Ethernet protocol  */
	RMNET_IOCTL_SET_LLP_IP       = 0x000089F2, /* Set RAWIP protocol     */
	RMNET_IOCTL_GET_LLP          = 0x000089F3, /* Get link protocol      */
	RMNET_IOCTL_SET_QOS_ENABLE   = 0x000089F4, /* Set QoS header enabled */
	RMNET_IOCTL_SET_QOS_DISABLE  = 0x000089F5, /* Set QoS header disabled*/
	RMNET_IOCTL_GET_QOS          = 0x000089F6, /* Get QoS header state   */
	RMNET_IOCTL_GET_OPMODE       = 0x000089F7, /* Get operation mode     */
	RMNET_IOCTL_OPEN             = 0x000089F8, /* Open transport port    */
	RMNET_IOCTL_CLOSE            = 0x000089F9, /* Close transport port   */
	RMNET_IOCTL_FLOW_ENABLE      = 0x000089FA, /* Flow enable            */
	RMNET_IOCTL_FLOW_DISABLE     = 0x000089FB, /* Flow disable           */
	RMNET_IOCTL_FLOW_SET_HNDL    = 0x000089FC, /* Set flow handle        */
	RMNET_IOCTL_EXTENDED         = 0x000089FD, /* Extended IOCTLs        */
	RMNET_IOCTL_MAX
};

enum rmnet_ioctl_extended_cmds_e {
/* RmNet Data Required IOCTLs */
	RMNET_IOCTL_GET_SUPPORTED_FEATURES     = 0x0000,   /* Get features    */
	RMNET_IOCTL_SET_MRU                    = 0x0001,   /* Set MRU         */
	RMNET_IOCTL_GET_MRU                    = 0x0002,   /* Get MRU         */
	RMNET_IOCTL_GET_EPID                   = 0x0003,   /* Get endpoint ID */
	RMNET_IOCTL_GET_DRIVER_NAME            = 0x0004,   /* Get driver name */
	RMNET_IOCTL_ADD_MUX_CHANNEL            = 0x0005,   /* Add MUX ID      */
	RMNET_IOCTL_SET_EGRESS_DATA_FORMAT     = 0x0006,   /* Set EDF         */
	RMNET_IOCTL_SET_INGRESS_DATA_FORMAT    = 0x0007,   /* Set IDF         */
	RMNET_IOCTL_SET_AGGREGATION_COUNT      = 0x0008,   /* Set agg count   */
	RMNET_IOCTL_GET_AGGREGATION_COUNT      = 0x0009,   /* Get agg count   */
	RMNET_IOCTL_SET_AGGREGATION_SIZE       = 0x000A,   /* Set agg size    */
	RMNET_IOCTL_GET_AGGREGATION_SIZE       = 0x000B,   /* Get agg size    */
	RMNET_IOCTL_FLOW_CONTROL               = 0x000C,   /* Do flow control */
	RMNET_IOCTL_GET_DFLT_CONTROL_CHANNEL   = 0x000D,   /* For legacy use  */
	RMNET_IOCTL_GET_HWSW_MAP               = 0x000E,   /* Get HW/SW map   */
	RMNET_IOCTL_SET_RX_HEADROOM            = 0x000F,   /* RX Headroom     */
	RMNET_IOCTL_GET_EP_PAIR                = 0x0010,   /* Endpoint pair   */
	RMNET_IOCTL_SET_QOS_VERSION            = 0x0011,   /* 8/6 byte QoS hdr*/
	RMNET_IOCTL_GET_QOS_VERSION            = 0x0012,   /* 8/6 byte QoS hdr*/
	RMNET_IOCTL_GET_SUPPORTED_QOS_MODES    = 0x0013,   /* Get QoS modes   */
	RMNET_IOCTL_SET_SLEEP_STATE            = 0x0014,   /* Set sleep state */
	RMNET_IOCTL_SET_XLAT_DEV_INFO          = 0x0015,   /* xlat dev name   */
	RMNET_IOCTL_DEREGISTER_DEV             = 0x0016,   /* Dereg a net dev */
	RMNET_IOCTL_GET_SG_SUPPORT             = 0x0017,   /* Query sg support*/
	RMNET_IOCTL_EXTENDED_MAX               = 0x0018
};

/* Return values for the RMNET_IOCTL_GET_SUPPORTED_FEATURES IOCTL */
#define RMNET_IOCTL_FEAT_NOTIFY_MUX_CHANNEL              (1<<0)
#define RMNET_IOCTL_FEAT_SET_EGRESS_DATA_FORMAT          (1<<1)
#define RMNET_IOCTL_FEAT_SET_INGRESS_DATA_FORMAT         (1<<2)
#define RMNET_IOCTL_FEAT_SET_AGGREGATION_COUNT           (1<<3)
#define RMNET_IOCTL_FEAT_GET_AGGREGATION_COUNT           (1<<4)
#define RMNET_IOCTL_FEAT_SET_AGGREGATION_SIZE            (1<<5)
#define RMNET_IOCTL_FEAT_GET_AGGREGATION_SIZE            (1<<6)
#define RMNET_IOCTL_FEAT_FLOW_CONTROL                    (1<<7)
#define RMNET_IOCTL_FEAT_GET_DFLT_CONTROL_CHANNEL        (1<<8)
#define RMNET_IOCTL_FEAT_GET_HWSW_MAP                    (1<<9)

/* Input values for the RMNET_IOCTL_SET_EGRESS_DATA_FORMAT IOCTL  */
#define RMNET_IOCTL_EGRESS_FORMAT_MAP                  (1<<1)
#define RMNET_IOCTL_EGRESS_FORMAT_AGGREGATION          (1<<2)
#define RMNET_IOCTL_EGRESS_FORMAT_MUXING               (1<<3)
#define RMNET_IOCTL_EGRESS_FORMAT_CHECKSUM             (1<<4)

/* Input values for the RMNET_IOCTL_SET_INGRESS_DATA_FORMAT IOCTL */
#define RMNET_IOCTL_INGRESS_FORMAT_MAP                 (1<<1)
#define RMNET_IOCTL_INGRESS_FORMAT_DEAGGREGATION       (1<<2)
#define RMNET_IOCTL_INGRESS_FORMAT_DEMUXING            (1<<3)
#define RMNET_IOCTL_INGRESS_FORMAT_CHECKSUM            (1<<4)
#define RMNET_IOCTL_INGRESS_FORMAT_AGG_DATA            (1<<5)

/* User space may not have this defined. */
#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif

struct rmnet_ioctl_extended_s {
	uint32_t   extended_ioctl;
	union {
		uint32_t data; /* Generic data field for most extended IOCTLs */

		/* Return values for
		 *    RMNET_IOCTL_GET_DRIVER_NAME
		 *    RMNET_IOCTL_GET_DFLT_CONTROL_CHANNEL */
		int8_t if_name[IFNAMSIZ];

		/* Input values for the RMNET_IOCTL_ADD_MUX_CHANNEL IOCTL */
		struct {
			uint32_t  mux_id;
			int8_t    vchannel_name[IFNAMSIZ];
		} rmnet_mux_val;

		/* Input values for the RMNET_IOCTL_FLOW_CONTROL IOCTL */
		struct {
			uint8_t   flow_mode;
			uint8_t   mux_id;
		} flow_control_prop;

		/* Return values for RMNET_IOCTL_GET_EP_PAIR */
		struct {
			uint32_t   consumer_pipe_num;
			uint32_t   producer_pipe_num;
		} ipa_ep_pair;

		struct {
			uint32_t __data; /* Placeholder for legacy data*/
			uint32_t agg_size;
			uint32_t agg_count;
		} ingress_format;
	} u;
};

struct rmnet_ioctl_data_s {
	union {
		uint32_t	operation_mode;
		uint32_t	tcm_handle;
	} u;
};

#define RMNET_IOCTL_QOS_MODE_6   (1<<0)
#define RMNET_IOCTL_QOS_MODE_8   (1<<1)

/* QMI QoS header definition */
#define QMI_QOS_HDR_S  __attribute((__packed__)) qmi_qos_hdr_s
struct QMI_QOS_HDR_S {
	unsigned char    version;
	unsigned char    flags;
	uint32_t         flow_id;
};

/* QMI QoS 8-byte header. */
struct qmi_qos_hdr8_s {
	struct QMI_QOS_HDR_S   hdr;
	uint8_t                reserved[2];
} __attribute((__packed__));

#endif /* _RMNET_DATA_H_ */
