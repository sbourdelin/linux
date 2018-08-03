/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (C) 2018 Intel Corporation.
 *  Zhu YiXin <Yixin.zhu@intel.com>
 *
 */

#ifndef __INTEL_GRX500_CLK_H
#define __INTEL_GRX500_CLK_H

/* OSC clock */
#define CLK_OSC			1

/* PLL clocks */
#define CLK_PLL0A		2
#define CLK_PLL0B		3
#define CLK_PLL3		4

/* clocks under pll0a */
#define CLK_CBM			11
#define CLK_NGI			12
#define CLK_SSX4		13
#define CLK_CPU0		14

/* clocks under pll0b */
#define CLK_PAE			21
#define CLK_GSWIP		22
#define CLK_DDR			23
#define CLK_CPU1		24

/* clocks under pll3 */
#define CLK_PCIE		33

/* clocks under gate1 */
#define GCLK_V_CODEC		106
#define GCLK_DMA0		107
#define GCLK_USB0		108
#define GCLK_SPI1		109
#define GCLK_SPI0		110
#define GCLK_CBM		111
#define GCLK_EBU		112
#define GCLK_SSO		113
#define GCLK_GPTC0		114
#define GCLK_GPTC1		115
#define GCLK_GPTC2		116
#define GCLK_UART		117
#define GCLK_EIP97		118
#define GCLK_EIP123		119
#define GCLK_TOE		120
#define GCLK_MPE		121
#define GCLK_TDM		122
#define GCLK_PAE		123
#define GCLK_USB1		124
#define GCLK_GSWIP		125

/* clocks under gate2 */
#define GCLK_PCIE0		126
#define GCLK_PCIE1		127
#define GCLK_PCIE2		128

/* other clocks */
#define CLK_CPU			150
#define CLK_DDRPHY		151
#define GCLK_I2C		152
#define CLK_VOICE		153

#define CLK_NR_CLKS		200

#endif /* __INTEL_GRX500_CLK_H */
