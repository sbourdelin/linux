// SPDX-License-Identifier: GPL-2.0+
/*
 * Actions S700 clock driver
 *
 * Copyright (c) 2014 Actions Semi Inc.
 * Author: David Liu <liuwei@actions-semi.com>
 *
 * Author: Pathiban Nallathambi <pn@denx.de>
 * Author: Saravanan Sekar <sravanhome@gmail.com>
 */

#ifndef __DT_BINDINGS_CLOCK_S700_H
#define __DT_BINDINGS_CLOCK_S700_H

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
#define CLK_TVOUT_PLL			8
#define CLK_CVBS_PLL			9
#define CLK_AUDIO_PLL			10
#define CLK_ETHERNET_PLL		11


/* system clock */
#define CLK_SYS_BASE			12
#define CLK_CPU				CLK_SYS_BASE
#define CLK_DEV				(CLK_SYS_BASE+1)
#define CLK_AHB				(CLK_SYS_BASE+2)
#define CLK_APB				(CLK_SYS_BASE+3)
#define CLK_DMAC			(CLK_SYS_BASE+4)
#define CLK_NOC0_CLK_MUX		(CLK_SYS_BASE+5)
#define CLK_NOC1_CLK_MUX		(CLK_SYS_BASE+6)
#define CLK_HP_CLK_MUX			(CLK_SYS_BASE+7)
#define CLK_HP_CLK_DIV			(CLK_SYS_BASE+8)
#define CLK_NOC1_CLK_DIV		(CLK_SYS_BASE+9)
#define CLK_NOC0			(CLK_SYS_BASE+10)
#define CLK_NOC1			(CLK_SYS_BASE+11)
#define CLK_SENOR_SRC			(CLK_SYS_BASE+12)

/* peripheral device clock */
#define CLK_PERIP_BASE			25
#define CLK_GPIO			(CLK_PERIP_BASE)
#define CLK_TIMER			(CLK_PERIP_BASE+1)
#define CLK_DSI				(CLK_PERIP_BASE+2)
#define CLK_CSI				(CLK_PERIP_BASE+3)
#define CLK_SI				(CLK_PERIP_BASE+4)
#define CLK_DE				(CLK_PERIP_BASE+5)
#define CLK_HDE				(CLK_PERIP_BASE+6)
#define CLK_VDE				(CLK_PERIP_BASE+7)
#define CLK_VCE				(CLK_PERIP_BASE+8)
#define CLK_NAND			(CLK_PERIP_BASE+9)
#define CLK_SD0				(CLK_PERIP_BASE+10)
#define CLK_SD1				(CLK_PERIP_BASE+11)
#define CLK_SD2				(CLK_PERIP_BASE+12)

#define CLK_UART0			(CLK_PERIP_BASE+13)
#define CLK_UART1			(CLK_PERIP_BASE+14)
#define CLK_UART2			(CLK_PERIP_BASE+15)
#define CLK_UART3			(CLK_PERIP_BASE+16)
#define CLK_UART4			(CLK_PERIP_BASE+17)
#define CLK_UART5			(CLK_PERIP_BASE+18)
#define CLK_UART6			(CLK_PERIP_BASE+19)

#define CLK_PWM0			(CLK_PERIP_BASE+20)
#define CLK_PWM1			(CLK_PERIP_BASE+21)
#define CLK_PWM2			(CLK_PERIP_BASE+22)
#define CLK_PWM3			(CLK_PERIP_BASE+23)
#define CLK_PWM4			(CLK_PERIP_BASE+24)
#define CLK_PWM5			(CLK_PERIP_BASE+25)
#define CLK_GPU3D			(CLK_PERIP_BASE+26)

#define CLK_I2C0			(CLK_PERIP_BASE+27)
#define CLK_I2C1			(CLK_PERIP_BASE+28)
#define CLK_I2C2			(CLK_PERIP_BASE+29)
#define CLK_I2C3			(CLK_PERIP_BASE+30)


#define CLK_SPI0			(CLK_PERIP_BASE+31)
#define CLK_SPI1			(CLK_PERIP_BASE+32)
#define CLK_SPI2			(CLK_PERIP_BASE+33)
#define CLK_SPI3			(CLK_PERIP_BASE+34)

#define CLK_USB3_480MPLL0		(CLK_PERIP_BASE+35)
#define CLK_USB3_480MPHY0		(CLK_PERIP_BASE+36)
#define CLK_USB3_5GPHY			(CLK_PERIP_BASE+37)
#define CLK_USB3_CCE			(CLK_PERIP_BASE+48)
#define CLK_USB3_MAC			(CLK_PERIP_BASE+49)


#define CLK_LCD				(CLK_PERIP_BASE+50)
#define CLK_HDMI_AUDIO			(CLK_PERIP_BASE+51)
#define CLK_I2SRX			(CLK_PERIP_BASE+52)
#define CLK_I2STX			(CLK_PERIP_BASE+53)

#define CLK_SENSOR0			(CLK_PERIP_BASE+54)
#define CLK_SENSOR1			(CLK_PERIP_BASE+55)

#define CLK_HDMI_DEV			(CLK_PERIP_BASE+56)

#define CLK_ETHERNET			(CLK_PERIP_BASE+59)
#define CLK_RMII_REF			(CLK_PERIP_BASE+60)

#define CLK_USB2H0_PLLEN		(CLK_PERIP_BASE+61)
#define CLK_USB2H0_PHY			(CLK_PERIP_BASE+62)
#define CLK_USB2H0_CCE			(CLK_PERIP_BASE+63)
#define CLK_USB2H1_PLLEN		(CLK_PERIP_BASE+64)
#define CLK_USB2H1_PHY			(CLK_PERIP_BASE+65)
#define CLK_USB2H1_CCE			(CLK_PERIP_BASE+66)


#define CLK_TVOUT			(CLK_PERIP_BASE+67)

#define CLK_THERMAL_SENSOR		(CLK_PERIP_BASE+68)

#define CLK_IRC_SWITCH			(CLK_PERIP_BASE+69)
#define CLK_PCM1			(CLK_PERIP_BASE+70)
#define CLK_NR_CLKS			(CLK_PCM1) /* update on adding new clk */

#endif /* __DT_BINDINGS_CLOCK_S700_H */
