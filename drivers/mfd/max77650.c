// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2018 BayLibre SAS
// Author: Bartosz Golaszewski <bgolaszewski@baylibre.com>
//
// Core MFD driver for MAXIM 77650/77651 charger/power-supply.

#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mfd/core.h>
#include <linux/mfd/max77650.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define MAX77650_INT_GPI_F_MSK		BIT(0)
#define MAX77650_INT_GPI_R_MSK		BIT(1)
#define MAX77650_INT_GPI_MSK \
			(MAX77650_INT_GPI_F_MSK | MAX77650_INT_GPI_R_MSK)
#define MAX77650_INT_nEN_F_MSK		BIT(2)
#define MAX77650_INT_nEN_R_MSK		BIT(3)
#define MAX77650_INT_TJAL1_R_MSK	BIT(4)
#define MAX77650_INT_TJAL2_R_MSK	BIT(5)
#define MAX77650_INT_DOD_R_MSK		BIT(6)

#define MAX77650_INT_THM_MSK		BIT(0)
#define MAX77650_INT_CHG_MSK		BIT(1)
#define MAX77650_INT_CHGIN_MSK		BIT(2)
#define MAX77650_INT_TJ_REG_MSK		BIT(3)
#define MAX77650_INT_CHGIN_CTRL_MSK	BIT(4)
#define MAX77650_INT_SYS_CTRL_MSK	BIT(5)
#define MAX77650_INT_SYS_CNFG_MSK	BIT(6)

#define MAX77650_INT_GLBL_OFFSET	0
#define MAX77650_INT_CHG_OFFSET		1

#define MAX77650_SBIA_LPM_MASK		BIT(5)
#define MAX77650_SBIA_LPM_DISABLED	0x00

enum {
	MAX77650_INT_GPI = 0,
	MAX77650_INT_nEN_F,
	MAX77650_INT_nEN_R,
	MAX77650_INT_TJAL1_R,
	MAX77650_INT_TJAL2_R,
	MAX77650_INT_DOD_R,
	MAX77650_INT_THM,
	MAX77650_INT_CHG,
	MAX77650_INT_CHGIN,
	MAX77650_INT_TJ_REG,
	MAX77650_INT_CHGIN_CTRL,
	MAX77650_INT_SYS_CTRL,
	MAX77650_INT_SYS_CNFG,
};

enum {
	MAX77650_CELL_REGULATOR = 0,
	MAX77650_CELL_CHARGER,
	MAX77650_CELL_GPIO,
	MAX77650_CELL_LED,
	MAX77650_CELL_ONKEY,
	MAX77650_NUM_CELLS,
};

struct max77650_irq_mapping {
	int cell_num;
	const int *irqs;
	const char *const *irq_names;
	unsigned int num_irqs;
};

static const int max77650_charger_irqs[] = {
	MAX77650_INT_CHG,
	MAX77650_INT_CHGIN,
};

static const int max77650_gpio_irqs[] = {
	MAX77650_INT_GPI,
};

static const int max77650_onkey_irqs[] = {
	MAX77650_INT_nEN_F,
	MAX77650_INT_nEN_R,
};

static const char *const max77650_charger_irq_names[] = {
	"CHG",
	"CHGIN",
};

static const char *const max77650_gpio_irq_names[] = {
	"GPI",
};

static const char *const max77650_onkey_irq_names[] = {
	"nEN_F",
	"nEN_R",
};

static const struct max77650_irq_mapping max77650_irq_mapping_table[] = {
	{
		.cell_num	= MAX77650_CELL_CHARGER,
		.irqs		= max77650_charger_irqs,
		.irq_names	= max77650_charger_irq_names,
		.num_irqs	= ARRAY_SIZE(max77650_charger_irqs),
	},
	{
		.cell_num	= MAX77650_CELL_GPIO,
		.irqs		= max77650_gpio_irqs,
		.irq_names	= max77650_gpio_irq_names,
		.num_irqs	= ARRAY_SIZE(max77650_gpio_irqs),
	},
	{
		.cell_num	= MAX77650_CELL_ONKEY,
		.irqs		= max77650_onkey_irqs,
		.irq_names	= max77650_onkey_irq_names,
		.num_irqs	= ARRAY_SIZE(max77650_onkey_irqs),
	},
};

static const struct mfd_cell max77650_cells[] = {
	[MAX77650_CELL_REGULATOR] = {
		.name		= "max77650-regulator",
		.of_compatible	= "maxim,max77650-regulator",
	},
	[MAX77650_CELL_CHARGER] = {
		.name		= "max77650-charger",
		.of_compatible	= "maxim,max77650-charger",
	},
	[MAX77650_CELL_GPIO] = {
		.name		= "max77650-gpio",
		.of_compatible	= "maxim,max77650-gpio",
	},
	[MAX77650_CELL_LED] = {
		.name		= "max77650-led",
		.of_compatible	= "maxim,max77650-led",
	},
	[MAX77650_CELL_ONKEY] = {
		.name		= "max77650-onkey",
		.of_compatible	= "maxim,max77650-onkey",
	},
};

static const struct regmap_irq max77650_irqs[] = {
	[MAX77650_INT_GPI] = {
		.reg_offset		= MAX77650_INT_GLBL_OFFSET,
		.mask			= MAX77650_INT_GPI_MSK,
		.type = {
			.type_falling_val	= MAX77650_INT_GPI_F_MSK,
			.type_rising_val	= MAX77650_INT_GPI_R_MSK,
			.types_supported	= IRQ_TYPE_EDGE_BOTH,
		},
	},
	[MAX77650_INT_nEN_F] = {
		.reg_offset		= MAX77650_INT_GLBL_OFFSET,
		.mask			= MAX77650_INT_nEN_F_MSK,
	},
	[MAX77650_INT_nEN_R] = {
		.reg_offset		= MAX77650_INT_GLBL_OFFSET,
		.mask			= MAX77650_INT_nEN_R_MSK,
	},
	[MAX77650_INT_TJAL1_R] = {
		.reg_offset		= MAX77650_INT_GLBL_OFFSET,
		.mask			= MAX77650_INT_TJAL1_R_MSK,
	},
	[MAX77650_INT_TJAL2_R] = {
		.reg_offset		= MAX77650_INT_GLBL_OFFSET,
		.mask			= MAX77650_INT_TJAL2_R_MSK,
	},
	[MAX77650_INT_DOD_R] = {
		.reg_offset		= MAX77650_INT_GLBL_OFFSET,
		.mask			= MAX77650_INT_DOD_R_MSK,
	},
	[MAX77650_INT_THM] = {
		.reg_offset		= MAX77650_INT_CHG_OFFSET,
		.mask			= MAX77650_INT_THM_MSK,
	},
	[MAX77650_INT_CHG] = {
		.reg_offset		= MAX77650_INT_CHG_OFFSET,
		.mask			= MAX77650_INT_CHG_MSK,
	},
	[MAX77650_INT_CHGIN] = {
		.reg_offset		= MAX77650_INT_CHG_OFFSET,
		.mask			= MAX77650_INT_CHGIN_MSK,
	},
	[MAX77650_INT_TJ_REG] = {
		.reg_offset		= MAX77650_INT_CHG_OFFSET,
		.mask			= MAX77650_INT_TJ_REG_MSK,
	},
	[MAX77650_INT_CHGIN_CTRL] = {
		.reg_offset		= MAX77650_INT_CHG_OFFSET,
		.mask			= MAX77650_INT_CHGIN_CTRL_MSK,
	},
	[MAX77650_INT_SYS_CTRL] = {
		.reg_offset		= MAX77650_INT_CHG_OFFSET,
		.mask			= MAX77650_INT_SYS_CTRL_MSK,
	},
	[MAX77650_INT_SYS_CNFG] = {
		.reg_offset		= MAX77650_INT_CHG_OFFSET,
		.mask			= MAX77650_INT_SYS_CNFG_MSK,
	},
};

static const struct regmap_irq_chip max77650_irq_chip = {
	.name			= "max77650-irq",
	.irqs			= max77650_irqs,
	.num_irqs		= ARRAY_SIZE(max77650_irqs),
	.num_regs		= 2,
	.status_base		= MAX77650_REG_INT_GLBL,
	.mask_base		= MAX77650_REG_INTM_GLBL,
	.type_in_mask		= true,
	.type_invert		= true,
	.init_ack_masked	= true,
	.clear_on_unmask	= true,
};

static const struct regmap_config max77650_regmap_config = {
	.name		= "max77650",
	.reg_bits	= 8,
	.val_bits	= 8,
};

static int max77650_setup_irqs(struct device *dev, struct mfd_cell *cells)
{
	const struct max77650_irq_mapping *mapping;
	struct regmap_irq_chip_data *irq_data;
	struct i2c_client *i2c;
	struct mfd_cell *cell;
	struct resource *res;
	struct regmap *map;
	int i, j, irq, rv;

	i2c = to_i2c_client(dev);

	map = dev_get_regmap(dev, NULL);
	if (!map)
		return -ENODEV;

	rv = devm_regmap_add_irq_chip(dev, map, i2c->irq,
				      IRQF_ONESHOT | IRQF_SHARED, -1,
				      &max77650_irq_chip, &irq_data);
	if (rv)
		return rv;

	for (i = 0; i < ARRAY_SIZE(max77650_irq_mapping_table); i++) {
		mapping = &max77650_irq_mapping_table[i];
		cell = &cells[mapping->cell_num];

		res = devm_kcalloc(dev, sizeof(*res),
				   mapping->num_irqs, GFP_KERNEL);
		if (!res)
			return -ENOMEM;

		cell->resources = res;
		cell->num_resources = mapping->num_irqs;

		for (j = 0; j < mapping->num_irqs; j++) {
			irq = regmap_irq_get_virq(irq_data, mapping->irqs[j]);
			if (irq < 0)
				return irq;

			res[j].start = res[j].end = irq;
			res[j].flags = IORESOURCE_IRQ;
			res[j].name = mapping->irq_names[j];
		}
	}

	return 0;
}

static int max77650_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct mfd_cell *cells;
	struct regmap *map;
	unsigned int val;
	int rv;

	map = devm_regmap_init_i2c(i2c, &max77650_regmap_config);
	if (IS_ERR(map))
		return PTR_ERR(map);

	rv = regmap_read(map, MAX77650_REG_CID, &val);
	if (rv)
		return rv;

	switch (MAX77650_CID_BITS(val)) {
	case MAX77650_CID_77650A:
	case MAX77650_CID_77650C:
	case MAX77650_CID_77651A:
	case MAX77650_CID_77651B:
		break;
	default:
		return -ENODEV;
	}

	/*
	 * This IC has a low-power mode which reduces the quiescent current
	 * consumption to ~5.6uA but is only suitable for systems consuming
	 * less than ~2mA. Since this is not likely the case even on
	 * linux-based wearables - keep the chip in normal power mode.
	 */
	rv = regmap_update_bits(map,
				MAX77650_REG_CNFG_GLBL,
				MAX77650_SBIA_LPM_MASK,
				MAX77650_SBIA_LPM_DISABLED);
	if (rv)
		return rv;

	cells = devm_kmemdup(dev, max77650_cells,
			     sizeof(max77650_cells), GFP_KERNEL);
	if (!cells)
		return -ENOMEM;

	rv = max77650_setup_irqs(dev, cells);
	if (rv)
		return rv;

	return devm_mfd_add_devices(dev, -1, cells,
				    MAX77650_NUM_CELLS, NULL, 0, NULL);
}

static const struct of_device_id max77650_of_match[] = {
	{ .compatible = "maxim,max77650" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77650_of_match);

static struct i2c_driver max77650_i2c_driver = {
	.driver = {
		.name = "max77650",
		.of_match_table = of_match_ptr(max77650_of_match),
	},
	.probe_new = max77650_i2c_probe,
};
module_i2c_driver(max77650_i2c_driver);

MODULE_DESCRIPTION("MAXIM 77650/77651 multi-function core driver");
MODULE_AUTHOR("Bartosz Golaszewski <bgolaszewski@baylibre.com>");
MODULE_LICENSE("GPL v2");
