/*
 * Copyright (c) 2016 HiSilicon Technologies Co., Ltd.
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __DTS_HISTB_CLOCK_H
#define __DTS_HISTB_CLOCK_H

/* clocks provided by core CRG */
#define OSC_CLK			0
#define APB_CLK			1
#define AHB_CLK			2
#define UART1_CLK		3
#define UART2_CLK		4
#define UART3_CLK		5
#define I2C0_CLK		6
#define I2C1_CLK		7
#define I2C2_CLK		8
#define I2C3_CLK		9
#define I2C4_CLK		10
#define I2C5_CLK		11
#define SPI0_CLK		12
#define SPI1_CLK		13
#define SPI2_CLK		14
#define SCI_CLK			15
#define FMC_CLK			16
#define MMC_BIU_CLK		17
#define MMC_CIU_CLK		18
#define MMC_DRV_CLK		19
#define MMC_SAMPLE_CLK		20
#define SDIO0_BIU_CLK		21
#define SDIO0_CIU_CLK		22
#define SDIO0_DRV_CLK		23
#define SDIO0_SAMPLE_CLK	24
#define PCIE_AUX_CLK		25
#define PCIE_PIPE_CLK		26
#define PCIE_SYS_CLK		27
#define PCIE_BUS_CLK		28
#define ETH0_MAC_CLK		29
#define ETH0_MACIF_CLK		30
#define ETH1_MAC_CLK		31
#define ETH1_MACIF_CLK		32

/* clocks provided by mcu CRG */
#define MCE_CLK	1
#define IR_CLK	2
#define TIMER01_CLK	3
#define LEDC_CLK	4
#define UART0_CLK	5
#define LSADC_CLK	6

#endif	/* __DTS_HISTB_CLOCK_H */
