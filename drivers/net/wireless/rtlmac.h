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

#define LLT_LAST_TX_PAGE		0xf8

#define RTL_FW_PAGE_SIZE		4096
#define RTLMAC_FIRMWARE_POLL_MAX	1000

#define EFUSE_MAP_LEN_8723A		256
#define EFUSE_BT_MAP_LEN_8723A		1024

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

struct rtlmac_reg8val {
	u16 reg;
	u8 val;
};

struct rtlmac_reg32val {
	u16 reg;
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
	u32 has_polarity_ctrl:1;
	u32 has_eeprom:1;
	u32 boot_eeprom:1;
	struct rtlmac_firmware_header *fw_data;
	size_t fw_size;
	u8 efuse_wifi[EFUSE_MAP_LEN_8723A];
};
