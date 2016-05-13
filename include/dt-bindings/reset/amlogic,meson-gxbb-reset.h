/*
 * Copyright (c) 2016 BayLibre, Inc.
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 *
 * This file is dual-licensed: you can use it either under the terms
 * of the GPL or the X11 license, at your option. Note that this dual
 * licensing only applies to this file, and not this project as a
 * whole.
 *
 *  a) This library is free software; you can redistribute it and/or
 *     modify it under the terms of the GNU General Public License as
 *     published by the Free Software Foundation; either version 2 of the
 *     License, or (at your option) any later version.
 *
 *     This library is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 * Or, alternatively,
 *
 *  b) Permission is hereby granted, free of charge, to any person
 *     obtaining a copy of this software and associated documentation
 *     files (the "Software"), to deal in the Software without
 *     restriction, including without limitation the rights to use,
 *     copy, modify, merge, publish, distribute, sublicense, and/or
 *     sell copies of the Software, and to permit persons to whom the
 *     Software is furnished to do so, subject to the following
 *     conditions:
 *
 *     The above copyright notice and this permission notice shall be
 *     included in all copies or substantial portions of the Software.
 *
 *     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *     EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *     OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *     NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 *     HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *     WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *     FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 *     OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _DT_BINDINGS_AMLOGIC_MESON_GXBB_RESET_H
#define _DT_BINDINGS_AMLOGIC_MESON_GXBB_RESET_H

#define RESET_VCBUS		0
/*				1	*/
/*				2	*/
#define RESET_GIC		3
#define RESET_CAPB3_DECODE	4
#define RESET_NAND_CAPB3	5
#define RESET_HDMITX_CAPB3	6
#define RESET_MALI_CAPB3	7
#define RESET_DOS_CAPB3		8
#define RESET_SYS_CPU_CAPB3	9
#define RESET_CBUS_CAPB3	10
#define RESET_AHB_CNTL		11
#define RESET_AHB_DATA		12
#define RESET_VCBUS_CLK81	13
#define RESET_MMC		14
#define RESET_MIPI		15
#define RESET_PARSER		16
#define RESET_BLKMV		17
#define RESET_ISA		18
#define RESET_Ethernet		19
#define RESET_SD_EMMC_A		20
#define RESET_SD_EMMC_B		21
#define RESET_SD_EMMC_C		22
#define RESET_ROM_BOOT		23
#define RESET_SYS_CPU_3_0	24
#define RESET_SYS_CPU_CORE_3_0	25
#define RESET_SYS_PLL_DIV	26
#define RESET_SYS_CPU_AXI	27
#define RESET_SYS_CPU_L2	28
#define RESET_SYS_CPU_P		29
#define RESET_SYS_CPU_MBIST	30
/*				31	*/
#define RESET_VD_RMEM		32
#define RESET_AUDIN		33
#define RESET_HDMI_TX		34
/*				35	*/
/*				36	*/
/*				37	*/
#define RESET_GE2D		38
#define RESET_PARSER_REG	39
#define RESET_PARSER_FETCH	40
#define RESET_PARSER_CTL	41
#define RESET_PARSER_TOP	42
/*				43	*/
/*				44	*/
#define RESET_AO_CPU_RESET	45
#define RESET_MALI		46
#define RESET_HDMI_SYSTEM_RESET	47
#define RESET_RING_OSCILLATOR	48
#define RESET_SYS_CPU		49
#define RESET_EFUSE		50
#define RESET_SYS_CPU_BVCI	51
#define RESET_AIFIFO		52
#define RESET_TVFE		53
#define RESET_AHB_BRIDGE_CNTL	54
/*				55	*/
#define RESET_AUDIO_DAC		56
#define RESET_DEMUX_TOP		57
#define RESET_DEMUX_DES		58
#define RESET_DEMUX_S2P_0	59
#define RESET_DEMUX_S2P_1	60
#define RESET_DEMUX_RESET_0	61
#define RESET_DEMUX_RESET_1	62
#define RESET_DEMUX_RESET_2	63
#define RESET_DDR_PLL		64
#define RESET_MISC_PLL		65
/*				66	*/
/*				67	*/
#define RESET_DVIN_RESET	68
#define RESET_RDMA		69
#define RESET_VENCI		70
#define RESET_VENCP		71
/*				72	*/
#define RESET_VDAC		73
#define RESET_RTC		74
/*				75	*/
#define RESET_VDI6		76
#define RESET_VENCL		77
#define RESET_I2C_MASTER_2	78
#define RESET_I2C_MASTER_1	79
#define RESET_PERIPHS_GENERAL	80
#define RESET_PERIPHS_SPICC	81
#define RESET_PERIPHS_SMART_CARD	82
#define RESET_PERIPHS_SAR_ADC	83
#define RESET_PERIPHS_I2C_MASTER_0	84
#define RESET_SANA		85
/*				86	*/
#define RESET_PERIPHS_STREAM_INTERFACE	87
#define RESET_PERIPHS_SDIO	88
#define RESET_PERIPHS_UART_0	89
#define RESET_PERIPHS_UART_1_2	90
#define RESET_PERIPHS_ASYNC_0	91
#define RESET_PERIPHS_ASYNC_1	92
#define RESET_PERIPHS_SPI_0	93
#define RESET_PERIPHS_SDHC	94
#define RESET_UART_SLIP		95
#define RESET_USB_DDR_0		96
#define RESET_USB_DDR_1		97
#define RESET_USB_DDR_2		98
#define RESET_USB_DDR_3		99
/*				100	*/
#define RESET_DEVICE_MMC_ARB	101
/*				102	*/
#define RESET_VID_LOCK		103
#define RESET_A9_DMC_PIPEL	104
/*				105	*/
/*				106	*/
/*				107	*/
/*				108	*/
/*				109	*/
/*				110	*/
/*				111	*/

#endif
