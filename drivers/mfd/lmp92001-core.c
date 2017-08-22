/*
 * lmp92001-core.c - Device access for TI LMP92001
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

#include <linux/mfd/lmp92001/core.h>
#include <linux/mfd/lmp92001/debug.h>

static const struct mfd_cell lmp92001_devs[] = {
	{
		.name = "lmp92001-gpio",
		.of_compatible = "ti,lmp92001-gpio",
	},
	{
		.name = "lmp92001-adc",
		.of_compatible = "ti,lmp92001-adc",
	},
	{
		.name = "lmp92001-dac",
		.of_compatible = "ti,lmp92001-dac",
	},
};

static struct reg_default lmp92001_defaults[] = {
	{ LMP92001_SGEN,  0x40   },
	{ LMP92001_SHIL,  0x00   },
	{ LMP92001_SLOL,  0x00   },
	{ LMP92001_CGEN,  0x00   },
	{ LMP92001_CDAC,  0x03   },
	{ LMP92001_CGPO,  0xFF   },
	{ LMP92001_CINH,  0x00   },
	{ LMP92001_CINL,  0x00   },
	{ LMP92001_CAD1,  0x00   },
	{ LMP92001_CAD2,  0x00   },
	{ LMP92001_CAD3,  0x00   },
	{ LMP92001_CTRIG, 0x00   },
	{ LMP92001_CREF,  0x07   },
	{ LMP92001_ADC1,  0x0000 },
	{ LMP92001_ADC2,  0x0000 },
	{ LMP92001_ADC3,  0x0000 },
	{ LMP92001_ADC4,  0x0000 },
	{ LMP92001_ADC5,  0x0000 },
	{ LMP92001_ADC6,  0x0000 },
	{ LMP92001_ADC7,  0x0000 },
	{ LMP92001_ADC8,  0x0000 },
	{ LMP92001_ADC9,  0x0000 },
	{ LMP92001_ADC10, 0x0000 },
	{ LMP92001_ADC11, 0x0000 },
	{ LMP92001_ADC12, 0x0000 },
	{ LMP92001_ADC13, 0x0000 },
	{ LMP92001_ADC14, 0x0000 },
	{ LMP92001_ADC15, 0x0000 },
	{ LMP92001_ADC16, 0x0000 },
	{ LMP92001_LIH1,  0x0FFF },
	{ LMP92001_LIH2,  0x0FFF },
	{ LMP92001_LIH3,  0x0FFF },
	{ LMP92001_LIH9,  0x0FFF },
	{ LMP92001_LIH10, 0x0FFF },
	{ LMP92001_LIH11, 0x0FFF },
	{ LMP92001_LIL1,  0x0000 },
	{ LMP92001_LIL2,  0x0000 },
	{ LMP92001_LIL3,  0x0000 },
	{ LMP92001_LIL9,  0x0000 },
	{ LMP92001_LIL10, 0x0000 },
	{ LMP92001_LIL11, 0x0000 },
	{ LMP92001_DAC1,  0x0000 },
	{ LMP92001_DAC2,  0x0000 },
	{ LMP92001_DAC3,  0x0000 },
	{ LMP92001_DAC4,  0x0000 },
	{ LMP92001_DAC5,  0x0000 },
	{ LMP92001_DAC6,  0x0000 },
	{ LMP92001_DAC7,  0x0000 },
	{ LMP92001_DAC8,  0x0000 },
	{ LMP92001_DAC9,  0x0000 },
	{ LMP92001_DAC10, 0x0000 },
	{ LMP92001_DAC11, 0x0000 },
	{ LMP92001_DAC12, 0x0000 },
	{ LMP92001_DALL,  0x0000 },
};

static bool lmp92001_reg_readable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LMP92001_ID:
	case LMP92001_VER:
	case LMP92001_SGEN:
	case LMP92001_SGPI:
	case LMP92001_SHIL:
	case LMP92001_SLOL:
	case LMP92001_CGEN:
	case LMP92001_CDAC:
	case LMP92001_CGPO:
	case LMP92001_CINH:
	case LMP92001_CINL:
	case LMP92001_CAD1:
	case LMP92001_CAD2:
	case LMP92001_CAD3:
	case LMP92001_ADC1:
	case LMP92001_ADC2:
	case LMP92001_ADC3:
	case LMP92001_ADC4:
	case LMP92001_ADC5:
	case LMP92001_ADC6:
	case LMP92001_ADC7:
	case LMP92001_ADC8:
	case LMP92001_ADC9:
	case LMP92001_ADC10:
	case LMP92001_ADC11:
	case LMP92001_ADC12:
	case LMP92001_ADC13:
	case LMP92001_ADC14:
	case LMP92001_ADC15:
	case LMP92001_ADC16:
	case LMP92001_ADC17:
	case LMP92001_LIH1:
	case LMP92001_LIH2:
	case LMP92001_LIH3:
	case LMP92001_LIH9:
	case LMP92001_LIH10:
	case LMP92001_LIH11:
	case LMP92001_LIL1:
	case LMP92001_LIL2:
	case LMP92001_LIL3:
	case LMP92001_LIL9:
	case LMP92001_LIL10:
	case LMP92001_LIL11:
	case LMP92001_CREF:
	case LMP92001_DAC1:
	case LMP92001_DAC2:
	case LMP92001_DAC3:
	case LMP92001_DAC4:
	case LMP92001_DAC5:
	case LMP92001_DAC6:
	case LMP92001_DAC7:
	case LMP92001_DAC8:
	case LMP92001_DAC9:
	case LMP92001_DAC10:
	case LMP92001_DAC11:
	case LMP92001_DAC12:
	case LMP92001_BLK0:
	case LMP92001_BLK1:
	case LMP92001_BLK2:
	case LMP92001_BLK3:
	case LMP92001_BLK4:
	case LMP92001_BLK5:
		return true;
	default:
		return false;
	}
}

static bool lmp92001_reg_writeable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LMP92001_CGEN:
	case LMP92001_CDAC:
	case LMP92001_CGPO:
	case LMP92001_CINH:
	case LMP92001_CINL:
	case LMP92001_CAD1:
	case LMP92001_CAD2:
	case LMP92001_CAD3:
	case LMP92001_CTRIG:
	case LMP92001_LIH1:
	case LMP92001_LIH2:
	case LMP92001_LIH3:
	case LMP92001_LIH9:
	case LMP92001_LIH10:
	case LMP92001_LIH11:
	case LMP92001_LIL1:
	case LMP92001_LIL2:
	case LMP92001_LIL3:
	case LMP92001_LIL9:
	case LMP92001_LIL10:
	case LMP92001_LIL11:
	case LMP92001_CREF:
	case LMP92001_DAC1:
	case LMP92001_DAC2:
	case LMP92001_DAC3:
	case LMP92001_DAC4:
	case LMP92001_DAC5:
	case LMP92001_DAC6:
	case LMP92001_DAC7:
	case LMP92001_DAC8:
	case LMP92001_DAC9:
	case LMP92001_DAC10:
	case LMP92001_DAC11:
	case LMP92001_DAC12:
	case LMP92001_DALL:
	case LMP92001_BLK0:
	case LMP92001_BLK1:
	case LMP92001_BLK4:
	case LMP92001_BLK5:
		return true;
	default:
		return false;
	}
}

static bool lmp92001_reg_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case LMP92001_SGEN:
	case LMP92001_SGPI:
	case LMP92001_SHIL:
	case LMP92001_SLOL:
	case LMP92001_CGEN:
	case LMP92001_ADC1:
	case LMP92001_ADC2:
	case LMP92001_ADC3:
	case LMP92001_ADC4:
	case LMP92001_ADC5:
	case LMP92001_ADC6:
	case LMP92001_ADC7:
	case LMP92001_ADC8:
	case LMP92001_ADC9:
	case LMP92001_ADC10:
	case LMP92001_ADC11:
	case LMP92001_ADC12:
	case LMP92001_ADC13:
	case LMP92001_ADC14:
	case LMP92001_ADC15:
	case LMP92001_ADC16:
	case LMP92001_ADC17:
	case LMP92001_BLK2:
	case LMP92001_BLK3:
		return true;
	default:
		return false;
	}
}

struct regmap_config lmp92001_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,

	.cache_type = REGCACHE_RBTREE,

	.reg_defaults = lmp92001_defaults,
	.num_reg_defaults = ARRAY_SIZE(lmp92001_defaults),

	.max_register = LMP92001_BLK5,
	.readable_reg = lmp92001_reg_readable,
	.writeable_reg = lmp92001_reg_writeable,
	.volatile_reg = lmp92001_reg_volatile,
};

int lmp92001_device_init(struct lmp92001 *lmp92001, unsigned long id, int irq)
{
	int ret;
	unsigned int comid, ver;

	dev_set_drvdata(lmp92001->dev, lmp92001);

	ret  = regmap_read(lmp92001->regmap, LMP92001_ID, &comid);
	if (ret < 0) {
		dev_err(lmp92001->dev, "failed to read Company ID: %d\n", ret);
		goto exit;
	}

	ret  = regmap_read(lmp92001->regmap, LMP92001_VER, &ver);
	if (ret < 0) {
		dev_err(lmp92001->dev, "failed to read Version: %d\n", ret);
		goto exit;
	}

	dev_info(lmp92001->dev, "Company ID 0x%.2x, Version 0x%.2x\n",
			comid, ver);

	ret = mfd_add_devices(lmp92001->dev, PLATFORM_DEVID_AUTO,
				lmp92001_devs, ARRAY_SIZE(lmp92001_devs),
				NULL, 0, NULL);
	if (ret != 0) {
		dev_err(lmp92001->dev, "failed to add children\n");
		goto exit;
	}

	ret = lmp92001_debug_init(lmp92001);
	if (ret < 0) {
		dev_err(lmp92001->dev, "failed to initial debug fs.\n");
		goto exit;
	}

exit:
	return ret;
}

void lmp92001_device_exit(struct lmp92001 *lmp92001)
{
	lmp92001_debug_exit(lmp92001);
	mfd_remove_devices(lmp92001->dev);
}
