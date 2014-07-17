/*
 * RTL8723au mac80211 USB driver
 *
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
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/usb.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/wireless.h>
#include <linux/firmware.h>
#include <net/mac80211.h>
#include "rtlmac.h"
#include "rtlmac_regs.h"

#define DRIVER_NAME "rtlmac"

MODULE_AUTHOR("Jes Sorensen <Jes.Sorensen@redhat.com>");
MODULE_DESCRIPTION("RTL8723au USB mac80211 Wireless LAN Driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("rtlwifi/rtl8723aufw_A.bin");
MODULE_FIRMWARE("rtlwifi/rtl8723aufw_B.bin");
MODULE_FIRMWARE("rtlwifi/rtl8723aufw_B_NoBT.bin");

#define USB_VENDER_ID_REALTEK		0x0BDA

static struct usb_device_id dev_table[] = {
	/* Generic AT76C503/3861 device */
	{USB_DEVICE_AND_INTERFACE_INFO(USB_VENDER_ID_REALTEK, 0x8724,
				       0xff, 0xff, 0xff)},
	{USB_DEVICE_AND_INTERFACE_INFO(USB_VENDER_ID_REALTEK, 0x1724,
				       0xff, 0xff, 0xff)},
	{USB_DEVICE_AND_INTERFACE_INFO(USB_VENDER_ID_REALTEK, 0x0724,
				       0xff, 0xff, 0xff)},
	{ }
};

MODULE_DEVICE_TABLE(usb, dev_table);

static struct rtlmac_reg8val rtl8723a_mac_init_table[] = {
	{0x420, 0x80}, {0x423, 0x00}, {0x430, 0x00}, {0x431, 0x00},
	{0x432, 0x00}, {0x433, 0x01}, {0x434, 0x04}, {0x435, 0x05},
	{0x436, 0x06}, {0x437, 0x07}, {0x438, 0x00}, {0x439, 0x00},
	{0x43a, 0x00}, {0x43b, 0x01}, {0x43c, 0x04}, {0x43d, 0x05},
	{0x43e, 0x06}, {0x43f, 0x07}, {0x440, 0x5d}, {0x441, 0x01},
	{0x442, 0x00}, {0x444, 0x15}, {0x445, 0xf0}, {0x446, 0x0f},
	{0x447, 0x00}, {0x458, 0x41}, {0x459, 0xa8}, {0x45a, 0x72},
	{0x45b, 0xb9}, {0x460, 0x66}, {0x461, 0x66}, {0x462, 0x08},
	{0x463, 0x03}, {0x4c8, 0xff}, {0x4c9, 0x08}, {0x4cc, 0xff},
	{0x4cd, 0xff}, {0x4ce, 0x01}, {0x500, 0x26}, {0x501, 0xa2},
	{0x502, 0x2f}, {0x503, 0x00}, {0x504, 0x28}, {0x505, 0xa3},
	{0x506, 0x5e}, {0x507, 0x00}, {0x508, 0x2b}, {0x509, 0xa4},
	{0x50a, 0x5e}, {0x50b, 0x00}, {0x50c, 0x4f}, {0x50d, 0xa4},
	{0x50e, 0x00}, {0x50f, 0x00}, {0x512, 0x1c}, {0x514, 0x0a},
	{0x515, 0x10}, {0x516, 0x0a}, {0x517, 0x10}, {0x51a, 0x16},
	{0x524, 0x0f}, {0x525, 0x4f}, {0x546, 0x40}, {0x547, 0x00},
	{0x550, 0x10}, {0x551, 0x10}, {0x559, 0x02}, {0x55a, 0x02},
	{0x55d, 0xff}, {0x605, 0x30}, {0x608, 0x0e}, {0x609, 0x2a},
	{0x652, 0x20}, {0x63c, 0x0a}, {0x63d, 0x0a}, {0x63e, 0x0e},
	{0x63f, 0x0e}, {0x66e, 0x05}, {0x700, 0x21}, {0x701, 0x43},
	{0x702, 0x65}, {0x703, 0x87}, {0x708, 0x21}, {0x709, 0x43},
	{0x70a, 0x65}, {0x70b, 0x87}, {0xffff, 0xff},
};

static struct rtlmac_reg32val rtl8723a_phy_1t_init_table[] = {
	{0x800, 0x80040000}, {0x804, 0x00000003},
	{0x808, 0x0000fc00}, {0x80c, 0x0000000a},
	{0x810, 0x10001331}, {0x814, 0x020c3d10},
	{0x818, 0x02200385}, {0x81c, 0x00000000},
	{0x820, 0x01000100}, {0x824, 0x00390004},
	{0x828, 0x00000000}, {0x82c, 0x00000000},
	{0x830, 0x00000000}, {0x834, 0x00000000},
	{0x838, 0x00000000}, {0x83c, 0x00000000},
	{0x840, 0x00010000}, {0x844, 0x00000000},
	{0x848, 0x00000000}, {0x84c, 0x00000000},
	{0x850, 0x00000000}, {0x854, 0x00000000},
	{0x858, 0x569a569a}, {0x85c, 0x001b25a4},
	{0x860, 0x66f60110}, {0x864, 0x061f0130},
	{0x868, 0x00000000}, {0x86c, 0x32323200},
	{0x870, 0x07000760}, {0x874, 0x22004000},
	{0x878, 0x00000808}, {0x87c, 0x00000000},
	{0x880, 0xc0083070}, {0x884, 0x000004d5},
	{0x888, 0x00000000}, {0x88c, 0xccc000c0},
	{0x890, 0x00000800}, {0x894, 0xfffffffe},
	{0x898, 0x40302010}, {0x89c, 0x00706050},
	{0x900, 0x00000000}, {0x904, 0x00000023},
	{0x908, 0x00000000}, {0x90c, 0x81121111},
	{0xa00, 0x00d047c8}, {0xa04, 0x80ff000c},
	{0xa08, 0x8c838300}, {0xa0c, 0x2e68120f},
	{0xa10, 0x9500bb78}, {0xa14, 0x11144028},
	{0xa18, 0x00881117}, {0xa1c, 0x89140f00},
	{0xa20, 0x1a1b0000}, {0xa24, 0x090e1317},
	{0xa28, 0x00000204}, {0xa2c, 0x00d30000},
	{0xa70, 0x101fbf00}, {0xa74, 0x00000007},
	{0xa78, 0x00000900}, {0xc00, 0x48071d40},
	{0xc04, 0x03a05611}, {0xc08, 0x000000e4},
	{0xc0c, 0x6c6c6c6c}, {0xc10, 0x08800000},
	{0xc14, 0x40000100}, {0xc18, 0x08800000},
	{0xc1c, 0x40000100}, {0xc20, 0x00000000},
	{0xc24, 0x00000000}, {0xc28, 0x00000000},
	{0xc2c, 0x00000000}, {0xc30, 0x69e9ac44},
#if 0	/* Not for USB */
	{0xff0f011f, 0xabcd},
	{0xc34, 0x469652cf},
	{0xcdcdcdcd, 0xcdcd},
#endif
	{0xc34, 0x469652af},
#if 0
	{0xff0f011f, 0xdead},
#endif
	{0xc38, 0x49795994},
	{0xc3c, 0x0a97971c}, {0xc40, 0x1f7c403f},
	{0xc44, 0x000100b7}, {0xc48, 0xec020107},
	{0xc4c, 0x007f037f}, {0xc50, 0x69543420},
	{0xc54, 0x43bc0094}, {0xc58, 0x69543420},
	{0xc5c, 0x433c0094}, {0xc60, 0x00000000},
#if 0	/* Not for USB */
	{0xff0f011f, 0xabcd},
	{0xc64, 0x7116848b},
	{0xcdcdcdcd, 0xcdcd},
#endif
	{0xc64, 0x7112848b},
#if 0
	{0xff0f011f, 0xdead},
#endif
	{0xc68, 0x47c00bff},
	{0xc6c, 0x00000036}, {0xc70, 0x2c7f000d},
	{0xc74, 0x018610db}, {0xc78, 0x0000001f},
	{0xc7c, 0x00b91612}, {0xc80, 0x40000100},
	{0xc84, 0x20f60000}, {0xc88, 0x40000100},
	{0xc8c, 0x20200000}, {0xc90, 0x00121820},
	{0xc94, 0x00000000}, {0xc98, 0x00121820},
	{0xc9c, 0x00007f7f}, {0xca0, 0x00000000},
	{0xca4, 0x00000080}, {0xca8, 0x00000000},
	{0xcac, 0x00000000}, {0xcb0, 0x00000000},
	{0xcb4, 0x00000000}, {0xcb8, 0x00000000},
	{0xcbc, 0x28000000}, {0xcc0, 0x00000000},
	{0xcc4, 0x00000000}, {0xcc8, 0x00000000},
	{0xccc, 0x00000000}, {0xcd0, 0x00000000},
	{0xcd4, 0x00000000}, {0xcd8, 0x64b22427},
	{0xcdc, 0x00766932}, {0xce0, 0x00222222},
	{0xce4, 0x00000000}, {0xce8, 0x37644302},
	{0xcec, 0x2f97d40c}, {0xd00, 0x00080740},
	{0xd04, 0x00020401}, {0xd08, 0x0000907f},
	{0xd0c, 0x20010201}, {0xd10, 0xa0633333},
	{0xd14, 0x3333bc43}, {0xd18, 0x7a8f5b6b},
	{0xd2c, 0xcc979975}, {0xd30, 0x00000000},
	{0xd34, 0x80608000}, {0xd38, 0x00000000},
	{0xd3c, 0x00027293}, {0xd40, 0x00000000},
	{0xd44, 0x00000000}, {0xd48, 0x00000000},
	{0xd4c, 0x00000000}, {0xd50, 0x6437140a},
	{0xd54, 0x00000000}, {0xd58, 0x00000000},
	{0xd5c, 0x30032064}, {0xd60, 0x4653de68},
	{0xd64, 0x04518a3c}, {0xd68, 0x00002101},
	{0xd6c, 0x2a201c16}, {0xd70, 0x1812362e},
	{0xd74, 0x322c2220}, {0xd78, 0x000e3c24},
	{0xe00, 0x2a2a2a2a}, {0xe04, 0x2a2a2a2a},
	{0xe08, 0x03902a2a}, {0xe10, 0x2a2a2a2a},
	{0xe14, 0x2a2a2a2a}, {0xe18, 0x2a2a2a2a},
	{0xe1c, 0x2a2a2a2a}, {0xe28, 0x00000000},
	{0xe30, 0x1000dc1f}, {0xe34, 0x10008c1f},
	{0xe38, 0x02140102}, {0xe3c, 0x681604c2},
	{0xe40, 0x01007c00}, {0xe44, 0x01004800},
	{0xe48, 0xfb000000}, {0xe4c, 0x000028d1},
	{0xe50, 0x1000dc1f}, {0xe54, 0x10008c1f},
	{0xe58, 0x02140102}, {0xe5c, 0x28160d05},
	{0xe60, 0x00000008}, {0xe68, 0x001b25a4},
	{0xe6c, 0x631b25a0}, {0xe70, 0x631b25a0},
	{0xe74, 0x081b25a0}, {0xe78, 0x081b25a0},
	{0xe7c, 0x081b25a0}, {0xe80, 0x081b25a0},
	{0xe84, 0x631b25a0}, {0xe88, 0x081b25a0},
	{0xe8c, 0x631b25a0}, {0xed0, 0x631b25a0},
	{0xed4, 0x631b25a0}, {0xed8, 0x631b25a0},
	{0xedc, 0x001b25a0}, {0xee0, 0x001b25a0},
	{0xeec, 0x6b1b25a0}, {0xf14, 0x00000003},
	{0xf4c, 0x00000000}, {0xf00, 0x00000300},
	{0xffff, 0xffffffff},
};

static struct rtlmac_rfregval rtl8723au_radioa_rf6052_1t_init_table[] = {
	{0x00, 0x00030159}, {0x01, 0x00031284},
	{0x02, 0x00098000},
#if 0	/* Only for PCIE */
	{0xff0f011f, 0xabcd},
	{0x03, 0x00018c63},
	{0xcdcdcdcd, 0xcdcd},
#endif
	{0x03, 0x00039c63},
#if 0
	{0xff0f011f, 0xdead},
#endif
	{0x04, 0x000210e7}, {0x09, 0x0002044f},
	{0x0a, 0x0001a3f1}, {0x0b, 0x00014787},
	{0x0c, 0x000896fe}, {0x0d, 0x0000e02c},
	{0x0e, 0x00039ce7}, {0x0f, 0x00000451},
	{0x19, 0x00000000}, {0x1a, 0x00030355},
	{0x1b, 0x00060a00}, {0x1c, 0x000fc378},
	{0x1d, 0x000a1250}, {0x1e, 0x0000024f},
	{0x1f, 0x00000000}, {0x20, 0x0000b614},
	{0x21, 0x0006c000}, {0x22, 0x00000000},
	{0x23, 0x00001558}, {0x24, 0x00000060},
	{0x25, 0x00000483}, {0x26, 0x0004f000},
	{0x27, 0x000ec7d9}, {0x28, 0x00057730},
	{0x29, 0x00004783}, {0x2a, 0x00000001},
	{0x2b, 0x00021334}, {0x2a, 0x00000000},
	{0x2b, 0x00000054}, {0x2a, 0x00000001},
	{0x2b, 0x00000808}, {0x2b, 0x00053333},
	{0x2c, 0x0000000c}, {0x2a, 0x00000002},
	{0x2b, 0x00000808}, {0x2b, 0x0005b333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000003},
	{0x2b, 0x00000808}, {0x2b, 0x00063333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000004},
	{0x2b, 0x00000808}, {0x2b, 0x0006b333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000005},
	{0x2b, 0x00000808}, {0x2b, 0x00073333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000006},
	{0x2b, 0x00000709}, {0x2b, 0x0005b333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000007},
	{0x2b, 0x00000709}, {0x2b, 0x00063333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000008},
	{0x2b, 0x0000060a}, {0x2b, 0x0004b333},
	{0x2c, 0x0000000d}, {0x2a, 0x00000009},
	{0x2b, 0x0000060a}, {0x2b, 0x00053333},
	{0x2c, 0x0000000d}, {0x2a, 0x0000000a},
	{0x2b, 0x0000060a}, {0x2b, 0x0005b333},
	{0x2c, 0x0000000d}, {0x2a, 0x0000000b},
	{0x2b, 0x0000060a}, {0x2b, 0x00063333},
	{0x2c, 0x0000000d}, {0x2a, 0x0000000c},
	{0x2b, 0x0000060a}, {0x2b, 0x0006b333},
	{0x2c, 0x0000000d}, {0x2a, 0x0000000d},
	{0x2b, 0x0000060a}, {0x2b, 0x00073333},
	{0x2c, 0x0000000d}, {0x2a, 0x0000000e},
	{0x2b, 0x0000050b}, {0x2b, 0x00066666},
	{0x2c, 0x0000001a}, {0x2a, 0x000e0000},
	{0x10, 0x0004000f}, {0x11, 0x000e31fc},
	{0x10, 0x0006000f}, {0x11, 0x000ff9f8},
	{0x10, 0x0002000f}, {0x11, 0x000203f9},
	{0x10, 0x0003000f}, {0x11, 0x000ff500},
	{0x10, 0x00000000}, {0x11, 0x00000000},
	{0x10, 0x0008000f}, {0x11, 0x0003f100},
	{0x10, 0x0009000f}, {0x11, 0x00023100},
	{0x12, 0x00032000}, {0x12, 0x00071000},
	{0x12, 0x000b0000}, {0x12, 0x000fc000},
	{0x13, 0x000287b3}, {0x13, 0x000244b7},
	{0x13, 0x000204ab}, {0x13, 0x0001c49f},
	{0x13, 0x00018493}, {0x13, 0x0001429b},
	{0x13, 0x00010299}, {0x13, 0x0000c29c},
	{0x13, 0x000081a0}, {0x13, 0x000040ac},
	{0x13, 0x00000020}, {0x14, 0x0001944c},
	{0x14, 0x00059444}, {0x14, 0x0009944c},
	{0x14, 0x000d9444},
#if 0	/* Only for PCIE */
	{0xff0f011f, 0xabcd},
	{0x15, 0x0000f424},
	{0x15, 0x0004f424},
	{0x15, 0x0008f424},
	{0x15, 0x000cf424},
	{0xcdcdcdcd, 0xcdcd},
#endif
	{0x15, 0x0000f474}, {0x15, 0x0004f477},
	{0x15, 0x0008f455}, {0x15, 0x000cf455},
#if 0
	{0xff0f011f, 0xdead},
#endif
	{0x16, 0x00000339}, {0x16, 0x00040339},
	{0x16, 0x00080339},
#if 0	/* Only for PCIE */
	{0xff0f011f, 0xabcd},
	{0x16, 0x000c0356},
	{0xcdcdcdcd, 0xcdcd},
#endif
	{0x16, 0x000c0366},
#if 0
	{0xff0f011f, 0xdead},
#endif
	{0x00, 0x00010159}, {0x18, 0x0000f401},
	{0xfe, 0x00000000}, {0xfe, 0x00000000},
	{0x1f, 0x00000003}, {0xfe, 0x00000000},
	{0xfe, 0x00000000}, {0x1e, 0x00000247},
	{0x1f, 0x00000000}, {0x00, 0x00030159},
	{0xff, 0xffffffff}
};

u8 rtl8723au_read8(struct rtlmac_priv *priv, u16 addr)
{
	struct usb_device *udev = priv->udev;
	int len;
	u8 data;

	mutex_lock(&priv->usb_buf_mutex);
	len = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_READ,
			      addr, 0, &priv->usb_buf.val8, sizeof(u8),
			      RTW_USB_CONTROL_MSG_TIMEOUT);
	data = priv->usb_buf.val8;
	mutex_unlock(&priv->usb_buf_mutex);

	printk(KERN_DEBUG "%s(%04x)   = 0x%02x, len %i\n",
	       __func__, addr, data, len);
	return data;
}

u16 rtl8723au_read16(struct rtlmac_priv *priv, u16 addr)
{
	struct usb_device *udev = priv->udev;
	int len;
	u16 data;

	mutex_lock(&priv->usb_buf_mutex);
	len = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_READ,
			      addr, 0, &priv->usb_buf.val16, sizeof(u16),
			      RTW_USB_CONTROL_MSG_TIMEOUT);
	data = le16_to_cpu(priv->usb_buf.val16);
	mutex_unlock(&priv->usb_buf_mutex);

	printk(KERN_DEBUG "%s(%04x)  = 0x%04x, len %i\n",
	       __func__, addr, data, len);
	return data;
}

u32 rtl8723au_read32(struct rtlmac_priv *priv, u16 addr)
{
	struct usb_device *udev = priv->udev;
	int len;
	u32 data;

	mutex_lock(&priv->usb_buf_mutex);
	len = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_READ,
			      addr, 0, &priv->usb_buf.val32, sizeof(u32),
			      RTW_USB_CONTROL_MSG_TIMEOUT);
	data = le32_to_cpu(priv->usb_buf.val32);
	mutex_unlock(&priv->usb_buf_mutex);

	printk(KERN_DEBUG "%s(%04x)  = 0x%08x, len %i\n",
	       __func__, addr, data, len);
	return data;
}

int rtl8723au_write8(struct rtlmac_priv *priv, u16 addr, u8 val)
{
	struct usb_device *udev = priv->udev;
	int ret;

	mutex_lock(&priv->usb_buf_mutex);
	priv->usb_buf.val8 = val;
	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, &priv->usb_buf.val8, sizeof(u8),
			      RTW_USB_CONTROL_MSG_TIMEOUT);

	mutex_unlock(&priv->usb_buf_mutex);

	printk(KERN_DEBUG "%s(%04x)  = 0x%02x, ret %i\n",
	       __func__, addr, val, ret);
	return ret;
}

int rtl8723au_write16(struct rtlmac_priv *priv, u16 addr, u16 val)
{
	struct usb_device *udev = priv->udev;
	int ret;

	mutex_lock(&priv->usb_buf_mutex);
	priv->usb_buf.val16 = cpu_to_le16(val);
	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, &priv->usb_buf.val16, sizeof(u16),
			      RTW_USB_CONTROL_MSG_TIMEOUT);
	mutex_unlock(&priv->usb_buf_mutex);

	printk(KERN_DEBUG "%s(%04x) = 0x%04x, ret %i\n",
	       __func__, addr, val, ret);
	return ret;
}

int rtl8723au_write32(struct rtlmac_priv *priv, u16 addr, u32 val)
{
	struct usb_device *udev = priv->udev;
	int ret;

	mutex_lock(&priv->usb_buf_mutex);
	priv->usb_buf.val32 = cpu_to_le32(val);
	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, &priv->usb_buf.val32, sizeof(u32),
			      RTW_USB_CONTROL_MSG_TIMEOUT);
	mutex_unlock(&priv->usb_buf_mutex);

	printk(KERN_DEBUG "%s(%04x) = 0x%08x, ret %i\n",
	       __func__, addr, val, ret);
	return ret;
}

int rtl8723au_writeN(struct rtlmac_priv *priv, u16 addr, u8 *buf, u16 len)
{
	struct usb_device *udev = priv->udev;
	int ret;

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, buf, len, RTW_USB_CONTROL_MSG_TIMEOUT);

	printk(KERN_DEBUG "%s(%04x) = %p, len 0x%02x\n",
	       __func__, addr, buf, len);
	return ret;
}

static u32 rtl8723au_read_rfreg(struct rtlmac_priv *priv, u8 reg)
{
	u32 hssia, val32, retval;
	printk(KERN_DEBUG "reading rfreg %02x\n", reg);

	hssia = rtl8723au_read32(priv, REG_FPGA0_XA_HSSI_PARM2);
	/*
	 * For path B it seems we should be reading REG_FPGA0_XB_HSSI_PARM1
	 * into val32
	 */
	val32 = hssia;
	val32 &= ~FPGA0_HSSI_PARM2_ADDR_MASK;
	val32 |= (reg << FPGA0_HSSI_PARM2_ADDR_SHIFT) |
		FPGA0_HSSI_PARM2_EDGE_READ;
	rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM2,
			  hssia &= ~FPGA0_HSSI_PARM2_EDGE_READ);
	udelay(10);
	/* Here use XB for path B */
	rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM2, val32);
	udelay(100);
	rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM2,
			  hssia |= FPGA0_HSSI_PARM2_EDGE_READ);
	udelay(10);
	/* Use XB for path B */
	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_HSSI_PARM1);
	if (val32 & BIT(8))	/* RF PI enabled */
		retval = rtl8723au_read32(priv, REG_HSPI_XA_READBACK);
	else
		retval = rtl8723au_read32(priv, REG_FPGA0_XA_LSSI_READBACK);

	retval &= 0xfffff;

	return retval;
}

static int rtl8723au_write_rfreg(struct rtlmac_priv *priv, u8 reg, u32 data)
{
	int ret, retval;
	u32 dataaddr;

	printk(KERN_DEBUG "%s(%02x) = 0x%06x\n", __func__, reg, data);

	data &= FPGA0_LSSI_PARM_DATA_MASK;
	dataaddr = (reg << FPGA0_LSSI_PARM_ADDR_SHIFT) | data;

	/* Use XB for path B */
	ret = rtl8723au_write32(priv, REG_FPGA0_XA_LSSI_PARM, dataaddr);
	if (ret != sizeof(dataaddr))
		retval = -EIO;
	else
		retval = 0;

	udelay(1);

	return retval;
}

/*
 * The rtl8723a has 3 channel groups for it's efuse settings. It only
 * supports the 2.4GHz band, so channels 1 - 14:
 *  group 0: channels 1 - 3
 *  group 1: channels 4 - 9
 *  group 2: channels 10 - 14
 *
 * Note: We index from 0 in the code
 */
static int rtl8723a_channel_to_group(int channel)
{
	int group;

	if (channel < 4)
		group = 0;
	else if (channel < 10)
		group = 1;
	else
		group = 2;

	return group;
}

static void
rtl8723a_set_tx_power(struct rtlmac_priv *priv, int channel, bool ht20)
{
	struct rtl8723au_efuse *efuse;
	u8 cck[RTL8723A_MAX_RF_PATHS], ofdm[RTL8723A_MAX_RF_PATHS];
	u8 ofdmbase[RTL8723A_MAX_RF_PATHS], mcsbase[RTL8723A_MAX_RF_PATHS];
	u32 val32;
	int group, i;

	efuse = &priv->efuse_wifi.efuse;

	group = rtl8723a_channel_to_group(channel);

	cck[0] = efuse->cck_tx_power_index_A[group];
	cck[1] = efuse->cck_tx_power_index_B[group];

	ofdm[0] = efuse->ht40_1s_tx_power_index_A[group];
	ofdm[1] = efuse->ht40_1s_tx_power_index_B[group];

	printk(KERN_DEBUG "%s: Setting TX power CCK A: %i, CCK B: %i, "
	       "OFDM A: %i, OFD`M B: %i\n", DRIVER_NAME,
	       cck[0], cck[1], ofdm[0], ofdm[1]);
	printk(KERN_DEBUG "%s: Regulatory 0x%02x\n",
	       DRIVER_NAME, efuse->rf_regulatory);

	for (i = 0; RTL8723A_MAX_RF_PATHS; i++) {
		if (cck[i] > RF6052_MAX_TX_PWR)
			cck[i] = RF6052_MAX_TX_PWR;
		if (ofdm[i] > RF6052_MAX_TX_PWR)
			ofdm[i] = RF6052_MAX_TX_PWR;
	}

	val32 = rtl8723au_read32(priv, REG_TX_AGC_A_CCK1_MCS32);
	val32 &= 0xffff00ff;
	val32 |= (cck[0] << 8);
	rtl8723au_write32(priv, REG_TX_AGC_A_CCK1_MCS32, val32);

	val32 = rtl8723au_read32(priv, REG_TX_AGC_B_CCK11_A_CCK2_11);
	val32 &= 0xff;
	val32 |= ((cck[0] << 8) | (cck[0] << 16) | (cck[0] << 24));
	rtl8723au_write32(priv, REG_TX_AGC_B_CCK11_A_CCK2_11, val32);

	val32 = rtl8723au_read32(priv, REG_TX_AGC_B_CCK11_A_CCK2_11);
	val32 &= 0xffffff00;
	val32 |= cck[1];
	rtl8723au_write32(priv, REG_TX_AGC_B_CCK11_A_CCK2_11, val32);

	val32 = rtl8723au_read32(priv, REG_TX_AGC_B_CCK1_55_MCS32);
	val32 &= 0xff;
	val32 |= ((cck[1] << 8) | (cck[1] << 16) | (cck[1] << 24));
	rtl8723au_write32(priv, REG_TX_AGC_B_CCK1_55_MCS32, val32);

	ofdmbase[0] = ofdm[0] +	efuse->ofdm_tx_power_index_diff[group].a;
	mcsbase[0] = ofdm[0];
	if (!ht20)
		mcsbase[0] += efuse->ht20_tx_power_index_diff[group].a;

	ofdmbase[1] = ofdm[1] +	efuse->ofdm_tx_power_index_diff[group].b;
	mcsbase[1] = ofdm[1];
	if (!ht20)
		mcsbase[1] += efuse->ht20_tx_power_index_diff[group].b;

	val32 = ofdmbase[0] | ofdmbase[0] << 8 |
		ofdmbase[0] << 16 | ofdmbase[0] <<24;
	rtl8723au_write32(priv, REG_TX_AGC_A_RATE18_06, val32);
	rtl8723au_write32(priv, REG_TX_AGC_A_RATE54_24, val32);

	val32 = mcsbase[0] | mcsbase[0] << 8 |
		mcsbase[0] << 16 | mcsbase[0] <<24;
	rtl8723au_write32(priv, REG_TX_AGC_A_MCS03_MCS00, val32);
	rtl8723au_write32(priv, REG_TX_AGC_A_MCS07_MCS04, val32);
	rtl8723au_write32(priv, REG_TX_AGC_A_MCS11_MCS08, val32);
	rtl8723au_write32(priv, REG_TX_AGC_A_MCS15_MCS12, val32);

	val32 = ofdmbase[1] | ofdmbase[1] << 8 |
		ofdmbase[1] << 16 | ofdmbase[1] <<24;
	rtl8723au_write32(priv, REG_TX_AGC_B_RATE18_06, val32);
	rtl8723au_write32(priv, REG_TX_AGC_B_RATE54_24, val32);

	val32 = mcsbase[1] | mcsbase[1] << 8 |
		mcsbase[1] << 16 | mcsbase[1] <<24;
	rtl8723au_write32(priv, REG_TX_AGC_B_MCS03_MCS00, val32);
	rtl8723au_write32(priv, REG_TX_AGC_B_MCS07_MCS04, val32);
	rtl8723au_write32(priv, REG_TX_AGC_B_MCS11_MCS08, val32);
	rtl8723au_write32(priv, REG_TX_AGC_B_MCS15_MCS12, val32);
}

static void rtlmac_set_linktype(struct rtlmac_priv *priv, u16 linktype)
{
	u16 val16;

	val16 = rtl8723au_read16(priv, REG_MSR);
	val16 &= ~MSR_LINKTYPE_MASK;
	val16 |= linktype;
	rtl8723au_write16(priv, REG_MSR, val16);
}

static void
rtlmac_set_retry(struct rtlmac_priv *priv, u16 short_retry, u16 long_retry)
{
	u16 val16;

	val16 = ((short_retry << RETRY_LIMIT_SHORT_SHIFT) &
		 RETRY_LIMIT_SHORT_MASK) |
		((long_retry << RETRY_LIMIT_LONG_SHIFT) &
		 RETRY_LIMIT_LONG_MASK);

	rtl8723au_write16(priv, REG_RETRY_LIMIT, val16);
}

static void rtlmac_set_spec_sifs(struct rtlmac_priv *priv, u16 cck, u16 ofdm)
{
	u16 val16;

	val16 = ((cck << SPEC_SIFS_CCK_SHIFT) & SPEC_SIFS_CCK_MASK) |
		((ofdm << SPEC_SIFS_OFDM_SHIFT) & SPEC_SIFS_OFDM_MASK);

	rtl8723au_write16(priv, REG_SPEC_SIFS, val16);
}

static int rtlmac_8723au_identify_chip(struct rtlmac_priv *priv)
{
	u32 val32;
	u16 val16;
	int ret = 0;
	char *cut;

	val32 = rtl8723au_read32(priv, REG_SYS_CFG);
	priv->chip_cut = (val32 & SYS_CFG_CHIP_VERSION_MASK) >>
		SYS_CFG_CHIP_VERSION_SHIFT;
	switch(priv->chip_cut) {
	case 0:
		cut = "A";
		break;
	case 1:
		cut = "B";
		break;
	default:
		cut = "unknown";
	}

	val32 = rtl8723au_read32(priv, REG_GPIO_OUTSTS);
	priv->rom_rev = (val32 & GPIO_RF_RL_ID) >> 28;

	val32 = rtl8723au_read32(priv, REG_MULTI_FUNC_CTRL);
	if (val32 & MULTI_WIFI_FUNC_EN)
		priv->has_wifi = 1;
	if (val32 & MULTI_BT_FUNC_EN)
		priv->has_bluetooth = 1;
	if (val32 & MULTI_GPS_FUNC_EN)
		priv->has_gps = 1;

	/* The rtl8192 presumably can have 2 */
	priv->rf_paths = 1;

	val16 = rtl8723au_read16(priv, REG_NORMAL_SIE_EP_TX);
	if (val16 & NORMAL_SIE_EP_TX_HIGH_MASK) {
		priv->ep_tx_high_queue = 1;
		priv->ep_tx_count++;
	}

	if (val16 & NORMAL_SIE_EP_TX_NORMAL_MASK) {
		priv->ep_tx_normal_queue = 1;
		priv->ep_tx_count++;
	}

	if (val16 & NORMAL_SIE_EP_TX_LOW_MASK) {
		priv->ep_tx_low_queue = 1;
		priv->ep_tx_count++;
	}

	printk(KERN_INFO
	       "%s: RTL8723au rev %s, features: WiFi=%i, BT=%i, GPS=%i\n",
	       DRIVER_NAME, cut, priv->has_wifi, priv->has_bluetooth,
	       priv->has_gps);

	printk(KERN_DEBUG "%s: RTL8723au number of TX queues: %i\n",
	       DRIVER_NAME, priv->ep_tx_count);

	return ret;
}

static int rtlmac_read_efuse8(struct rtlmac_priv *priv, u16 offset, u8 *data)
{
	int i;
	u8 val8;
	u32 val32;

	/* Write Address */
	rtl8723au_write8(priv, REG_EFUSE_CTRL + 1, offset & 0xff);
	val8 = rtl8723au_read8(priv, REG_EFUSE_CTRL + 2);
	val8 &= 0xfc;
	val8 |= (offset >> 8) & 0x03;
	rtl8723au_write8(priv, REG_EFUSE_CTRL + 2, val8);

	val8 = rtl8723au_read8(priv, REG_EFUSE_CTRL + 3);
	rtl8723au_write8(priv, REG_EFUSE_CTRL + 3, val8 & 0x7f);

	/* Poll for data read */
	val32 = rtl8723au_read32(priv, REG_EFUSE_CTRL);
	for (i = 0; i < RTLMAC_MAX_REG_POLL; i++) {
		val32 = rtl8723au_read32(priv, REG_EFUSE_CTRL);
		if (val32 & BIT(31))
			break;
	}

	if (i == RTLMAC_MAX_REG_POLL)
		return -EIO;

	udelay(50);
	val32 = rtl8723au_read32(priv, REG_EFUSE_CTRL);

	*data = val32 & 0xff;
	return 0;
}

static int rtlmac_read_efuse(struct rtlmac_priv *priv)
{
	int i, ret = 0;
	u8 val8, word_mask, header, extheader;
	u16 val16, efuse_addr, offset;
	u32 val32;

	val16 = rtl8723au_read16(priv, REG_9346CR);
	if (val16 & EEPROM_ENABLE)
		priv->has_eeprom = 1;
	if (val16 & EEPROM_BOOT)
		priv->boot_eeprom = 1;

	val32 = rtl8723au_read32(priv, REG_EFUSE_TEST);
	val32 = (val32 & ~EFUSE_SELECT_MASK) | EFUSE_WIFI_SELECT;
	rtl8723au_write32(priv, REG_EFUSE_TEST, val32);

	printk(KERN_DEBUG "%s: Booting from %s\n", DRIVER_NAME,
	       priv->boot_eeprom ? "EEPROM" : "EFUSE");

	rtl8723au_write8(priv, REG_EFUSE_ACCESS, EFUSE_ACCESS_ENABLE);

	/*  1.2V Power: From VDDON with Power Cut(0x0000[15]), default valid */
	val16 = rtl8723au_read16(priv, REG_SYS_ISO_CTRL);
	if (!(val16 & SYS_ISO_PWC_EV12V)) {
		val16 |= SYS_ISO_PWC_EV12V;
		rtl8723au_write16(priv, REG_SYS_ISO_CTRL, val16);
	}
	/*  Reset: 0x0000[28], default valid */
	val16 = rtl8723au_read16(priv, REG_SYS_FUNC);
	if (!(val16 & SYS_FUNC_ELDR)) {
		val16 |= SYS_FUNC_ELDR;
		rtl8723au_write16(priv, REG_SYS_FUNC, val16);
	}

	/* Clock: Gated(0x0008[5]) 8M(0x0008[1]) clock from ANA,
	   default valid */
	val16 = rtl8723au_read16(priv, REG_SYS_CLKR);
	if (!(val16 & SYS_CLK_LOADER_ENABLE) || !(val16 & SYS_CLK_ANA8M)) {
		val16 |= (SYS_CLK_LOADER_ENABLE | SYS_CLK_ANA8M);
		rtl8723au_write16(priv, REG_SYS_CLKR, val16);
	}

	/* Default value is 0xff */
	memset(priv->efuse_wifi.raw, 0xff, EFUSE_MAP_LEN_8723A);

	efuse_addr = 0;
	while (efuse_addr < EFUSE_REAL_CONTENT_LEN_8723A) {
		ret = rtlmac_read_efuse8(priv, efuse_addr++, &header);
		if (ret || header == 0xff)
			goto exit;

		if ((header & 0x1f) == 0x0f) {	/* extended header */
			offset = (header & 0xe0) >> 5;

			ret = rtlmac_read_efuse8(priv, efuse_addr++,
						 &extheader);
			if (ret)
				goto exit;
			/* All words disabled */
			if ((extheader & 0x0f) == 0x0f)
				continue;

			offset |= ((extheader & 0xf0) >> 1);
			word_mask = extheader & 0x0f;
		} else {
			offset = (header >> 4) & 0x0f;
			word_mask = header & 0x0f;
		}

		if (offset < EFUSE_MAX_SECTION_8723A) {
			u16 map_addr;
			/* Get word enable value from PG header */

			/* We have 8 bits to indicate validity */
			map_addr = offset * 8;
			if (map_addr >= EFUSE_MAP_LEN_8723A) {
				printk(KERN_DEBUG "%s: %s: Illegal map_addr "
				       "(%04x), efuse corrupt!\n", DRIVER_NAME,
				       __func__, map_addr);
			ret = -EINVAL;
			goto exit;

				ret = -EINVAL;
			}
			for (i = 0; i < EFUSE_MAX_WORD_UNIT; i++) {
				/* Check word enable condition in the section */
				if (!(word_mask & BIT(i))) {
					ret = rtlmac_read_efuse8(priv,
								 efuse_addr++,
								 &val8);
					priv->efuse_wifi.raw[map_addr++] = val8;

					ret = rtlmac_read_efuse8(priv,
								 efuse_addr++,
								 &val8);
					priv->efuse_wifi.raw[map_addr++] = val8;
				} else
					map_addr += 2;
			}
		} else {
			printk(KERN_DEBUG "%s: %s: Illegal offset (%04x), "
			       "efuse corrupt!\n", DRIVER_NAME, __func__,
			       offset);
			ret = -EINVAL;
			goto exit;
		}
	}

exit:
	rtl8723au_write8(priv, REG_EFUSE_ACCESS, EFUSE_ACCESS_DISABLE);

	if (priv->efuse_wifi.efuse.rtl_id != cpu_to_le16(0x8129))
		ret = EINVAL;

	return ret;
}

static int rtlmac_start_firmware(struct rtlmac_priv *priv)
{
	int ret = 0, i;
	u32 val32;

	/* Poll checksum report */
	for (i = 0; i < RTLMAC_FIRMWARE_POLL_MAX; i++) {
		val32 = rtl8723au_read32(priv, REG_MCU_FW_DL);
		if (val32 & MCU_FW_DL_CSUM_REPORT)
			break;
	}

	if (i == RTLMAC_FIRMWARE_POLL_MAX) {
		printk(KERN_WARNING "%s: Firmware checksum poll timed out\n",
		       DRIVER_NAME);
		ret = -EAGAIN;
		goto exit;
	}

	val32 = rtl8723au_read32(priv, REG_MCU_FW_DL);
	val32 |= MCU_FW_DL_READY;
	val32 &= ~MCU_WINT_INIT_READY;
	rtl8723au_write32(priv, REG_MCU_FW_DL, val32);

	/* Wait for firmware to become ready */
	for (i = 0; i < RTLMAC_FIRMWARE_POLL_MAX; i++) {
		val32 = rtl8723au_read32(priv, REG_MCU_FW_DL);
		if (val32 & MCU_WINT_INIT_READY)
			break;

		udelay(100);
	}

	if (i == RTLMAC_FIRMWARE_POLL_MAX) {
		printk(KERN_WARNING "%s: Firmware failed to start\n",
		       DRIVER_NAME);
		ret = -EAGAIN;
		goto exit;
	}

exit:
	return ret;
}

static int rtlmac_download_firmware(struct rtlmac_priv *priv)
{
	int pages, remainder, i, ret;
	u8 val8;
	u16 val16;
	u32 val32;
	u8 *fwptr;

	/* 8051 enable */
	val16 = rtl8723au_read16(priv, REG_SYS_FUNC);
	rtl8723au_write16(priv, REG_SYS_FUNC, val16 | SYS_FUNC_CPU_ENABLE);

	/* MCU firmware download enable */
	val8 = rtl8723au_read8(priv, REG_MCU_FW_DL);
	rtl8723au_write8(priv, REG_MCU_FW_DL, val8 | MCU_FW_DL_ENABLE);

	/* 8051 reset */
	val32 = rtl8723au_read32(priv, REG_MCU_FW_DL);
	rtl8723au_write32(priv, REG_MCU_FW_DL, val32 & ~BIT(19));

	/* Reset firmware download checksum */
	val8 = rtl8723au_read8(priv, REG_MCU_FW_DL);
	rtl8723au_write8(priv, REG_MCU_FW_DL, val8 | MCU_FW_DL_CSUM_REPORT);

	pages = priv->fw_size / RTL_FW_PAGE_SIZE;
	remainder = priv->fw_size % RTL_FW_PAGE_SIZE;

	fwptr = priv->fw_data->data;

	for (i = 0; i < pages; i++) {
		val8 = rtl8723au_read8(priv, REG_MCU_FW_DL + 2) & 0xF8;
		rtl8723au_write8(priv, REG_MCU_FW_DL + 2, val8 | i);

		ret = rtl8723au_writeN(priv, REG_8723A_FW_START_ADDRESS,
				       fwptr, RTL_FW_PAGE_SIZE);
		if (ret != RTL_FW_PAGE_SIZE) {
			ret = -EAGAIN;
			goto fw_abort;
		}

		fwptr += RTL_FW_PAGE_SIZE;
	}

	if (remainder) {
		val8 = rtl8723au_read8(priv, REG_MCU_FW_DL + 2) & 0xF8;
		rtl8723au_write8(priv, REG_MCU_FW_DL + 2, val8 | i);
		ret = rtl8723au_writeN(priv, REG_8723A_FW_START_ADDRESS,
				       fwptr, remainder);
		if (ret != remainder) {
			ret = -EAGAIN;
			goto fw_abort;
		}

	}

	ret = 0;
fw_abort:
	/* MCU firmware download disable */
	val16 = rtl8723au_read16(priv, REG_MCU_FW_DL);
	rtl8723au_write16(priv, REG_MCU_FW_DL,
			  val16 & (~MCU_FW_DL_ENABLE & 0xff));

	return ret;
}

static int rtlmac_load_firmware(struct rtlmac_priv *priv)
{
	const struct firmware *fw;
	char *fw_name;
	int ret = 0;
	u16 signature;

	switch(priv->chip_cut) {
	case 0:
		fw_name = "rtlwifi/rtl8723aufw_A.bin";
		break;
	case 1:
		if (priv->enable_bluetooth)
			fw_name = "rtlwifi/rtl8723aufw_B.bin";
		else
			fw_name = "rtlwifi/rtl8723aufw_B_NoBT.bin";

		break;
	default:
		return -EINVAL;
	}

	printk(KERN_DEBUG "%s: Loading firmware %s\n", DRIVER_NAME, fw_name);
	if (request_firmware(&fw, fw_name, &priv->udev->dev)) {
		printk(KERN_WARNING "%s: request_firmware(%s) failed\n",
		       DRIVER_NAME, fw_name);
		ret = -EAGAIN;
		goto exit;
	}
	if (!fw) {
		printk(KERN_WARNING "%s: Firmware data not available\n",
		       DRIVER_NAME);
		ret = -EINVAL;
		goto exit;
	}

	priv->fw_data = kmemdup(fw->data, fw->size, GFP_KERNEL);
	priv->fw_size = fw->size - sizeof(struct rtlmac_firmware_header);

	signature = le16_to_cpu(priv->fw_data->signature);
	switch(signature & 0xfff0) {
	case 0x92c0:
	case 0x88c0:
	case 0x2300:
		break;
	default:
		ret = -EINVAL;
		printk(KERN_DEBUG "%s: Invalid firmware signature: 0x%04x\n",
		       DRIVER_NAME, signature);
	}

	printk(KERN_DEBUG "%s: Firmware revision %i.%i (signature 0x%04x)\n",
	       DRIVER_NAME, le16_to_cpu(priv->fw_data->major_version),
	       priv->fw_data->minor_version, signature);

exit:
	release_firmware(fw);
	return ret;
}

static int rtlmac_init_mac(struct rtlmac_priv *priv,
			   struct rtlmac_reg8val *array)
{
	int i, ret;
	u16 reg;
	u8 val;

	for (i = 0; ; i++) {
		reg = array[i].reg;
		val = array[i].val;

		if (reg == 0xffff && val == 0xff)
			break;

		ret = rtl8723au_write8(priv, reg, val);
		if (ret != 1) {
			printk(KERN_WARNING "%s: Failed to initialize MAC\n",
			       DRIVER_NAME);
			return -EAGAIN;
		}
	}

	return 0;
}

static int rtlmac_init_phy_regs(struct rtlmac_priv *priv,
				struct rtlmac_reg32val *array)
{
	int i, ret;
	u16 reg;
	u32 val;

	for (i = 0; ; i++) {
		reg = array[i].reg;
		val = array[i].val;

		if (reg == 0xffff && val == 0xffffffff)
			break;

		ret = rtl8723au_write32(priv, reg, val);
		if (ret != sizeof(val)) {
			printk(KERN_WARNING "%s: Failed to initialize PHY\n",
			       DRIVER_NAME);
			return -EAGAIN;
		}
		udelay(1);
	}

	return 0;
}

/*
 * Most of this is black magic retrieved from the old rtl8723au driver
 */
static int rtlmac_init_phy_bb(struct rtlmac_priv *priv)
{
	u8 val8, ldoa15, ldov12d, lpldo, ldohci12;
	u32 val32;

	/*
	 * Todo: The vendor driver maintains a table of PHY register
	 *       addresses, which is initialized here. Do we need this?
	 */

	val8 = rtl8723au_read8(priv, REG_AFE_PLL_CTRL);
	udelay(2);
	val8 |= AFE_PLL_320_ENABLE;
	rtl8723au_write8(priv, REG_AFE_PLL_CTRL, val8);
	udelay(2);

	rtl8723au_write8(priv, REG_AFE_PLL_CTRL + 1, 0xff);
	udelay(2);

	val8 = rtl8723au_read8(priv, REG_SYS_FUNC);
	val8 |= SYS_FUNC_BB_GLB_RSTN | SYS_FUNC_BBRSTB;
	rtl8723au_write8(priv, REG_SYS_FUNC, val8);

	/* AFE_XTAL_RF_GATE (bit 14) if addressing as 32 bit register */
	val8 = rtl8723au_read8(priv, REG_AFE_XTAL_CTRL + 1);
	val8 &= ~BIT(6);
	rtl8723au_write8(priv, REG_AFE_XTAL_CTRL + 1, val8);

	/* AFE_XTAL_BT_GATE (bit 20) if addressing as 32 bit register */
	val8 = rtl8723au_read8(priv, REG_AFE_XTAL_CTRL + 2);
	val8 &= ~BIT(4);
	rtl8723au_write8(priv, REG_AFE_XTAL_CTRL + 2, val8);

	/* 6. 0x1f[7:0] = 0x07 */
	val8 = RF_ENABLE | RF_RSTB | RF_SDMRSTB;
	rtl8723au_write8(priv, REG_RF_CTRL, val8);

	rtlmac_init_phy_regs(priv, rtl8723a_phy_1t_init_table);
	if (priv->efuse_wifi.efuse.version >= 0x01) {
		val32 = rtl8723au_read32(priv, REG_MAC_PHY_CTRL);

		val8 = priv->efuse_wifi.efuse.xtal_k & 0x3f;
		val32 &= 0xff000fff;
		val32 |= ((val8 | (val8 << 6)) << 12);

		rtl8723au_write32(priv, REG_MAC_PHY_CTRL, val32);
	}

	ldoa15 = LDOA15_ENABLE | LDOA15_OBUF;
	ldov12d = LDOV12D_ENABLE | BIT(2) | (2 << LDOV12D_VADJ_SHIFT);
	ldohci12 = 0x57;
	lpldo = 1;
	val32 = (lpldo << 24) | (ldohci12 << 16) | (ldov12d << 8)| ldoa15;

	rtl8723au_write32(priv, REG_LDOA15_CTRL, val32);

	return 0;
}

static int rtlmac_init_rf_regs(struct rtlmac_priv *priv,
			       struct rtlmac_rfregval *array)
{
	int i, ret;
	u8 reg;
	u32 val;

	for (i = 0; ; i++) {
		reg = array[i].reg;
		val = array[i].val;

		if (reg == 0xff && val == 0xffffffff)
			break;

		switch(reg) {
		case 0xfe:
			msleep(50);
			break;
		case 0xfd:
			mdelay(5);
			break;
		case 0xfc:
			mdelay(1);
			break;
		case 0xfb:
			udelay(50);
			break;
		case 0xfa:
			udelay(5);
			break;
		case 0xf9:
			udelay(1);
			break;
		}

		reg &= 0x3f;

		ret = rtl8723au_write_rfreg(priv, reg, val);
		if (ret) {
			printk(KERN_WARNING "%s: Failed to initialize RF\n",
			       DRIVER_NAME);
			return -EAGAIN;
		}
		udelay(1);
	}

	return 0;
}

static int rtlmac_init_phy_rf(struct rtlmac_priv *priv)
{
	u32 val32;
	u16 val16, rfsi_rfenv;

	/* For path B, use XB */
	rfsi_rfenv = rtl8723au_read16(priv, REG_FPGA0_XA_RF_SW_CTRL);
	rfsi_rfenv &= FPGA0_RF_RFENV;

	/*
	 * These two we might be able to optimize into one
	 */
	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_RF_INT_OE);
	val32 |= BIT(20);	/* 0x10 << 16 */
	rtl8723au_write32(priv, REG_FPGA0_XA_RF_INT_OE, val32);
	udelay(1);

	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_RF_INT_OE);
	val32 |= BIT(4);
	rtl8723au_write32(priv, REG_FPGA0_XA_RF_INT_OE, val32);
	udelay(1);

	/*
	 * These two we might be able to optimize into one
	 */
	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_HSSI_PARM2);
	val32 &= ~FPGA0_HSSI_3WIRE_ADDR_LEN;
	rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM2, val32);
	udelay(1);

	val32 = rtl8723au_read32(priv, REG_FPGA0_XA_HSSI_PARM2);
	val32 &= ~FPGA0_HSSI_3WIRE_DATA_LEN;
	rtl8723au_write32(priv, REG_FPGA0_XA_HSSI_PARM2, val32);
	udelay(1);

	rtlmac_init_rf_regs(priv, rtl8723au_radioa_rf6052_1t_init_table);

	/* For path B, use XB */
	val16 = rtl8723au_read16(priv, REG_FPGA0_XA_RF_SW_CTRL);
	val16 &= ~FPGA0_RF_RFENV;
	val16 |= rfsi_rfenv;
	rtl8723au_write16(priv, REG_FPGA0_XA_RF_SW_CTRL, val16);

	priv->rf_mode_ag[0] = rtl8723au_read_rfreg(priv, RF6052_REG_MODE_AG);

	return 0;
}

static int rtlmac_llt_write(struct rtlmac_priv *priv, u8 address, u8 data)
{
	int ret = -EBUSY;
	int count = 0;
	u32 value;

	value = LLT_OP_WRITE | address << 8 | data;

	rtl8723au_write32(priv, REG_LLT_INIT, value);

	do {
		value = rtl8723au_read32(priv, REG_LLT_INIT);
		if ((value & LLT_OP_MASK) == LLT_OP_INACTIVE) {
			ret = 0;
			break;
		}
	} while (count++ < 20);

	return ret;
}

static int rtlmac_init_llt_table(struct rtlmac_priv *priv, u8 last_tx_page)
{
	int ret;
	int i;

	for (i = 0; i < last_tx_page; i++) {
		ret = rtlmac_llt_write(priv, i, i + 1);
		if (ret)
			goto exit;
	}

	ret = rtlmac_llt_write(priv, last_tx_page, 0xff);
	if (ret)
		goto exit;

	/* Mark remaining pages as a ring buffer */
	for (i = last_tx_page + 1; i < 0xff; i++) {
		ret = rtlmac_llt_write(priv, i, (i + 1));
		if (ret)
			goto exit;
	}

	/*  Let last entry point to the start entry of ring buffer */
	ret = rtlmac_llt_write(priv, 0xff, last_tx_page + 1);
	if (ret)
		goto exit;

exit:
	return ret;
}

static int rtlmac_init_queue_priotiy(struct rtlmac_priv *priv)
{
	u16 val16, hi, lo;
	u16 hiq, mgq, bkq, beq, viq, voq;
	int ret = 0;

	switch(priv->ep_tx_count) {
	case 1:
		if (priv->ep_tx_high_queue) {
			hi = TRXDMA_QUEUE_HIGH;
		} else if (priv->ep_tx_low_queue) {
			hi = TRXDMA_QUEUE_LOW;
		} else if (priv->ep_tx_normal_queue) {
			hi = TRXDMA_QUEUE_NORMAL;
		} else {
			hi = 0;
			ret = -EINVAL;
		}

		hiq = hi;
		mgq = hi;
		bkq = hi;
		beq = hi;
		viq = hi;
		voq = hi;

		break;
	case 2:
		if (priv->ep_tx_high_queue && priv->ep_tx_low_queue) {
			hi = TRXDMA_QUEUE_HIGH;
			lo = TRXDMA_QUEUE_LOW;
		} else if (priv->ep_tx_normal_queue && priv->ep_tx_low_queue) {
			hi = TRXDMA_QUEUE_NORMAL;
			lo = TRXDMA_QUEUE_LOW;
		} else if (priv->ep_tx_high_queue && priv->ep_tx_normal_queue) {
			hi = TRXDMA_QUEUE_HIGH;
			lo = TRXDMA_QUEUE_NORMAL;
		} else {
			ret = -EINVAL;
			hi = 0;
			lo = 0;
		}
		hiq = hi;
		mgq = lo;
		bkq = hi;
		beq = lo;
		viq = hi;
		voq = lo;

		break;
	case 3:
		beq = TRXDMA_QUEUE_LOW;
		bkq = TRXDMA_QUEUE_NORMAL;
		viq = TRXDMA_QUEUE_NORMAL;
		voq = TRXDMA_QUEUE_HIGH;
		mgq = TRXDMA_QUEUE_HIGH;
		hiq = TRXDMA_QUEUE_HIGH;
		break;
	default:
		ret = -EINVAL;
	}

	if (!ret) {
		val16 = (voq << TRXDMA_CTRL_VOQ_SHIFT) |
			(viq << TRXDMA_CTRL_VIQ_SHIFT) |
			(beq << TRXDMA_CTRL_BEQ_SHIFT) |
			(bkq << TRXDMA_CTRL_BKQ_SHIFT) |
			(mgq << TRXDMA_CTRL_MGQ_SHIFT) |
			(hiq << TRXDMA_CTRL_HIQ_SHIFT);
		rtl8723au_write16(priv, REG_TRXDMA_CTRL, val16);
	}

	return ret;
}

static int rtlmac_set_mac(struct rtlmac_priv *priv)
{
	int i;
	u16 reg;

	reg = REG_MACID;

	for (i = 0; i < ETH_ALEN; i++)
		rtl8723au_write8(priv, reg + i, priv->mac_addr[i]);

	return 0;
}

static int rtlmac_low_power_flow(struct rtlmac_priv *priv)
{
	u8 val8;
	u32 val32;
	int count, ret = -EBUSY;

	/* Active to Low Power sequence */
	rtl8723au_write8(priv, REG_TXPAUSE, 0xff);

	for (count = 0 ; count < RTLMAC_MAX_REG_POLL; count ++) {
		val32 = rtl8723au_read32(priv, 0x05f8);
		if (val32 == 0x00) {
			ret = 0;
			break;
		}
		udelay(10);
	}

	/* CCK and OFDM are disabled, and clock are gated */
	val8 = rtl8723au_read8(priv, REG_SYS_FUNC);
	val8 &= ~BIT(0);
	rtl8723au_write8(priv, REG_SYS_FUNC, val8);

	udelay(2);

	/*Whole BB is reset*/
	val8 = rtl8723au_read8(priv, REG_SYS_FUNC);
	val8 &= ~BIT(1);
	rtl8723au_write8(priv, REG_SYS_FUNC, val8);

	/* Reset MAC T/RX */
	rtl8723au_write8(priv, REG_CR,
			 CR_HCI_TXDMA_ENABLE | CR_HCI_RXDMA_ENABLE);

	/* Disable security - BIT(9) */
	val8 = rtl8723au_read8(priv, REG_CR + 1);
	val8 &= ~BIT(1);
	rtl8723au_write8(priv, REG_CR + 1, val8);

	/* Respond TxOK to scheduler */
	val8 = rtl8723au_read8(priv, REG_DUAL_TSF_RST);
	val8 |= BIT(5);
	rtl8723au_write8(priv, REG_DUAL_TSF_RST, val8);

	return ret;
}

static int rtlmac_active_to_emu(struct rtlmac_priv *priv)
{
	u8 val8;
	int count, ret;

	/* Start of rtl8723AU_card_enable_flow */
	/* Act to Cardemu sequence*/
	/* Turn off RF */
	rtl8723au_write8(priv, REG_RF_CTRL, 0);

	/* 0x004E[7] = 0, switch DPDT_SEL_P output from register 0x0065[2] */
	val8 = rtl8723au_read8(priv, REG_LEDCFG2);
	val8 &= ~BIT(7);
	rtl8723au_write8(priv, REG_LEDCFG2, val8);

	/* 0x0005[1] = 1 turn off MAC by HW state machine*/
	val8 = rtl8723au_read8(priv, 0x05);
	val8 |= BIT(1);
	rtl8723au_write8(priv, 0x05, val8);

	for (count = 0 ; count < RTLMAC_MAX_REG_POLL; count ++) {
		val8 = rtl8723au_read8(priv, 0x05);
		if ((val8 & BIT(1)) == 0)
			break;
		udelay(10);
	}

	if (count == RTLMAC_MAX_REG_POLL) {
		printk(KERN_WARNING "%s: Turn off MAC timed out\n", __func__);
		ret = -EBUSY;
		goto exit;
	}

	/* 0x0000[5] = 1 analog Ips to digital, 1:isolation */
	val8 = rtl8723au_read8(priv, REG_SYS_ISO_CTRL);
	val8 |= BIT(5);
	rtl8723au_write8(priv, REG_SYS_ISO_CTRL, val8);

	/* 0x0020[0] = 0 disable LDOA12 MACRO block*/
	val8 = rtl8723au_read8(priv, REG_LDOA15_CTRL);
	val8 &= ~BIT(0);
	rtl8723au_write8(priv, REG_LDOA15_CTRL, val8);

exit:
	return ret;
}

static int rtlmac_disabled_to_emu(struct rtlmac_priv *priv)
{
	u8 val8;
	int ret = 0;

	/* Clear suspend enable and power down enable*/
	val8 = rtl8723au_read8(priv, 0x05);
	val8 &= ~(BIT(3) | BIT(7));
	rtl8723au_write8(priv, 0x05, val8);

	/* 0x48[16] = 0 to disable GPIO9 as EXT WAKEUP*/
	val8 = rtl8723au_read8(priv, 0x4a);
	val8 &= ~BIT(0);
	rtl8723au_write8(priv, 0x4a, val8);

	/* 0x04[12:11] = 11 enable WL suspend*/
	val8 = rtl8723au_read8(priv, 0x05);
	val8 &= ~(BIT(3) | BIT(4));
	rtl8723au_write8(priv, 0x05, val8);

	return ret;
}

static int rtlmac_emu_to_active(struct rtlmac_priv *priv)
{
	u8 val8;
	u32 val32;
	int count, ret = 0;

	/* 0x20[0] = 1 enable LDOA12 MACRO block for all interface*/
	val8 = rtl8723au_read8(priv, REG_LDOA15_CTRL);
	val8 |= BIT(0);
	rtl8723au_write8(priv, REG_LDOA15_CTRL, val8);

	/* 0x67[0] = 0 to disable BT_GPS_SEL pins*/
	val8 = rtl8723au_read8(priv, 0x0067);
	val8 &= ~BIT(4);
	rtl8723au_write8(priv, 0x0067, val8);

	mdelay(1);

	/* 0x00[5] = 0 release analog Ips to digital, 1:isolation */
	val8 = rtl8723au_read8(priv, REG_SYS_ISO_CTRL);
	val8 &= ~BIT(5);
	rtl8723au_write8(priv, REG_SYS_ISO_CTRL, val8);

	/* disable SW LPS 0x04[10]= 0 */
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 &= ~BIT(2);
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	/* wait till 0x04[17] = 1 power ready*/
	for (count = 0; count < RTLMAC_MAX_REG_POLL; count ++) {
		val32 = rtl8723au_read32(priv, REG_APS_FSMCO);
		if (val32 & BIT(17)) {
			break;
		}
		udelay(10);
	}

	if (count == RTLMAC_MAX_REG_POLL) {
		ret = -EBUSY;
		goto exit;
	}

	/* We should be able to optimize the following three entries into one */

	/* release WLON reset 0x04[16]= 1*/
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 2);
	val8 |= BIT(0);
	rtl8723au_write8(priv, REG_APS_FSMCO + 2, val8);

	/* disable HWPDN 0x04[15]= 0*/
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 &= ~BIT(7);
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	/* disable WL suspend*/
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 &= ~(BIT(3) | BIT(4));
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	/* set, then poll until 0 */
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 |= BIT(0);
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	for (count = 0; count < RTLMAC_MAX_REG_POLL; count ++) {
		val32 = rtl8723au_read32(priv, REG_APS_FSMCO);
		if ((val32 & BIT(8)) == 0) {
			ret = 0;
			break;
		}
		udelay(10);
	}

	if (count == RTLMAC_MAX_REG_POLL) {
		ret = -EBUSY;
		goto exit;
	}

	/* 0x4C[23] = 0x4E[7] = 1, switch DPDT_SEL_P output from WL BB */
	val8 = rtl8723au_read8(priv, REG_LEDCFG2);
	val8 |= BIT(7);
	rtl8723au_write8(priv, REG_LEDCFG2, val8);

exit:
	return ret;
}

static int rtlmac_emu_to_powerdown(struct rtlmac_priv *priv)
{
	u8 val8;

	/* 0x0007[7:0] = 0x20 SOP option to disable BG/MB/ACK/SWR*/
	rtl8723au_write8(priv, REG_APS_FSMCO + 3, 0x20);

	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 2);
	val8 &= ~BIT(0);
	rtl8723au_write8(priv, REG_APS_FSMCO + 2, val8);

	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 1);
	val8 |= BIT(7);
	rtl8723au_write8(priv, REG_APS_FSMCO + 1, val8);

	return 0;
}

static int rtlmac_power_on(struct rtlmac_priv *priv)
{
	u8 val8;
	u16 val16;
	u32 val32;
	int ret = 0;

	/*  RSV_CTRL 0x001C[7:0] = 0x00
	    unlock ISO/CLK/Power control register */
	rtl8723au_write8(priv, REG_RSV_CTRL, 0x0);

	ret = rtlmac_disabled_to_emu(priv);
	if (ret)
		goto exit;

	ret = rtlmac_emu_to_active(priv);
	if (ret)
		goto exit;

	/*  0x0004[19] = 1, reset 8051 */
	val8 = rtl8723au_read8(priv, REG_APS_FSMCO + 2);
	val8 |= BIT(3);
	rtl8723au_write8(priv, REG_APS_FSMCO + 2, val8);

	/*  Enable MAC DMA/WMAC/SCHEDULE/SEC block */
	/*  Set CR bit10 to enable 32k calibration. */
	val16 = rtl8723au_read16(priv, REG_CR);
	val16 |= (CR_HCI_TXDMA_ENABLE | CR_HCI_RXDMA_ENABLE |
		  CR_TXDMA_ENABLE | CR_RXDMA_ENABLE |
		  CR_PROTOCOL_ENABLE | CR_SCHEDULE_ENABLE |
		  CR_MAC_TX_ENABLE | CR_MAC_RX_ENABLE |
		  CR_SECURITY_ENABLE | CR_CALTIMER_ENABLE);
	rtl8723au_write16(priv, REG_CR, val16);

	/* for Efuse PG */
	val32 = rtl8723au_read32(priv, REG_EFUSE_CTRL);
	val32 &= ~(BIT(28)|BIT(29)|BIT(30));
	val32 |= (0x06 << 28);
	rtl8723au_write32(priv, REG_EFUSE_CTRL, val32);
exit:
	return ret;
}

static int rtlmac_power_off(struct rtlmac_priv *priv)
{
	int ret = 0;

	rtlmac_low_power_flow(priv);

	return ret;
}

static int rtlmac_init_device(struct ieee80211_hw *hw)
{
	struct rtlmac_priv *priv = hw->priv;
	bool macpower;
	int ret = 0;
	u8 val8;
	u16 val16;
	u32 val32;

	/* Check if MAC is already powered on */
	val8 = rtl8723au_read8(priv, REG_CR);

	/* Fix 92DU-VC S3 hang with the reason is that secondary mac is not
	   initialized. First MAC returns 0xea, second MAC returns 0x00 */
	if (val8 == 0xea)
		macpower = false;
	else
		macpower = true;

	ret = rtlmac_power_on(priv);
	if (ret < 0) {
		printk(KERN_WARNING "%s: Failed power on\n", __func__);
		goto exit;
	}

	printk(KERN_DEBUG "macpower %i\n", macpower);
	if (!macpower) {
		ret = rtlmac_init_llt_table(priv, TX_TOTAL_PAGE_NUM);
		if (ret) {
			printk(KERN_DEBUG "%s: LLT table init failed\n",
			       __func__);
			goto exit;
		}
	}

	ret = rtlmac_download_firmware(priv);
	if (ret)
		goto exit;
	ret = rtlmac_start_firmware(priv);
	if (ret)
		goto exit;

	ret = rtlmac_init_mac(priv, rtl8723a_mac_init_table);
	if (ret)
		goto exit;

	ret = rtlmac_init_phy_bb(priv);
	if (ret)
		goto exit;

	ret = rtlmac_init_phy_rf(priv);
	if (ret)
		goto exit;
#if 0
	/*
	 * The RTL driver does this, but it cannot be correct since
	 * RF_T_METER is an RF register, and should be written with
	 * rtl8723au_write_rfreg() not rtl8723au_write32()
	 */
	/* reducing 80M spur */
	rtl8723au_write32(priv, RF_T_METER, 0x0381808d);
	rtl8723au_write32(priv, RF_SYN_G4, 0xf2ffff83);
	rtl8723au_write32(priv, RF_SYN_G4, 0xf2ffff82);
	rtl8723au_write32(priv, RF_SYN_G4, 0xf2ffff83);
#endif

	/* RFSW Control - clear bit 14 ?? */
	rtl8723au_write32(priv, REG_FPGA0_TXINFO, 0x00000003);
	/* 0x07000760 */
	val32 = 0x07000000 | FPGA0_RF_TRSW | FPGA0_RF_TRSWB |
		FPGA0_RF_ANTSW | FPGA0_RF_ANTSWB | FPGA0_RF_PAPE;
	rtl8723au_write32(priv, REG_FPGA0_XAB_RF_SW_CTRL, val32);
	 /* 0x860[6:5]= 00 - why? - this sets antenna B */
	rtl8723au_write32(priv, REG_FPGA0_XA_RF_INT_OE, 0x66F60210);

	if (!macpower) {
		if (priv->ep_tx_normal_queue)
			val8 = TX_PAGE_NUM_NORM_PQ;
		else
			val8 = 0;

		rtl8723au_write8(priv, REG_RQPN_NPQ, val8);

		val32 = (TX_PAGE_NUM_PUBQ << RQPN_NORM_PQ_SHIFT) | RQPN_LOAD;

		if (priv->ep_tx_high_queue)
			val32 |= (TX_PAGE_NUM_HI_PQ << RQPN_HI_PQ_SHIFT);
		if (priv->ep_tx_low_queue)
			val32 |= (TX_PAGE_NUM_LO_PQ << RQPN_LO_PQ_SHIFT);

		rtl8723au_write32(priv, REG_RQPN, val32);

		/*
		 * Set TX buffer boundary
		 */
		val8 = TX_TOTAL_PAGE_NUM + 1;
		rtl8723au_write8(priv, REG_TXPKTBUF_BCNQ_BDNY, val8);
		rtl8723au_write8(priv, REG_TXPKTBUF_MGQ_BDNY, val8);
		rtl8723au_write8(priv, REG_TXPKTBUF_WMAC_LBK_BF_HD, val8);
		rtl8723au_write8(priv, REG_TRXFF_BNDY, val8);
		rtl8723au_write8(priv, REG_TDECTRL + 1, val8);
	}

	ret = rtlmac_init_queue_priotiy(priv);
	if (ret)
		goto exit;

	/*
	 * Set RX page boundary
	 */
	rtl8723au_write16(priv, REG_TRXFF_BNDY + 2, 0x27ff);
	/*
	 * Transfer page size is always 128
	 */
	val8 = (PBP_PAGE_SIZE_128 << PBP_PAGE_SIZE_RX_SHIFT) |
		(PBP_PAGE_SIZE_128 << PBP_PAGE_SIZE_TX_SHIFT);
	rtl8723au_write8(priv, REG_PBP, val8);

	/*
	 * Unit in 8 bytes, not obvious what it is used for
	 */
	rtl8723au_write8(priv, REG_RX_DRVINFO_SZ, 4);

	/*
	 * Enable all interrupts - not obvious USB needs to do this
	 */
	rtl8723au_write32(priv, REG_HISR, 0xffffffff);
	rtl8723au_write32(priv, REG_HIMR, 0xffffffff);

	rtlmac_set_mac(priv);
	rtlmac_set_linktype(priv, MSR_LINKTYPE_STATION);

	/*
	 * Configure initial WMAC settings
	 */
	val32 = RCR_ACCEPT_PM | RCR_ACCEPT_MCAST | RCR_ACCEPT_BCAST |
		RCR_ACCEPT_BSSID_MATCH | RCR_ACCEPT_BSSID_BEACON |
		RCR_ACCEPT_MGMT_FRAME | RCR_HTC_LOC_CTRL |
		RCR_APPEND_PHYSTAT | RCR_APPEND_ICV | RCR_APPEND_MIC;
	rtl8723au_write32(priv, REG_RCR, val32);

	/*
	 * Accept all multicast
	 */
	rtl8723au_write32(priv, REG_MAR, 0xffffffff);
	rtl8723au_write32(priv, REG_MAR + 4, 0xffffffff);

	/*
	 * Init adaptive controls
	 */
	val32 = rtl8723au_read32(priv, REG_RESPONSE_RATE_SET);
	val32 &= ~RESPONSE_RATE_BITMAP_ALL;
	val32 |= RESPONSE_RATE_RRSR_CCK_ONLY_1M;
	rtl8723au_write32(priv, REG_RESPONSE_RATE_SET, val32);

	/* CCK = 0x0a, OFDM = 0x10 */
	rtlmac_set_spec_sifs(priv, 0x0a, 0x10);
	rtlmac_set_retry(priv, 0x30, 0x30);

	/*
	 * Init EDCA
	 */
	rtl8723au_write16(priv, REG_MAC_SPEC_SIFS, 0x100a);

	/* Set CCK SIFS */
	rtl8723au_write16(priv, REG_SIFS_CTX, 0x100a);

	/* Set OFDM SIFS */
	rtl8723au_write16(priv, REG_SIFS_TRX, 0x100a);

	/* TXOP */
	rtl8723au_write32(priv, REG_EDCA_BE_PARAM, 0x005ea42b);
	rtl8723au_write32(priv, REG_EDCA_BK_PARAM, 0x0000a44f);
	rtl8723au_write32(priv, REG_EDCA_VI_PARAM, 0x005ea324);
	rtl8723au_write32(priv, REG_EDCA_VO_PARAM, 0x002fa226);

	/* Set data auto rate fallback retry count */
	rtl8723au_write32(priv, REG_DARFRC, 0x00000000);
	rtl8723au_write32(priv, REG_DARFRC + 4, 0x10080404);
	rtl8723au_write32(priv, REG_RARFRC, 0x04030201);
	rtl8723au_write32(priv, REG_RARFRC + 4, 0x08070605);

	/*
	 * Initialize beacon parameters
	 */
	val16 = BEACON_TSF_UPDATE | (BEACON_TSF_UPDATE << 8);
	rtl8723au_write16(priv, REG_BEACON_CTRL, val16);
	rtl8723au_write16(priv, REG_TBTT_PROHIBIT, 0x6404);
	rtl8723au_write8(priv, REG_DRIVER_EARLY_INT, DRIVER_EARLY_INT_TIME);
	rtl8723au_write8(priv, REG_BEACON_DMA_TIME, BEACON_DMA_ATIME_INT_TIME);
	rtl8723au_write16(priv, REG_BEACON_TCFG, 0x660F);

	/*
	 * Enable CCK and OFDM block
	 */
	val32 = rtl8723au_read32(priv, REG_FPGA0_RF_MODE);
	val32 |= (FPGA0_RF_MODE_CCK | FPGA0_RF_MODE_OFDM);
	rtl8723au_write32(priv, REG_FPGA0_RF_MODE, val32);

	/*
	 * Invalidate all CAM entries - bit 30 is undocumented
	 */
	rtl8723au_write32(priv, REG_CAMCMD, CAM_CMD_POLLINIG | BIT(30));

	/*
	 * Start out with default power levels for channel 6, 20MHz
	 */
	rtl8723a_set_tx_power(priv, 6, true);
#if 0
	rtl8723a_InitAntenna_Selection(Adapter);

	/*  HW SEQ CTRL */
	/* set 0x0 to 0xFF by tynli. Default enable HW SEQ NUM. */
	rtl8723au_write8(priv, REG_HWSEQ_CTRL, 0xff);

	/*  */
	/*  Disable BAR, suggested by Scott */
	/*  2010.04.09 add by hpfan */
	/*  */
	rtl8723au_write32(priv, REG_BAR_MODE_CTRL, 0x0201ffff);

	if (pregistrypriv->wifi_spec)
		rtl8723au_write16(priv, REG_FAST_EDCA_CTRL, 0);

	/*  Move by Neo for USB SS from above setp */
	_RfPowerSave(Adapter);

	/*  2010/08/26 MH Merge from 8192CE. */
	/* sherry masked that it has been done in _RfPowerSave */
	/* 20110927 */
	/* recovery for 8192cu and 9723Au 20111017 */
	if (pwrctrlpriv->rf_pwrstate == rf_on) {
		if (pHalData->bIQKInitialized) {
			rtl8723a_phy_iq_calibrate(priv, true);
		} else {
			rtl8723a_phy_iq_calibrate(priv, false);
			pHalData->bIQKInitialized = true;
		}

		rtl8723a_odm_check_tx_power_tracking(Adapter);

		rtl8723a_phy_lc_calibrate(Adapter);

		rtl8723a_dual_antenna_detection(Adapter);
	}

	/* fix USB interface interference issue */
	rtl8723au_write8(priv, 0xfe40, 0xe0);
	rtl8723au_write8(priv, 0xfe41, 0x8d);
	rtl8723au_write8(priv, 0xfe42, 0x80);
	rtl8723au_write32(priv, REG_TXDMA_OFFSET_CHK, 0xfd0320);
	/* Solve too many protocol error on USB bus */
	if (!IS_81xxC_VENDOR_UMC_A_CUT(pHalData->VersionID)) {
		/*  0xe6 = 0x94 */
		rtl8723au_write8(priv, 0xfe40, 0xe6);
		rtl8723au_write8(priv, 0xfe41, 0x94);
		rtl8723au_write8(priv, 0xfe42, 0x80);

		/*  0xe0 = 0x19 */
		rtl8723au_write8(priv, 0xfe40, 0xe0);
		rtl8723au_write8(priv, 0xfe41, 0x19);
		rtl8723au_write8(priv, 0xfe42, 0x80);

		/*  0xe5 = 0x91 */
		rtl8723au_write8(priv, 0xfe40, 0xe5);
		rtl8723au_write8(priv, 0xfe41, 0x91);
		rtl8723au_write8(priv, 0xfe42, 0x80);

		/*  0xe2 = 0x81 */
		rtl8723au_write8(priv, 0xfe40, 0xe2);
		rtl8723au_write8(priv, 0xfe41, 0x81);
		rtl8723au_write8(priv, 0xfe42, 0x80);
	}

/*	_InitPABias(Adapter); */

	/*  Init BT hw config. */
	rtl8723a_BT_init_hwconfig(Adapter);

	rtl8723a_InitHalDm(Adapter);

	rtl8723a_set_nav_upper(priv, WiFiNavUpperUs);

	/*  2011/03/09 MH debug only, UMC-B cut pass 2500 S5 test, but we need to fin root cause. */
	if (((rtl8723au_read32(priv, rFPGA0_RFMOD) & 0xff000000) !=
	     0x83000000)) {
		PHY_SetBBReg(priv, rFPGA0_RFMOD, BIT(24), 1);
		RT_TRACE(_module_hci_hal_init_c_, _drv_err_, ("%s: IQK fail recorver\n", __func__));
	}

	/* ack for xmit mgmt frames. */
	rtl8723au_write32(priv, REG_FWHW_TXQ_CTRL,
			  rtl8723au_read32(priv, REG_FWHW_TXQ_CTRL)|BIT(12));
#endif

exit:
	return ret;
}

static int rtlmac_disable_device(struct ieee80211_hw *hw)
{
	struct rtlmac_priv *priv = hw->priv;

	rtlmac_power_off(priv);
	return 0;
}

static void rtlmac_tx(struct ieee80211_hw *hw,
		      struct ieee80211_tx_control *control, struct sk_buff *skb)
{
	printk(KERN_DEBUG "%s\n", __func__);
}

static int rtlmac_add_interface(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif)
{
	int ret;

	ret = -EOPNOTSUPP;

	printk(KERN_DEBUG "%s\n", __func__);
	return ret;
}

static void rtlmac_remove_interface(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif)
{
	printk(KERN_DEBUG "%s\n", __func__);
}

static int rtlmac_config(struct ieee80211_hw *hw, u32 changed)
{
	printk(KERN_DEBUG "%s\n", __func__);
	return 0;
}

static void rtlmac_configure_filter(struct ieee80211_hw *hw,
				    unsigned int changed_flags,
				    unsigned int *total_flags, u64 multicast)
{
	printk(KERN_DEBUG "%s\n", __func__);
}

static int rtlmac_start(struct ieee80211_hw *hw)
{
	int ret;

	ret = 0;

	printk(KERN_DEBUG "%s\n", __func__);
	return ret;
}

static void rtlmac_stop(struct ieee80211_hw *hw)
{
	printk(KERN_DEBUG "%s\n", __func__);
}

#if 0
static int rtlmac_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			  struct cfg80211_scan_request *req)
{
	printk(KERN_DEBUG "%s\n", __func__);

	return 0;
}
#endif

static const struct ieee80211_ops rtlmac_ops = {
	.tx = rtlmac_tx,
	.add_interface = rtlmac_add_interface,
	.remove_interface = rtlmac_remove_interface,
	.config = rtlmac_config,
#if 0
	.bss_info_changed = rtlmac_bss_info_changed,
#endif
	.configure_filter = rtlmac_configure_filter,
	.start = rtlmac_start,
	.stop = rtlmac_stop,
#if 0
	.hw_scan = rtlmac_hw_scan,
	.set_key = rtlmac_set_key,
#endif
};

static int rtlmac_probe(struct usb_interface *interface,
			const struct usb_device_id *id)
{
	struct rtlmac_priv *priv;
	struct ieee80211_hw *hw;
	struct usb_device *udev;
	int ret = 0;

	udev = usb_get_dev(interface_to_usbdev(interface));

	hw = ieee80211_alloc_hw(sizeof(struct rtlmac_priv), &rtlmac_ops);
	if (!hw) {
		ret = -ENOMEM;
		goto exit;
	}

	priv = hw->priv;
	priv->hw = hw;
	priv->udev = udev;
	mutex_init(&priv->usb_buf_mutex);

	usb_set_intfdata(interface, hw);

	rtlmac_8723au_identify_chip(priv);
	rtlmac_read_efuse(priv);
	ether_addr_copy(priv->mac_addr, priv->efuse_wifi.efuse.mac_addr);

	printk(KERN_INFO "%s: RTL8723au %02x:%02x:%02x:%02x:%02x:%02x\n",
	       DRIVER_NAME,
	       priv->efuse_wifi.efuse.mac_addr[0],
	       priv->efuse_wifi.efuse.mac_addr[1],
	       priv->efuse_wifi.efuse.mac_addr[2],
	       priv->efuse_wifi.efuse.mac_addr[3],
	       priv->efuse_wifi.efuse.mac_addr[4],
	       priv->efuse_wifi.efuse.mac_addr[5]);

	rtlmac_load_firmware(priv);

	ret = rtlmac_init_device(hw);


exit:
	if (ret < 0)
		usb_put_dev(udev);
	return ret;
}

static void rtlmac_disconnect(struct usb_interface *interface)
{
	struct rtlmac_priv *priv;
	struct ieee80211_hw *hw;

	hw = usb_get_intfdata(interface);
	priv = hw->priv;

	rtlmac_disable_device(hw);
	usb_set_intfdata(interface, NULL);

	kfree(priv->fw_data);
	mutex_destroy(&priv->usb_buf_mutex);

	ieee80211_free_hw(hw);

	wiphy_info(hw->wiphy, "disconnecting\n");
}

static struct usb_driver rtlmac_driver = {
	.name = DRIVER_NAME,
	.probe = rtlmac_probe,
	.disconnect = rtlmac_disconnect,
	.id_table = dev_table,
	.disable_hub_initiated_lpm = 1,
};

static int __init rtlmac_module_init(void)
{
	int res = 0;

	res = usb_register(&rtlmac_driver);
	if (res < 0)
		printk(KERN_ERR DRIVER_NAME ": usb_register() failed (%i)\n",
		       res);

	return res;
}

static void __exit rtlmac_module_exit(void)
{
	usb_deregister(&rtlmac_driver);
}

module_init(rtlmac_module_init);
module_exit(rtlmac_module_exit);
