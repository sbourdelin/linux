/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Defines macros and constants for Renesas RZ/N1 pin controller pin
 * muxing functions.
 */
#ifndef __DT_BINDINGS_RZN1_PINCTRL_H
#define __DT_BINDINGS_RZN1_PINCTRL_H

#define RZN1_MUX_FUNC_BIT			8
#define RZN1_MUX_HAS_FUNC_BIT			15
#define RZN1_MUX_HAS_DRIVE_BIT			16
#define RZN1_MUX_DRIVE_BIT			17
#define RZN1_MUX_HAS_PULL_BIT			19
#define RZN1_MUX_PULL_BIT			20

#define RZN1_MUX_PULL_UP			1
#define RZN1_MUX_PULL_DOWN			3
#define RZN1_MUX_PULL_NONE			0

#define RZN1_MUX_DRIVE_4MA			0
#define RZN1_MUX_DRIVE_6MA			1
#define RZN1_MUX_DRIVE_8MA			2
#define RZN1_MUX_DRIVE_12MA			3

#define RZN1_MUX(_gpio, _func) \
	(((RZN1_FUNC_##_func) << RZN1_MUX_FUNC_BIT) | \
		(1 << RZN1_MUX_HAS_FUNC_BIT) | \
		(_gpio))

#define RZN1_MUX_PULL(_pull) \
		((1 << RZN1_MUX_HAS_PULL_BIT) | \
		((_pull) << RZN1_MUX_PULL_BIT))

#define RZN1_MUX_DRIVE(_drive) \
		((1 << RZN1_MUX_HAS_DRIVE_BIT) | \
		((_drive) << RZN1_MUX_DRIVE_BIT))

#define RZN1_MUX_PUP(_gpio, _func) \
	(RZN1_MUX(_gpio, _func) | RZN1_MUX_PULL(RZN1_MUX_PULL_UP))
#define RZN1_MUX_PDOWN(_gpio, _func) \
	(RZN1_MUX(_gpio, _func) | RZN1_MUX_PULL(RZN1_MUX_PULL_DOWN))
#define RZN1_MUX_PNONE(_gpio, _func) \
	(RZN1_MUX(_gpio, _func) | RZN1_MUX_PULL(RZN1_MUX_PULL_NONE))

#define RZN1_MUX_4MA(_gpio, _func) \
	(RZN1_MUX(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_4MA))
#define RZN1_MUX_6MA(_gpio, _func) \
	(RZN1_MUX(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_6MA))
#define RZN1_MUX_8MA(_gpio, _func) \
	(RZN1_MUX(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_8MA))
#define RZN1_MUX_12MA(_gpio, _func) \
	(RZN1_MUX(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_12MA))

#define RZN1_MUX_PUP_4MA(_gpio, _func) \
	(RZN1_MUX_PUP(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_4MA))
#define RZN1_MUX_PUP_6MA(_gpio, _func) \
	(RZN1_MUX_PUP(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_6MA))
#define RZN1_MUX_PUP_8MA(_gpio, _func) \
	(RZN1_MUX_PUP(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_8MA))
#define RZN1_MUX_PUP_12MA(_gpio, _func) \
	(RZN1_MUX_PUP(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_12MA))

#define RZN1_MUX_PDOWN_4MA(_gpio, _func) \
	(RZN1_MUX_PDOWN(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_4MA))
#define RZN1_MUX_PDOWN_6MA(_gpio, _func) \
	(RZN1_MUX_PDOWN(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_6MA))
#define RZN1_MUX_PDOWN_8MA(_gpio, _func) \
	(RZN1_MUX_PDOWN(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_8MA))
#define RZN1_MUX_PDOWN_12MA(_gpio, _func) \
	(RZN1_MUX_PDOWN(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_12MA))

#define RZN1_MUX_PNONE_4MA(_gpio, _func) \
	(RZN1_MUX_PNONE(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_4MA))
#define RZN1_MUX_PNONE_6MA(_gpio, _func) \
	(RZN1_MUX_PNONE(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_6MA))
#define RZN1_MUX_PNONE_8MA(_gpio, _func) \
	(RZN1_MUX_PNONE(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_8MA))
#define RZN1_MUX_PNONE_12MA(_gpio, _func) \
	(RZN1_MUX_PNONE(_gpio, _func) | RZN1_MUX_DRIVE(RZN1_MUX_DRIVE_12MA))

/*
 * Use these "gpio" numbers with the RZN1_FUNC_MDIO_MUX* functions
 * to set the destination of the two MDIO busses.
 */
#define RZN1_MDIO_BUS0				170
#define RZN1_MDIO_BUS1				171

/*
 * This can be used to set pullups/down/driver for pins without changing
 * any function that might have already been set
 */
#define RZN1_FUNC_NONE				0xff

/*
 * Given the different levels of muxing on the SoC, it was decided to
 * 'linearize' them into one numerical space. So mux level 1, 2 and the MDIO
 * muxes are all represented by one single value.
 *
 * You can derive the hardware value pretty easily too, as
 * 0...9   are Level 1
 * 10...71 are Level 2. The Level 2 mux will be set to this
 *         value - RZN1_FUNC_LEVEL2_OFFSET, and the Level 1 mux will be
 *         set accordingly.
 * 72...79 are for the 2 MUDIO muxes for "GPIO" 170/171. These muxes will
 *         be set to this value - 72.
 */
#define RZN1_FUNC_HIGHZ				0
#define RZN1_FUNC_0L				1
#define RZN1_FUNC_CLK_ETH_MII_RGMII_RMII	2
#define RZN1_FUNC_CLK_ETH_NAND			3
#define RZN1_FUNC_QSPI				4
#define RZN1_FUNC_SDIO				5
#define RZN1_FUNC_LCD				6
#define RZN1_FUNC_LCD_E				7
#define RZN1_FUNC_MSEBIM			8
#define RZN1_FUNC_MSEBIS			9
#define RZN1_FUNC_LEVEL2_OFFSET			10	/* I'm Special */
#define RZN1_FUNC_HIGHZ1			10
#define RZN1_FUNC_ETHERCAT			11
#define RZN1_FUNC_SERCOS3			12
#define RZN1_FUNC_SDIO_E			13
#define RZN1_FUNC_ETH_MDIO			14
#define RZN1_FUNC_ETH_MDIO_E1			15
#define RZN1_FUNC_USB				16
#define RZN1_FUNC_MSEBIM_E			17
#define RZN1_FUNC_MSEBIS_E			18
#define RZN1_FUNC_RSV				19
#define RZN1_FUNC_RSV_E				20
#define RZN1_FUNC_RSV_E1			21
#define RZN1_FUNC_UART0_I			22
#define RZN1_FUNC_UART0_I_E			23
#define RZN1_FUNC_UART1_I			24
#define RZN1_FUNC_UART1_I_E			25
#define RZN1_FUNC_UART2_I			26
#define RZN1_FUNC_UART2_I_E			27
#define RZN1_FUNC_UART0				28
#define RZN1_FUNC_UART0_E			29
#define RZN1_FUNC_UART1				30
#define RZN1_FUNC_UART1_E			31
#define RZN1_FUNC_UART2				32
#define RZN1_FUNC_UART2_E			33
#define RZN1_FUNC_UART3				34
#define RZN1_FUNC_UART3_E			35
#define RZN1_FUNC_UART4				36
#define RZN1_FUNC_UART4_E			37
#define RZN1_FUNC_UART5				38
#define RZN1_FUNC_UART5_E			39
#define RZN1_FUNC_UART6				40
#define RZN1_FUNC_UART6_E			41
#define RZN1_FUNC_UART7				42
#define RZN1_FUNC_UART7_E			43
#define RZN1_FUNC_SPI0_M			44
#define RZN1_FUNC_SPI0_M_E			45
#define RZN1_FUNC_SPI1_M			46
#define RZN1_FUNC_SPI1_M_E			47
#define RZN1_FUNC_SPI2_M			48
#define RZN1_FUNC_SPI2_M_E			49
#define RZN1_FUNC_SPI3_M			50
#define RZN1_FUNC_SPI3_M_E			51
#define RZN1_FUNC_SPI4_S			52
#define RZN1_FUNC_SPI4_S_E			53
#define RZN1_FUNC_SPI5_S			54
#define RZN1_FUNC_SPI5_S_E			55
#define RZN1_FUNC_SGPIO0_M			56
#define RZN1_FUNC_SGPIO1_M			57
#define RZN1_FUNC_GPIO				58
#define RZN1_FUNC_CAN				59
#define RZN1_FUNC_I2C				60
#define RZN1_FUNC_SAFE				61
#define RZN1_FUNC_PTO_PWM			62
#define RZN1_FUNC_PTO_PWM1			63
#define RZN1_FUNC_PTO_PWM2			64
#define RZN1_FUNC_PTO_PWM3			65
#define RZN1_FUNC_PTO_PWM4			66
#define RZN1_FUNC_DELTA_SIGMA			67
#define RZN1_FUNC_SGPIO2_M			68
#define RZN1_FUNC_SGPIO3_M			69
#define RZN1_FUNC_SGPIO4_S			70
#define RZN1_FUNC_MAC_MTIP_SWITCH		71
/* These correspond to the functions used for the two MDIO muxes. */
#define RZN1_FUNC_MDIO_MUX_HIGHZ		72
#define RZN1_FUNC_MDIO_MUX_MAC0			73
#define RZN1_FUNC_MDIO_MUX_MAC1			74
#define RZN1_FUNC_MDIO_MUX_ECAT			75
#define RZN1_FUNC_MDIO_MUX_S3_MDIO0		76
#define RZN1_FUNC_MDIO_MUX_S3_MDIO1		77
#define RZN1_FUNC_MDIO_MUX_HWRTOS		78
#define RZN1_FUNC_MDIO_MUX_SWITCH		79
#define RZN1_FUNC_MAX				80

#endif /* __DT_BINDINGS_RZN1_PINCTRL_H */
