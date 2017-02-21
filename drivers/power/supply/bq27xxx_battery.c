/*
 * BQ27xxx battery driver
 *
 * Copyright (C) 2008 Rodolfo Giometti <giometti@linux.it>
 * Copyright (C) 2008 Eurotech S.p.A. <info@eurotech.it>
 * Copyright (C) 2010-2011 Lars-Peter Clausen <lars@metafoo.de>
 * Copyright (C) 2011 Pali Rohár <pali.rohar@gmail.com>
 *
 * Based on a previous work by Copyright (C) 2008 Texas Instruments, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Datasheets:
 * http://www.ti.com/product/bq27000
 * http://www.ti.com/product/bq27200
 * http://www.ti.com/product/bq27010
 * http://www.ti.com/product/bq27210
 * http://www.ti.com/product/bq27500
 * http://www.ti.com/product/bq27510-g1
 * http://www.ti.com/product/bq27510-g2
 * http://www.ti.com/product/bq27510-g3
 * http://www.ti.com/product/bq27520-g4
 * http://www.ti.com/product/bq27520-g1
 * http://www.ti.com/product/bq27520-g2
 * http://www.ti.com/product/bq27520-g3
 * http://www.ti.com/product/bq27520-g4
 * http://www.ti.com/product/bq27530-g1
 * http://www.ti.com/product/bq27531-g1
 * http://www.ti.com/product/bq27541-g1
 * http://www.ti.com/product/bq27542-g1
 * http://www.ti.com/product/bq27546-g1
 * http://www.ti.com/product/bq27742-g1
 * http://www.ti.com/product/bq27545-g1
 * http://www.ti.com/product/bq27421-g1
 * http://www.ti.com/product/bq27425-g1
 * http://www.ti.com/product/bq27411-g1
 * http://www.ti.com/product/bq27621-g1
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/of.h>

#include <linux/power/bq27xxx_battery.h>

#define DRIVER_VERSION		"1.3.0"

#define BQ27XXX_MANUFACTURER	"Texas Instruments"

/* BQ27XXX Flags */
#define BQ27XXX_FLAG_DSC	BIT(0)
#define BQ27XXX_FLAG_SOCF	BIT(1) /* State-of-Charge threshold final */
#define BQ27XXX_FLAG_SOC1	BIT(2) /* State-of-Charge threshold 1 */
#define BQ27XXX_FLAG_FC		BIT(9)
#define BQ27XXX_FLAG_OTD	BIT(14)
#define BQ27XXX_FLAG_OTC	BIT(15)
#define BQ27XXX_FLAG_UT		BIT(14)
#define BQ27XXX_FLAG_OT		BIT(15)

/* BQ27000 has different layout for Flags register */
#define BQ27000_FLAG_EDVF	BIT(0) /* Final End-of-Discharge-Voltage flag */
#define BQ27000_FLAG_EDV1	BIT(1) /* First End-of-Discharge-Voltage flag */
#define BQ27000_FLAG_CI		BIT(4) /* Capacity Inaccurate flag */
#define BQ27000_FLAG_FC		BIT(5)
#define BQ27000_FLAG_CHGS	BIT(7) /* Charge state flag */

#define BQ27XXX_RS			(20) /* Resistor sense mOhm */
#define BQ27XXX_POWER_CONSTANT		(29200) /* 29.2 µV^2 * 1000 */
#define BQ27XXX_CURRENT_CONSTANT	(3570) /* 3.57 µV * 1000 */

#define INVALID_REG_ADDR	0xff

/*
 * bq27xxx_reg_index - Register names
 *
 * These are indexes into a device's register mapping array.
 */

enum bq27xxx_reg_index {
	BQ27XXX_REG_CTRL = 0,	/* Control */
	BQ27XXX_REG_TEMP,	/* Temperature */
	BQ27XXX_REG_INT_TEMP,	/* Internal Temperature */
	BQ27XXX_REG_VOLT,	/* Voltage */
	BQ27XXX_REG_AI,		/* Average Current */
	BQ27XXX_REG_FLAGS,	/* Flags */
	BQ27XXX_REG_TTE,	/* Time-to-Empty */
	BQ27XXX_REG_TTF,	/* Time-to-Full */
	BQ27XXX_REG_TTES,	/* Time-to-Empty Standby */
	BQ27XXX_REG_TTECP,	/* Time-to-Empty at Constant Power */
	BQ27XXX_REG_NAC,	/* Nominal Available Capacity */
	BQ27XXX_REG_FCC,	/* Full Charge Capacity */
	BQ27XXX_REG_CYCT,	/* Cycle Count */
	BQ27XXX_REG_AE,		/* Available Energy */
	BQ27XXX_REG_SOC,	/* State-of-Charge */
	BQ27XXX_REG_DCAP,	/* Design Capacity */
	BQ27XXX_REG_AP,		/* Average Power */
	BQ27XXX_REG_MAX,	/* sentinel */
};

/* Register mappings */
static u8 bq27xxx_regs[][BQ27XXX_REG_MAX] = {
	[BQ27000] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = 0x18,
		[BQ27XXX_REG_TTES] = 0x1c,
		[BQ27XXX_REG_TTECP] = 0x26,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = 0x22,
		[BQ27XXX_REG_SOC] = 0x0b,
		[BQ27XXX_REG_DCAP] = 0x76,
		[BQ27XXX_REG_AP] = 0x24,
	},
	[BQ27010] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = 0x18,
		[BQ27XXX_REG_TTES] = 0x1c,
		[BQ27XXX_REG_TTECP] = 0x26,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_SOC] = 0x0b,
		[BQ27XXX_REG_DCAP] = 0x76,
		[BQ27XXX_REG_AP] = INVALID_REG_ADDR,
	},
	[BQ2750X] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = 0x28,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTES] = 0x1a,
		[BQ27XXX_REG_TTECP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_SOC] = 0x2c,
		[BQ27XXX_REG_DCAP] = 0x3c,
		[BQ27XXX_REG_AP] = INVALID_REG_ADDR,
	},
	[BQ2751X] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = 0x28,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTES] = 0x1a,
		[BQ27XXX_REG_TTECP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x1e,
		[BQ27XXX_REG_AE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_SOC] = 0x20,
		[BQ27XXX_REG_DCAP] = 0x2e,
		[BQ27XXX_REG_AP] = INVALID_REG_ADDR,
	},
	[BQ27500] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = 0x18,
		[BQ27XXX_REG_TTES] = 0x1c,
		[BQ27XXX_REG_TTECP] = 0x26,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = 0x22,
		[BQ27XXX_REG_SOC] = 0x2c,
		[BQ27XXX_REG_DCAP] = 0x3c,
		[BQ27XXX_REG_AP] = 0x24,
	},
	[BQ27510G1] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = 0x18,
		[BQ27XXX_REG_TTES] = 0x1c,
		[BQ27XXX_REG_TTECP] = 0x26,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = 0x22,
		[BQ27XXX_REG_SOC] = 0x2c,
		[BQ27XXX_REG_DCAP] = 0x3c,
		[BQ27XXX_REG_AP] = 0x24,
	},
	[BQ27510G2] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = 0x18,
		[BQ27XXX_REG_TTES] = 0x1c,
		[BQ27XXX_REG_TTECP] = 0x26,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = 0x22,
		[BQ27XXX_REG_SOC] = 0x2c,
		[BQ27XXX_REG_DCAP] = 0x3c,
		[BQ27XXX_REG_AP] = 0x24,
	},
	[BQ27510G3] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = 0x28,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTES] = 0x1a,
		[BQ27XXX_REG_TTECP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x1e,
		[BQ27XXX_REG_AE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_SOC] = 0x20,
		[BQ27XXX_REG_DCAP] = 0x2e,
		[BQ27XXX_REG_AP] = INVALID_REG_ADDR,
	},
	[BQ27520G1] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = 0x18,
		[BQ27XXX_REG_TTES] = 0x1c,
		[BQ27XXX_REG_TTECP] = 0x26,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = INVALID_REG_ADDR,
		[BQ27XXX_REG_AE] = 0x22,
		[BQ27XXX_REG_SOC] = 0x2c,
		[BQ27XXX_REG_DCAP] = 0x3c,
		[BQ27XXX_REG_AP] = 0x24,
	},
	[BQ27520G2] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = 0x36,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = 0x18,
		[BQ27XXX_REG_TTES] = 0x1c,
		[BQ27XXX_REG_TTECP] = 0x26,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = 0x22,
		[BQ27XXX_REG_SOC] = 0x2c,
		[BQ27XXX_REG_DCAP] = 0x3c,
		[BQ27XXX_REG_AP] = 0x24,
	},
	[BQ27520G3] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = 0x36,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTES] = 0x1c,
		[BQ27XXX_REG_TTECP] = 0x26,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = 0x22,
		[BQ27XXX_REG_SOC] = 0x2c,
		[BQ27XXX_REG_DCAP] = 0x3c,
		[BQ27XXX_REG_AP] = 0x24,
	},
	[BQ27520G4] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = 0x28,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTES] = 0x1c,
		[BQ27XXX_REG_TTECP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x1e,
		[BQ27XXX_REG_AE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_SOC] = 0x20,
		[BQ27XXX_REG_DCAP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_AP] = INVALID_REG_ADDR,
	},
	[BQ27530] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = 0x32,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTES] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTECP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_SOC] = 0x2c,
		[BQ27XXX_REG_DCAP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_AP] = 0x24,
	},
	[BQ27541] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = 0x28,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTES] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTECP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_SOC] = 0x2c,
		[BQ27XXX_REG_DCAP] = 0x3c,
		[BQ27XXX_REG_AP] = 0x24,
	},
	[BQ27545] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x06,
		[BQ27XXX_REG_INT_TEMP] = 0x28,
		[BQ27XXX_REG_VOLT] = 0x08,
		[BQ27XXX_REG_AI] = 0x14,
		[BQ27XXX_REG_FLAGS] = 0x0a,
		[BQ27XXX_REG_TTE] = 0x16,
		[BQ27XXX_REG_TTF] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTES] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTECP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_NAC] = 0x0c,
		[BQ27XXX_REG_FCC] = 0x12,
		[BQ27XXX_REG_CYCT] = 0x2a,
		[BQ27XXX_REG_AE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_SOC] = 0x2c,
		[BQ27XXX_REG_DCAP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_AP] = 0x24,
	},
	[BQ27421] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x02,
		[BQ27XXX_REG_INT_TEMP] = 0x1e,
		[BQ27XXX_REG_VOLT] = 0x04,
		[BQ27XXX_REG_AI] = 0x10,
		[BQ27XXX_REG_FLAGS] = 0x06,
		[BQ27XXX_REG_TTE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTF] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTES] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTECP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_NAC] = 0x08,
		[BQ27XXX_REG_FCC] = 0x0e,
		[BQ27XXX_REG_CYCT] = INVALID_REG_ADDR,
		[BQ27XXX_REG_AE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_SOC] = 0x1c,
		[BQ27XXX_REG_DCAP] = 0x3c,
		[BQ27XXX_REG_AP] = 0x18,
	},
	[BQ27425] = {
		[BQ27XXX_REG_CTRL] = 0x00,
		[BQ27XXX_REG_TEMP] = 0x02,
		[BQ27XXX_REG_INT_TEMP] = 0x1e,
		[BQ27XXX_REG_VOLT] = 0x04,
		[BQ27XXX_REG_AI] = 0x10,
		[BQ27XXX_REG_FLAGS] = 0x06,
		[BQ27XXX_REG_TTE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTF] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTES] = INVALID_REG_ADDR,
		[BQ27XXX_REG_TTECP] = INVALID_REG_ADDR,
		[BQ27XXX_REG_NAC] = 0x08,
		[BQ27XXX_REG_FCC] = 0x0e,
		[BQ27XXX_REG_CYCT] = INVALID_REG_ADDR,
		[BQ27XXX_REG_AE] = INVALID_REG_ADDR,
		[BQ27XXX_REG_SOC] = 0x1c,
		[BQ27XXX_REG_DCAP] = 0x3c,
		[BQ27XXX_REG_AP] = 0x18,
	},
};

static enum power_supply_property bq27000_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27010_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq2750x_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq2751x_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27500_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27510g1_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27510g2_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27510g3_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27520g1_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27520g2_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27520g3_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27520g4_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27530_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_POWER_AVG,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27541_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_POWER_AVG,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27545_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_POWER_AVG,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static enum power_supply_property bq27421_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

#define BQ27XXX_PROP(_id, _prop)		\
	[_id] = {				\
		.props = _prop,			\
		.size = ARRAY_SIZE(_prop),	\
	}

static struct {
	enum power_supply_property *props;
	size_t size;
} bq27xxx_battery_props[] = {
	BQ27XXX_PROP(BQ27000, bq27000_battery_props),
	BQ27XXX_PROP(BQ27010, bq27010_battery_props),
	BQ27XXX_PROP(BQ2750X, bq2750x_battery_props),
	BQ27XXX_PROP(BQ2751X, bq2751x_battery_props),
	BQ27XXX_PROP(BQ27500, bq27500_battery_props),
	BQ27XXX_PROP(BQ27510G1, bq27510g1_battery_props),
	BQ27XXX_PROP(BQ27510G2, bq27510g2_battery_props),
	BQ27XXX_PROP(BQ27510G3, bq27510g3_battery_props),
	BQ27XXX_PROP(BQ27520G1, bq27520g1_battery_props),
	BQ27XXX_PROP(BQ27520G2, bq27520g2_battery_props),
	BQ27XXX_PROP(BQ27520G3, bq27520g3_battery_props),
	BQ27XXX_PROP(BQ27520G4, bq27520g4_battery_props),
	BQ27XXX_PROP(BQ27530, bq27530_battery_props),
	BQ27XXX_PROP(BQ27541, bq27541_battery_props),
	BQ27XXX_PROP(BQ27545, bq27545_battery_props),
	BQ27XXX_PROP(BQ27421, bq27421_battery_props),
	BQ27XXX_PROP(BQ27425, bq27421_battery_props),
};

static DEFINE_MUTEX(bq27xxx_list_lock);
static LIST_HEAD(bq27xxx_battery_devices);

/* writable registers */
#define BQ27XXX_CONTROL			0x00
#define BQ27XXX_DATA_CLASS		0x3E
#define BQ27XXX_DATA_BLOCK		0x3F
#define BQ27XXX_BLOCK_DATA		0x40
#define BQ27XXX_BLOCK_DATA_CHECKSUM	0x60
#define BQ27XXX_BLOCK_DATA_CONTROL	0x61

/* control register params */
#define BQ27XXX_SEALED			0x20
#define BQ27XXX_SET_CFGUPDATE		0x13
#define BQ27XXX_SOFT_RESET		0x42

#define BQ27XXX_MSLEEP(i) usleep_range((i)*1000, (i)*1000+500)

struct bq27xxx_dm_reg {
	u8 subclass_id;
	u8 offset;
	u8 bytes;
	u16 min, max;
};

struct bq27xxx_dm_buf {
	u8 class;
	u8 block;
	u8 a[32];
	bool full, updt;
};

#define BQ27XXX_DM_BUF_PTR(b, r) \
	( (u16*) &(b).a[ (r)->offset % sizeof (b).a ] )

#define BQ27XXX_DM_BUF(di, i) { \
	.class = bq27xxx_dm_regs[(di)->chip][i].subclass_id, \
	.block = bq27xxx_dm_regs[(di)->chip][i].offset / sizeof ((struct bq27xxx_dm_buf*)0)->a, \
}

static inline bool bq27xxx_dm_buf_set(struct bq27xxx_dm_buf *buf,
				      struct bq27xxx_dm_reg *reg) {
	if (buf->class == reg->subclass_id
	 && buf->block == reg->offset / sizeof buf->a
	 && buf->full)
		return false;
	buf->class = reg->subclass_id;
	buf->block = reg->offset / sizeof buf->a;
	buf->full = buf->updt = false;
	return true;
}

enum bq27xxx_dm_reg_id {
	BQ27XXX_DM_DESIGN_CAPACITY = 0,
	BQ27XXX_DM_DESIGN_ENERGY,
	BQ27XXX_DM_TERMINATE_VOLTAGE,
	BQ27XXX_DM_END,
};

static const char* bq27xxx_dm_reg_name[] = {
	[BQ27XXX_DM_DESIGN_CAPACITY] = "design-capacity",
	[BQ27XXX_DM_DESIGN_ENERGY] = "design-energy",
	[BQ27XXX_DM_TERMINATE_VOLTAGE] = "terminate-voltage",
};

static struct bq27xxx_dm_reg bq27425_dm_regs[] = {
	[BQ27XXX_DM_DESIGN_CAPACITY]   = { 82, 12, 2,    0, 32767 },
	[BQ27XXX_DM_DESIGN_ENERGY]     = { 82, 14, 2,    0, 32767 },
	[BQ27XXX_DM_TERMINATE_VOLTAGE] = { 82, 18, 2, 2800,  3700 },
};

static struct bq27xxx_dm_reg bq27421_dm_regs[] = { /* not tested */
	[BQ27XXX_DM_DESIGN_CAPACITY]   = { 82, 10, 2,    0,  8000 },
	[BQ27XXX_DM_DESIGN_ENERGY]     = { 82, 12, 2,    0, 32767 },
	[BQ27XXX_DM_TERMINATE_VOLTAGE] = { 82, 16, 2, 2500,  3700 },
};

static struct bq27xxx_dm_reg bq27621_dm_regs[] = { /* not tested */
	[BQ27XXX_DM_DESIGN_CAPACITY]   = { 82, 3, 2,    0,  8000 },
	[BQ27XXX_DM_DESIGN_ENERGY]     = { 82, 5, 2,    0, 32767 },
	[BQ27XXX_DM_TERMINATE_VOLTAGE] = { 82, 9, 2, 2500,  3700 },
};

static struct bq27xxx_dm_reg *bq27xxx_dm_regs[] = {
	[BQ27421] = bq27421_dm_regs, /* and BQ27441 */
	[BQ27425] = bq27425_dm_regs,
/*	[BQ27621] = */ bq27621_dm_regs,
};

static u32 bq27xxx_unseal_keys[] = {
	[BQ27421] = 0x80008000, /* and BQ27441 */
	[BQ27425] = 0x04143672,
/*	[BQ27621] = */ 0x80008000,
};


static int poll_interval_param_set(const char *val, const struct kernel_param *kp)
{
	struct bq27xxx_device_info *di;
	unsigned int prev_val = *(unsigned int *) kp->arg;
	int ret;

	ret = param_set_uint(val, kp);
	if (ret < 0 || prev_val == *(unsigned int *) kp->arg)
		return ret;

	mutex_lock(&bq27xxx_list_lock);
	list_for_each_entry(di, &bq27xxx_battery_devices, list) {
		cancel_delayed_work_sync(&di->work);
		schedule_delayed_work(&di->work, 0);
	}
	mutex_unlock(&bq27xxx_list_lock);

	return ret;
}

static const struct kernel_param_ops param_ops_poll_interval = {
	.get = param_get_uint,
	.set = poll_interval_param_set,
};

static unsigned int poll_interval = 360;
module_param_cb(poll_interval, &param_ops_poll_interval, &poll_interval, 0644);
MODULE_PARM_DESC(poll_interval,
		 "battery poll interval in seconds - 0 disables polling");

/*
 * Common code for BQ27xxx devices
 */

static inline int bq27xxx_read(struct bq27xxx_device_info *di, int reg_index,
			       bool single)
{
	/* Reports EINVAL for invalid/missing registers */
	if (!di || di->regs[reg_index] == INVALID_REG_ADDR)
		return -EINVAL;

	return di->bus.read(di, di->regs[reg_index], single);
}

static int bq27xxx_battery_set_seal_state(struct bq27xxx_device_info *di,
					  bool state)
{
	u32 key = bq27xxx_unseal_keys[di->chip];
	int ret;

	if (state) {
		ret = di->bus.write(di, BQ27XXX_CONTROL, BQ27XXX_SEALED, false);
		if (ret < 0)
			goto out;
	} else {
		ret = di->bus.write(di, BQ27XXX_CONTROL, (u16)(key >> 16), false);
		if (ret < 0)
			goto out;

		ret = di->bus.write(di, BQ27XXX_CONTROL, (u16)key, false);
		if (ret < 0)
			goto out;
	}
	return 0;

out:
	dev_err(di->dev, "bus error in %s: %d\n", __func__, ret);
	return ret;
}

static u8 bq27xxx_battery_checksum(struct bq27xxx_dm_buf *buf)
{
	u16 sum = 0;
	int i;

	for (i = 0; i < sizeof buf->a; i++)
		sum += buf->a[i];
	sum &= 0xff;

	return 0xff - sum;
}

static int bq27xxx_battery_read_dm_block(struct bq27xxx_device_info *di,
					 struct bq27xxx_dm_buf *buf)
{
	int ret;

	ret = di->bus.write(di, BQ27XXX_DATA_CLASS, buf->class, true);
	if (ret < 0)
		goto out;

	ret = di->bus.write(di, BQ27XXX_DATA_BLOCK, buf->block, true);
	if (ret < 0)
		goto out;

	BQ27XXX_MSLEEP(1);

	ret = di->bus.read_bulk(di, BQ27XXX_BLOCK_DATA, buf->a, sizeof buf->a);
	if (ret < 0)
		goto out;

	ret = di->bus.read(di, BQ27XXX_BLOCK_DATA_CHECKSUM, true);
	if (ret < 0)
		goto out;

	if ((u8)ret != bq27xxx_battery_checksum(buf)) {
		ret = -EINVAL;
		goto out;
	}

	buf->full = true;
	buf->updt = false;
	return 0;

out:
	dev_err(di->dev, "bus error in %s: %d\n", __func__, ret);
	return ret;
}

static void bq27xxx_battery_print_config(struct bq27xxx_device_info *di)
{
	struct bq27xxx_dm_reg *reg = bq27xxx_dm_regs[di->chip];
	struct bq27xxx_dm_buf buf = { .class = 0xFF };
	int i;

	for (i = 0; i < BQ27XXX_DM_END; i++, reg++) {
		const char* str = bq27xxx_dm_reg_name[i];

		if (bq27xxx_dm_buf_set(&buf, reg))
			if (bq27xxx_battery_read_dm_block(di, &buf) < 0)
				continue;

		if (reg->bytes == 2)
			dev_info(di->dev, "config register %s is %d\n", str,
				 be16_to_cpup(BQ27XXX_DM_BUF_PTR(buf, reg)));
		else
			dev_warn(di->dev, "unsupported config register %s\n", str);
	}
}

static void bq27xxx_battery_update_dm_block(struct bq27xxx_device_info *di,
					    struct bq27xxx_dm_buf *buf,
					    enum bq27xxx_dm_reg_id reg_id,
					    unsigned int val)
{
	struct bq27xxx_dm_reg *reg = &bq27xxx_dm_regs[di->chip][reg_id];
	u16 *prev;

	if (reg->bytes != 2)
		return;

	prev = BQ27XXX_DM_BUF_PTR(*buf, reg);
	if (be16_to_cpup(prev) == val)
		return;
	*prev = cpu_to_be16(val);

	dev_info(di->dev, "update chip %d, class %u, block %u, offset %u, value %u\n",
		 di->chip, buf->class, buf->block, reg->offset, val);

	buf->updt = true;
	return;
}

static int bq27xxx_battery_write_dm_block(struct bq27xxx_device_info *di,
					  struct bq27xxx_dm_buf *buf)
{
	bool cfgup = di->chip == BQ27425 || di->chip == BQ27421; /* || BQ27441 || BQ27621 */
	int ret;

	if (cfgup) {
		ret = di->bus.write(di, BQ27XXX_CONTROL, BQ27XXX_SET_CFGUPDATE, false);
		if (ret < 0)
			goto out1;
	}

	ret = di->bus.write(di, BQ27XXX_BLOCK_DATA_CONTROL, 0, true);
	if (ret < 0)
		goto out2;

	ret = di->bus.write(di, BQ27XXX_DATA_CLASS, buf->class, true);
	if (ret < 0)
		goto out2;

	ret = di->bus.write(di, BQ27XXX_DATA_BLOCK, buf->block, true);
	if (ret < 0)
		goto out2;

	BQ27XXX_MSLEEP(1);

	ret = di->bus.write_bulk(di, BQ27XXX_BLOCK_DATA, buf->a, sizeof buf->a);
	if (ret < 0)
		goto out2;

	ret = di->bus.write(di, BQ27XXX_BLOCK_DATA_CHECKSUM,
			    bq27xxx_battery_checksum(buf), true);
	if (ret < 0)
		goto out2;

	/* THE FOLLOWING CODE IS TOXIC. DO NOT USE!
	 * If the 350ms delay is insufficient, NVM corruption results
	 * on the '425 chip, which could damage the chip.
	 * It was suggested in this TI tool:
	 *   http://git.ti.com/bms-linux/bqtool/blobs/master/gauge.c#line328
	 *
	 * BQ27XXX_MSLEEP(350);
	 * ret = di->bus.write(di, BQ27XXX_DATA_BLOCK, buf->block, true);
	 * BQ27XXX_MSLEEP(1);
	 * ret = di->bus.read(di, BQ27XXX_BLOCK_DATA_CHECKSUM, true);
	 * if ((u8)ret != bq27xxx_battery_checksum(buf))
	 * 	ret = -EINVAL;
	 */

	if (cfgup) {
		BQ27XXX_MSLEEP(1);
		ret = di->bus.write(di, BQ27XXX_CONTROL, BQ27XXX_SOFT_RESET, false);
		if (ret < 0)
			goto out1;
	}

	buf->updt = false;
	return 0;

out2:
	if (cfgup)
		di->bus.write(di, BQ27XXX_CONTROL, BQ27XXX_SOFT_RESET, false);
out1:
	dev_err(di->dev, "bus error in %s: %d\n", __func__, ret);
	return ret;
}

static void bq27xxx_battery_set_config(struct bq27xxx_device_info *di,
				       struct power_supply_battery_info *info)
{
	struct bq27xxx_dm_buf bd = BQ27XXX_DM_BUF(di, BQ27XXX_DM_DESIGN_ENERGY);
	struct bq27xxx_dm_buf bt = BQ27XXX_DM_BUF(di, BQ27XXX_DM_TERMINATE_VOLTAGE);

	if (info->charge_full_design_uah != -EINVAL
	 && info->energy_full_design_uwh != -EINVAL) {
		bq27xxx_battery_read_dm_block(di, &bd);
		if (bd.full) {
			/* assume design energy & capacity are in same block */
			bq27xxx_battery_update_dm_block(di, &bd,
					BQ27XXX_DM_DESIGN_CAPACITY,
					info->charge_full_design_uah / 1000);
			bq27xxx_battery_update_dm_block(di, &bd,
					BQ27XXX_DM_DESIGN_ENERGY,
					info->energy_full_design_uwh / 1000);
		}
	}

	if (info->voltage_min_design_uv != -EINVAL) {
		bool same = bd.full && bd.class == bt.class && bd.block == bt.block;
		if (!same)
			bq27xxx_battery_read_dm_block(di, &bt);
		if (same ? bd.full : bt.full)
			bq27xxx_battery_update_dm_block(di, same ? &bd : &bt,
					BQ27XXX_DM_TERMINATE_VOLTAGE,
					info->voltage_min_design_uv / 1000);
	}

	if (bd.updt)
		bq27xxx_battery_write_dm_block(di, &bd);
	if (bt.updt)
		bq27xxx_battery_write_dm_block(di, &bt);
}

void bq27xxx_battery_settings(struct bq27xxx_device_info *di)
{
	struct power_supply_battery_info info = {};
	unsigned int min, max;

	/* functions don't exist for writing data so abort */
	if (!di->bus.write || !di->bus.write_bulk)
		return;

	/* no settings to be set for this chipset so abort */
	if (!bq27xxx_dm_regs[di->chip])
		return;

	if (bq27xxx_battery_set_seal_state(di, false) < 0)
		return;

	if (power_supply_get_battery_info(di->bat, &info) < 0)
		goto out;

	if (info.energy_full_design_uwh != info.charge_full_design_uah) {
		if (info.energy_full_design_uwh == -EINVAL)
			dev_warn(di->dev,
				"missing battery:energy-full-design-microwatt-hours\n");
		else if (info.charge_full_design_uah == -EINVAL)
			dev_warn(di->dev,
				"missing battery:charge-full-design-microamp-hours\n");
	}

	/* assume min == 0 */
	max = bq27xxx_dm_regs[di->chip][BQ27XXX_DM_DESIGN_ENERGY].max;
	if (info.energy_full_design_uwh > max * 1000) {
		dev_err(di->dev,
		       "invalid battery:energy-full-design-microwatt-hours %d\n",
			info.energy_full_design_uwh);
		info.energy_full_design_uwh = -EINVAL;
	}

	/* assume min == 0 */
	max = bq27xxx_dm_regs[di->chip][BQ27XXX_DM_DESIGN_CAPACITY].max;
	if (info.charge_full_design_uah > max * 1000) {
		dev_err(di->dev,
		       "invalid battery:charge-full-design-microamp-hours %d\n",
			info.charge_full_design_uah);
		info.charge_full_design_uah = -EINVAL;
	}

	min = bq27xxx_dm_regs[di->chip][BQ27XXX_DM_TERMINATE_VOLTAGE].min;
	max = bq27xxx_dm_regs[di->chip][BQ27XXX_DM_TERMINATE_VOLTAGE].max;
	if ((info.voltage_min_design_uv < min * 1000
	  || info.voltage_min_design_uv > max * 1000)
	  && info.voltage_min_design_uv != -EINVAL) {
		dev_err(di->dev,
		       "invalid battery:voltage-min-design-microvolt %d\n",
			info.voltage_min_design_uv);
		info.voltage_min_design_uv = -EINVAL;
	}

	if ((info.energy_full_design_uwh == -EINVAL
	  || info.charge_full_design_uah == -EINVAL)
	  && info.voltage_min_design_uv  == -EINVAL)
		goto out;

	bq27xxx_battery_set_config(di, &info);

out:
	bq27xxx_battery_print_config(di);
	bq27xxx_battery_set_seal_state(di, true);
}

/*
 * Return the battery State-of-Charge
 * Or < 0 if something fails.
 */
static int bq27xxx_battery_read_soc(struct bq27xxx_device_info *di)
{
	int soc;

	if (di->chip == BQ27000 || di->chip == BQ27010)
		soc = bq27xxx_read(di, BQ27XXX_REG_SOC, true);
	else
		soc = bq27xxx_read(di, BQ27XXX_REG_SOC, false);

	if (soc < 0)
		dev_dbg(di->dev, "error reading State-of-Charge\n");

	return soc;
}

/*
 * Return a battery charge value in µAh
 * Or < 0 if something fails.
 */
static int bq27xxx_battery_read_charge(struct bq27xxx_device_info *di, u8 reg)
{
	int charge;

	charge = bq27xxx_read(di, reg, false);
	if (charge < 0) {
		dev_dbg(di->dev, "error reading charge register %02x: %d\n",
			reg, charge);
		return charge;
	}

	if (di->chip == BQ27000 || di->chip == BQ27010)
		charge *= BQ27XXX_CURRENT_CONSTANT / BQ27XXX_RS;
	else
		charge *= 1000;

	return charge;
}

/*
 * Return the battery Nominal available capacity in µAh
 * Or < 0 if something fails.
 */
static inline int bq27xxx_battery_read_nac(struct bq27xxx_device_info *di)
{
	int flags;

	if (di->chip == BQ27000 || di->chip == BQ27010) {
		flags = bq27xxx_read(di, BQ27XXX_REG_FLAGS, true);
		if (flags >= 0 && (flags & BQ27000_FLAG_CI))
			return -ENODATA;
	}

	return bq27xxx_battery_read_charge(di, BQ27XXX_REG_NAC);
}

/*
 * Return the battery Full Charge Capacity in µAh
 * Or < 0 if something fails.
 */
static inline int bq27xxx_battery_read_fcc(struct bq27xxx_device_info *di)
{
	return bq27xxx_battery_read_charge(di, BQ27XXX_REG_FCC);
}

/*
 * Return the Design Capacity in µAh
 * Or < 0 if something fails.
 */
static int bq27xxx_battery_read_dcap(struct bq27xxx_device_info *di)
{
	int dcap;

	if (di->chip == BQ27000 || di->chip == BQ27010)
		dcap = bq27xxx_read(di, BQ27XXX_REG_DCAP, true);
	else
		dcap = bq27xxx_read(di, BQ27XXX_REG_DCAP, false);

	if (dcap < 0) {
		dev_dbg(di->dev, "error reading initial last measured discharge\n");
		return dcap;
	}

	if (di->chip == BQ27000 || di->chip == BQ27010)
		dcap = (dcap << 8) * BQ27XXX_CURRENT_CONSTANT / BQ27XXX_RS;
	else
		dcap *= 1000;

	return dcap;
}

/*
 * Return the battery Available energy in µWh
 * Or < 0 if something fails.
 */
static int bq27xxx_battery_read_energy(struct bq27xxx_device_info *di)
{
	int ae;

	ae = bq27xxx_read(di, BQ27XXX_REG_AE, false);
	if (ae < 0) {
		dev_dbg(di->dev, "error reading available energy\n");
		return ae;
	}

	if (di->chip == BQ27000 || di->chip == BQ27010)
		ae *= BQ27XXX_POWER_CONSTANT / BQ27XXX_RS;
	else
		ae *= 1000;

	return ae;
}

/*
 * Return the battery temperature in tenths of degree Kelvin
 * Or < 0 if something fails.
 */
static int bq27xxx_battery_read_temperature(struct bq27xxx_device_info *di)
{
	int temp;

	temp = bq27xxx_read(di, BQ27XXX_REG_TEMP, false);
	if (temp < 0) {
		dev_err(di->dev, "error reading temperature\n");
		return temp;
	}

	if (di->chip == BQ27000 || di->chip == BQ27010)
		temp = 5 * temp / 2;

	return temp;
}

/*
 * Return the battery Cycle count total
 * Or < 0 if something fails.
 */
static int bq27xxx_battery_read_cyct(struct bq27xxx_device_info *di)
{
	int cyct;

	cyct = bq27xxx_read(di, BQ27XXX_REG_CYCT, false);
	if (cyct < 0)
		dev_err(di->dev, "error reading cycle count total\n");

	return cyct;
}

/*
 * Read a time register.
 * Return < 0 if something fails.
 */
static int bq27xxx_battery_read_time(struct bq27xxx_device_info *di, u8 reg)
{
	int tval;

	tval = bq27xxx_read(di, reg, false);
	if (tval < 0) {
		dev_dbg(di->dev, "error reading time register %02x: %d\n",
			reg, tval);
		return tval;
	}

	if (tval == 65535)
		return -ENODATA;

	return tval * 60;
}

/*
 * Read an average power register.
 * Return < 0 if something fails.
 */
static int bq27xxx_battery_read_pwr_avg(struct bq27xxx_device_info *di)
{
	int tval;

	tval = bq27xxx_read(di, BQ27XXX_REG_AP, false);
	if (tval < 0) {
		dev_err(di->dev, "error reading average power register  %02x: %d\n",
			BQ27XXX_REG_AP, tval);
		return tval;
	}

	if (di->chip == BQ27000 || di->chip == BQ27010)
		return (tval * BQ27XXX_POWER_CONSTANT) / BQ27XXX_RS;
	else
		return tval;
}

/*
 * Returns true if a battery over temperature condition is detected
 */
static bool bq27xxx_battery_overtemp(struct bq27xxx_device_info *di, u16 flags)
{
	switch (di->chip) {
	case BQ2750X:
	case BQ2751X:
	case BQ27500:
	case BQ27510G1:
	case BQ27510G2:
	case BQ27510G3:
	case BQ27520G1:
	case BQ27520G2:
	case BQ27520G3:
	case BQ27520G4:
	case BQ27541:
	case BQ27545:
		return flags & (BQ27XXX_FLAG_OTC | BQ27XXX_FLAG_OTD);
	case BQ27530:
	case BQ27421:
	case BQ27425:
		return flags & BQ27XXX_FLAG_OT;
	default:
		return false;
	}
}

/*
 * Returns true if a battery under temperature condition is detected
 */
static bool bq27xxx_battery_undertemp(struct bq27xxx_device_info *di, u16 flags)
{
	if (di->chip == BQ27530 || di->chip == BQ27421 || di->chip == BQ27425)
		return flags & BQ27XXX_FLAG_UT;

	return false;
}

/*
 * Returns true if a low state of charge condition is detected
 */
static bool bq27xxx_battery_dead(struct bq27xxx_device_info *di, u16 flags)
{
	if (di->chip == BQ27000 || di->chip == BQ27010)
		return flags & (BQ27000_FLAG_EDV1 | BQ27000_FLAG_EDVF);
	else
		return flags & (BQ27XXX_FLAG_SOC1 | BQ27XXX_FLAG_SOCF);
}

/*
 * Read flag register.
 * Return < 0 if something fails.
 */
static int bq27xxx_battery_read_health(struct bq27xxx_device_info *di)
{
	int flags;
	bool has_singe_flag = di->chip == BQ27000 || di->chip == BQ27010;

	flags = bq27xxx_read(di, BQ27XXX_REG_FLAGS, has_singe_flag);
	if (flags < 0) {
		dev_err(di->dev, "error reading flag register:%d\n", flags);
		return flags;
	}

	/* Unlikely but important to return first */
	if (unlikely(bq27xxx_battery_overtemp(di, flags)))
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	if (unlikely(bq27xxx_battery_undertemp(di, flags)))
		return POWER_SUPPLY_HEALTH_COLD;
	if (unlikely(bq27xxx_battery_dead(di, flags)))
		return POWER_SUPPLY_HEALTH_DEAD;

	return POWER_SUPPLY_HEALTH_GOOD;
}

void bq27xxx_battery_update(struct bq27xxx_device_info *di)
{
	struct bq27xxx_reg_cache cache = {0, };
	bool has_ci_flag = di->chip == BQ27000 || di->chip == BQ27010;
	bool has_singe_flag = di->chip == BQ27000 || di->chip == BQ27010;

	cache.flags = bq27xxx_read(di, BQ27XXX_REG_FLAGS, has_singe_flag);
	if ((cache.flags & 0xff) == 0xff)
		cache.flags = -1; /* read error */
	if (cache.flags >= 0) {
		cache.temperature = bq27xxx_battery_read_temperature(di);
		if (has_ci_flag && (cache.flags & BQ27000_FLAG_CI)) {
			dev_info_once(di->dev, "battery is not calibrated! ignoring capacity values\n");
			cache.capacity = -ENODATA;
			cache.energy = -ENODATA;
			cache.time_to_empty = -ENODATA;
			cache.time_to_empty_avg = -ENODATA;
			cache.time_to_full = -ENODATA;
			cache.charge_full = -ENODATA;
			cache.health = -ENODATA;
		} else {
			if (di->regs[BQ27XXX_REG_TTE] != INVALID_REG_ADDR)
				cache.time_to_empty = bq27xxx_battery_read_time(di, BQ27XXX_REG_TTE);
			if (di->regs[BQ27XXX_REG_TTECP] != INVALID_REG_ADDR)
				cache.time_to_empty_avg = bq27xxx_battery_read_time(di, BQ27XXX_REG_TTECP);
			if (di->regs[BQ27XXX_REG_TTF] != INVALID_REG_ADDR)
				cache.time_to_full = bq27xxx_battery_read_time(di, BQ27XXX_REG_TTF);
			cache.charge_full = bq27xxx_battery_read_fcc(di);
			cache.capacity = bq27xxx_battery_read_soc(di);
			if (di->regs[BQ27XXX_REG_AE] != INVALID_REG_ADDR)
				cache.energy = bq27xxx_battery_read_energy(di);
			cache.health = bq27xxx_battery_read_health(di);
		}
		if (di->regs[BQ27XXX_REG_CYCT] != INVALID_REG_ADDR)
			cache.cycle_count = bq27xxx_battery_read_cyct(di);
		if (di->regs[BQ27XXX_REG_AP] != INVALID_REG_ADDR)
			cache.power_avg = bq27xxx_battery_read_pwr_avg(di);

		/* We only have to read charge design full once */
		if (di->charge_design_full <= 0)
			di->charge_design_full = bq27xxx_battery_read_dcap(di);
	}

	if (di->cache.capacity != cache.capacity)
		power_supply_changed(di->bat);

	if (memcmp(&di->cache, &cache, sizeof(cache)) != 0)
		di->cache = cache;

	di->last_update = jiffies;
}
EXPORT_SYMBOL_GPL(bq27xxx_battery_update);

static void bq27xxx_battery_poll(struct work_struct *work)
{
	struct bq27xxx_device_info *di =
			container_of(work, struct bq27xxx_device_info,
				     work.work);

	bq27xxx_battery_update(di);

	if (poll_interval > 0)
		schedule_delayed_work(&di->work, poll_interval * HZ);
}

/*
 * Return the battery average current in µA
 * Note that current can be negative signed as well
 * Or 0 if something fails.
 */
static int bq27xxx_battery_current(struct bq27xxx_device_info *di,
				   union power_supply_propval *val)
{
	int curr;
	int flags;

	curr = bq27xxx_read(di, BQ27XXX_REG_AI, false);
	if (curr < 0) {
		dev_err(di->dev, "error reading current\n");
		return curr;
	}

	if (di->chip == BQ27000 || di->chip == BQ27010) {
		flags = bq27xxx_read(di, BQ27XXX_REG_FLAGS, true);
		if (flags & BQ27000_FLAG_CHGS) {
			dev_dbg(di->dev, "negative current!\n");
			curr = -curr;
		}

		val->intval = curr * BQ27XXX_CURRENT_CONSTANT / BQ27XXX_RS;
	} else {
		/* Other gauges return signed value */
		val->intval = (int)((s16)curr) * 1000;
	}

	return 0;
}

static int bq27xxx_battery_status(struct bq27xxx_device_info *di,
				  union power_supply_propval *val)
{
	int status;

	if (di->chip == BQ27000 || di->chip == BQ27010) {
		if (di->cache.flags & BQ27000_FLAG_FC)
			status = POWER_SUPPLY_STATUS_FULL;
		else if (di->cache.flags & BQ27000_FLAG_CHGS)
			status = POWER_SUPPLY_STATUS_CHARGING;
		else if (power_supply_am_i_supplied(di->bat))
			status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else
			status = POWER_SUPPLY_STATUS_DISCHARGING;
	} else {
		if (di->cache.flags & BQ27XXX_FLAG_FC)
			status = POWER_SUPPLY_STATUS_FULL;
		else if (di->cache.flags & BQ27XXX_FLAG_DSC)
			status = POWER_SUPPLY_STATUS_DISCHARGING;
		else
			status = POWER_SUPPLY_STATUS_CHARGING;
	}

	val->intval = status;

	return 0;
}

static int bq27xxx_battery_capacity_level(struct bq27xxx_device_info *di,
					  union power_supply_propval *val)
{
	int level;

	if (di->chip == BQ27000 || di->chip == BQ27010) {
		if (di->cache.flags & BQ27000_FLAG_FC)
			level = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		else if (di->cache.flags & BQ27000_FLAG_EDV1)
			level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else if (di->cache.flags & BQ27000_FLAG_EDVF)
			level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		else
			level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	} else {
		if (di->cache.flags & BQ27XXX_FLAG_FC)
			level = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		else if (di->cache.flags & BQ27XXX_FLAG_SOC1)
			level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else if (di->cache.flags & BQ27XXX_FLAG_SOCF)
			level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		else
			level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	}

	val->intval = level;

	return 0;
}

/*
 * Return the battery Voltage in millivolts
 * Or < 0 if something fails.
 */
static int bq27xxx_battery_voltage(struct bq27xxx_device_info *di,
				   union power_supply_propval *val)
{
	int volt;

	volt = bq27xxx_read(di, BQ27XXX_REG_VOLT, false);
	if (volt < 0) {
		dev_err(di->dev, "error reading voltage\n");
		return volt;
	}

	val->intval = volt * 1000;

	return 0;
}

static int bq27xxx_simple_value(int value,
				union power_supply_propval *val)
{
	if (value < 0)
		return value;

	val->intval = value;

	return 0;
}

static int bq27xxx_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	int ret = 0;
	struct bq27xxx_device_info *di = power_supply_get_drvdata(psy);

	mutex_lock(&di->lock);
	if (time_is_before_jiffies(di->last_update + 5 * HZ)) {
		cancel_delayed_work_sync(&di->work);
		bq27xxx_battery_poll(&di->work.work);
	}
	mutex_unlock(&di->lock);

	if (psp != POWER_SUPPLY_PROP_PRESENT && di->cache.flags < 0)
		return -ENODEV;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = bq27xxx_battery_status(di, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = bq27xxx_battery_voltage(di, val);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = di->cache.flags < 0 ? 0 : 1;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = bq27xxx_battery_current(di, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = bq27xxx_simple_value(di->cache.capacity, val);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		ret = bq27xxx_battery_capacity_level(di, val);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = bq27xxx_simple_value(di->cache.temperature, val);
		if (ret == 0)
			val->intval -= 2731; /* convert decidegree k to c */
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		ret = bq27xxx_simple_value(di->cache.time_to_empty, val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		ret = bq27xxx_simple_value(di->cache.time_to_empty_avg, val);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		ret = bq27xxx_simple_value(di->cache.time_to_full, val);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = bq27xxx_simple_value(bq27xxx_battery_read_nac(di), val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = bq27xxx_simple_value(di->cache.charge_full, val);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = bq27xxx_simple_value(di->charge_design_full, val);
		break;
	/*
	 * TODO: Implement these to make registers set from
	 * power_supply_battery_info visible in sysfs.
	 */
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		return -EINVAL;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		ret = bq27xxx_simple_value(di->cache.cycle_count, val);
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		ret = bq27xxx_simple_value(di->cache.energy, val);
		break;
	case POWER_SUPPLY_PROP_POWER_AVG:
		ret = bq27xxx_simple_value(di->cache.power_avg, val);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = bq27xxx_simple_value(di->cache.health, val);
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = BQ27XXX_MANUFACTURER;
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static void bq27xxx_external_power_changed(struct power_supply *psy)
{
	struct bq27xxx_device_info *di = power_supply_get_drvdata(psy);

	cancel_delayed_work_sync(&di->work);
	schedule_delayed_work(&di->work, 0);
}

int bq27xxx_battery_setup(struct bq27xxx_device_info *di)
{
	struct power_supply_desc *psy_desc;
	struct power_supply_config psy_cfg = {
		.of_node = di->dev->of_node,
		.drv_data = di,
	};

	INIT_DELAYED_WORK(&di->work, bq27xxx_battery_poll);
	mutex_init(&di->lock);
	di->regs = bq27xxx_regs[di->chip];

	psy_desc = devm_kzalloc(di->dev, sizeof(*psy_desc), GFP_KERNEL);
	if (!psy_desc)
		return -ENOMEM;

	psy_desc->name = di->name;
	psy_desc->type = POWER_SUPPLY_TYPE_BATTERY;
	psy_desc->properties = bq27xxx_battery_props[di->chip].props;
	psy_desc->num_properties = bq27xxx_battery_props[di->chip].size;
	psy_desc->get_property = bq27xxx_battery_get_property;
	psy_desc->external_power_changed = bq27xxx_external_power_changed;

	di->bat = power_supply_register_no_ws(di->dev, psy_desc, &psy_cfg);
	if (IS_ERR(di->bat)) {
		dev_err(di->dev, "failed to register battery\n");
		return PTR_ERR(di->bat);
	}

	dev_info(di->dev, "support ver. %s enabled\n", DRIVER_VERSION);

	bq27xxx_battery_settings(di);
	bq27xxx_battery_update(di);

	mutex_lock(&bq27xxx_list_lock);
	list_add(&di->list, &bq27xxx_battery_devices);
	mutex_unlock(&bq27xxx_list_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(bq27xxx_battery_setup);

void bq27xxx_battery_teardown(struct bq27xxx_device_info *di)
{
	/*
	 * power_supply_unregister call bq27xxx_battery_get_property which
	 * call bq27xxx_battery_poll.
	 * Make sure that bq27xxx_battery_poll will not call
	 * schedule_delayed_work again after unregister (which cause OOPS).
	 */
	poll_interval = 0;

	cancel_delayed_work_sync(&di->work);

	power_supply_unregister(di->bat);

	mutex_lock(&bq27xxx_list_lock);
	list_del(&di->list);
	mutex_unlock(&bq27xxx_list_lock);

	mutex_destroy(&di->lock);
}
EXPORT_SYMBOL_GPL(bq27xxx_battery_teardown);

static int bq27xxx_battery_platform_read(struct bq27xxx_device_info *di, u8 reg,
					 bool single)
{
	struct device *dev = di->dev;
	struct bq27xxx_platform_data *pdata = dev->platform_data;
	unsigned int timeout = 3;
	int upper, lower;
	int temp;

	if (!single) {
		/* Make sure the value has not changed in between reading the
		 * lower and the upper part */
		upper = pdata->read(dev, reg + 1);
		do {
			temp = upper;
			if (upper < 0)
				return upper;

			lower = pdata->read(dev, reg);
			if (lower < 0)
				return lower;

			upper = pdata->read(dev, reg + 1);
		} while (temp != upper && --timeout);

		if (timeout == 0)
			return -EIO;

		return (upper << 8) | lower;
	}

	return pdata->read(dev, reg);
}

static int bq27xxx_battery_platform_probe(struct platform_device *pdev)
{
	struct bq27xxx_device_info *di;
	struct bq27xxx_platform_data *pdata = pdev->dev.platform_data;

	if (!pdata) {
		dev_err(&pdev->dev, "no platform_data supplied\n");
		return -EINVAL;
	}

	if (!pdata->read) {
		dev_err(&pdev->dev, "no hdq read callback supplied\n");
		return -EINVAL;
	}

	if (!pdata->chip) {
		dev_err(&pdev->dev, "no device supplied\n");
		return -EINVAL;
	}

	di = devm_kzalloc(&pdev->dev, sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	platform_set_drvdata(pdev, di);

	di->dev = &pdev->dev;
	di->chip = pdata->chip;
	di->name = pdata->name ?: dev_name(&pdev->dev);
	di->bus.read = bq27xxx_battery_platform_read;

	return bq27xxx_battery_setup(di);
}

static int bq27xxx_battery_platform_remove(struct platform_device *pdev)
{
	struct bq27xxx_device_info *di = platform_get_drvdata(pdev);

	bq27xxx_battery_teardown(di);

	return 0;
}

static const struct platform_device_id bq27xxx_battery_platform_id_table[] = {
	{ "bq27000-battery", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, bq27xxx_battery_platform_id_table);

#ifdef CONFIG_OF
static const struct of_device_id bq27xxx_battery_platform_of_match_table[] = {
	{ .compatible = "ti,bq27000" },
	{},
};
MODULE_DEVICE_TABLE(of, bq27xxx_battery_platform_of_match_table);
#endif

static struct platform_driver bq27xxx_battery_platform_driver = {
	.probe	= bq27xxx_battery_platform_probe,
	.remove = bq27xxx_battery_platform_remove,
	.driver = {
		.name = "bq27000-battery",
		.of_match_table = of_match_ptr(bq27xxx_battery_platform_of_match_table),
	},
	.id_table = bq27xxx_battery_platform_id_table,
};
module_platform_driver(bq27xxx_battery_platform_driver);

MODULE_ALIAS("platform:bq27000-battery");

MODULE_AUTHOR("Rodolfo Giometti <giometti@linux.it>");
MODULE_DESCRIPTION("BQ27xxx battery monitor driver");
MODULE_LICENSE("GPL");
