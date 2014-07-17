/*
 * Copyright (c) 2014 Jes Sorensen <Jes.Sorensen@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * Register definitions taken from original Realttek rtl8723au driver
 */

#define RTL_MAX_VENDOR_REQ_CMD_SIZE	254
#define RTW_USB_CONTROL_MSG_TIMEOUT	500
#define RTLMAC_MAX_REG_POLL		500

#define REALTEK_USB_READ		0xc0
#define REALTEK_USB_WRITE		0x40
#define REALTEK_USB_CMD_REQ		0x05
#define REALTEK_USB_CMD_IDX		0x00

#define TX_TOTAL_PAGE_NUM		0xf8
/* (HPQ + LPQ + NPQ + PUBQ) = TX_TOTAL_PAGE_NUM */
#define TX_PAGE_NUM_PUBQ		0xe7
#define TX_PAGE_NUM_HI_PQ		0x0c
#define TX_PAGE_NUM_LO_PQ		0x02
#define TX_PAGE_NUM_NORM_PQ		0x02

#define RTL_FW_PAGE_SIZE		4096
#define RTLMAC_FIRMWARE_POLL_MAX	1000

#define RTL8723A_CHANNEL_GROUPS		3
#define RTL8723A_MAX_RF_PATHS		2
#define RF6052_MAX_TX_PWR		0x3f

#define EFUSE_MAP_LEN_8723A		256
#define EFUSE_MAX_SECTION_8723A		32
#define EFUSE_REAL_CONTENT_LEN_8723A	512
#define EFUSE_BT_MAP_LEN_8723A		1024
#define EFUSE_MAX_WORD_UNIT		4

struct rtlmac_firmware_header {
	__le16	signature;		/*  92C0: test chip; 92C,
					    88C0: test chip;
					    88C1: MP A-cut;
					    92C1: MP A-cut */
	u8	category;		/*  AP/NIC and USB/PCI */
	u8	function;

	__le16	major_version;		/*  FW Version */
	u8	minor_version;		/*  FW Subversion, default 0x00 */
	u8	reserved1;

	u8	month;			/*  Release time Month field */
	u8	date;			/*  Release time Date field */
	u8	hour;			/*  Release time Hour field */
	u8	minute;			/*  Release time Minute field */

	__le16	ramcodesize;		/*  Size of RAM code */
	u16	reserved2;

	__le32	svn_idx;		/*  SVN entry index */
	u32	reserved3;

	u32	reserved4;
	u32	reserved5;

	u8	data[0];
};

/*
 * The 8723au has 3 channel groups: 1-3, 4-9, and 10-14
 */
struct rtl8723au_idx {
#if defined (__LITTLE_ENDIAN)
	int	a:4;
	int	b:4;
#elif defined (__LITTLE_ENDIAN)
	int	b:4;
	int	a:4;
#else
#error "no endianess defined"
#endif
} __attribute__((packed));

struct rtl8723au_efuse {
	__le16 rtl_id;
	u8 res0[0xe];
	u8 cck_tx_power_index_A[3];	/* 0x10 */
	u8 cck_tx_power_index_B[3];
	u8 ht40_1s_tx_power_index_A[3];	/* 0x16 */
	u8 ht40_1s_tx_power_index_B[3];
	/*
	 * The following entries are half-bytes split as:
	 * bits 0-3: path A, bits 4-7: path B, all values 4 bits signed
	 */
	struct rtl8723au_idx ht20_tx_power_index_diff[3];
	struct rtl8723au_idx ofdm_tx_power_index_diff[3];
	struct rtl8723au_idx ht40_max_power_offset[3];
	struct rtl8723au_idx ht20_max_power_offset[3];
	u8 channel_plan;		/* 0x28 */
	u8 tssi_a;
	u8 thermal_meter;
	u8 rf_regulatory;
	u8 rf_option_2;
	u8 rf_option_3;
	u8 rf_option_4;
	u8 res7;
	u8 version			/* 0x30 */;
	u8 customer_id_major;
	u8 customer_id_minor;
	u8 xtal_k;
	u8 chipset;			/* 0x34 */
	u8 res8[0x82];
	u8 vid;				/* 0xb7 */
	u8 res9;
	u8 pid;				/* 0xb9 */
	u8 res10[0x0c];
	u8 mac_addr[ETH_ALEN];		/* 0xc6 */
	u8 res11[2];
	u8 vendor_name[7];
	u8 res12[2];
	u8 device_name[0x29];		/* 0xd7 */
};

struct rtlmac_reg8val {
	u16 reg;
	u8 val;
};

struct rtlmac_reg32val {
	u16 reg;
	u32 val;
};

struct rtlmac_rfregval {
	u8 reg;
	u32 val;
};

struct rtlmac_priv {
	struct ieee80211_hw *hw;
	struct usb_device *udev;
	u8 mac_addr[ETH_ALEN];
	u32 chip_cut:4;
	u32 rom_rev:4;
	u32 has_wifi:1;
	u32 has_bluetooth:1;
	u32 enable_bluetooth:1;
	u32 has_gps:1;
	u32 vendor_umc:1;
	u32 has_polarity_ctrl:1;
	u32 has_eeprom:1;
	u32 boot_eeprom:1;
	u32 ep_tx_high_queue:1;
	u32 ep_tx_normal_queue:1;
	u32 ep_tx_low_queue:1;
	int ep_tx_count;
	int rf_paths;
	u32 rf_mode_ag[2];
	struct rtlmac_firmware_header *fw_data;
	size_t fw_size;
	struct mutex usb_buf_mutex;
	union {
		__le32 val32;
		__le16 val16;
		u8 val8;
	} usb_buf;
	union {
		u8 raw[EFUSE_MAP_LEN_8723A];
		struct rtl8723au_efuse efuse;
	} efuse_wifi;
};
