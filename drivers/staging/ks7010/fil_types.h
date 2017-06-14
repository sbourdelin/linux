/*
 * Driver for KeyStream wireless LAN cards.
 *
 * Copyright (C) 2005-2008 KeyStream Corp.
 * Copyright (C) 2009 Renesas Technology Corp.
 * Copyright (C) 2017 Tobin C. Harding.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

/**
 * DOC: Internal types for the Firmware Interface Layer.
 */

#define KS7010_SDIO_ALIGN 32

/**
 * fil_align_size() - Device alignment.
 * @size: size to align.
 */
static inline size_t fil_align_size(size_t size)
{
	/* FIXME can we use kernel macro ALIGN here */
	if (size % KS7010_SDIO_ALIGN)
		return size + KS7010_SDIO_ALIGN - (size % KS7010_SDIO_ALIGN);

	return size;
}

/**
 * struct fil_t_hdr - Firmware Interface Layer header.
 *
 * @size: Value is tx/rx dependent.
 * @event: &enum fil_t_event
 *
 * Do not access size manually, use helper functions.
 * tx_fil_hdr_to_frame_size()
 * tx_frame_size_to_fil_hdr_size()
 * rx_fil_hdr_to_frame_size()
 * rx_frame_size_to_fil_hdr_size()
 */
struct fil_t_hdr {
	__le16 size;
	__le16 event;
} __packed;

/**
 * enum fil_t_event - Host interface events
 *
 * Events include;
 *  - get/set requests, i.e commands to the target.
 *  - confirmation and indication events.
 *
 * @FIL_T_MIB_SET_REQ: Management Information Base set request.
 * @FIL_T_MIB_GET_REQ: Management Information Base get request.
 */
enum fil_t_event {
	FIL_T_DATA_REQ		= 0xE001,
	FIL_T_MIB_GET_REQ	= 0xE002,
	FIL_T_MIB_SET_REQ	= 0xE003,
	FIL_T_POWER_MGMT_REQ	= 0xE004,
	FIL_T_START_REQ		= 0xE005,
	FIL_T_STOP_REQ		= 0xE006,
	/* FIL_T_PS_ADH_SET_REQ	= 0xE007, */
	FIL_T_INFRA_SET_REQ	= 0xE008,
	/* FIL_T_ADH_SET_REQ	= 0xE009, */
	/* FIL_T_ADH_SET2_REQ	= 0xE010, */
	/* FIL_T_AP_SET_REQ	= 0xE00A, */
	FIL_T_MIC_FAILURE_REQ	= 0xE00B,
	FIL_T_SCAN_REQ		= 0xE00C,
	FIL_T_PHY_INFO_REQ	= 0xE00D,
	FIL_T_SLEEP_REQ		= 0xE00E,
	FIL_T_INFRA_SET2_REQ	= 0xE00F,

	FIL_T_REQ_MAX		= 0xE010,

	FIL_T_DATA_IND		= 0xE801,
	FIL_T_MIB_GET_CONF	= 0xE802,
	FIL_T_MIB_SET_CONF	= 0xE803,
	FIL_T_POWER_MGMT_CONF	= 0xE804,
	FIL_T_START_CONF	= 0xE805,
	FIL_T_CONNECT_IND	= 0xE806,
	FIL_T_STOP_CONF		= 0xE807,
	/* FIL_T_PS_ADH_SET_CONF= 0xE808, */
	FIL_T_INFRA_SET_CONF	= 0xE809,
	/* FIL_T_ADH_SET_CONF	= 0xE80A, */
	/* FIL_T_AP_SET_CONF	= 0xE80B, */
	FIL_T_ASSOC_IND		= 0xE80C,
	FIL_T_MIC_FAILURE_CONF	= 0xE80D,
	FIL_T_SCAN_CONF		= 0xE80E,
	FIL_T_PHY_INFO_CONF	= 0xE80F,
	FIL_T_SLEEP_CONF	= 0xE810,
	FIL_T_PHY_INFO_IND	= 0xE811,
	FIL_T_SCAN_IND		= 0xE812,
	FIL_T_INFRA_SET2_CONF	= 0xE813,
	/* FIL_T_ADH_SET2_CONF	= 0xE814, */
};

/**
 * struct fil_t_mib_get_req - Management Information Base get request frame.
 * @fhdr: &struct fil_t_hdr.
 * @attribute: &enum mib_attribute
 */
struct fil_t_mib_get_req {
	struct fil_t_hdr fhdr;
	__le32 attribute;
} __packed;

/**
 * struct fil_t_mib_set_req - Management Information Base set request frame.
 * @fhdr: &struct fil_t_hdr.
 * @attribute: &enum mib_attribute
 * @data_size: Size of data in octets.
 * @type: &enum mib_data_type.
 * @data: MIB request data.
 */
struct fil_t_mib_set_req {
	struct fil_t_hdr fhdr;
	__le32 attribute;
	__le16 data_size;
	__le16 data_type;
	u8 data[0];
} __packed;

/**
 * enum mib_attribute - Management Information Base attribute.
 *
 * Attribute value used for accessing and updating the
 * Management Information Base, set/get req/ind.
 *
 * R is read only.
 * W is write only.
 * R/W is read and write.
 *
 * @DOT11_MAC_ADDRESS: MAC Address (R)
 * @MIB_FIRMWARE_VERSION: FirmWare Version (R)
 * @LOCAL_EEPROM_SUM: EEPROM checksum information (R)
 *
 * @LOCAL_CURRENT_ADDRESS: MAC Address change (W)
 *
 * @LOCAL_MULTICAST_ADDRESS: Multicast address (W)
 * @LOCAL_MULTICAST_FILTER: Multicast filter enable/disable (W)
 *
 * @DOT11_PRIVACY_INVOKED: Use encryption (WEP/WPA/RSN)
 *
 * @MIB_DEFAULT_KEY_INDEX: WEP key index or WPA txkey (W)
 * @MIB_KEY_VALUE_1: WEP Key 1 or TKIP/CCMP PTK (W)
 * @MIB_KEY_VALUE_2: WEP Key 2 or TKIP/CCMP GTK 1 (W)
 * @MIB_KEY_VALUE_3: WEP Key 3 or TKIP/CCMP GTK 2 (W)
 * @MIB_KEY_VALUE_4: WEP Key 4 (not currently used for TKIP/CCMP) (W)

 * @MIB_WPA_ENABLE: WPA/RSN enable/disable (W)
 * @MIB_WPA_MODE: WPA or RSN (W)
 * @MIB_WPA_CONFIG_UCAST_SUITE: Pairwise key cipher suite (W)
 * @MIB_WPA_CONFIG_MCAST_SUITE: Group key cipher suite (W)
 * @MIB_WPA_CONFIG_AUTH_SUITE: Authentication key management suite (W)
 * @MIB_PTK_TSC: PTK sequence counter (W)
 * @MIB_GTK_1_TSC: GTK 1 sequence counter (W)
 * @MIB_GTK_2_TSC: GTK 2 sequence counter (W)
 *
 * @LOCAL_PMK: Pairwise Master Key cache (W)
 *
 * @LOCAL_REGION: Region setting (W)
 *
 * @DOT11_RTS_THRESHOLD: Request To Send Threshold (R/W)
 * @DOT11_FRAGMENTATION_THRESHOLD: Fragment Threshold (R/W)
 * @LOCAL_GAIN: Carrier sense threshold for demo ato show (R/W)
 *
 * @DOT11_WEP_LIST:
 * @DOT11_DESIRED_SSID:
 * @DOT11_CURRENT_CHANNEL:
 * @DOT11_OPERATION_RATE_SET:
 * @LOCAL_AP_SEARCH_INTEAVAL:
 * @LOCAL_SEARCHED_AP_LIST:
 * @LOCAL_LINK_AP_STATUS:
 * @LOCAL_PACKET_STATISTICS:
 * @LOCAL_AP_SCAN_LIST_TYPE_SET:
 * @DOT11_RSN_CONFIG_VERSION:
 * @LOCAL_RSN_CONFIG_ALL:
 * @DOT11_GMK3_TSC:
 */
enum mib_attribute {
	DOT11_MAC_ADDRESS		= 0x21010100,
	MIB_FIRMWARE_VERSION		= 0x31024100,
	LOCAL_EEPROM_SUM		= 0xF10E0100,

	LOCAL_CURRENT_ADDRESS		= 0xF1050100,

	LOCAL_MULTICAST_ADDRESS		= 0xF1060100,
	LOCAL_MULTICAST_FILTER		= 0xF1060200,

	DOT11_PRIVACY_INVOKED		= 0x15010100,
	MIB_DEFAULT_KEY_INDEX		= 0x15020100,

	MIB_KEY_VALUE_1			= 0x13020101,
	MIB_KEY_VALUE_2			= 0x13020102,
	MIB_KEY_VALUE_3			= 0x13020103,
	MIB_KEY_VALUE_4			= 0x13020104,

	MIB_WPA_ENABLE			= 0x15070100,
	MIB_WPA_MODE			= 0x56010100,
	MIB_WPA_CONFIG_UCAST_SUITE	= 0x52020100,
	MIB_WPA_CONFIG_MCAST_SUITE	= 0x51040100,
	MIB_WPA_CONFIG_AUTH_SUITE	= 0x53020100,

	MIB_PTK_TSC			= 0x55010100,
	MIB_GTK_1_TSC			= 0x55010101,
	MIB_GTK_2_TSC			= 0x55010102,

	LOCAL_PMK			= 0x58010100,

	LOCAL_REGION			= 0xF10A0100,

	DOT11_RTS_THRESHOLD		= 0x21020100,
	DOT11_FRAGMENTATION_THRESHOLD	= 0x21050100,
	LOCAL_GAIN			= 0xF10D0100,

	 /* unused */
	DOT11_WEP_LIST			= 0x13020100,
	DOT11_RSN_CONFIG_VERSION	= 0x51020100,
	LOCAL_RSN_CONFIG_ALL		= 0x5F010100,
	DOT11_DESIRED_SSID		= 0x11090100,
	DOT11_CURRENT_CHANNEL		= 0x45010100,
	DOT11_OPERATION_RATE_SET	= 0x11110100,
	LOCAL_AP_SEARCH_INTEAVAL	= 0xF1010100,
	LOCAL_SEARCHED_AP_LIST		= 0xF1030100,
	LOCAL_LINK_AP_STATUS		= 0xF1040100,
	LOCAL_PACKET_STATISTICS		= 0xF1020100,
	LOCAL_AP_SCAN_LIST_TYPE_SET	= 0xF1030200,
	DOT11_GMK3_TSC                  = 0x55010103
};

/**
 * enum mib_type - Message Information Base data type.
 * @FIL_T_MIB_TYPE_NULL: Null type
 * @FIL_T_MIB_TYPE_INT: Integer type
 * @FIL_T_MIB_TYPE_BOOL: Boolean type
 * @FIL_T_MIB_TYPE_COUNT32: unused
 * @FIL_T_MIB_TYPE_OSTRING: Memory chunk
 */
enum mib_data_type {
	FIL_T_MIB_TYPE_NULL = 0,
	FIL_T_MIB_TYPE_INT,
	FIL_T_MIB_TYPE_BOOL,
	FIL_T_MIB_TYPE_COUNT32,
	FIL_T_MIB_TYPE_OSTRING,
};

/**
 * struct fil_t_phy_info_req - PHY information request frame.
 * @fhdr: &struct fil_t_hdr
 * @type: &enum fil_t_phy_info_type
 * @time: unit 100ms
 */
struct fil_t_phy_info_req {
	struct fil_t_hdr fhdr;
	__le16 type;
	__le16 time;
} __packed;

/**
 * enum fil_t_phy_info_type - TODO document this enum
 * @FIL_T_PHY_INFO_TYPE_NORMAL:
 * @FIL_T_PHY_INFO_TYPE_TIME:
 */
enum fil_t_phy_info_type {
	FIL_T_PHY_INFO_TYPE_NORMAL = 0,
	FIL_T_PHY_INFO_TYPE_TIME,
};

/**
 * struct fil_t_start_req - Start request frame.
 * @fhdr: &struct fil_t_hdr
 * @nw_type: &enum fil_t_nw_type
 */
struct fil_t_start_req {
	struct fil_t_hdr fhdr;
	__le16 nw_type;
} __packed;

/**
 * enum fil_t_nw_type - Network type.
 * @FIL_T_NW_TYPE_PSEUDO_ADHOC: Pseudo adhoc mode.
 * @FIL_T_NW_TYPE_INFRASTRUCTURE: Infrastructure mode.
 * @FIL_T_NW_TYPE_AP: Access point mode, not supported.
 * @FIL_T_NW_TYPE_ADHOC: Adhoc mode.
 */
enum fil_t_nw_type {
	FIL_T_NW_TYPE_PSEUDO_ADHOC = 0,
	FIL_T_NW_TYPE_INFRASTRUCTURE,
	FIL_T_NW_TYPE_AP,
	FIL_T_NW_TYPE_ADHOC
};

/**
 * struct fil_t_power_mgmt_req - Power management request frame.
 * @fhdr: &struct fil_t_hdr
 * @mode: enum fil_t_power_mgmt_mode
 * @wake_up: enum fil_t_power_mgmt_wake_up
 * @receive_dtims: enum fil_t_power_mgmt_receive_dtims
 */
struct fil_t_power_mgmt_req {
	struct fil_t_hdr fhdr;
	__le32 mode;
	__le32 wake_up;
	__le32 receive_dtims;
} __packed;

/**
 * enum fil_t_power_mgmt_mode - Power management mode.
 * @FIL_T_POWER_MGMT_MODE_ACTIVE: Disable power management, device
 *	may not sleep.
 * @FIL_T_POWER_MGMT_MODE_SAVE: Enable power management, used for
 *	'sleep' mode and 'deep sleep' mode.
 */
enum fil_t_power_mgmt_mode {
	FIL_T_POWER_MGMT_MODE_ACTIVE = 1,
	FIL_T_POWER_MGMT_MODE_SAVE
};

/**
 * enum fil_t_power_mgmt_wake_up - Wake up the device if it is asleep.
 * @FIL_T_POWER_MGMT_WAKE_UP_FALSE:
 * @FIL_T_POWER_MGMT_WAKE_UP_TRUE:
 *
 * Variable is unused in original Renesas open source driver, we have
 * no indication of its purpose except the name.
 *
 * TODO test device and verify variables usage.
 */
enum fil_t_power_mgmt_wake_up {
	FIL_T_POWER_MGMT_WAKE_UP_FALSE = 0,
	FIL_T_POWER_MGMT_WAKE_UP_TRUE
};

/**
 * enum fil_t_power_mgmt_receive_dtims - Receive DTIM's
 * @FIL_T_POWER_MGMT_RECEIVE_DTIMS_FALSE: Do not wake up to receive DTIM.
 * @FIL_T_POWER_MGMT_RECEIVE_DTIMS_TRUE: Wake up periodically to receive DTIM.
 */
enum fil_t_power_mgmt_receive_dtims {
	FIL_T_POWER_MGMT_RECEIVE_DTIMS_FALSE = 0,
	FIL_T_POWER_MGMT_RECEIVE_DTIMS_TRUE
};

#define FIL_T_CHANNELS_MAX_SIZE 14

/**
 * struct fil_t_channels - Channel list
 * @size: Size of list, i.e number of channels.
 * @body: List data.
 * @pad: Unused, structure padding.
 *
 * Each channel number is a single octet.
 */
struct fil_t_channels {
	u8 size;
	u8 body[FIL_T_CHANNELS_MAX_SIZE];
	u8 pad;
} __packed;

#define FIL_T_SSID_MAX_SIZE 32

/**
 * struct fil_t_ssid - Service Set Identity
 * @size: Size of SSID in octets.
 * @body: SSID data.
 * @pad: Unused, structure padding.
 */
struct fil_t_ssid {
	u8 size;
	u8 body[FIL_T_SSID_MAX_SIZE];
	u8 pad;
} __packed;

/**
 * enum fil_t_default_channel_time - Default channel times.
 * @FIL_T_DEFAULT_CH_TIME_MIN: Default minimum time.
 * @FIL_T_DEFAULT_CH_TIME_MAX: Default maximum time.
 */
enum fil_t_default_channel_time {
	FIL_T_DEFAULT_CH_TIME_MIN = 110,
	FIL_T_DEFAULT_CH_TIME_MAX = 130
};

/**
 * struct fil_t_scan_req - Scan request frame.
 * @fhdr: &struct fil_t_hdr
 * @scan_type: &enum fil_scan_type
 * @pad: Unused, structure padding.
 * @ch_time_min: Minimum scan time per channel in time units.
 * @ch_time_max: Maximum scan time per channel in time units.
 * @channels: List of channels to scan, &struct fil_t_channels.
 * @ssid: SSID used during scan, &struct fil_t_ssid.
 */
struct fil_t_scan_req {
	struct fil_t_hdr fhdr;
	u8 scan_type;
	u8 pad[3];
	__le32 ch_time_min;
	__le32 ch_time_max;
	struct fil_t_channels channels;
	struct fil_t_ssid ssid;
} __packed;

#define FIL_T_INFRA_SET_REQ_RATES_MAX_SIZE 16

/**
 * struct fil_t_rates - List of rates.
 * @size: Size of list, i.e number of rates.
 * @body: List data.
 * @pad: Unused, structure padding.
 *
 * Each rate number is a single octet.
 */
struct fil_t_rates {
	u8 size;
	u8 body[FIL_T_INFRA_SET_REQ_RATES_MAX_SIZE];
	u8 pad;
} __packed;

/**
 * struct _infra_set_req - Network type infrastructure request frame.
 * @fhdr: &struct fil_t_hdr
 * @phy_type: &enum fil_phy_type
 * @cts_mode: &enum cts_mode
 * @rates: Supported data rates, &struct fil_t_rates
 * @ssid: SSID, &struct fil_t_ssid
 * @capability: Network capability flags, &enum fil_bss_capability_flags.
 * @beacon_lost_count: TODO document this
 * @auth_type: &enum fil_dot11_auth_type
 * @channels: &struct fil_t_channels
 * @scan_type: &enum fil_scan_type
 */
struct _infra_set_req {
	struct fil_t_hdr fhdr;
	__le16 phy_type;
	__le16 cts_mode;
	struct fil_t_rates rates;
	struct fil_t_ssid ssid;
	__le16 capability;
	__le16 beacon_lost_count;
	__le16 auth_type;
	struct fil_t_channels channels;
	__le16 scan_type;
} __packed;

/**
 * struct fil_t_infra_set_req - Set BSS mode without specifying the BSSID
 * @req: &struct _infra_set_req
 */
struct fil_t_infra_set_req {
	struct _infra_set_req req;
} __packed;

/**
 * struct fil_t_infra_set_req - Set BSS mode specifying the BSSID
 * @req: &struct _infra_set_req
 * @bssid: BSSID to use for request.
 */
struct fil_t_infra_set2_req {
	struct _infra_set_req req;
	u8 bssid[ETH_ALEN];
} __packed;

/**
 * struct fil_t_mic_failure_req - Michael MIC failure event frame.
 * @fhdr: &struct fil_t_hdr
 * @count: Notify firmware that this is failure number @count.
 * @timer: Number of jiffies since the last failure.
 *
 * Michael Message Integrity Check must be done by the driver, in the
 * event of a failure use this frame type to notify the firmware of
 * the failure.
 */
struct fil_t_mic_failure_req {
	struct fil_t_hdr fhdr;
	__le16 count;
	__le16 timer;
} __packed;

/**
 * struct fil_t_data_req - Tx data and auth frames.
 * @fhdr: &struct fil_t_hdr
 * @type: &enum data_req_type.
 * @reserved: Unused, reserved.
 * @data: Upper layer data.
 *
 * Frame used when building tx frames out of sk_buff passed down from
 * networking stack, used for data frames and authentication frames.
 */
struct fil_t_data_req {
	struct fil_t_hdr fhdr;
	__le16 type;
	__le16 reserved;
	u8 data[0];
} __packed;

/**
 * enum fil_data_req_type - Tx frame.
 * @FIL_DATA_REQ_TYPE_DATA: Data requests frame.
 * @FIL_DATA_REQ_TYTE_AUTH: Data authentication frame.
 */
enum fil_t_data_req_type {
	FIL_T_DATA_REQ_TYPE_DATA = 0x0000,
	FIL_T_DATA_REQ_TYPE_AUTH
};

/**
 * struct fil_t_data_ind - Rx frame.
 * @fhdr: &struct fil_t_hdr
 * @auth_type: &struct data_ind_auth_type.
 * @reserved: Unused, reserved.
 * @data: Rx data.
 */
struct fil_t_data_ind {
	struct fil_t_hdr fhdr;
	__le16 auth_type;
	__le16 reserved;
	u8 data[0];
} __packed;

/**
 * enum data_ind_auth_type - Key used for encryption.
 * @AUTH_TYPE_PTK: Pairwise Transient Key
 * @AUTH_TYPE_GTK1: Group Transient Key 1
 * @AUTH_TYPE_GTK2: Group Transient Key 2
 */
enum data_ind_auth_type {
	AUTH_TYPE_PTK = 0x0001,
	AUTH_TYPE_GTK1,
	AUTH_TYPE_GTK2
};

/**
 * struct fil_t_mib_set_conf - 'MIB set' confirmation frame.
 * @fhdr: &struct fil_t_hdr
 * @status: &enum mib_status
 * @attribute: &enum mib_attribute
 */
struct fil_t_mib_set_conf {
	struct fil_t_hdr fhdr;
	__le32 status;
	__le32 attribute;
} __packed;

/**
 * struct fil_t_mib_get_conf - 'MIB get' confirmation frame.
 * @fhdr: &struct fil_t_hdr
 * @status: &enum mib_status
 * @attribute: &enum mib_attribute
 * @data_size: Size of @data in octets.
 * @data_type: &enum mib_data_type
 */
struct fil_t_mib_get_conf {
	struct fil_t_hdr fhdr;
	__le32 status;
	__le32 attribute;
	__le16 data_size;
	__le16 data_type;
	u8 data[0];
} __packed;

/**
 * enum mib_status - Result status of a MIB get/set request.
 * @MIB_STATUS_SUCCESS: Request successful.
 * @MIB_STATUS_INVALID: Request invalid.
 * @MIB_STATUS_READ_ONLY: Request failed, attribute is read only.
 * @MIB_STATUS_WRITE_ONLY: Request failed, attribute is write only.
 */
enum mib_status {
	MIB_STATUS_SUCCESS = 0,
	MIB_STATUS_INVALID,
	MIB_STATUS_READ_ONLY,
	MIB_STATUS_WRITE_ONLY,
};

/**
 * struct fil_t_result_code_conf - Generic confirmation frame.
 * @fhdr: &struct fil_t_hdr
 * @relust_code: &struct fil_t_result_code
 */
struct fil_t_result_code_conf {
	struct fil_t_hdr fhdr;
	__le16 result_code;
} __packed;

/* TODO document struct fil_t_phy_info_ind */

/**
 * struct fil_t_phy_info_ind - PHY information frame.
 * @fhdr: &struct fil_t_hdr
 * @rssi: Received signal strength indication.
 * @signal:
 * @noise:
 * @link_speed:
 * @tx_frame:
 * @rx_frame:
 * @tx_error:
 * @rx_error:
 */
struct fil_t_phy_info_ind {
	struct fil_t_hdr fhdr;
	u8 rssi;
	u8 signal;
	u8 noise;
	u8 link_speed;
	__le32 tx_frame;
	__le32 rx_frame;
	__le32 tx_error;
	__le32 rx_error;
} __packed;

/**
 * struct fil_t_scan_conf - Scan confirmation frame.
 * @fhdr: &struct fil_t_hdr
 * @relust_code: &struct fil_t_result_code
 * @reserved: Unused, reserved.
 */
struct fil_t_scan_conf {
	struct fil_t_hdr fhdr;
	__le16 result_code;
	__le16 reserved;
} __packed;

/* TODO document struct fil_t_phy_info_ind */

#define FIL_T_AP_INFO_MAX_SIZE

/**
 * struct fil_t_scan_ind - Scan result information frame.
 * @fhdr: &struct fil_t_hdr
 * @bssid: Basic service set identifier.
 * @rssi: Received signal strength indication.
 * @signal:
 * @noise:
 * @pad0: Unused, structure padding.
 * @beacon_period: Beacon period (interval) in time units.
 * @capability: Network capability flags, &enum fil_bss_capability_flags.
 * @frame_type: &enum fil_t_scan_ind_frame_type
 * @channel: Channel to use.
 * @body_size: Size of @body in octets.
 * @body: Scan indication data, made up of consecutive &struct fil_ap_info.
 */
struct fil_t_scan_ind {
	struct fil_t_hdr fhdr;
	u8 bssid[ETH_ALEN];
	u8 rssi;
	u8 signal;
	u8 noise;
	u8 pad0;
	__le16 beacon_period;
	__le16 capability;
	u8 frame_type;
	u8 channel;
	__le16 body_size;
	u8 body[FIL_AP_INFO_MAX_SIZE];
} __packed;

/**
 * enum fil_t_scan_ind_frame_type - FIL scan frame type.
 * @FIL_T_FRAME_TYPE_PROBE_RESP: Probe response frame type.
 * @FIL_T_FRAME_TYPE_BEACON: Beacon frame type.
 */
enum fil_t_scan_ind_frame_type {
	FIL_T_FRAME_TYPE_PROBE_RESP		= 0x50,
	FIL_T_FRAME_TYPE_BEACON			= 0x80
};

#define FIL_T_IE_MAX_SIZE 128
#define FIL_T_CONN_IND_RATES_MAX_SIZE	8

/**
 * struct fil_t_conn_ind - Connection event indication frame.
 * @fhdr: &struct fil_t_hdr
 * @conn_code: &struct fil_conn_code
 * @bssid: Basic service set identifier.
 * @rssi: Received signal strength indication.
 * @signal: TODO document this
 * @noise: TODO document this
 * @pad0: Unused, structure padding.
 * @beacon_period: Beacon period (interval) in time units.
 * @capability: Network capability flags, &enum fil_bss_capability_flags.
 * @rates: List of supported data rates.
 * @fh: Frequency hopping parameters.
 * @ds: Direct sequence parameters.
 * @cf: Contention free parameters.
 * @ibss: Adhoc network parameters.
 * @erp: Extended rate PHY parameters.
 * @pad1: Unused, structure padding.
 * @ext_rates: Extended rates list.
 * @dtim_period: Delivery traffic indication map period.
 * @wpa_mode: &struct fil_wpa_mode.
 * @ies: Information elements
 */
struct fil_t_conn_ind {
	struct fil_t_hdr fhdr;
	__le16 conn_code;
	u8 bssid[ETH_ALEN];
	u8 rssi;
	u8 signal;
	u8 noise;
	u8 pad0;
	__le16 beacon_period;
	__le16 capability;

	struct {
		u8 size;
		u8 body[FIL_T_CONN_IND_RATES_MAX_SIZE];
		u8 pad;
	} __packed rates;

	struct {
		__le16 dwell_time;
		u8 hop_set;
		u8 hop_pattern;
		u8 hop_index;
	} __packed fh;

	struct {
		u8 channel;
	} __packed ds;

	struct {
		u8 count;
		u8 period;
		__le16 max_duration;
		__le16 dur_remaining;
	} __packed cf;

	struct {
		__le16 atim_window;
	} __packed ibss;

	struct {
		u8 info;
	} __packed erp;

	u8 pad1;

	struct {
		u8 size;
		u8 body[FIL_T_CONN_IND_RATES_MAX_SIZE];
		u8 pad;
	} __packed ext_rates;

	u8 dtim_period;
	u8 wpa_mode;

	struct {
		u8 size;
		u8 body[FIL_T_IE_MAX_SIZE];
	} __packed ies;
} __packed;

/**
 * struct fil_t_assoc_ind_req_info - Association event request information.
 * @type: &enum fil_t_assoc_req_frame_type
 * @pad: Unused, structure padding.
 * @capability: Network capability flags, &enum fil_bss_capability_flags.
 * @listen_interval: Management frame listen interval.
 * @ap_addr: Current access point MAC address.
 * @ie_size: Number of octets in the request portion of the
 *	information elements data.
 */
struct fil_t_assoc_ind_req_info {
	u8 type;
	u8 pad;
	__le16 capability;
	__le16 listen_interval;
	u8 ap_addr[ETH_ALEN];
	__le16 ie_size;
} __packed;

/**
 * enum fil_t_assoc_req_frame_type - Association request frame type.
 * @FIL_T_FRAME_TYPE_ASSOC_REQ: Association request frame type.
 * @FIL_T_FRAME_TYPE_REASSOC_REQ: Re-association request frame type.
 */
enum fil_t_assoc_req_frame_type {
	FIL_T_FRAME_TYPE_ASSOC_REQ		= 0x00,
	FIL_T_FRAME_TYPE_REASSOC_REQ		= 0x20
};

/**
 * struct fil_t_assoc_ind_resp_info - Association event response information.
 * @type: &enum fil_t_assoc_resp_frame_type
 * @pad: Unused, structure padding.
 * @capability: Network capability flags, &enum fil_bss_capability_flags.
 * @status: No known information. Most likely this is a subset of
 *	the 802.11 fixed-length management frame 'status' field.
 * @assoc_id: Management frame association identifier.
 * @ie_size: Number of octets in the request portion of the
 *	information elements data.
 */
struct fil_t_assoc_ind_resp_info {
	u8 type;
	u8 pad;
	__le16 capability;
	__le16 status;
	__le16 assoc_id;
	__le16 ie_size;
} __packed;

/**
 * enum fil_t_assoc_resp_frame_type - Association response frame type.
 * @FIL_T_FRAME_TYPE_ASSOC_RESP: Association response frame type.
 * @FIL_T_FRAME_TYPE_REASSOC_RESP: Re-association response frame type.
 */
enum fil_t_assoc_resp_frame_type {
	FIL_T_FRAME_TYPE_ASSOC_RESP		= 0x10,
	FIL_T_FRAME_TYPE_REASSOC_RESP		= 0x30
};

/**
 * struct fil_t_assoc_ind - y
 * @fhdr: &struct fil_t_hdr
 * @req: &struct fil_t_assoc_ind_req_info
 * @resp: &struct fil_t_assoc_ind_resp_info
 * @ies: Consecutive information elements, @req IE's followed by @resp IE's.
 */
struct fil_t_assoc_ind {
	struct fil_t_hdr fhdr;
	struct fil_t_assoc_ind_req_info req;
	struct fil_t_assoc_ind_resp_info resp;
	u8 ies[0];
	/* followed by (req->ie_size + resp->ie_size) octets of data */
} __packed;

/**
 * struct fil_eth_hdr - Firmware Interface Layer Ethernet frame header.
 * @h_dest: Destination MAC address.
 * @h_source: Source MAC address.
 * @snap: SNAP header.
 * @h_proto: Protocol ID.
 * @data: Upper layer data.
 */
struct fil_eth_hdr {
	u8 h_dest[ETH_ALEN];
	u8 h_source[ETH_ALEN];
	struct snap_hdr snap;
	__be16 h_proto;
	u8 data[0];
};
