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
#include <linux/regulator/machine.h>

#include <linux/regulator/madera-ldo1.h>
#include <linux/regulator/madera-micsupp.h>
#include <linux/irqchip/irq-madera-pdata.h>
#include <sound/madera-pdata.h>

#define MADERA_MAX_MICBIAS		4
#define MADERA_MAX_CHILD_MICBIAS	4

#define MADERA_MAX_GPSW			2

struct pinctrl_map;

/** MICBIAS pin configuration */
struct madera_micbias_pin_pdata {
	/** Regulator configuration for pin switch */
	struct regulator_init_data init_data;
};

/** Regulator configuration for an on-chip MICBIAS */
struct madera_micbias_pdata {
	/** Configuration of the MICBIAS generator */
	struct regulator_init_data init_data;

	bool ext_cap;    /** External capacitor fitted */

	/**
	 * Configuration for each output pin from this MICBIAS
	 * (Not used on CS47L85 and WM1840)
	 */
	struct madera_micbias_pin_pdata pin[MADERA_MAX_CHILD_MICBIAS];
};

struct madera_pdata {
	/** GPIO controlling /RESET, if any */
	int reset;

	/** Substruct of pdata for the LDO1 regulator */
	struct madera_ldo1_pdata ldo1;

	/** Substruct of pdata for the MICSUPP regulator */
	struct madera_micsupp_pdata micsupp;

	/** Substruct of pdata for the irqchip driver */
	struct madera_irqchip_pdata irqchip;

	/** Base GPIO */
	int gpio_base;

	/**
	 * Array of GPIO configurations
	 * See Documentation/pinctrl.txt
	 */
	const struct pinctrl_map *gpio_configs;
	int n_gpio_configs;

	/** MICBIAS configurations */
	struct madera_micbias_pdata micbias[MADERA_MAX_MICBIAS];

	/**
	 * Substructure of pdata for the ASoC codec driver
	 * See include/sound/madera-pdata.h
	 */
	struct madera_codec_pdata codec;

	/**
	 * General purpose switch mode setting
	 * See the SW1_MODE field in the datasheet for the available values
	 */
	u32 gpsw[MADERA_MAX_GPSW];
};

#endif
