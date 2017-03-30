/*
 * Driver for the Axis ARTPEC-6 pin controller
 *
 * Author: Chris Paterson <chris.paterson@linux.pieboy.co.uk>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/slab.h>
#include "core.h"
#include "pinconf.h"
#include "pinctrl-utils.h"
#include "pinctrl-artpec6.h"

struct artpec6_pmx {
	struct device	   *dev;
	struct pinctrl_dev *pctl;
	void __iomem	   *base;
	struct		   pinctrl_pin_desc *pins;
	unsigned int	   num_pins;
	struct		   artpec6_pin_group *pin_groups;
	unsigned int	   num_pin_groups;
	struct		   artpec6_pmx_func *functions;
	unsigned int	   num_functions;
};

struct artpec6_pin_group {
	const char	   *name;
	const unsigned int *pins;
	const unsigned int num_pins;
	const unsigned int *reg_offsets;
	const unsigned int num_regs;
	unsigned char	   config;
};

struct artpec6_pmx_func {
	const char	   *name;
	const char * const *groups;
	const unsigned int num_groups;
};

/* pins */
static struct pinctrl_pin_desc artpec6_pins[] = {
	PINCTRL_PIN(0, "GPIO0"),
	PINCTRL_PIN(1, "GPIO1"),
	PINCTRL_PIN(2, "GPIO2"),
	PINCTRL_PIN(3, "GPIO3"),
	PINCTRL_PIN(4, "GPIO4"),
	PINCTRL_PIN(5, "GPIO5"),
	PINCTRL_PIN(6, "GPIO6"),
	PINCTRL_PIN(7, "GPIO7"),
	PINCTRL_PIN(8, "GPIO8"),
	PINCTRL_PIN(9, "GPIO9"),
	PINCTRL_PIN(10, "GPIO10"),
	PINCTRL_PIN(11, "GPIO11"),
	PINCTRL_PIN(12, "GPIO12"),
	PINCTRL_PIN(13, "GPIO13"),
	PINCTRL_PIN(14, "GPIO14"),
	PINCTRL_PIN(15, "GPIO15"),
	PINCTRL_PIN(16, "GPIO16"),
	PINCTRL_PIN(17, "GPIO17"),
	PINCTRL_PIN(18, "GPIO18"),
	PINCTRL_PIN(19, "GPIO19"),
	PINCTRL_PIN(20, "GPIO20"),
	PINCTRL_PIN(21, "GPIO21"),
	PINCTRL_PIN(22, "GPIO22"),
	PINCTRL_PIN(23, "GPIO23"),
	PINCTRL_PIN(24, "GPIO24"),
	PINCTRL_PIN(25, "GPIO25"),
	PINCTRL_PIN(26, "GPIO26"),
	PINCTRL_PIN(27, "GPIO27"),
	PINCTRL_PIN(28, "GPIO28"),
	PINCTRL_PIN(29, "GPIO29"),
	PINCTRL_PIN(30, "GPIO30"),
	PINCTRL_PIN(31, "GPIO31"),
	PINCTRL_PIN(32, "UART3_TXD"),
	PINCTRL_PIN(33, "UART3_RXD"),
	PINCTRL_PIN(34, "UART3_RTS"),
	PINCTRL_PIN(35, "UART3_CTS"),
	PINCTRL_PIN(36, "NF_ALE"),
	PINCTRL_PIN(37, "NF_CE0_N"),
	PINCTRL_PIN(38, "NF_CE1_N"),
	PINCTRL_PIN(39, "NF_CLE"),
	PINCTRL_PIN(40, "NF_RE_N"),
	PINCTRL_PIN(41, "NF_WE_N"),
	PINCTRL_PIN(42, "NF_WP0_N"),
	PINCTRL_PIN(43, "NF_WP1_N"),
	PINCTRL_PIN(44, "NF_IO0"),
	PINCTRL_PIN(45, "NF_IO1"),
	PINCTRL_PIN(46, "NF_IO2"),
	PINCTRL_PIN(47, "NF_IO3"),
	PINCTRL_PIN(48, "NF_IO4"),
	PINCTRL_PIN(49, "NF_IO5"),
	PINCTRL_PIN(50, "NF_IO6"),
	PINCTRL_PIN(51, "NF_IO7"),
	PINCTRL_PIN(52, "NF_RB0_N"),
	PINCTRL_PIN(53, "SDIO0_CLK"),
	PINCTRL_PIN(54, "SDIO0_CMD"),
	PINCTRL_PIN(55, "SDIO0_DAT0"),
	PINCTRL_PIN(56, "SDIO0_DAT1"),
	PINCTRL_PIN(57, "SDIO0_DAT2"),
	PINCTRL_PIN(58, "SDIO0_DAT3"),
	PINCTRL_PIN(59, "SDI0_CD"),
	PINCTRL_PIN(60, "SDI0_WP"),
	PINCTRL_PIN(61, "SDIO1_CLK"),
	PINCTRL_PIN(62, "SDIO1_CMD"),
	PINCTRL_PIN(63, "SDIO1_DAT0"),
	PINCTRL_PIN(64, "SDIO1_DAT1"),
	PINCTRL_PIN(65, "SDIO1_DAT2"),
	PINCTRL_PIN(66, "SDIO1_DAT3"),
	PINCTRL_PIN(67, "SDIO1_CD"),
	PINCTRL_PIN(68, "SDIO1_WP"),
	PINCTRL_PIN(69, "GBE_REFCLk"),
	PINCTRL_PIN(70, "GBE_GTX_CLK"),
	PINCTRL_PIN(71, "GBE_TX_CLK"),
	PINCTRL_PIN(72, "GBE_TX_EN"),
	PINCTRL_PIN(73, "GBE_TX_ER"),
	PINCTRL_PIN(74, "GBE_TXD0"),
	PINCTRL_PIN(75, "GBE_TXD1"),
	PINCTRL_PIN(76, "GBE_TXD2"),
	PINCTRL_PIN(77, "GBE_TXD3"),
	PINCTRL_PIN(78, "GBE_TXD4"),
	PINCTRL_PIN(79, "GBE_TXD5"),
	PINCTRL_PIN(80, "GBE_TXD6"),
	PINCTRL_PIN(81, "GBE_TXD7"),
	PINCTRL_PIN(82, "GBE_RX_CLK"),
	PINCTRL_PIN(83, "GBE_RX_DV"),
	PINCTRL_PIN(84, "GBE_RX_ER"),
	PINCTRL_PIN(85, "GBE_RXD0"),
	PINCTRL_PIN(86, "GBE_RXD1"),
	PINCTRL_PIN(87, "GBE_RXD2"),
	PINCTRL_PIN(88, "GBE_RXD3"),
	PINCTRL_PIN(89, "GBE_RXD4"),
	PINCTRL_PIN(90, "GBE_RXD5"),
	PINCTRL_PIN(91, "GBE_RXD6"),
	PINCTRL_PIN(92, "GBE_RXD7"),
	PINCTRL_PIN(93, "GBE_CRS"),
	PINCTRL_PIN(94, "GBE_COL"),
	PINCTRL_PIN(95, "GBE_MDC"),
	PINCTRL_PIN(96, "GBE_MDIO"),
};

static const unsigned int pin_regs[] = {
	ARTPEC6_PINMUX_P0_0_CTRL,	/* Pin 0 */
	ARTPEC6_PINMUX_P0_1_CTRL,
	ARTPEC6_PINMUX_P0_2_CTRL,
	ARTPEC6_PINMUX_P0_3_CTRL,
	ARTPEC6_PINMUX_P0_4_CTRL,
	ARTPEC6_PINMUX_P0_5_CTRL,
	ARTPEC6_PINMUX_P0_6_CTRL,
	ARTPEC6_PINMUX_P0_7_CTRL,
	ARTPEC6_PINMUX_P0_8_CTRL,
	ARTPEC6_PINMUX_P0_9_CTRL,
	ARTPEC6_PINMUX_P0_10_CTRL,
	ARTPEC6_PINMUX_P0_11_CTRL,
	ARTPEC6_PINMUX_P0_12_CTRL,
	ARTPEC6_PINMUX_P0_13_CTRL,
	ARTPEC6_PINMUX_P0_14_CTRL,
	ARTPEC6_PINMUX_P0_15_CTRL,
	ARTPEC6_PINMUX_P1_0_CTRL,
	ARTPEC6_PINMUX_P1_1_CTRL,
	ARTPEC6_PINMUX_P1_2_CTRL,
	ARTPEC6_PINMUX_P1_3_CTRL,
	ARTPEC6_PINMUX_P1_4_CTRL,
	ARTPEC6_PINMUX_P1_5_CTRL,
	ARTPEC6_PINMUX_P1_6_CTRL,
	ARTPEC6_PINMUX_P1_7_CTRL,
	ARTPEC6_PINMUX_P1_8_CTRL,
	ARTPEC6_PINMUX_P1_9_CTRL,
	ARTPEC6_PINMUX_P1_10_CTRL,
	ARTPEC6_PINMUX_P1_11_CTRL,
	ARTPEC6_PINMUX_P1_12_CTRL,
	ARTPEC6_PINMUX_P1_13_CTRL,
	ARTPEC6_PINMUX_P1_14_CTRL,
	ARTPEC6_PINMUX_P1_15_CTRL,
	ARTPEC6_PINMUX_P2_0_CTRL,
	ARTPEC6_PINMUX_P2_1_CTRL,
	ARTPEC6_PINMUX_P2_2_CTRL,
	ARTPEC6_PINMUX_P2_3_CTRL,
	ARTPEC6_PINMUX_P4_0_CTRL,
	ARTPEC6_PINMUX_P4_1_CTRL,
	ARTPEC6_PINMUX_P4_2_CTRL,
	ARTPEC6_PINMUX_P4_3_CTRL,
	ARTPEC6_PINMUX_P4_4_CTRL,
	ARTPEC6_PINMUX_P4_5_CTRL,
	ARTPEC6_PINMUX_P4_6_CTRL,
	ARTPEC6_PINMUX_P4_7_CTRL,
	ARTPEC6_PINMUX_P4_8_CTRL,
	ARTPEC6_PINMUX_P4_9_CTRL,
	ARTPEC6_PINMUX_P4_10_CTRL,
	ARTPEC6_PINMUX_P4_11_CTRL,
	ARTPEC6_PINMUX_P4_12_CTRL,
	ARTPEC6_PINMUX_P4_13_CTRL,
	ARTPEC6_PINMUX_P4_14_CTRL,
	ARTPEC6_PINMUX_P4_15_CTRL,
	ARTPEC6_PINMUX_P5_0_CTRL,
	ARTPEC6_PINMUX_P6_0_CTRL,
	ARTPEC6_PINMUX_P6_1_CTRL,
	ARTPEC6_PINMUX_P6_2_CTRL,
	ARTPEC6_PINMUX_P6_3_CTRL,
	ARTPEC6_PINMUX_P6_4_CTRL,
	ARTPEC6_PINMUX_P6_5_CTRL,
	ARTPEC6_PINMUX_P6_6_CTRL,
	ARTPEC6_PINMUX_P6_7_CTRL,
	ARTPEC6_PINMUX_P6_8_CTRL,
	ARTPEC6_PINMUX_P6_9_CTRL,
	ARTPEC6_PINMUX_P6_10_CTRL,
	ARTPEC6_PINMUX_P6_11_CTRL,
	ARTPEC6_PINMUX_P6_12_CTRL,
	ARTPEC6_PINMUX_P6_13_CTRL,
	ARTPEC6_PINMUX_P6_14_CTRL,
	ARTPEC6_PINMUX_P6_15_CTRL,
	ARTPEC6_PINMUX_P7_0_CTRL,
	ARTPEC6_PINMUX_P7_1_CTRL,
	ARTPEC6_PINMUX_P7_2_CTRL,
	ARTPEC6_PINMUX_P7_3_CTRL,
	ARTPEC6_PINMUX_P7_4_CTRL,
	ARTPEC6_PINMUX_P7_5_CTRL,
	ARTPEC6_PINMUX_P7_6_CTRL,
	ARTPEC6_PINMUX_P7_7_CTRL,
	ARTPEC6_PINMUX_P7_8_CTRL,
	ARTPEC6_PINMUX_P7_9_CTRL,
	ARTPEC6_PINMUX_P7_10_CTRL,
	ARTPEC6_PINMUX_P7_11_CTRL,
	ARTPEC6_PINMUX_P7_12_CTRL,
	ARTPEC6_PINMUX_P7_13_CTRL,
	ARTPEC6_PINMUX_P7_14_CTRL,
	ARTPEC6_PINMUX_P7_15_CTRL,
	ARTPEC6_PINMUX_P8_0_CTRL,
	ARTPEC6_PINMUX_P8_1_CTRL,
	ARTPEC6_PINMUX_P8_2_CTRL,
	ARTPEC6_PINMUX_P8_3_CTRL,
	ARTPEC6_PINMUX_P8_4_CTRL,
	ARTPEC6_PINMUX_P8_5_CTRL,
	ARTPEC6_PINMUX_P8_6_CTRL,
	ARTPEC6_PINMUX_P8_7_CTRL,
	ARTPEC6_PINMUX_P8_8_CTRL,
	ARTPEC6_PINMUX_P8_9_CTRL,
	ARTPEC6_PINMUX_P8_10_CTRL,
	ARTPEC6_PINMUX_P8_11_CTRL,	/* Pin 96 */
};

static const unsigned int cpuclkout_regs0[] = {
	ARTPEC6_PINMUX_P0_0_CTRL,
};

static const unsigned int udlclkout_regs0[] = {
	ARTPEC6_PINMUX_P0_1_CTRL,
};

static const unsigned int i2c1_regs0[] = {
	ARTPEC6_PINMUX_P0_2_CTRL,
	ARTPEC6_PINMUX_P0_3_CTRL,
};

static const unsigned int i2c2_regs0[] = {
	ARTPEC6_PINMUX_P0_4_CTRL,
	ARTPEC6_PINMUX_P0_5_CTRL,
};

static const unsigned int i2c3_regs0[] = {
	ARTPEC6_PINMUX_P0_6_CTRL,
	ARTPEC6_PINMUX_P0_7_CTRL,
};

static const unsigned int i2s0_regs0[] = {
	ARTPEC6_PINMUX_P0_8_CTRL,
	ARTPEC6_PINMUX_P0_9_CTRL,
	ARTPEC6_PINMUX_P0_10_CTRL,
	ARTPEC6_PINMUX_P0_11_CTRL,
};

static const unsigned int i2s1_regs0[] = {
	ARTPEC6_PINMUX_P0_12_CTRL,
	ARTPEC6_PINMUX_P0_13_CTRL,
	ARTPEC6_PINMUX_P0_14_CTRL,
	ARTPEC6_PINMUX_P0_15_CTRL,
};

static const unsigned int i2srefclk_regs0[] = {
	ARTPEC6_PINMUX_P1_3_CTRL,
};

static const unsigned int spi0_regs0[] = {
	ARTPEC6_PINMUX_P0_12_CTRL,
	ARTPEC6_PINMUX_P0_13_CTRL,
	ARTPEC6_PINMUX_P0_14_CTRL,
	ARTPEC6_PINMUX_P0_15_CTRL,
};

static const unsigned int spi1_regs0[] = {
	ARTPEC6_PINMUX_P1_0_CTRL,
	ARTPEC6_PINMUX_P1_1_CTRL,
	ARTPEC6_PINMUX_P1_2_CTRL,
	ARTPEC6_PINMUX_P1_3_CTRL,
};

static const unsigned int pciedebug_regs0[] = {
	ARTPEC6_PINMUX_P0_12_CTRL,
	ARTPEC6_PINMUX_P0_13_CTRL,
	ARTPEC6_PINMUX_P0_14_CTRL,
	ARTPEC6_PINMUX_P0_15_CTRL,
};

static const unsigned int uart0_regs0[] = {
	ARTPEC6_PINMUX_P1_0_CTRL,
	ARTPEC6_PINMUX_P1_1_CTRL,
	ARTPEC6_PINMUX_P1_2_CTRL,
	ARTPEC6_PINMUX_P1_3_CTRL,
	ARTPEC6_PINMUX_P1_4_CTRL,
	ARTPEC6_PINMUX_P1_5_CTRL,
	ARTPEC6_PINMUX_P1_6_CTRL,
	ARTPEC6_PINMUX_P1_7_CTRL,
	ARTPEC6_PINMUX_P1_8_CTRL,
	ARTPEC6_PINMUX_P1_9_CTRL,
};

static const unsigned int uart0_regs1[] = {
	ARTPEC6_PINMUX_P1_4_CTRL,
	ARTPEC6_PINMUX_P1_5_CTRL,
	ARTPEC6_PINMUX_P1_6_CTRL,
	ARTPEC6_PINMUX_P1_7_CTRL,
};

static const unsigned int uart1_regs0[] = {
	ARTPEC6_PINMUX_P1_8_CTRL,
	ARTPEC6_PINMUX_P1_9_CTRL,
	ARTPEC6_PINMUX_P1_10_CTRL,
	ARTPEC6_PINMUX_P1_11_CTRL,
};

static const unsigned int uart2_regs0[] = {
	ARTPEC6_PINMUX_P1_10_CTRL,
	ARTPEC6_PINMUX_P1_11_CTRL,
	ARTPEC6_PINMUX_P1_12_CTRL,
	ARTPEC6_PINMUX_P1_13_CTRL,
	ARTPEC6_PINMUX_P1_14_CTRL,
	ARTPEC6_PINMUX_P1_15_CTRL,
	ARTPEC6_PINMUX_P2_0_CTRL,
	ARTPEC6_PINMUX_P2_1_CTRL,
	ARTPEC6_PINMUX_P2_2_CTRL,
	ARTPEC6_PINMUX_P2_3_CTRL,
};

static const unsigned int uart2_regs1[] = {
	ARTPEC6_PINMUX_P1_12_CTRL,
	ARTPEC6_PINMUX_P1_13_CTRL,
	ARTPEC6_PINMUX_P1_14_CTRL,
	ARTPEC6_PINMUX_P1_15_CTRL,
};

static const unsigned int uart3_regs0[] = {
	ARTPEC6_PINMUX_P2_0_CTRL,
	ARTPEC6_PINMUX_P2_1_CTRL,
	ARTPEC6_PINMUX_P2_2_CTRL,
	ARTPEC6_PINMUX_P2_3_CTRL,
};

static const unsigned int uart4_regs0[] = {
	ARTPEC6_PINMUX_P1_4_CTRL,
	ARTPEC6_PINMUX_P1_5_CTRL,
	ARTPEC6_PINMUX_P1_6_CTRL,
	ARTPEC6_PINMUX_P1_7_CTRL,
};

static const unsigned int uart5_regs0[] = {
	ARTPEC6_PINMUX_P1_12_CTRL,
	ARTPEC6_PINMUX_P1_13_CTRL,
	ARTPEC6_PINMUX_P1_14_CTRL,
	ARTPEC6_PINMUX_P1_15_CTRL,
};

static const unsigned int nand_regs0[] = {
	ARTPEC6_PINMUX_P4_0_CTRL,
	ARTPEC6_PINMUX_P4_1_CTRL,
	ARTPEC6_PINMUX_P4_2_CTRL,
	ARTPEC6_PINMUX_P4_3_CTRL,
	ARTPEC6_PINMUX_P4_4_CTRL,
	ARTPEC6_PINMUX_P4_5_CTRL,
	ARTPEC6_PINMUX_P4_6_CTRL,
	ARTPEC6_PINMUX_P4_7_CTRL,
	ARTPEC6_PINMUX_P4_8_CTRL,
	ARTPEC6_PINMUX_P4_9_CTRL,
	ARTPEC6_PINMUX_P4_10_CTRL,
	ARTPEC6_PINMUX_P4_11_CTRL,
	ARTPEC6_PINMUX_P4_12_CTRL,
	ARTPEC6_PINMUX_P4_13_CTRL,
	ARTPEC6_PINMUX_P4_14_CTRL,
	ARTPEC6_PINMUX_P4_15_CTRL,
	ARTPEC6_PINMUX_P5_0_CTRL,
};

static const unsigned int sdio0_regs0[] = {
	ARTPEC6_PINMUX_P6_0_CTRL,
	ARTPEC6_PINMUX_P6_1_CTRL,
	ARTPEC6_PINMUX_P6_2_CTRL,
	ARTPEC6_PINMUX_P6_3_CTRL,
	ARTPEC6_PINMUX_P6_4_CTRL,
	ARTPEC6_PINMUX_P6_5_CTRL,
	ARTPEC6_PINMUX_P6_6_CTRL,
	ARTPEC6_PINMUX_P6_7_CTRL,
};

static const unsigned int sdio1_regs0[] = {
	ARTPEC6_PINMUX_P6_8_CTRL,
	ARTPEC6_PINMUX_P6_9_CTRL,
	ARTPEC6_PINMUX_P6_10_CTRL,
	ARTPEC6_PINMUX_P6_11_CTRL,
	ARTPEC6_PINMUX_P6_12_CTRL,
	ARTPEC6_PINMUX_P6_13_CTRL,
	ARTPEC6_PINMUX_P6_14_CTRL,
	ARTPEC6_PINMUX_P6_15_CTRL,
};

static const unsigned int ethernet_regs0[] = {
	ARTPEC6_PINMUX_P7_0_CTRL,
	ARTPEC6_PINMUX_P7_1_CTRL,
	ARTPEC6_PINMUX_P7_2_CTRL,
	ARTPEC6_PINMUX_P7_3_CTRL,
	ARTPEC6_PINMUX_P7_4_CTRL,
	ARTPEC6_PINMUX_P7_5_CTRL,
	ARTPEC6_PINMUX_P7_6_CTRL,
	ARTPEC6_PINMUX_P7_7_CTRL,
	ARTPEC6_PINMUX_P7_8_CTRL,
	ARTPEC6_PINMUX_P7_9_CTRL,
	ARTPEC6_PINMUX_P7_10_CTRL,
	ARTPEC6_PINMUX_P7_11_CTRL,
	ARTPEC6_PINMUX_P7_12_CTRL,
	ARTPEC6_PINMUX_P7_13_CTRL,
	ARTPEC6_PINMUX_P7_14_CTRL,
	ARTPEC6_PINMUX_P7_15_CTRL,
	ARTPEC6_PINMUX_P8_0_CTRL,
	ARTPEC6_PINMUX_P8_1_CTRL,
	ARTPEC6_PINMUX_P8_2_CTRL,
	ARTPEC6_PINMUX_P8_3_CTRL,
	ARTPEC6_PINMUX_P8_4_CTRL,
	ARTPEC6_PINMUX_P8_5_CTRL,
	ARTPEC6_PINMUX_P8_6_CTRL,
	ARTPEC6_PINMUX_P8_7_CTRL,
	ARTPEC6_PINMUX_P8_8_CTRL,
	ARTPEC6_PINMUX_P8_9_CTRL,
	ARTPEC6_PINMUX_P8_10_CTRL,
	ARTPEC6_PINMUX_P8_11_CTRL,
};

static const unsigned int cpuclkout_pins0[] = { 0 };
static const unsigned int udlclkout_pins0[] = { 1 };
static const unsigned int i2c1_pins0[] = { 2, 3 };
static const unsigned int i2c2_pins0[] = { 4, 5 };
static const unsigned int i2c3_pins0[] = { 6, 7 };
static const unsigned int i2s0_pins0[] = { 8, 9, 10, 11 };
static const unsigned int i2s1_pins0[] = { 12, 13, 14, 15 };
static const unsigned int i2srefclk_pins0[] = { 19 };
static const unsigned int spi0_pins0[] = { 12, 13, 14, 15 };
static const unsigned int spi1_pins0[] = { 16, 17, 18, 19 };
static const unsigned int pciedebug_pins0[] = { 12, 13, 14, 15 };
static const unsigned int uart0_pins0[] = { 16, 17, 18, 19, 20,
					    21, 22, 23, 24, 25 };
static const unsigned int uart0_pins1[] = { 20, 21, 22, 23 };
static const unsigned int uart1_pins0[] = { 24, 25, 26, 27 };
static const unsigned int uart2_pins0[] = { 26, 27, 28, 29, 30,
					    31, 32, 33, 34, 35 };
static const unsigned int uart2_pins1[] = { 28, 29, 30, 31 };
static const unsigned int uart3_pins0[] = { 32, 33, 34, 35 };
static const unsigned int uart4_pins0[] = { 20, 21, 22, 23 };
static const unsigned int uart5_pins0[] = { 28, 29, 30, 31 };
static const unsigned int nand_pins0[]  = { 36, 37, 38, 39, 40, 41,
					    42, 43, 44, 45, 46, 47,
					    48, 49, 50, 51, 52 };
static const unsigned int sdio0_pins0[] = { 53, 54, 55, 56, 57, 58, 59, 60 };
static const unsigned int sdio1_pins0[] = { 61, 62, 63, 64, 65, 66, 67, 68 };
static const unsigned int ethernet_pins0[]  = { 69, 70, 71, 72, 73, 74, 75,
						76, 77, 78, 79, 80, 81, 82,
						83, 84,	85, 86, 87, 88, 89,
						90, 91, 92, 93, 94, 95, 96 };

static struct artpec6_pin_group artpec6_pin_groups[] = {
	{
		.name = "cpuclkoutgrp0",
		.pins = cpuclkout_pins0,
		.num_pins = ARRAY_SIZE(cpuclkout_pins0),
		.reg_offsets = cpuclkout_regs0,
		.num_regs = ARRAY_SIZE(cpuclkout_regs0),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "udlclkoutgrp0",
		.pins = udlclkout_pins0,
		.num_pins = ARRAY_SIZE(udlclkout_pins0),
		.reg_offsets = udlclkout_regs0,
		.num_regs = ARRAY_SIZE(udlclkout_regs0),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "i2c1grp0",
		.pins = i2c1_pins0,
		.num_pins = ARRAY_SIZE(i2c1_pins0),
		.reg_offsets = i2c1_regs0,
		.num_regs = ARRAY_SIZE(i2c1_regs0),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "i2c2grp0",
		.pins = i2c2_pins0,
		.num_pins = ARRAY_SIZE(i2c2_pins0),
		.reg_offsets = i2c2_regs0,
		.num_regs = ARRAY_SIZE(i2c2_regs0),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "i2c3grp0",
		.pins = i2c3_pins0,
		.num_pins = ARRAY_SIZE(i2c3_pins0),
		.reg_offsets = i2c3_regs0,
		.num_regs = ARRAY_SIZE(i2c3_regs0),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "i2s0grp0",
		.pins = i2s0_pins0,
		.num_pins = ARRAY_SIZE(i2s0_pins0),
		.reg_offsets = i2s0_regs0,
		.num_regs = ARRAY_SIZE(i2s0_regs0),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "i2s1grp0",
		.pins = i2s1_pins0,
		.num_pins = ARRAY_SIZE(i2s1_pins0),
		.reg_offsets = i2s1_regs0,
		.num_regs = ARRAY_SIZE(i2s1_regs0),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "i2srefclkgrp0",
		.pins = i2srefclk_pins0,
		.num_pins = ARRAY_SIZE(i2srefclk_pins0),
		.reg_offsets = i2srefclk_regs0,
		.num_regs = ARRAY_SIZE(i2srefclk_regs0),
		.config = ARTPEC6_CONFIG_3,
	},
	{
		.name = "spi0grp0",
		.pins = spi0_pins0,
		.num_pins = ARRAY_SIZE(spi0_pins0),
		.reg_offsets = spi0_regs0,
		.num_regs = ARRAY_SIZE(spi0_regs0),
		.config = ARTPEC6_CONFIG_2,
	},
	{
		.name = "spi1grp0",
		.pins = spi1_pins0,
		.num_pins = ARRAY_SIZE(spi1_pins0),
		.reg_offsets = spi1_regs0,
		.num_regs = ARRAY_SIZE(spi1_regs0),
		.config = ARTPEC6_CONFIG_2,
	},
	{
		.name = "pciedebuggrp0",
		.pins = pciedebug_pins0,
		.num_pins = ARRAY_SIZE(pciedebug_pins0),
		.reg_offsets = pciedebug_regs0,
		.num_regs = ARRAY_SIZE(pciedebug_regs0),
		.config = ARTPEC6_CONFIG_3,
	},
	{
		.name = "uart0grp0",
		.pins = uart0_pins0,
		.num_pins = ARRAY_SIZE(uart0_pins0),
		.reg_offsets = uart0_regs0,
		.num_regs = ARRAY_SIZE(uart0_regs0),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "uart0grp1",
		.pins = uart0_pins1,
		.num_pins = ARRAY_SIZE(uart0_pins1),
		.reg_offsets = uart0_regs1,
		.num_regs = ARRAY_SIZE(uart0_regs1),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "uart1grp0",
		.pins = uart1_pins0,
		.num_pins = ARRAY_SIZE(uart1_pins0),
		.reg_offsets = uart1_regs0,
		.num_regs = ARRAY_SIZE(uart1_regs0),
		.config = ARTPEC6_CONFIG_2,
	},
	{
		.name = "uart2grp0",
		.pins = uart2_pins0,
		.num_pins = ARRAY_SIZE(uart2_pins0),
		.reg_offsets = uart2_regs0,
		.num_regs = ARRAY_SIZE(uart2_regs0),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "uart2grp1",
		.pins = uart2_pins1,
		.num_pins = ARRAY_SIZE(uart2_pins1),
		.reg_offsets = uart2_regs1,
		.num_regs = ARRAY_SIZE(uart2_regs1),
		.config = ARTPEC6_CONFIG_1,
	},
	{
		.name = "uart3grp0",
		.pins = uart3_pins0,
		.num_pins = ARRAY_SIZE(uart3_pins0),
		.reg_offsets = uart3_regs0,
		.num_regs = ARRAY_SIZE(uart3_regs0),
		.config = ARTPEC6_CONFIG_0,
	},
	{
		.name = "uart4grp0",
		.pins = uart4_pins0,
		.num_pins = ARRAY_SIZE(uart4_pins0),
		.reg_offsets = uart4_regs0,
		.num_regs = ARRAY_SIZE(uart4_regs0),
		.config = ARTPEC6_CONFIG_2,
	},
	{
		.name = "uart5grp0",
		.pins = uart5_pins0,
		.num_pins = ARRAY_SIZE(uart5_pins0),
		.reg_offsets = uart5_regs0,
		.num_regs = ARRAY_SIZE(uart5_regs0),
		.config = ARTPEC6_CONFIG_2,
	},
	{
		.name = "uart5nocts",
		.pins = uart5_pins0,
		.num_pins = ARRAY_SIZE(uart5_pins0) - 1,
		.reg_offsets = uart5_regs0,
		.num_regs = ARRAY_SIZE(uart5_regs0) - 1,
		.config = ARTPEC6_CONFIG_2,
	},
	{
		.name = "nandgrp0",
		.pins = nand_pins0,
		.num_pins = ARRAY_SIZE(nand_pins0),
		.reg_offsets = nand_regs0,
		.num_regs = ARRAY_SIZE(nand_regs0),
		.config = ARTPEC6_CONFIG_0,
	},
	{
		.name = "sdio0grp0",
		.pins = sdio0_pins0,
		.num_pins = ARRAY_SIZE(sdio0_pins0),
		.reg_offsets = sdio0_regs0,
		.num_regs = ARRAY_SIZE(sdio0_regs0),
		.config = ARTPEC6_CONFIG_0,
	},
	{
		.name = "sdio1grp0",
		.pins = sdio1_pins0,
		.num_pins = ARRAY_SIZE(sdio1_pins0),
		.reg_offsets = sdio1_regs0,
		.num_regs = ARRAY_SIZE(sdio1_regs0),
		.config = ARTPEC6_CONFIG_0,
	},
	{
		.name = "ethernetgrp0",
		.pins = ethernet_pins0,
		.num_pins = ARRAY_SIZE(ethernet_pins0),
		.reg_offsets = ethernet_regs0,
		.num_regs = ARRAY_SIZE(ethernet_regs0),
		.config = ARTPEC6_CONFIG_0,
	},
};

static int artpec6_get_groups_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(artpec6_pin_groups);
}

static const char *artpec6_get_group_name(struct pinctrl_dev *pctldev,
					  unsigned int group)
{
	return artpec6_pin_groups[group].name;
}

static int artpec6_get_group_pins(struct pinctrl_dev *pctldev,
				  unsigned int group,
				  const unsigned int **pins,
				  unsigned int *num_pins)
{
	*pins = (unsigned int *)artpec6_pin_groups[group].pins;
	*num_pins = artpec6_pin_groups[group].num_pins;
	return 0;
}

static int artpec6_pconf_drive_mA_to_field(unsigned int mA)
{
	switch (mA) {
	case 4:
		return ARTPEC6_DRIVE_4mA_SET;
	case 6:
		return ARTPEC6_DRIVE_6mA_SET;
	case 8:
		return ARTPEC6_DRIVE_8mA_SET;
	case 9:
		return ARTPEC6_DRIVE_9mA_SET;
	default:
		return -EINVAL;
	}
}

static unsigned int artpec6_pconf_drive_field_to_mA(int field)
{
	switch (field) {
	case ARTPEC6_DRIVE_4mA_SET:
		return 4;
	case ARTPEC6_DRIVE_6mA_SET:
		return 6;
	case ARTPEC6_DRIVE_8mA_SET:
		return 8;
	case ARTPEC6_DRIVE_9mA_SET:
		return 9;
	default:
		/* Shouldn't happen */
		return 0;
	}
}

static struct pinctrl_ops artpec6_pctrl_ops = {
	.get_group_pins	  = artpec6_get_group_pins,
	.get_groups_count = artpec6_get_groups_count,
	.get_group_name	  = artpec6_get_group_name,
	.dt_node_to_map   = pinconf_generic_dt_node_to_map_all,
	.dt_free_map      = pinctrl_utils_free_map,
};

static const char * const gpiogrps[] = {
	"cpuclkoutgrp0", "udlclkoutgrp0", "i2c1grp0",      "i2c2grp0",
	"i2c3grp0",      "i2s0grp0",      "i2s1grp0",      "i2srefclkgrp0",
	"spi0grp0",      "spi1grp0",      "pciedebuggrp0", "uart0grp0",
	"uart0grp1",     "uart1grp0",     "uart2grp0",     "uart2grp1",
	"uart4grp0",     "uart5grp0",
};
static const char * const cpuclkoutgrps[] = { "cpuclkoutgrp0" };
static const char * const udlclkoutgrps[] = { "udlclkoutgrp0" };
static const char * const i2c1grps[]	  = { "i2c1grp0" };
static const char * const i2c2grps[]	  = { "i2c2grp0" };
static const char * const i2c3grps[]	  = { "i2c3grp0" };
static const char * const i2s0grps[]	  = { "i2s0grp0" };
static const char * const i2s1grps[]	  = { "i2s1grp0" };
static const char * const i2srefclkgrps[] = { "i2srefclkgrp0" };
static const char * const spi0grps[]	  = { "spi0grp0" };
static const char * const spi1grps[]	  = { "spi1grp0" };
static const char * const pciedebuggrps[] = { "pciedebuggrp0" };
static const char * const uart0grps[]	  = { "uart0grp0", "uart0grp1" };
static const char * const uart1grps[]	  = { "uart1grp0" };
static const char * const uart2grps[]	  = { "uart2grp0", "uart2grp1" };
static const char * const uart3grps[]	  = { "uart3grp0" };
static const char * const uart4grps[]	  = { "uart4grp0" };
static const char * const uart5grps[]	  = { "uart5grp0", "uart5nocts" };
static const char * const nandgrps[]	  = { "nandgrp0" };
static const char * const sdio0grps[]	  = { "sdio0grp0" };
static const char * const sdio1grps[]	  = { "sdio1grp0" };
static const char * const ethernetgrps[]  = { "ethernetgrp0" };

static struct artpec6_pmx_func artpec6_pmx_functions[] = {
	{
		.name = "gpio",
		.groups = gpiogrps,
		.num_groups = ARRAY_SIZE(gpiogrps),
	},
	{
		.name = "cpuclkout",
		.groups = cpuclkoutgrps,
		.num_groups = ARRAY_SIZE(cpuclkoutgrps),
	},
	{
		.name = "udlclkout",
		.groups = udlclkoutgrps,
		.num_groups = ARRAY_SIZE(udlclkoutgrps),
	},
	{
		.name = "i2c1",
		.groups = i2c1grps,
		.num_groups = ARRAY_SIZE(i2c1grps),
	},
	{
		.name = "i2c2",
		.groups = i2c2grps,
		.num_groups = ARRAY_SIZE(i2c2grps),
	},
	{
		.name = "i2c3",
		.groups = i2c3grps,
		.num_groups = ARRAY_SIZE(i2c3grps),
	},
	{
		.name = "i2s0",
		.groups = i2s0grps,
		.num_groups = ARRAY_SIZE(i2s0grps),
	},
	{
		.name = "i2s1",
		.groups = i2s1grps,
		.num_groups = ARRAY_SIZE(i2s1grps),
	},
	{
		.name = "i2srefclk",
		.groups = i2srefclkgrps,
		.num_groups = ARRAY_SIZE(i2srefclkgrps),
	},
	{
		.name = "spi0",
		.groups = spi0grps,
		.num_groups = ARRAY_SIZE(spi0grps),
	},
	{
		.name = "spi1",
		.groups = spi1grps,
		.num_groups = ARRAY_SIZE(spi1grps),
	},
	{
		.name = "pciedebug",
		.groups = pciedebuggrps,
		.num_groups = ARRAY_SIZE(pciedebuggrps),
	},
	{
		.name = "uart0",
		.groups = uart0grps,
		.num_groups = ARRAY_SIZE(uart0grps),
	},
	{
		.name = "uart1",
		.groups = uart1grps,
		.num_groups = ARRAY_SIZE(uart1grps),
	},
	{
		.name = "uart2",
		.groups = uart2grps,
		.num_groups = ARRAY_SIZE(uart2grps),
	},
	{
		.name = "uart3",
		.groups = uart3grps,
		.num_groups = ARRAY_SIZE(uart3grps),
	},
	{
		.name = "uart4",
		.groups = uart4grps,
		.num_groups = ARRAY_SIZE(uart4grps),
	},
	{
		.name = "uart5",
		.groups = uart5grps,
		.num_groups = ARRAY_SIZE(uart5grps),
	},
	{
		.name = "nand",
		.groups = nandgrps,
		.num_groups = ARRAY_SIZE(nandgrps),
	},
	{
		.name = "sdio0",
		.groups = sdio0grps,
		.num_groups = ARRAY_SIZE(sdio0grps),
	},
	{
		.name = "sdio1",
		.groups = sdio1grps,
		.num_groups = ARRAY_SIZE(sdio1grps),
	},
	{
		.name = "ethernet",
		.groups = ethernetgrps,
		.num_groups = ARRAY_SIZE(ethernetgrps),
	},
};

static int artpec6_pmx_get_functions_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(artpec6_pmx_functions);
}

static const char *artpec6_pmx_get_fname(struct pinctrl_dev *pctldev,
				  unsigned int function)
{
	return artpec6_pmx_functions[function].name;
}

static int artpec6_pmx_get_fgroups(struct pinctrl_dev *pctldev,
				   unsigned int function,
				   const char * const **groups,
				   unsigned int * const num_groups)
{
	*groups = artpec6_pmx_functions[function].groups;
	*num_groups = artpec6_pmx_functions[function].num_groups;
	return 0;
}

static void artpec6_pmx_select_func(struct pinctrl_dev *pctldev,
				    unsigned int function, unsigned int group,
				    bool enable)
{
	unsigned int regval, val;
	int i;
	const unsigned int *pmx_regs;
	struct artpec6_pmx *pmx = pinctrl_dev_get_drvdata(pctldev);

	pmx_regs = artpec6_pin_groups[group].reg_offsets;

	for (i = 0; i < artpec6_pin_groups[group].num_regs; i++) {
		/* Ports 4-8 do not have a SEL field and are always selected */
		if (pmx_regs[i] >= ARTPEC6_PINMUX_P4_0_CTRL)
			continue;

		if (!strcmp(artpec6_pmx_get_fname(pctldev, function), "gpio")) {
			/* GPIO is always config 0 */
			val = ARTPEC6_CONFIG_0 << ARTPEC6_PINMUX_SEL_SHIFT;
		} else {
			if (enable)
				val = artpec6_pin_groups[group].config
					<< ARTPEC6_PINMUX_SEL_SHIFT;
			else
				val = ARTPEC6_CONFIG_0
					<< ARTPEC6_PINMUX_SEL_SHIFT;
		}

		regval = readl(pmx->base + pmx_regs[i]);
		regval &= ~ARTPEC6_PINMUX_SEL_MASK;
		regval |= val;
		writel(regval, pmx->base + pmx_regs[i]);
	}
}

int artpec6_pmx_enable(struct pinctrl_dev *pctldev, unsigned int function,
		       unsigned int group)
{
	struct artpec6_pmx *pmx = pinctrl_dev_get_drvdata(pctldev);

	dev_dbg(pmx->dev, "enabling %s function for pin group %s\n",
		artpec6_pmx_get_fname(pctldev, function),
		artpec6_get_group_name(pctldev, group));

	artpec6_pmx_select_func(pctldev, function, group, true);

	return 0;
}

void artpec6_pmx_disable(struct pinctrl_dev *pctldev, unsigned int function,
			 unsigned int group)
{
	struct artpec6_pmx *pmx = pinctrl_dev_get_drvdata(pctldev);

	dev_dbg(pmx->dev, "disabling %s function for pin group %s\n",
		artpec6_pmx_get_fname(pctldev, function),
		artpec6_get_group_name(pctldev, group));

	artpec6_pmx_select_func(pctldev, function, group, false);
}

static int artpec6_pmx_request_gpio(struct pinctrl_dev *pctldev,
				    struct pinctrl_gpio_range *range,
				    unsigned int pin)
{
	struct artpec6_pmx *pmx = pinctrl_dev_get_drvdata(pctldev);
	unsigned int reg = ARTPEC6_PINMUX_P0_0_CTRL + pin * 4;
	u32 val;

	if (pin >= 32)
		return -EINVAL;

	val = readl_relaxed(pmx->base + reg);
	val &= ~ARTPEC6_PINMUX_SEL_MASK;
	val |= ARTPEC6_CONFIG_0 << ARTPEC6_PINMUX_SEL_SHIFT;
	writel_relaxed(val, pmx->base + reg);

	return 0;
}

static const struct pinmux_ops artpec6_pmx_ops = {
	.get_functions_count = artpec6_pmx_get_functions_count,
	.get_function_name   = artpec6_pmx_get_fname,
	.get_function_groups = artpec6_pmx_get_fgroups,
	.set_mux	     = artpec6_pmx_enable,
	.gpio_request_enable = artpec6_pmx_request_gpio,
};

static int artpec6_pconf_get(struct pinctrl_dev *pctldev, unsigned int pin,
			     unsigned long *config)
{
	struct artpec6_pmx *pmx = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	unsigned int regval;

	/* Check for valid pin */
	if (pin >= pmx->num_pins) {
		dev_dbg(pmx->dev, "pinconf is not supported for pin %s\n",
			pmx->pins[pin].name);
		return -ENOTSUPP;
	}

	dev_dbg(pmx->dev, "getting configuration for pin %s\n",
		pmx->pins[pin].name);

	/* Read pin register values */
	regval = readl(pmx->base + pin_regs[pin]);

	/* If valid, get configuration for parameter */
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		if (!(regval & ARTPEC6_PINMUX_UDC1_MASK))
			return -EINVAL;
		break;

	case PIN_CONFIG_BIAS_PULL_UP:
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (regval & ARTPEC6_PINMUX_UDC1_MASK)
			return -EINVAL;

		regval = regval & ARTPEC6_PINMUX_UDC0_MASK;
		if ((param == PIN_CONFIG_BIAS_PULL_UP && !regval) ||
		    (param == PIN_CONFIG_BIAS_PULL_DOWN && regval))
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		regval = (regval & ARTPEC6_PINMUX_DRV_MASK)
			>> ARTPEC6_PINMUX_DRV_SHIFT;
		regval = artpec6_pconf_drive_field_to_mA(regval);
		*config = pinconf_to_config_packed(param, regval);
		break;
	default:
		return -ENOTSUPP;
	}

	return 0;
}

/*
 * Valid combinations of param and arg:
 *
 * param                      arg
 * PIN_CONFIG_BIAS_DISABLE:   x (disable bias)
 * PIN_CONFIG_BIAS_PULL_UP:   1 (pull up bias + enable)
 * PIN_CONFIG_BIAS_PULL_DOWN: 1 (pull down bias + enable)
 * PIN_CONFIG_DRIVE_STRENGTH: 0 (4mA), 1 (6mA), 2 (8mA), 3 (9mA)
 *
 * All other args are invalid. All other params are not supported.
 */
static int artpec6_pconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
			     unsigned long *configs, unsigned int num_configs)
{
	struct artpec6_pmx *pmx = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param;
	unsigned int arg;
	unsigned int regval;
	unsigned int *reg;
	int i;

	/* Check for valid pin */
	if (pin >= pmx->num_pins) {
		dev_dbg(pmx->dev, "pinconf is not supported for pin %s\n",
			pmx->pins[pin].name);
		return -ENOTSUPP;
	}

	dev_dbg(pmx->dev, "setting configuration for pin %s\n",
		pmx->pins[pin].name);

	reg = pmx->base + pin_regs[pin];

	/* For each config */
	for (i = 0; i < num_configs; i++) {
		int drive;

		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
			regval = readl(reg);
			regval |= (1 << ARTPEC6_PINMUX_UDC1_SHIFT);
			writel(regval, reg);
			break;

		case PIN_CONFIG_BIAS_PULL_UP:
			if (arg != 1) {
				dev_dbg(pctldev->dev, "%s: arg %u out of range\n",
					__func__, arg);
				return -EINVAL;
			}

			regval = readl(reg);
			regval |= (arg << ARTPEC6_PINMUX_UDC0_SHIFT);
			regval &= ~ARTPEC6_PINMUX_UDC1_MASK; /* Enable */
			writel(regval, reg);
			break;

		case PIN_CONFIG_BIAS_PULL_DOWN:
			if (arg != 1) {
				dev_dbg(pctldev->dev, "%s: arg %u out of range\n",
					__func__, arg);
				return -EINVAL;
			}

			regval = readl(reg);
			regval &= ~(arg << ARTPEC6_PINMUX_UDC0_SHIFT);
			regval &= ~ARTPEC6_PINMUX_UDC1_MASK; /* Enable */
			writel(regval, reg);
			break;

		case PIN_CONFIG_DRIVE_STRENGTH:
			drive = artpec6_pconf_drive_mA_to_field(arg);
			if (drive < 0) {
				dev_dbg(pctldev->dev, "%s: arg %u out of range\n",
					__func__, arg);
				return -EINVAL;
			}

			regval = readl(reg);
			regval &= ~ARTPEC6_PINMUX_DRV_MASK;
			regval |= (drive << ARTPEC6_PINMUX_DRV_SHIFT);
			writel(regval, reg);
			break;

		default:
			dev_dbg(pmx->dev, "parameter not supported\n");
			return -ENOTSUPP;
		}
	}

	return 0;
}

static int artpec6_pconf_group_set(struct pinctrl_dev *pctldev,
				   unsigned int group, unsigned long *configs,
				   unsigned int num_configs)
{
	unsigned int num_pins, current_pin;
	int ret;

	dev_dbg(pctldev->dev, "setting group %s configuration\n",
		artpec6_get_group_name(pctldev, group));

	num_pins = artpec6_pin_groups[group].num_pins;

	for (current_pin = 0; current_pin < num_pins; current_pin++) {
		ret = artpec6_pconf_set(pctldev,
				artpec6_pin_groups[group].pins[current_pin],
				configs, num_configs);

		if (ret < 0)
			return ret;
	}

	return 0;
}

static const struct pinconf_ops artpec6_pconf_ops = {
	.is_generic	      = true,
	.pin_config_get	      = artpec6_pconf_get,
	.pin_config_set	      = artpec6_pconf_set,
	.pin_config_group_set = artpec6_pconf_group_set,
};

static struct pinctrl_desc artpec6_desc = {
	.name	 = "artpec6-pinctrl",
	.owner	 = THIS_MODULE,
	.pins	 = artpec6_pins,
	.npins	 = ARRAY_SIZE(artpec6_pins),
	.pctlops = &artpec6_pctrl_ops,
	.pmxops	 = &artpec6_pmx_ops,
	.confops = &artpec6_pconf_ops,
};

/* The reset values say 4mA, but we want 8mA as default. */
static void artpec6_pmx_reset(struct artpec6_pmx *pmx)
{
	void __iomem *base = pmx->base;
	int i;

	for (i = 0; i < ARRAY_SIZE(pin_regs); i++) {
		u32 val;

		val = readl_relaxed(pmx->base + pin_regs[i]);
		val &= ~ARTPEC6_PINMUX_DRV_MASK;
		val |= ARTPEC6_DRIVE_8mA_SET << ARTPEC6_PINMUX_DRV_SHIFT;
		writel_relaxed(val, base + pin_regs[i]);
	}
}

static int artpec6_pmx_probe(struct platform_device *pdev)
{
	struct artpec6_pmx *pmx;
	struct resource *res;

	pmx = devm_kzalloc(&pdev->dev, sizeof(*pmx), GFP_KERNEL);
	if (!pmx)
		return -ENOMEM;

	pmx->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pmx->base = devm_ioremap_resource(&pdev->dev, res);

	if (IS_ERR(pmx->base))
		return PTR_ERR(pmx->base);

	artpec6_pmx_reset(pmx);

	pmx->pins	    = artpec6_pins;
	pmx->num_pins	    = ARRAY_SIZE(artpec6_pins);
	pmx->functions	    = artpec6_pmx_functions;
	pmx->num_functions  = ARRAY_SIZE(artpec6_pmx_functions);
	pmx->pin_groups	    = artpec6_pin_groups;
	pmx->num_pin_groups = ARRAY_SIZE(artpec6_pin_groups);
	pmx->pctl	    = pinctrl_register(&artpec6_desc, &pdev->dev, pmx);

	if (!pmx->pctl) {
		dev_err(&pdev->dev, "could not register pinctrl driver\n");
		return -EINVAL;
	}

	platform_set_drvdata(pdev, pmx);

	dev_info(&pdev->dev, "initialised Axis ARTPEC-6 pinctrl driver\n");

	return 0;
}

static int artpec6_pmx_remove(struct platform_device *pdev)
{
	struct artpec6_pmx *pmx = platform_get_drvdata(pdev);

	pinctrl_unregister(pmx->pctl);

	return 0;
}

static const struct of_device_id artpec6_pinctrl_match[] = {
	{ .compatible = "axis,artpec6-pinctrl" },
	{},
};

static struct platform_driver artpec6_pmx_driver = {
	.driver = {
		.name = "artpec6-pinctrl",
		.owner = THIS_MODULE,
		.of_match_table = artpec6_pinctrl_match,
	},
	.probe = artpec6_pmx_probe,
	.remove = artpec6_pmx_remove,
};

static int __init artpec6_pmx_init(void)
{
	return platform_driver_register(&artpec6_pmx_driver);
}
arch_initcall(artpec6_pmx_init);

static void __exit artpec6_pmx_exit(void)
{
	platform_driver_unregister(&artpec6_pmx_driver);
}
module_exit(artpec6_pmx_exit);

MODULE_AUTHOR("Chris Paterson <chris.paterson@linux.pieboy.co.uk>");
MODULE_DESCRIPTION("Axis ARTPEC-6 pin control driver");
MODULE_LICENSE("GPL v2");
MODULE_DEVICE_TABLE(of, artpec6_pinctrl_match);
