/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Synopsys MIPI D-PHY driver
 *
 * Copyright (c) 2018 Synopsys, Inc. and/or its affiliates.
 * Author: Luis Oliveira <Luis.Oliveira@synopsys.com>
 */

#ifndef __PHY_SNPS_DPHY_RX_H__
#define __PHY_SNPS_DPHY_RX_H__

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

/* DPHY interface register bank*/
#define R_CSI2_DPHY_SHUTDOWNZ 0x0
#define R_CSI2_DPHY_RSTZ 0x4
#define R_CSI2_DPHY_RX 0x8
#define	R_CSI2_DPHY_STOPSTATE 0xC
#define R_CSI2_DPHY_TST_CTRL0 0x10
#define R_CSI2_DPHY_TST_CTRL1 0x14
#define R_CSI2_DPHY2_TST_CTRL0 0x18
#define R_CSI2_DPHY2_TST_CTRL1 0x1C

enum dphy_id_mask {
	DPHY_ID_LANE_SUPPORT = 0,
	DPHY_ID_IF = 4,
	DPHY_ID_GEN = 8,
};

enum dphy_gen_values {
	GEN1 = 0,
	GEN2 = 1,
	GEN3 = 2,
};

enum dphy_interface_length {
	BIT8 = 8,
	BIT12 = 12,
};

enum tst_ctrl0 {
	PHY_TESTCLR = 0,
	PHY_TESTCLK = 1,
};

enum tst_ctrl1 {
	PHY_TESTDIN = 0,
	PHY_TESTDOUT = 8,
	PHY_TESTEN = 16,
};

enum lanes_config_values {
	CTRL_4_LANES = 0,
	CTRL_8_LANES = 1,
};

enum dphy_tc {
	CFGCLKFREQRANGE_TX = 0x02,
	CFGCLKFREQRANGE_RX = 0x05,
	BYPASS = 0x20,
	IO_DS = 0x30,
};

enum dphy_8bit_interface_addr {
	BANDGAP_CTRL = 0x24,
	HS_RX_CTRL_LANE0 = 0x42,
	HSFREQRANGE_8BIT = 0x44,
	OSC_FREQ_TARGET_RX0_LSB	= 0x4e,
	OSC_FREQ_TARGET_RX0_MSB	= 0x4f,
	HS_RX_CTRL_LANE1 = 0x52,
	OSC_FREQ_TARGET_RX1_LSB	= 0x5e,
	OSC_FREQ_TARGET_RX1_MSB	= 0x5f,
	RX_SKEW_CAL	= 0x7e,
	HS_RX_CTRL_LANE2 = 0x82,
	OSC_FREQ_TARGET_RX2_LSB	= 0x8e,
	OSC_FREQ_TARGET_RX2_MSB	= 0x8f,
	HS_RX_CTRL_LANE3 = 0x92,
	OSC_FREQ_TARGET_RX3_LSB	= 0x9e,
	OSC_FREQ_TARGET_RX3_MSB	= 0x9f,
};

enum dphy_12bit_interface_addr {
	RX_SYS_0 = 0x01,
	RX_SYS_1 = 0x02,
	RX_SYS_7 = 0x08,
	RX_RX_STARTUP_OVR_0 = 0xe0,
	RX_RX_STARTUP_OVR_1 = 0xe1,
	RX_RX_STARTUP_OVR_2 = 0xe2,
	RX_RX_STARTUP_OVR_3 = 0xe3,
	RX_RX_STARTUP_OVR_4 = 0xe4,
};

/* Gen3 interface register bank*/
#define IDLYCFG	0x00
#define IDLYSEL	0x04
#define IDLYCNTINVAL 0x08
#define IDLYCNTOUTVAL 0x0c
#define DPHY1REGRSTN 0x10
#define DPHYZCALSTAT 0x14
#define DPHYZCALCTRL 0x18
#define DPHYLANE0STAT 0x1c
#define DPHYLANE1STAT 0x20
#define DPHYLANE2STAT 0x24
#define DPHYLANE3STAT 0x28
#define DPHYCLKSTAT 0x2c
#define DPHYZCLKCTRL 0x30
#define TCGENPURPOSOUT 0x34
#define TCGENPURPOSIN 0x38
#define DPHYGENERICOUT 0x3c
#define DPHYGENERICIN 0x40
#define DPHYGLUEIFTESTER 0x44
#define DPHYID 0x100

enum glueiftester {
	GLUELOGIC = 0x4,
	RX_PHY = 0x2,
	TX_PHY = 0x1,
	RESET = 0x0,
};

struct dw_dphy_rx {
	spinlock_t slock;
	struct phy *phy;
	uint32_t dphy_freq;
	uint32_t dphy_gen;
	uint32_t dphy_te_len;
	uint32_t lanes_config;
	uint32_t max_lanes;
	uint32_t compat_mode;
	uint32_t lp_time;

	void __iomem *base_address; /* test interface */
	void __iomem *dphy1_if_addr; /* gluelogic phy 1 */
	void __iomem *dphy2_if_addr; /* gluelogic phy 2 */

	int config_gpio;
	uint8_t setup_config;
};

int dw_dphy_init(struct phy *phy);
int dw_dphy_reset(struct phy *phy);
int dw_dphy_power_off(struct phy *phy);
int dw_dphy_power_on(struct phy *phy);

u8 dw_dphy_setup_config(struct dw_dphy_rx *dphy);
u32 dw_dphy_if_read(struct dw_dphy_rx *dphy, u64 address);
void dw_dphy_write(struct dw_dphy_rx *dphy, u32 address, u32 data);
u32 dw_dphy_read(struct dw_dphy_rx *dphy, u64 address);
int dw_dphy_te_read(struct dw_dphy_rx *dphy, u32 addr);
int dw_dphy_if_get_idelay(struct dw_dphy_rx *dphy);
int dw_dphy_if_set_idelay_lane(struct dw_dphy_rx *dphy, u8 dly, u8 lane);

static inline
u32 dw_dphy_if_read_msk(struct dw_dphy_rx *dphy,
		u32 address, u8 shift, u8 width)
{
	return (dw_dphy_if_read(dphy, address) >> shift) & ((1 << width) - 1);
}

static inline
u32 dw_dphy_read_msk(struct dw_dphy_rx *dev, u32 address, u8 shift,  u8 width)
{
	return (dw_dphy_read(dev, address) >> shift) & ((1 << width) - 1);
}
#endif /*__PHY_SNPS_DPHY_RX_H__*/
