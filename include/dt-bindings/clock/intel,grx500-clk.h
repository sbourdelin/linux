/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (C) 2016 Intel Corporation.
 *  Zhu YiXin <Yixin.zhu@intel.com>
 *
 */

#ifndef __INTEL_GRX500_CLK_H
#define __INTEL_GRX500_CLK_H

/* clocks under pll0a-clk */
#define CBM_CLK			0
#define NGI_CLK			1
#define SSX4_CLK		2
#define CPU0_CLK		3

/* clocks under pll0b-clk */
#define PAE_CLK			0
#define GSWIP_CLK		1
#define DDR_CLK			2
#define CPU1_CLK		3

/* clocks under lcpll-clk */
#define GRX500_PCIE_CLK		0

/* clocks under gate0-clk */
#define GATE_XBAR0_CLK		0
#define GATE_XBAR1_CLK		1
#define GATE_XBAR2_CLK		2
#define GATE_XBAR3_CLK		3
#define GATE_XBAR6_CLK		4
#define GATE_XBAR7_CLK		5

/* clocks under gate1-clk */
#define GATE_V_CODEC_CLK	0
#define GATE_DMA0_CLK		1
#define GATE_USB0_CLK		2
#define GATE_SPI1_CLK		3
#define GATE_SPI0_CLK		4
#define GATE_CBM_CLK		5
#define GATE_EBU_CLK		6
#define GATE_SSO_CLK		7
#define GATE_GPTC0_CLK		8
#define GATE_GPTC1_CLK		9
#define GATE_GPTC2_CLK		10
#define GATE_URT_CLK		11
#define GATE_EIP97_CLK		12
#define GATE_EIP123_CLK		13
#define GATE_TOE_CLK		14
#define GATE_MPE_CLK		15
#define GATE_TDM_CLK		16
#define GATE_PAE_CLK		17
#define GATE_USB1_CLK		18
#define GATE_GSWIP_CLK		19

/* clocks under gate2-clk */
#define GATE_PCIE0_CLK		0
#define GATE_PCIE1_CLK		1
#define GATE_PCIE2_CLK		2

#endif /* __INTEL_GRX500_CLK_H */
