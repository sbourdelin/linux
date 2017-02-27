/*
 * Copyright 2017 Priit Laes
 *
 * Priit Laes <plaes@plaes.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _CCU_SUN7I_A20_H_
#define _CCU_SUN7I_A20_H_

#include <dt-bindings/clock/sun7i-ccu.h>
#include <dt-bindings/reset/sun7i-ccu.h>

/* The HOSC is exported */
#define CLK_PLL_CORE		2
#define CLK_PLL_AUDIO_BASE	3
#define CLK_PLL_AUDIO		4
#define CLK_PLL_AUDIO_2X	5
#define CLK_PLL_AUDIO_4X	6
#define CLK_PLL_AUDIO_8X	7
#define CLK_PLL_VIDEO0		8
#define CLK_PLL_VIDEO0_2X	9
#define CLK_PLL_VE		10
#define CLK_PLL_DDR_BASE	11
#define CLK_PLL_DDR		12
#define CLK_PLL_DDR_OTHER	13
#define CLK_PLL_PERIPH		14
#define CLK_PLL_PERIPH_2X	15
#define CLK_PLL_VIDEO1		17
#define CLK_PLL_VIDEO1_2X	18

/* The CPU clock is exported */
#define CLK_AXI			20
#define CLK_AHB			21
#define CLK_APB0		22
#define CLK_APB1		23

/* Some AHB gates are exported */
#define CLK_AHB_BIST		31
#define CLK_AHB_MS		36
#define CLK_AHB_SDRAM		38
#define CLK_AHB_ACE		39
#define CLK_AHB_TS		41
#define CLK_AHB_VE		48
#define CLK_AHB_TVD		49
#define CLK_AHB_TVE1		51
#define CLK_AHB_LCD1		53
#define CLK_AHB_CSI0		54
#define CLK_AHB_CSI1		55
#define CLK_AHB_HDMI0		56
#define CLK_AHB_DE_BE1		59
#define CLK_AHB_DE_FE0		60
#define CLK_AHB_DE_FE1		61
#define CLK_AHB_MP		63
#define CLK_AHB_GPU		64

/* Some APB0 gates are exported */
#define CLK_APB0_AC97		67
#define CLK_APB0_KEYPAD		74

/* Some APB1 gates are exported */
#define CLK_APB1_CAN		79
#define CLK_APB1_SCR		80

/* Some IP module clocks are exported */
#define CLK_MS			93
#define CLK_TS			106
#define CLK_PATA		111
#define CLK_AC97		115
#define CLK_KEYPAD		117
#define CLK_SATA		118

/* Some DRAM gates are exported */
#define CLK_DRAM_VE		125
#define CLK_DRAM_CSI0		126
#define CLK_DRAM_CSI1		127
#define CLK_DRAM_TS		128
#define CLK_DRAM_TVD		129
#define CLK_DRAM_TVE1		131
#define CLK_DRAM_OUT		132
#define CLK_DRAM_DE_FE1		133
#define CLK_DRAM_DE_FE0		134
#define CLK_DRAM_DE_BE1		136
#define CLK_DRAM_MP		137
#define CLK_DRAM_ACE		138

#define CLK_DE_BE1		140
#define CLK_DE_FE0		141
#define CLK_DE_FE1		142
#define CLK_DE_MP		143
#define CLK_TCON1_CH0		145
#define CLK_CSI_SPECIAL		146
#define CLK_TVD			147
#define CLK_TCON0_CH1_SCLK2	148
#define CLK_TCON1_CH1_SCLK2	150
#define CLK_TCON1_CH1		151
#define CLK_CSI0		152
#define CLK_CSI1		153
#define CLK_VE			154
#define CLK_AVS			156
#define CLK_ACE			157
#define CLK_HDMI		158
#define CLK_GPU			159
#define CLK_MBUS		160
#define CLK_HDMI1_SLOW		161
#define CLK_HDMI1_REPEAT	162
#define CLK_OUT_A		163
#define CLK_OUT_B		164

#define CLK_NUMBER		(CLK_OUT_B + 1)

#endif /* _CCU_SUN7I_A20_H_ */
