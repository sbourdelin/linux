/*
 * Synopsys Designware HDMI PHY E405 driver
 *
 * This Synopsys dw-phy-e405 software and associated documentation
 * (hereinafter the "Software") is an unsupported proprietary work of
 * Synopsys, Inc. unless otherwise expressly agreed to in writing between
 * Synopsys and you. The Software IS NOT an item of Licensed Software or a
 * Licensed Product under any End User Software License Agreement or
 * Agreement for Licensed Products with Synopsys or any supplement thereto.
 * Synopsys is a registered trademark of Synopsys, Inc. Other names included
 * in the SOFTWARE may be the trademarks of their respective owners.
 *
 * The contents of this file are dual-licensed; you may select either version 2
 * of the GNU General Public License (“GPL”) or the MIT license (“MIT”).
 *
 * Copyright (c) 2017 Synopsys, Inc. and/or its affiliates.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS"  WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING, BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT, OR OTHERWISE
 * ARISING FROM, OUT OF, OR IN CONNECTION WITH THE SOFTWARE THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef __DW_HDMI_PHY_E405_H__
#define __DW_HDMI_PHY_E405_H__

#define PHY_CMU_CONFIG				0x02
#define PHY_SYSTEM_CONFIG			0x03
#define PHY_MAINFSM_CTRL			0x05
#define PHY_MAINFSM_OVR2			0x08
#define PHY_MAINFSM_STATUS1			0x09
#define PHY_OVL_PROT_CTRL			0x0D
#define PHY_CDR_CTRL_CNT			0x0E
#define PHY_CLK_MPLL_STATUS			0x2F
#define PHY_CH0_EQ_CTRL1			0x32
#define PHY_CH0_EQ_CTRL2			0x33
#define PHY_CH0_EQ_STATUS			0x34
#define PHY_CH0_EQ_CTRL3			0x3E
#define PHY_CH0_EQ_CTRL4			0x3F
#define PHY_CH0_EQ_STATUS2			0x40
#define PHY_CH0_EQ_STATUS3			0x42
#define PHY_CH0_EQ_CTRL6			0x43
#define PHY_CH1_EQ_CTRL1			0x52
#define PHY_CH1_EQ_CTRL2			0x53
#define PHY_CH1_EQ_STATUS			0x54
#define PHY_CH1_EQ_CTRL3			0x5E
#define PHY_CH1_EQ_CTRL4			0x5F
#define PHY_CH1_EQ_STATUS2			0x60
#define PHY_CH1_EQ_STATUS3			0x62
#define PHY_CH1_EQ_CTRL6			0x63
#define PHY_CH2_EQ_CTRL1			0x72
#define PHY_CH2_EQ_CTRL2			0x73
#define PHY_CH2_EQ_STATUS			0x74
#define PHY_CH2_EQ_CTRL3			0x7E
#define PHY_CH2_EQ_CTRL4			0x7F
#define PHY_CH2_EQ_STATUS2			0x80
#define PHY_CH2_EQ_STATUS3			0x82
#define PHY_CH2_EQ_CTRL6			0x83

#endif /* __DW_HDMI_PHY_E405_H__ */
