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

u8 rtl8723au_read8(struct rtlmac_priv *priv, u16 addr)
{
	struct usb_device *udev = priv->udev;
	int len;
	u8 data;

	len = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_READ,
			      addr, 0, &data, sizeof(data),
			      RTW_USB_CONTROL_MSG_TIMEOUT);

	printk(KERN_DEBUG "%s(%04x) = 0x%02x len %i\n",
	       __func__, addr, data, len);
	return data;
}

u16 rtl8723au_read16(struct rtlmac_priv *priv, u16 addr)
{
	struct usb_device *udev = priv->udev;
	int len;
	__le16 data;

	len = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_READ,
			      addr, 0, &data, sizeof(data),
			      RTW_USB_CONTROL_MSG_TIMEOUT);

	printk(KERN_DEBUG "%s(%04x) = 0x%04x len %i\n",
	       __func__, addr, le16_to_cpu(data), len);
	return le16_to_cpu(data);
}

u32 rtl8723au_read32(struct rtlmac_priv *priv, u16 addr)
{
	struct usb_device *udev = priv->udev;
	int len;
	__le32 data;

	len = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_READ,
			      addr, 0, &data, sizeof(data),
			      RTW_USB_CONTROL_MSG_TIMEOUT);

	printk(KERN_DEBUG "%s(%04x) = 0x%08x, len %i\n",
	       __func__, addr, le32_to_cpu(data), len);
	return le32_to_cpu(data);
}

int rtl8723au_write8(struct rtlmac_priv *priv, u16 addr, u8 val)
{
	struct usb_device *udev = priv->udev;
	int ret;
	u8 data = val;

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, &data, sizeof(data),
			      RTW_USB_CONTROL_MSG_TIMEOUT);

	printk(KERN_DEBUG "%s(%04x) = %02x, ret %i\n",
	       __func__, addr, data, ret);
	return ret;
}

int rtl8723au_write16(struct rtlmac_priv *priv, u16 addr, u16 val)
{
	struct usb_device *udev = priv->udev;
	int ret;
	__le16 data = cpu_to_le16(val);

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, &data, sizeof(data),
			      RTW_USB_CONTROL_MSG_TIMEOUT);

	printk(KERN_DEBUG "%s(%04x) = %04x, ret %i\n",
	       __func__, addr, data, ret);
	return ret;
}

int rtl8723au_write32(struct rtlmac_priv *priv, u16 addr, u32 val)
{
	struct usb_device *udev = priv->udev;
	int ret;
	__le32 data = cpu_to_le32(val);

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, &data, sizeof(data),
			      RTW_USB_CONTROL_MSG_TIMEOUT);

	printk(KERN_DEBUG "%s(%04x) = %04x, ret %i\n",
	       __func__, addr, data, ret);
	return ret;
}

int rtl8723au_writeN(struct rtlmac_priv *priv, u16 addr, u8 *buf, u16 len)
{
	struct usb_device *udev = priv->udev;
	int ret;

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      REALTEK_USB_CMD_REQ, REALTEK_USB_WRITE,
			      addr, 0, buf, len, RTW_USB_CONTROL_MSG_TIMEOUT);

	printk(KERN_DEBUG "%s(%04x) = %p, len %02x\n",
	       __func__, addr, buf, len);
	return ret;
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
	val8 = rtl8723au_read8(priv, REG_SYS_FUNC_EN);
	val8 &= ~BIT(0);
	rtl8723au_write8(priv, REG_SYS_FUNC_EN, val8);

	udelay(2);

	/*Whole BB is reset*/
	val8 = rtl8723au_read8(priv, REG_SYS_FUNC_EN);
	val8 &= ~BIT(1);
	rtl8723au_write8(priv, REG_SYS_FUNC_EN, val8);

	/*Reset MAC TRX*/
	rtl8723au_write8(priv, REG_CR, HCI_TXDMA_EN | HCI_RXDMA_EN);

	/* 'ENSEC' */
	val8 = rtl8723au_read8(priv, REG_CR);
	val8 &= ~BIT(1);
	rtl8723au_write8(priv, REG_CR, val8);

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
	val16 |= (HCI_TXDMA_EN | HCI_RXDMA_EN | TXDMA_EN | RXDMA_EN |
		  PROTOCOL_EN | SCHEDULE_EN | MACTXEN | MACRXEN |
		  ENSEC | CALTMR_EN);
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
	int macpower;
	int ret = 0;
	u8 val8;

	/* Check if MAC is already powered on */
	val8 = rtl8723au_read8(priv, REG_CR);

	/* Fix 92DU-VC S3 hang with the reason is that secondary mac is not
	   initialized. First MAC returns 0xea, second MAC returns 0x00 */
	if (val8 == 0xea)
		macpower = 0;
	else
		macpower = 1;

	ret = rtlmac_power_on(priv);
	if (ret < 0) {
		printk(KERN_WARNING "%s: Failed power on\n", __func__);
		goto exit;
	}

	printk(KERN_DEBUG "macpower %i\n", macpower);
	if (!macpower) {
		ret = rtlmac_init_llt_table(priv, LLT_LAST_TX_PAGE);
		if (ret) {
			printk(KERN_DEBUG "%s: LLT table init failed\n",
			       __func__);
			goto exit;
		}
	}

#if 0
	if (pHalData->bRDGEnable)
		_InitRDGSetting(Adapter);
#endif

#if 0
	ret = rtl8723a_FirmwareDownload(Adapter);
	if (ret != _SUCCESS) {
		Adapter->bFWReady = false;
		pHalData->fw_ractrl = false;
		DBG_8723A("fw download fail!\n");
		goto exit;
	} else {
		Adapter->bFWReady = true;
		pHalData->fw_ractrl = true;
		DBG_8723A("fw download ok!\n");
	}

	rtl8723a_InitializeFirmwareVars(Adapter);

	if (pwrctrlpriv->reg_rfoff == true) {
		pwrctrlpriv->rf_pwrstate = rf_off;
	}

	/*  2010/08/09 MH We need to check if we need to turnon or off RF after detecting */
	/*  HW GPIO pin. Before PHY_RFConfig8192C. */
	/* HalDetectPwrDownMode(Adapter); */
	/*  2010/08/26 MH If Efuse does not support sective suspend then disable the function. */
	/* HalDetectSelectiveSuspendMode(Adapter); */

	/*  Set RF type for BB/RF configuration */
	_InitRFType(Adapter);/* _ReadRFType() */

	/*  Save target channel */
	/*  <Roger_Notes> Current Channel will be updated again later. */
	pHalData->CurrentChannel = 6;/* default set to 6 */

	ret = PHY_MACConfig8723A(Adapter);
	if (ret == _FAIL) {
		DBG_8723A("PHY_MACConfig8723A fault !!\n");
		goto exit;
	}

	/*  */
	/* d. Initialize BB related configurations. */
	/*  */
	ret = PHY_BBConfig8723A(Adapter);
	if (ret == _FAIL) {
		DBG_8723A("PHY_BBConfig8723A fault !!\n");
		goto exit;
	}

	/*  Add for tx power by rate fine tune. We need to call the function after BB config. */
	/*  Because the tx power by rate table is inited in BB config. */

	ret = PHY_RFConfig8723A(Adapter);
	if (ret == _FAIL) {
		DBG_8723A("PHY_RFConfig8723A fault !!\n");
		goto exit;
	}

	/* reducing 80M spur */
	PHY_SetBBReg(Adapter, RF_T_METER, bMaskDWord, 0x0381808d);
	PHY_SetBBReg(Adapter, RF_SYN_G4, bMaskDWord, 0xf2ffff83);
	PHY_SetBBReg(Adapter, RF_SYN_G4, bMaskDWord, 0xf2ffff82);
	PHY_SetBBReg(Adapter, RF_SYN_G4, bMaskDWord, 0xf2ffff83);

	/* RFSW Control */
	PHY_SetBBReg(Adapter, rFPGA0_TxInfo, bMaskDWord, 0x00000003);	/* 0x804[14]= 0 */
	PHY_SetBBReg(Adapter, rFPGA0_XAB_RFInterfaceSW, bMaskDWord, 0x07000760);	/* 0x870[6:5]= b'11 */
	PHY_SetBBReg(Adapter, rFPGA0_XA_RFInterfaceOE, bMaskDWord, 0x66F60210); /* 0x860[6:5]= b'00 */

	RT_TRACE(_module_hci_hal_init_c_, _drv_info_, ("%s: 0x870 = value 0x%x\n", __func__, PHY_QueryBBReg(Adapter, 0x870, bMaskDWord)));

	/*  */
	/*  Joseph Note: Keep RfRegChnlVal for later use. */
	/*  */
	pHalData->RfRegChnlVal[0] = PHY_QueryRFReg(Adapter, (enum RF_RADIO_PATH)0, RF_CHNLBW, bRFRegOffsetMask);
	pHalData->RfRegChnlVal[1] = PHY_QueryRFReg(Adapter, (enum RF_RADIO_PATH)1, RF_CHNLBW, bRFRegOffsetMask);

	if (!pHalData->bMACFuncEnable) {
		_InitQueueReservedPage(Adapter);
		_InitTxBufferBoundary(Adapter);
	}
	_InitQueuePriority(Adapter);
	_InitPageBoundary(Adapter);
	_InitTransferPageSize(Adapter);

	/*  Get Rx PHY status in order to report RSSI and others. */
	_InitDriverInfoSize(Adapter, DRVINFO_SZ);

	_InitInterrupt(Adapter);
	hw_var_set_macaddr(Adapter, Adapter->eeprompriv.mac_addr);
	_InitNetworkType(Adapter);/* set msr */
	_InitWMACSetting(Adapter);
	_InitAdaptiveCtrl(Adapter);
	_InitEDCA(Adapter);
	_InitRateFallback(Adapter);
	_InitRetryFunction(Adapter);
	InitUsbAggregationSetting(Adapter);
	_InitOperationMode(Adapter);/* todo */
	rtl8723a_InitBeaconParameters(Adapter);

	_InitHWLed(Adapter);

	_BBTurnOnBlock(Adapter);
	/* NicIFSetMacAddress(padapter, padapter->PermanentAddress); */

	invalidate_cam_all23a(Adapter);

	/*  2010/12/17 MH We need to set TX power according to EFUSE content at first. */
	PHY_SetTxPowerLevel8723A(Adapter, pHalData->CurrentChannel);

	rtl8723a_InitAntenna_Selection(Adapter);

	/*  HW SEQ CTRL */
	/* set 0x0 to 0xFF by tynli. Default enable HW SEQ NUM. */
	rtl8723au_write8(Adapter, REG_HWSEQ_CTRL, 0xFF);

	/*  */
	/*  Disable BAR, suggested by Scott */
	/*  2010.04.09 add by hpfan */
	/*  */
	rtl8723au_write32(Adapter, REG_BAR_MODE_CTRL, 0x0201ffff);

	if (pregistrypriv->wifi_spec)
		rtl8723au_write16(Adapter, REG_FAST_EDCA_CTRL, 0);

	/*  Move by Neo for USB SS from above setp */
	_RfPowerSave(Adapter);

	/*  2010/08/26 MH Merge from 8192CE. */
	/* sherry masked that it has been done in _RfPowerSave */
	/* 20110927 */
	/* recovery for 8192cu and 9723Au 20111017 */
	if (pwrctrlpriv->rf_pwrstate == rf_on) {
		if (pHalData->bIQKInitialized) {
			rtl8723a_phy_iq_calibrate(Adapter, true);
		} else {
			rtl8723a_phy_iq_calibrate(Adapter, false);
			pHalData->bIQKInitialized = true;
		}

		rtl8723a_odm_check_tx_power_tracking(Adapter);

		rtl8723a_phy_lc_calibrate(Adapter);

		rtl8723a_dual_antenna_detection(Adapter);
	}

	/* fixed USB interface interference issue */
	rtl8723au_write8(Adapter, 0xfe40, 0xe0);
	rtl8723au_write8(Adapter, 0xfe41, 0x8d);
	rtl8723au_write8(Adapter, 0xfe42, 0x80);
	rtl8723au_write32(Adapter, 0x20c, 0xfd0320);
	/* Solve too many protocol error on USB bus */
	if (!IS_81xxC_VENDOR_UMC_A_CUT(pHalData->VersionID)) {
		/*  0xE6 = 0x94 */
		rtl8723au_write8(Adapter, 0xFE40, 0xE6);
		rtl8723au_write8(Adapter, 0xFE41, 0x94);
		rtl8723au_write8(Adapter, 0xFE42, 0x80);

		/*  0xE0 = 0x19 */
		rtl8723au_write8(Adapter, 0xFE40, 0xE0);
		rtl8723au_write8(Adapter, 0xFE41, 0x19);
		rtl8723au_write8(Adapter, 0xFE42, 0x80);

		/*  0xE5 = 0x91 */
		rtl8723au_write8(Adapter, 0xFE40, 0xE5);
		rtl8723au_write8(Adapter, 0xFE41, 0x91);
		rtl8723au_write8(Adapter, 0xFE42, 0x80);

		/*  0xE2 = 0x81 */
		rtl8723au_write8(Adapter, 0xFE40, 0xE2);
		rtl8723au_write8(Adapter, 0xFE41, 0x81);
		rtl8723au_write8(Adapter, 0xFE42, 0x80);

	}

/*	_InitPABias(Adapter); */

	/*  Init BT hw config. */
	rtl8723a_BT_init_hwconfig(Adapter);

	rtl8723a_InitHalDm(Adapter);

	rtl8723a_set_nav_upper(Adapter, WiFiNavUpperUs);

	/*  2011/03/09 MH debug only, UMC-B cut pass 2500 S5 test, but we need to fin root cause. */
	if (((rtl8723au_read32(Adapter, rFPGA0_RFMOD) & 0xFF000000) !=
	     0x83000000)) {
		PHY_SetBBReg(Adapter, rFPGA0_RFMOD, BIT(24), 1);
		RT_TRACE(_module_hci_hal_init_c_, _drv_err_, ("%s: IQK fail recorver\n", __func__));
	}

	/* ack for xmit mgmt frames. */
	rtl8723au_write32(Adapter, REG_FWHW_TXQ_CTRL,
			  rtl8723au_read32(Adapter, REG_FWHW_TXQ_CTRL)|BIT(12));
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

	usb_set_intfdata(interface, hw);

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
