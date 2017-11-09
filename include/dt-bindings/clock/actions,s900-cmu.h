/*
 * Device Tree binding constants for Actions S900 Clock Management Unit
 *
 * Copyright (c) 2014 Actions Semi Inc.
 * Copyright (c) 2017 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __DT_BINDINGS_CLOCK_S900_CMU_H
#define __DT_BINDINGS_CLOCK_S900_CMU_H

#define CLK_NONE			0

/* fixed rate clocks */
#define CLK_LOSC			1
#define CLK_HOSC			2

/* pll clocks */
#define CLK_CORE_PLL			3
#define CLK_DEV_PLL			4
#define CLK_DDR_PLL			5
#define CLK_NAND_PLL			6
#define CLK_DISPLAY_PLL			7
#define CLK_DSI_PLL			8
#define CLK_ASSIST_PLL			9
#define CLK_AUDIO_PLL			10

/* system clock */
#define CLK_CPU				15
#define CLK_DEV				16
#define CLK_NOC				17
#define CLK_NOC_CLK_MUX			18
#define CLK_NOC_CLK_DIV			19
#define CLK_AHB				20
#define CLK_APB				21
#define CLK_DMAC			22

/* peripheral device clock */
#define CLK_GPIO			23

#define CLK_BISP			24
#define CLK_CSI0			25
#define CLK_CSI1			26

#define CLK_DE				27
#define CLK_DE1				28
#define CLK_DE2				29
#define CLK_DE3				30
#define CLK_DSI				32

#define CLK_GPU				33
#define CLK_GPU_CORE			34
#define CLK_GPU_MEM			35
#define CLK_GPU_SYS			36

#define CLK_HDE				37
#define CLK_I2C0			38
#define CLK_I2C1			39
#define CLK_I2C2			40
#define CLK_I2C3			41
#define CLK_I2C4			42
#define CLK_I2C5			43
#define CLK_I2SRX			44
#define CLK_I2STX			45
#define CLK_IMX				46
#define CLK_LCD				47
#define CLK_NAND0			48
#define CLK_NAND1			49
#define CLK_PWM0			50
#define CLK_PWM1			51
#define CLK_PWM2			52
#define CLK_PWM3			53
#define CLK_PWM4			54
#define CLK_PWM5			55
#define CLK_SD0				56
#define CLK_SD1				57
#define CLK_SD2				58
#define CLK_SD3				59
#define CLK_SENSOR			60
#define CLK_SPEED_SENSOR		61
#define CLK_SPI0			62
#define CLK_SPI1			63
#define CLK_SPI2			64
#define CLK_SPI3			65
#define CLK_THERMAL_SENSOR		66
#define CLK_UART0			67
#define CLK_UART1			68
#define CLK_UART2			69
#define CLK_UART3			70
#define CLK_UART4			71
#define CLK_UART5			72
#define CLK_UART6			73
#define CLK_VCE				74
#define CLK_VDE				75

#define CLK_USB3_480MPLL0		76
#define CLK_USB3_480MPHY0		77
#define CLK_USB3_5GPHY			78
#define CLK_USB3_CCE			79
#define CLK_USB3_MAC			80

#define CLK_TIMER			83

#define CLK_HDMI_AUDIO			84

#define CLK_24M				85

#define CLK_EDP				86

#define CLK_24M_EDP			87
#define CLK_EDP_PLL			88
#define CLK_EDP_LINK			89

#define CLK_USB2H0_PLLEN		90
#define CLK_USB2H0_PHY			91
#define CLK_USB2H0_CCE			92
#define CLK_USB2H1_PLLEN		93
#define CLK_USB2H1_PHY			94
#define CLK_USB2H1_CCE			95

#define CLK_DDR0			96
#define CLK_DDR1			97
#define CLK_DMM				98

#define CLK_ETH_MAC			99
#define CLK_RMII_REF			100

#define CLK_NR_CLKS			110

#endif /* __DT_BINDINGS_CLOCK_S900_CMU_H */
