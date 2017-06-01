/*
 * Copyright (c) 2017 Intel Corporation
 *
 * Functions to access TPS68470 power management chip.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __TI_PMIC_H
#define __TI_PMIC_H

struct ti_pmic_table {
	u32 address;		/* operation region address */
	u32 reg;		/* corresponding register */
	u32 bitmask;		/* bit mask for power, clock */
};

struct ti_pmic_opregion_data {
	/* Voltage regulators */
	int (*get_power)(struct regmap *r, int reg, int bit, u64 *value);
	int (*update_power)(struct regmap *r, int reg, int bit, u64 value);
	struct ti_pmic_table *power_table;
	int power_table_size;
	int (*get_vr_val)(struct regmap *r, int reg, int bit, u64 *value);
	int (*update_vr_val)(struct regmap *r, int reg, int bit, u64 value);
	struct ti_pmic_table *vr_val_table;
	int vr_val_table_size;
	/* Clocks */
	int (*get_clk)(struct regmap *r, int reg, int bit, u64 *value);
	int (*update_clk)(struct regmap *r, int reg, int bit, u64 value);
	struct ti_pmic_table *clk_table;
	int clk_table_size;
	int (*get_clk_freq)(struct regmap *r, int reg, int bit, u64 *value);
	int (*update_clk_freq)(struct regmap *r, int reg, int bit, u64 value);
	struct ti_pmic_table *clk_freq_table;
	int clk_freq_table_size;
};

int ti_pmic_install_opregion_handler(struct device *dev, acpi_handle handle,
				     struct regmap *regmap,
				     struct ti_pmic_opregion_data *d);

#endif
