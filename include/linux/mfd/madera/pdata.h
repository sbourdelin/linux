/*
 * Platform data for Cirrus Logic Madera codecs
 *
 * Copyright 2015-2017 Cirrus Logic
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef MADERA_PDATA_H
#define MADERA_PDATA_H

#include <linux/kernel.h>
#include <linux/irqchip/irq-madera-pdata.h>
#include <linux/regulator/arizona-ldo1.h>
#include <linux/regulator/arizona-micsupp.h>
#include <linux/regulator/machine.h>
#include <sound/madera-pdata.h>

#define MADERA_MAX_MICBIAS		4
#define MADERA_MAX_CHILD_MICBIAS	4

#define MADERA_MAX_GPSW			2

struct pinctrl_map;

/**
 * struct madera_pdata - Configuration data for Madera devices
 *
 * @reset:	    GPIO controlling /RESET (0 = none)
 * @ldo1:	    Substruct of pdata for the LDO1 regulator
 * @micvdd:	    Substruct of pdata for the MICVDD regulator
 * @irqchip:	    Substruct of pdata for the irqchip driver
 * @gpio_base:	    Base GPIO number
 * @gpio_configs:   Array of GPIO configurations (See Documentation/pinctrl.txt)
 * @n_gpio_configs: Number of entries in gpio_configs
 * @codec:	    Substructure of pdata for the ASoC codec driver
 *		    See include/sound/madera-pdata.h
 * @gpsw:	    General purpose switch mode setting (See the SW1_MODE field
 *		    in the datasheet for the available values for your codec)
 */
struct madera_pdata {
	int reset;

	struct arizona_ldo1_pdata ldo1;
	struct arizona_micsupp_pdata micvdd;

	struct madera_irqchip_pdata irqchip;

	int gpio_base;

	const struct pinctrl_map *gpio_configs;
	int n_gpio_configs;

	struct madera_codec_pdata codec;

	u32 gpsw[MADERA_MAX_GPSW];
};

#endif
