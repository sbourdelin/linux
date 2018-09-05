// SPDX-License-Identifier: GPL-2.0
/*
 * lochnagar-i2c.c  --  Lochnagar I2C bus interface
 *
 * Copyright (c) 2012-2018 Cirrus Logic Inc.
 *
 * Author: Charles Keepax <ckeepax@opensource.cirrus.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>

#include <linux/mfd/lochnagar.h>

#define LOCHNAGAR_BOOT_RETRIES		10
#define LOCHNAGAR_BOOT_DELAY_MS		350

#define LOCHNAGAR_CONFIG_POLL_US	10000

static bool lochnagar1_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LOCHNAGAR_SOFTWARE_RESET:
	case LOCHNAGAR_FIRMWARE_ID1:
	case LOCHNAGAR_FIRMWARE_ID2:
	case LOCHNAGAR1_CDC_AIF1_SEL:
	case LOCHNAGAR1_CDC_AIF2_SEL:
	case LOCHNAGAR1_CDC_AIF3_SEL:
	case LOCHNAGAR1_CDC_MCLK1_SEL:
	case LOCHNAGAR1_CDC_MCLK2_SEL:
	case LOCHNAGAR1_CDC_AIF_CTRL1:
	case LOCHNAGAR1_CDC_AIF_CTRL2:
	case LOCHNAGAR1_EXT_AIF_CTRL:
	case LOCHNAGAR1_DSP_AIF1_SEL:
	case LOCHNAGAR1_DSP_AIF2_SEL:
	case LOCHNAGAR1_DSP_CLKIN_SEL:
	case LOCHNAGAR1_DSP_AIF:
	case LOCHNAGAR1_GF_AIF1:
	case LOCHNAGAR1_GF_AIF2:
	case LOCHNAGAR1_PSIA_AIF:
	case LOCHNAGAR1_PSIA1_SEL:
	case LOCHNAGAR1_PSIA2_SEL:
	case LOCHNAGAR1_SPDIF_AIF_SEL:
	case LOCHNAGAR1_GF_AIF3_SEL:
	case LOCHNAGAR1_GF_AIF4_SEL:
	case LOCHNAGAR1_GF_CLKOUT1_SEL:
	case LOCHNAGAR1_GF_AIF1_SEL:
	case LOCHNAGAR1_GF_AIF2_SEL:
	case LOCHNAGAR1_GF_GPIO2:
	case LOCHNAGAR1_GF_GPIO3:
	case LOCHNAGAR1_GF_GPIO7:
	case LOCHNAGAR1_RST:
	case LOCHNAGAR1_LED1:
	case LOCHNAGAR1_LED2:
	case LOCHNAGAR1_I2C_CTRL:
		return true;
	default:
		return false;
	}
}

static const struct reg_default lochnagar1_reg_defaults[] = {
	{ LOCHNAGAR1_CDC_AIF1_SEL,    0x00 },
	{ LOCHNAGAR1_CDC_AIF2_SEL,    0x00 },
	{ LOCHNAGAR1_CDC_AIF3_SEL,    0x00 },
	{ LOCHNAGAR1_CDC_MCLK1_SEL,   0x00 },
	{ LOCHNAGAR1_CDC_MCLK2_SEL,   0x00 },
	{ LOCHNAGAR1_CDC_AIF_CTRL1,   0x00 },
	{ LOCHNAGAR1_CDC_AIF_CTRL2,   0x00 },
	{ LOCHNAGAR1_EXT_AIF_CTRL,    0x00 },
	{ LOCHNAGAR1_DSP_AIF1_SEL,    0x00 },
	{ LOCHNAGAR1_DSP_AIF2_SEL,    0x00 },
	{ LOCHNAGAR1_DSP_CLKIN_SEL,   0x01 },
	{ LOCHNAGAR1_DSP_AIF,         0x08 },
	{ LOCHNAGAR1_GF_AIF1,         0x00 },
	{ LOCHNAGAR1_GF_AIF2,         0x00 },
	{ LOCHNAGAR1_PSIA_AIF,        0x00 },
	{ LOCHNAGAR1_PSIA1_SEL,       0x00 },
	{ LOCHNAGAR1_PSIA2_SEL,       0x00 },
	{ LOCHNAGAR1_SPDIF_AIF_SEL,   0x00 },
	{ LOCHNAGAR1_GF_AIF3_SEL,     0x00 },
	{ LOCHNAGAR1_GF_AIF4_SEL,     0x00 },
	{ LOCHNAGAR1_GF_CLKOUT1_SEL,  0x00 },
	{ LOCHNAGAR1_GF_AIF1_SEL,     0x00 },
	{ LOCHNAGAR1_GF_AIF2_SEL,     0x00 },
	{ LOCHNAGAR1_GF_GPIO2,        0x00 },
	{ LOCHNAGAR1_GF_GPIO3,        0x00 },
	{ LOCHNAGAR1_GF_GPIO7,        0x00 },
	{ LOCHNAGAR1_RST,             0x00 },
	{ LOCHNAGAR1_LED1,            0x00 },
	{ LOCHNAGAR1_LED2,            0x00 },
	{ LOCHNAGAR1_I2C_CTRL,        0x01 },
};

static const struct regmap_config lochnagar1_i2c_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,

	.max_register = 0x50,
	.readable_reg = lochnagar1_readable_register,

	.use_single_rw = true,

	.cache_type = REGCACHE_RBTREE,

	.reg_defaults = lochnagar1_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(lochnagar1_reg_defaults),
};

static const struct reg_sequence lochnagar1_patch[] = {
	{ 0x40, 0x0083 },
	{ 0x46, 0x0001 },
	{ 0x47, 0x0018 },
	{ 0x50, 0x0000 },
};

static struct mfd_cell lochnagar1_devs[] = {
	{
		.name = "lochnagar-pinctrl"
	},
	{
		.name = "lochnagar-clk"
	},
};

static bool lochnagar2_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LOCHNAGAR_SOFTWARE_RESET:
	case LOCHNAGAR_FIRMWARE_ID1:
	case LOCHNAGAR_FIRMWARE_ID2:
	case LOCHNAGAR2_CDC_AIF1_CTRL:
	case LOCHNAGAR2_CDC_AIF2_CTRL:
	case LOCHNAGAR2_CDC_AIF3_CTRL:
	case LOCHNAGAR2_DSP_AIF1_CTRL:
	case LOCHNAGAR2_DSP_AIF2_CTRL:
	case LOCHNAGAR2_PSIA1_CTRL:
	case LOCHNAGAR2_PSIA2_CTRL:
	case LOCHNAGAR2_GF_AIF3_CTRL:
	case LOCHNAGAR2_GF_AIF4_CTRL:
	case LOCHNAGAR2_GF_AIF1_CTRL:
	case LOCHNAGAR2_GF_AIF2_CTRL:
	case LOCHNAGAR2_SPDIF_AIF_CTRL:
	case LOCHNAGAR2_USB_AIF1_CTRL:
	case LOCHNAGAR2_USB_AIF2_CTRL:
	case LOCHNAGAR2_ADAT_AIF_CTRL:
	case LOCHNAGAR2_CDC_MCLK1_CTRL:
	case LOCHNAGAR2_CDC_MCLK2_CTRL:
	case LOCHNAGAR2_DSP_CLKIN_CTRL:
	case LOCHNAGAR2_PSIA1_MCLK_CTRL:
	case LOCHNAGAR2_PSIA2_MCLK_CTRL:
	case LOCHNAGAR2_SPDIF_MCLK_CTRL:
	case LOCHNAGAR2_GF_CLKOUT1_CTRL:
	case LOCHNAGAR2_GF_CLKOUT2_CTRL:
	case LOCHNAGAR2_ADAT_MCLK_CTRL:
	case LOCHNAGAR2_SOUNDCARD_MCLK_CTRL:
	case LOCHNAGAR2_GPIO_FPGA_GPIO1:
	case LOCHNAGAR2_GPIO_FPGA_GPIO2:
	case LOCHNAGAR2_GPIO_FPGA_GPIO3:
	case LOCHNAGAR2_GPIO_FPGA_GPIO4:
	case LOCHNAGAR2_GPIO_FPGA_GPIO5:
	case LOCHNAGAR2_GPIO_FPGA_GPIO6:
	case LOCHNAGAR2_GPIO_CDC_GPIO1:
	case LOCHNAGAR2_GPIO_CDC_GPIO2:
	case LOCHNAGAR2_GPIO_CDC_GPIO3:
	case LOCHNAGAR2_GPIO_CDC_GPIO4:
	case LOCHNAGAR2_GPIO_CDC_GPIO5:
	case LOCHNAGAR2_GPIO_CDC_GPIO6:
	case LOCHNAGAR2_GPIO_CDC_GPIO7:
	case LOCHNAGAR2_GPIO_CDC_GPIO8:
	case LOCHNAGAR2_GPIO_DSP_GPIO1:
	case LOCHNAGAR2_GPIO_DSP_GPIO2:
	case LOCHNAGAR2_GPIO_DSP_GPIO3:
	case LOCHNAGAR2_GPIO_DSP_GPIO4:
	case LOCHNAGAR2_GPIO_DSP_GPIO5:
	case LOCHNAGAR2_GPIO_DSP_GPIO6:
	case LOCHNAGAR2_GPIO_GF_GPIO2:
	case LOCHNAGAR2_GPIO_GF_GPIO3:
	case LOCHNAGAR2_GPIO_GF_GPIO7:
	case LOCHNAGAR2_GPIO_CDC_AIF1_BCLK:
	case LOCHNAGAR2_GPIO_CDC_AIF1_RXDAT:
	case LOCHNAGAR2_GPIO_CDC_AIF1_LRCLK:
	case LOCHNAGAR2_GPIO_CDC_AIF1_TXDAT:
	case LOCHNAGAR2_GPIO_CDC_AIF2_BCLK:
	case LOCHNAGAR2_GPIO_CDC_AIF2_RXDAT:
	case LOCHNAGAR2_GPIO_CDC_AIF2_LRCLK:
	case LOCHNAGAR2_GPIO_CDC_AIF2_TXDAT:
	case LOCHNAGAR2_GPIO_CDC_AIF3_BCLK:
	case LOCHNAGAR2_GPIO_CDC_AIF3_RXDAT:
	case LOCHNAGAR2_GPIO_CDC_AIF3_LRCLK:
	case LOCHNAGAR2_GPIO_CDC_AIF3_TXDAT:
	case LOCHNAGAR2_GPIO_DSP_AIF1_BCLK:
	case LOCHNAGAR2_GPIO_DSP_AIF1_RXDAT:
	case LOCHNAGAR2_GPIO_DSP_AIF1_LRCLK:
	case LOCHNAGAR2_GPIO_DSP_AIF1_TXDAT:
	case LOCHNAGAR2_GPIO_DSP_AIF2_BCLK:
	case LOCHNAGAR2_GPIO_DSP_AIF2_RXDAT:
	case LOCHNAGAR2_GPIO_DSP_AIF2_LRCLK:
	case LOCHNAGAR2_GPIO_DSP_AIF2_TXDAT:
	case LOCHNAGAR2_GPIO_PSIA1_BCLK:
	case LOCHNAGAR2_GPIO_PSIA1_RXDAT:
	case LOCHNAGAR2_GPIO_PSIA1_LRCLK:
	case LOCHNAGAR2_GPIO_PSIA1_TXDAT:
	case LOCHNAGAR2_GPIO_PSIA2_BCLK:
	case LOCHNAGAR2_GPIO_PSIA2_RXDAT:
	case LOCHNAGAR2_GPIO_PSIA2_LRCLK:
	case LOCHNAGAR2_GPIO_PSIA2_TXDAT:
	case LOCHNAGAR2_GPIO_GF_AIF3_BCLK:
	case LOCHNAGAR2_GPIO_GF_AIF3_RXDAT:
	case LOCHNAGAR2_GPIO_GF_AIF3_LRCLK:
	case LOCHNAGAR2_GPIO_GF_AIF3_TXDAT:
	case LOCHNAGAR2_GPIO_GF_AIF4_BCLK:
	case LOCHNAGAR2_GPIO_GF_AIF4_RXDAT:
	case LOCHNAGAR2_GPIO_GF_AIF4_LRCLK:
	case LOCHNAGAR2_GPIO_GF_AIF4_TXDAT:
	case LOCHNAGAR2_GPIO_GF_AIF1_BCLK:
	case LOCHNAGAR2_GPIO_GF_AIF1_RXDAT:
	case LOCHNAGAR2_GPIO_GF_AIF1_LRCLK:
	case LOCHNAGAR2_GPIO_GF_AIF1_TXDAT:
	case LOCHNAGAR2_GPIO_GF_AIF2_BCLK:
	case LOCHNAGAR2_GPIO_GF_AIF2_RXDAT:
	case LOCHNAGAR2_GPIO_GF_AIF2_LRCLK:
	case LOCHNAGAR2_GPIO_GF_AIF2_TXDAT:
	case LOCHNAGAR2_GPIO_DSP_UART1_RX:
	case LOCHNAGAR2_GPIO_DSP_UART1_TX:
	case LOCHNAGAR2_GPIO_DSP_UART2_RX:
	case LOCHNAGAR2_GPIO_DSP_UART2_TX:
	case LOCHNAGAR2_GPIO_GF_UART2_RX:
	case LOCHNAGAR2_GPIO_GF_UART2_TX:
	case LOCHNAGAR2_GPIO_USB_UART_RX:
	case LOCHNAGAR2_GPIO_CDC_PDMCLK1:
	case LOCHNAGAR2_GPIO_CDC_PDMDAT1:
	case LOCHNAGAR2_GPIO_CDC_PDMCLK2:
	case LOCHNAGAR2_GPIO_CDC_PDMDAT2:
	case LOCHNAGAR2_GPIO_CDC_DMICCLK1:
	case LOCHNAGAR2_GPIO_CDC_DMICDAT1:
	case LOCHNAGAR2_GPIO_CDC_DMICCLK2:
	case LOCHNAGAR2_GPIO_CDC_DMICDAT2:
	case LOCHNAGAR2_GPIO_CDC_DMICCLK3:
	case LOCHNAGAR2_GPIO_CDC_DMICDAT3:
	case LOCHNAGAR2_GPIO_CDC_DMICCLK4:
	case LOCHNAGAR2_GPIO_CDC_DMICDAT4:
	case LOCHNAGAR2_GPIO_DSP_DMICCLK1:
	case LOCHNAGAR2_GPIO_DSP_DMICDAT1:
	case LOCHNAGAR2_GPIO_DSP_DMICCLK2:
	case LOCHNAGAR2_GPIO_DSP_DMICDAT2:
	case LOCHNAGAR2_GPIO_I2C2_SCL:
	case LOCHNAGAR2_GPIO_I2C2_SDA:
	case LOCHNAGAR2_GPIO_I2C3_SCL:
	case LOCHNAGAR2_GPIO_I2C3_SDA:
	case LOCHNAGAR2_GPIO_I2C4_SCL:
	case LOCHNAGAR2_GPIO_I2C4_SDA:
	case LOCHNAGAR2_GPIO_DSP_STANDBY:
	case LOCHNAGAR2_GPIO_CDC_MCLK1:
	case LOCHNAGAR2_GPIO_CDC_MCLK2:
	case LOCHNAGAR2_GPIO_DSP_CLKIN:
	case LOCHNAGAR2_GPIO_PSIA1_MCLK:
	case LOCHNAGAR2_GPIO_PSIA2_MCLK:
	case LOCHNAGAR2_GPIO_GF_GPIO1:
	case LOCHNAGAR2_GPIO_GF_GPIO5:
	case LOCHNAGAR2_GPIO_DSP_GPIO20:
	case LOCHNAGAR2_GPIO_CHANNEL1:
	case LOCHNAGAR2_GPIO_CHANNEL2:
	case LOCHNAGAR2_GPIO_CHANNEL3:
	case LOCHNAGAR2_GPIO_CHANNEL4:
	case LOCHNAGAR2_GPIO_CHANNEL5:
	case LOCHNAGAR2_GPIO_CHANNEL6:
	case LOCHNAGAR2_GPIO_CHANNEL7:
	case LOCHNAGAR2_GPIO_CHANNEL8:
	case LOCHNAGAR2_GPIO_CHANNEL9:
	case LOCHNAGAR2_GPIO_CHANNEL10:
	case LOCHNAGAR2_GPIO_CHANNEL11:
	case LOCHNAGAR2_GPIO_CHANNEL12:
	case LOCHNAGAR2_GPIO_CHANNEL13:
	case LOCHNAGAR2_GPIO_CHANNEL14:
	case LOCHNAGAR2_GPIO_CHANNEL15:
	case LOCHNAGAR2_GPIO_CHANNEL16:
	case LOCHNAGAR2_MINICARD_RESETS:
	case LOCHNAGAR2_ANALOGUE_PATH_CTRL1:
	case LOCHNAGAR2_ANALOGUE_PATH_CTRL2:
	case LOCHNAGAR2_COMMS_CTRL4:
	case LOCHNAGAR2_SPDIF_CTRL:
	case LOCHNAGAR2_POWER_CTRL:
	case LOCHNAGAR2_MICVDD_CTRL1:
	case LOCHNAGAR2_MICVDD_CTRL2:
	case LOCHNAGAR2_VDDCORE_CDC_CTRL1:
	case LOCHNAGAR2_VDDCORE_CDC_CTRL2:
	case LOCHNAGAR2_SOUNDCARD_AIF_CTRL:
		return true;
	default:
		return false;
	}
}

static bool lochnagar2_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LOCHNAGAR2_GPIO_CHANNEL1:
	case LOCHNAGAR2_GPIO_CHANNEL2:
	case LOCHNAGAR2_GPIO_CHANNEL3:
	case LOCHNAGAR2_GPIO_CHANNEL4:
	case LOCHNAGAR2_GPIO_CHANNEL5:
	case LOCHNAGAR2_GPIO_CHANNEL6:
	case LOCHNAGAR2_GPIO_CHANNEL7:
	case LOCHNAGAR2_GPIO_CHANNEL8:
	case LOCHNAGAR2_GPIO_CHANNEL9:
	case LOCHNAGAR2_GPIO_CHANNEL10:
	case LOCHNAGAR2_GPIO_CHANNEL11:
	case LOCHNAGAR2_GPIO_CHANNEL12:
	case LOCHNAGAR2_GPIO_CHANNEL13:
	case LOCHNAGAR2_GPIO_CHANNEL14:
	case LOCHNAGAR2_GPIO_CHANNEL15:
	case LOCHNAGAR2_GPIO_CHANNEL16:
	case LOCHNAGAR2_ANALOGUE_PATH_CTRL1:
		return true;
	default:
		return false;
	}
}

static const struct reg_default lochnagar2_reg_defaults[] = {
	{ LOCHNAGAR2_CDC_AIF1_CTRL,         0x0000 },
	{ LOCHNAGAR2_CDC_AIF2_CTRL,         0x0000 },
	{ LOCHNAGAR2_CDC_AIF3_CTRL,         0x0000 },
	{ LOCHNAGAR2_DSP_AIF1_CTRL,         0x0000 },
	{ LOCHNAGAR2_DSP_AIF2_CTRL,         0x0000 },
	{ LOCHNAGAR2_PSIA1_CTRL,            0x0000 },
	{ LOCHNAGAR2_PSIA2_CTRL,            0x0000 },
	{ LOCHNAGAR2_GF_AIF3_CTRL,          0x0000 },
	{ LOCHNAGAR2_GF_AIF4_CTRL,          0x0000 },
	{ LOCHNAGAR2_GF_AIF1_CTRL,          0x0000 },
	{ LOCHNAGAR2_GF_AIF2_CTRL,          0x0000 },
	{ LOCHNAGAR2_SPDIF_AIF_CTRL,        0x0000 },
	{ LOCHNAGAR2_USB_AIF1_CTRL,         0x0000 },
	{ LOCHNAGAR2_USB_AIF2_CTRL,         0x0000 },
	{ LOCHNAGAR2_ADAT_AIF_CTRL,         0x0000 },
	{ LOCHNAGAR2_CDC_MCLK1_CTRL,        0x0000 },
	{ LOCHNAGAR2_CDC_MCLK2_CTRL,        0x0000 },
	{ LOCHNAGAR2_DSP_CLKIN_CTRL,        0x0000 },
	{ LOCHNAGAR2_PSIA1_MCLK_CTRL,       0x0000 },
	{ LOCHNAGAR2_PSIA2_MCLK_CTRL,       0x0000 },
	{ LOCHNAGAR2_SPDIF_MCLK_CTRL,       0x0000 },
	{ LOCHNAGAR2_GF_CLKOUT1_CTRL,       0x0000 },
	{ LOCHNAGAR2_GF_CLKOUT2_CTRL,       0x0000 },
	{ LOCHNAGAR2_ADAT_MCLK_CTRL,        0x0000 },
	{ LOCHNAGAR2_SOUNDCARD_MCLK_CTRL,   0x0000 },
	{ LOCHNAGAR2_GPIO_FPGA_GPIO1,       0x0000 },
	{ LOCHNAGAR2_GPIO_FPGA_GPIO2,       0x0000 },
	{ LOCHNAGAR2_GPIO_FPGA_GPIO3,       0x0000 },
	{ LOCHNAGAR2_GPIO_FPGA_GPIO4,       0x0000 },
	{ LOCHNAGAR2_GPIO_FPGA_GPIO5,       0x0000 },
	{ LOCHNAGAR2_GPIO_FPGA_GPIO6,       0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_GPIO1,        0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_GPIO2,        0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_GPIO3,        0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_GPIO4,        0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_GPIO5,        0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_GPIO6,        0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_GPIO7,        0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_GPIO8,        0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_GPIO1,        0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_GPIO2,        0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_GPIO3,        0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_GPIO4,        0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_GPIO5,        0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_GPIO6,        0x0000 },
	{ LOCHNAGAR2_GPIO_GF_GPIO2,         0x0000 },
	{ LOCHNAGAR2_GPIO_GF_GPIO3,         0x0000 },
	{ LOCHNAGAR2_GPIO_GF_GPIO7,         0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF1_BCLK,    0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF1_RXDAT,   0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF1_LRCLK,   0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF1_TXDAT,   0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF2_BCLK,    0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF2_RXDAT,   0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF2_LRCLK,   0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF2_TXDAT,   0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF3_BCLK,    0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF3_RXDAT,   0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF3_LRCLK,   0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_AIF3_TXDAT,   0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_AIF1_BCLK,    0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_AIF1_RXDAT,   0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_AIF1_LRCLK,   0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_AIF1_TXDAT,   0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_AIF2_BCLK,    0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_AIF2_RXDAT,   0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_AIF2_LRCLK,   0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_AIF2_TXDAT,   0x0000 },
	{ LOCHNAGAR2_GPIO_PSIA1_BCLK,       0x0000 },
	{ LOCHNAGAR2_GPIO_PSIA1_RXDAT,      0x0000 },
	{ LOCHNAGAR2_GPIO_PSIA1_LRCLK,      0x0000 },
	{ LOCHNAGAR2_GPIO_PSIA1_TXDAT,      0x0000 },
	{ LOCHNAGAR2_GPIO_PSIA2_BCLK,       0x0000 },
	{ LOCHNAGAR2_GPIO_PSIA2_RXDAT,      0x0000 },
	{ LOCHNAGAR2_GPIO_PSIA2_LRCLK,      0x0000 },
	{ LOCHNAGAR2_GPIO_PSIA2_TXDAT,      0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF3_BCLK,     0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF3_RXDAT,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF3_LRCLK,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF3_TXDAT,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF4_BCLK,     0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF4_RXDAT,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF4_LRCLK,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF4_TXDAT,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF1_BCLK,     0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF1_RXDAT,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF1_LRCLK,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF1_TXDAT,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF2_BCLK,     0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF2_RXDAT,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF2_LRCLK,    0x0000 },
	{ LOCHNAGAR2_GPIO_GF_AIF2_TXDAT,    0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_UART1_RX,     0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_UART1_TX,     0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_UART2_RX,     0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_UART2_TX,     0x0000 },
	{ LOCHNAGAR2_GPIO_GF_UART2_RX,      0x0000 },
	{ LOCHNAGAR2_GPIO_GF_UART2_TX,      0x0000 },
	{ LOCHNAGAR2_GPIO_USB_UART_RX,      0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_PDMCLK1,      0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_PDMDAT1,      0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_PDMCLK2,      0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_PDMDAT2,      0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_DMICCLK1,     0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_DMICDAT1,     0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_DMICCLK2,     0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_DMICDAT2,     0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_DMICCLK3,     0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_DMICDAT3,     0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_DMICCLK4,     0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_DMICDAT4,     0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_DMICCLK1,     0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_DMICDAT1,     0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_DMICCLK2,     0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_DMICDAT2,     0x0000 },
	{ LOCHNAGAR2_GPIO_I2C2_SCL,         0x0000 },
	{ LOCHNAGAR2_GPIO_I2C2_SDA,         0x0000 },
	{ LOCHNAGAR2_GPIO_I2C3_SCL,         0x0000 },
	{ LOCHNAGAR2_GPIO_I2C3_SDA,         0x0000 },
	{ LOCHNAGAR2_GPIO_I2C4_SCL,         0x0000 },
	{ LOCHNAGAR2_GPIO_I2C4_SDA,         0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_STANDBY,      0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_MCLK1,        0x0000 },
	{ LOCHNAGAR2_GPIO_CDC_MCLK2,        0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_CLKIN,        0x0000 },
	{ LOCHNAGAR2_GPIO_PSIA1_MCLK,       0x0000 },
	{ LOCHNAGAR2_GPIO_PSIA2_MCLK,       0x0000 },
	{ LOCHNAGAR2_GPIO_GF_GPIO1,         0x0000 },
	{ LOCHNAGAR2_GPIO_GF_GPIO5,         0x0000 },
	{ LOCHNAGAR2_GPIO_DSP_GPIO20,       0x0000 },
	{ LOCHNAGAR2_MINICARD_RESETS,       0x0000 },
	{ LOCHNAGAR2_ANALOGUE_PATH_CTRL2,   0x0000 },
	{ LOCHNAGAR2_COMMS_CTRL4,           0x0001 },
	{ LOCHNAGAR2_SPDIF_CTRL,            0x0008 },
	{ LOCHNAGAR2_POWER_CTRL,            0x0001 },
	{ LOCHNAGAR2_SOUNDCARD_AIF_CTRL,    0x0000 },
};

static const struct regmap_config lochnagar2_i2c_regmap = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,

	.max_register = 0x1F1F,
	.readable_reg = lochnagar2_readable_register,
	.volatile_reg = lochnagar2_volatile_register,

	.cache_type = REGCACHE_RBTREE,

	.reg_defaults = lochnagar2_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(lochnagar2_reg_defaults),
};

static const struct reg_sequence lochnagar2_patch[] = {
	{ 0x00EE, 0x0000 },
	{ 0x00F0, 0x0001 },
};

static struct mfd_cell lochnagar2_devs[] = {
	{
		.name = "lochnagar-pinctrl"
	},
	{
		.name = "lochnagar-clk"
	},
	{
		.name = "lochnagar-regulator"
	},
	{
		.name = "lochnagar2-sound-card",
	},
};

static struct lochnagar_config {
	int id;
	const char * const name;
	enum lochnagar_type type;
	const struct regmap_config *regmap;
	struct mfd_cell *devs;
	int ndevs;
	const struct reg_sequence *patch;
	int npatch;
} lochnagar_configs[] = {
	{
		.id = 0x50,
		.name = "lochnagar1",
		.type = LOCHNAGAR1,
		.regmap = &lochnagar1_i2c_regmap,
		.devs = lochnagar1_devs,
		.ndevs = ARRAY_SIZE(lochnagar1_devs),
		.patch = lochnagar1_patch,
		.npatch = ARRAY_SIZE(lochnagar1_patch),
	},
	{
		.id = 0xCB58,
		.name = "lochnagar2",
		.type = LOCHNAGAR2,
		.regmap = &lochnagar2_i2c_regmap,
		.devs = lochnagar2_devs,
		.ndevs = ARRAY_SIZE(lochnagar2_devs),
		.patch = lochnagar2_patch,
		.npatch = ARRAY_SIZE(lochnagar2_patch),
	},
};

static const struct of_device_id lochnagar_of_match[] = {
	{ .compatible = "cirrus,lochnagar1", .data = &lochnagar_configs[0] },
	{ .compatible = "cirrus,lochnagar2", .data = &lochnagar_configs[1] },
	{},
};

static int lochnagar_wait_for_boot(struct regmap *regmap, unsigned int *id)
{
	int i, ret;

	for (i = 0; i < LOCHNAGAR_BOOT_RETRIES; ++i) {
		msleep(LOCHNAGAR_BOOT_DELAY_MS);

		ret = regmap_read(regmap, LOCHNAGAR_SOFTWARE_RESET, id);
		if (!ret)
			return ret;
	}

	return -ETIMEDOUT;
}

int lochnagar_update_config(struct lochnagar *lochnagar)
{
	struct regmap *regmap = lochnagar->regmap;
	unsigned int done = LOCHNAGAR2_ANALOGUE_PATH_UPDATE_STS_MASK;
	int timeout_ms = LOCHNAGAR_BOOT_DELAY_MS * LOCHNAGAR_BOOT_RETRIES;
	unsigned int val = 0;
	int ret;

	switch (lochnagar->type) {
	case LOCHNAGAR1:
		return 0;
	case LOCHNAGAR2:
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_write(regmap, LOCHNAGAR2_ANALOGUE_PATH_CTRL1, 0);
	if (ret < 0)
		return ret;

	ret = regmap_write(regmap, LOCHNAGAR2_ANALOGUE_PATH_CTRL1,
			   LOCHNAGAR2_ANALOGUE_PATH_UPDATE_MASK);
	if (ret < 0)
		return ret;

	ret = regmap_read_poll_timeout(regmap,
				       LOCHNAGAR2_ANALOGUE_PATH_CTRL1, val,
				       (val & done), LOCHNAGAR_CONFIG_POLL_US,
				       timeout_ms * 1000);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(lochnagar_update_config);

static int lochnagar_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	const struct lochnagar_config *config = NULL;
	const struct of_device_id *of_id;
	struct lochnagar *lochnagar;
	unsigned int val;
	struct gpio_desc *reset, *present;
	unsigned int firmwareid;
	int devid, rev;
	int ret;

	lochnagar = devm_kzalloc(dev, sizeof(*lochnagar), GFP_KERNEL);
	if (!lochnagar)
		return -ENOMEM;

	of_id = of_match_device(lochnagar_of_match, dev);
	if (!of_id)
		return -EINVAL;

	config = of_id->data;

	lochnagar->dev = dev;
	mutex_init(&lochnagar->analogue_config_lock);

	dev_set_drvdata(dev, lochnagar);

	reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset)) {
		ret = PTR_ERR(reset);
		dev_err(dev, "Failed to get reset GPIO: %d\n", ret);
		return ret;
	}

	present = devm_gpiod_get_optional(dev, "present", GPIOD_OUT_HIGH);
	if (IS_ERR(present)) {
		ret = PTR_ERR(present);
		dev_err(dev, "Failed to get present GPIO: %d\n", ret);
		return ret;
	}

	msleep(20);

	/* Bring Lochnagar out of reset */
	gpiod_set_value_cansleep(reset, 1);

	/* Identify Lochnagar */
	lochnagar->type = config->type;
	lochnagar->regmap = devm_regmap_init_i2c(i2c, config->regmap);
	if (IS_ERR(lochnagar->regmap)) {
		ret = PTR_ERR(lochnagar->regmap);
		dev_err(dev, "Failed to allocate register map: %d\n", ret);
		return ret;
	}

	/* Wait for Lochnagar to boot */
	ret = lochnagar_wait_for_boot(lochnagar->regmap, &val);
	if (ret < 0) {
		dev_err(dev, "Failed to read device ID: %d\n", ret);
		return ret;
	}

	devid = val & LOCHNAGAR_DEVICE_ID_MASK;
	rev = val & LOCHNAGAR_REV_ID_MASK;

	if (devid != config->id) {
		dev_err(dev,
			"ID does not match %s (expected 0x%x got 0x%x)\n",
			config->name, config->id, devid);
		return -ENODEV;
	}

	/* Identify firmware */
	ret = regmap_read(lochnagar->regmap, LOCHNAGAR_FIRMWARE_ID1, &val);
	if (ret < 0) {
		dev_err(dev, "Failed to read firmware id 1: %d\n", ret);
		return ret;
	}

	firmwareid = val;

	ret = regmap_read(lochnagar->regmap, LOCHNAGAR_FIRMWARE_ID2, &val);
	if (ret < 0) {
		dev_err(dev, "Failed to read firmware id 2: %d\n", ret);
		return ret;
	}

	firmwareid |= (val << config->regmap->val_bits);

	dev_info(dev, "Found %s (0x%x) revision %d firmware 0x%.6x\n",
		 config->name, devid, rev + 1, firmwareid);

	ret = regmap_register_patch(lochnagar->regmap, config->patch,
				    config->npatch);
	if (ret < 0) {
		dev_err(dev, "Failed to register patch: %d\n", ret);
		return ret;
	}

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_NONE, config->devs,
				   config->ndevs, NULL, 0, NULL);
	if (ret != 0) {
		dev_err(dev, "Failed to add subdevices: %d\n", ret);
		return ret;
	}

	ret = devm_of_platform_populate(dev);
	if (ret < 0) {
		dev_err(dev, "Failed to populate child nodes: %d\n", ret);
		return ret;
	}

	return ret;
}

static struct i2c_driver lochnagar_i2c_driver = {
	.driver = {
		.name = "lochnagar",
		.of_match_table = of_match_ptr(lochnagar_of_match),
		.suppress_bind_attrs = true,
	},

	.probe_new = lochnagar_i2c_probe,
};

static int __init lochnagar_i2c_init(void)
{
	int ret;

	ret = i2c_add_driver(&lochnagar_i2c_driver);
	if (ret != 0)
		pr_err("Failed to register Lochnagar driver: %d\n", ret);

	return ret;
}
subsys_initcall(lochnagar_i2c_init);
