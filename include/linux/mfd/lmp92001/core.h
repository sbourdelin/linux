/*
 * include/linux/mfd/lmp92001/core.h - Core interface for TI LMP92001
 *
 * Copyright 2016-2017 Celestica Ltd.
 *
 * Author: Abhisit Sangjan <s.abhisit@gmail.com>
 *
 * Inspired by wm831x driver.
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
 */

#ifndef __MFD_LMP92001_CORE_H__
#define __MFD_LMP92001_CORE_H__

#include <linux/device.h>
#include <linux/mfd/core.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

/*
 * Register values.
 */
/* ID */
#define LMP92001_ID	0x0E
#define LMP92001_VER	0x0F
/* STATUS */
#define LMP92001_SGEN	0x10
#define LMP92001_SGPI	0x11
#define LMP92001_SHIL	0x12
#define LMP92001_SLOL	0x13
/* CONTROL */
#define LMP92001_CGEN	0x14
#define LMP92001_CDAC	0x15
#define LMP92001_CGPO	0x16
#define LMP92001_CINH	0x17
#define LMP92001_CINL	0x18
#define LMP92001_CAD1	0x19
#define LMP92001_CAD2	0x1A
#define LMP92001_CAD3	0x1B
#define LMP92001_CTRIG	0x1C
/* ADC OUTPUT DATA */
#define LMP92001_ADC1	0x20
#define LMP92001_ADC2	0x21
#define LMP92001_ADC3	0x22
#define LMP92001_ADC4	0x23
#define LMP92001_ADC5	0x24
#define LMP92001_ADC6	0x25
#define LMP92001_ADC7	0x26
#define LMP92001_ADC8	0x27
#define LMP92001_ADC9	0x28
#define LMP92001_ADC10	0x29
#define LMP92001_ADC11	0x2A
#define LMP92001_ADC12	0x2B
#define LMP92001_ADC13	0x2C
#define LMP92001_ADC14	0x2D
#define LMP92001_ADC15	0x2E
#define LMP92001_ADC16	0x2F
#define LMP92001_ADC17	0x30
/* ADC WINDOW COMPARATOR LIMITS */
#define LMP92001_LIH1	0x40
#define LMP92001_LIH2	0x41
#define LMP92001_LIH3	0x42
#define LMP92001_LIH9	0x43
#define LMP92001_LIH10	0x44
#define LMP92001_LIH11	0x45
#define LMP92001_LIL1	0x46
#define LMP92001_LIL2	0x47
#define LMP92001_LIL3	0x48
#define LMP92001_LIL9	0x49
#define LMP92001_LIL10	0x4A
#define LMP92001_LIL11	0x4B
/* INTERNAL REFERENCE CONTROL */
#define LMP92001_CREF	0x66
/* DAC INPUT DATA */
#define LMP92001_DAC1	0x80
#define LMP92001_DAC2	0x81
#define LMP92001_DAC3	0x82
#define LMP92001_DAC4	0x83
#define LMP92001_DAC5	0x84
#define LMP92001_DAC6	0x85
#define LMP92001_DAC7	0x86
#define LMP92001_DAC8	0x87
#define LMP92001_DAC9	0x88
#define LMP92001_DAC10	0x89
#define LMP92001_DAC11	0x8A
#define LMP92001_DAC12	0x8B
#define LMP92001_DALL	0x90
/* MEMORY MAPPED BLOCK COMMANDS */
#define LMP92001_BLK0	0xF0
#define LMP92001_BLK1	0xF1
#define LMP92001_BLK2	0xF2
#define LMP92001_BLK3	0xF3
#define LMP92001_BLK4	0xF4
#define LMP92001_BLK5	0xF5

struct lmp92001 {
	struct device *dev;
	struct regmap *regmap;

	struct mutex adc_lock;
	struct mutex dac_lock;
};

extern struct regmap_config lmp92001_regmap_config;

int lmp92001_device_init(struct lmp92001 *lmp92001, unsigned long id, int irq);
void lmp92001_device_exit(struct lmp92001 *lmp92001);

#endif /* __MFD_LMP92001_CORE_H__ */
