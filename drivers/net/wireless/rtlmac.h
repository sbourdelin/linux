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

struct rtlmac_priv {
	struct ieee80211_hw *hw;
	struct usb_device *udev;
	u8 mac_addr[ETH_ALEN];
};

#define RTL_MAX_VENDOR_REQ_CMD_SIZE	254
#define RTW_USB_CONTROL_MSG_TIMEOUT	500
#define RTLMAC_MAX_REG_POLL		500

#define REALTEK_USB_READ		0xc0
#define REALTEK_USB_WRITE		0x40
#define REALTEK_USB_CMD_REQ		0x05
#define REALTEK_USB_CMD_IDX		0x00

#define LLT_LAST_TX_PAGE		0xf8

#define RTL_FW_PAGE_SIZE		4096
