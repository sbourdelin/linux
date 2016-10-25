/*
 * pv88080-core.c - Device access for PV88080
 *
 * Copyright (C) 2016  Powerventure Semiconductor Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/module.h>

#include <linux/mfd/pv88080.h>

#define	PV88080_REG_EVENT_A_OFFSET	0
#define	PV88080_REG_EVENT_B_OFFSET	1
#define	PV88080_REG_EVENT_C_OFFSET	2

static const struct resource regulators_aa_resources[] = {
	{
		.name	= "regulator-irq",
		.start  = PV88080_AA_IRQ_VDD_FLT,
		.end	= PV88080_AA_IRQ_OVER_TEMP,
		.flags	= IORESOURCE_IRQ,
	},
};

static const struct resource regulators_ba_resources[] = {
	{
		.name	= "regulator-irq",
		.start  = PV88080_BA_IRQ_VDD_FLT,
		.end	= PV88080_BA_IRQ_OVER_TEMP,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct mfd_cell pv88080_cells[] = {
	{
		.name = "pv88080-regulator",
	},
	{
		.name = "pv88080-gpio",
	},
};

static const struct regmap_irq pv88080_aa_irqs[] = {
	/* PV88080 event A register for AA/AB silicon */
	[PV88080_AA_IRQ_VDD_FLT] = {
		.reg_offset = PV88080_REG_EVENT_A_OFFSET,
		.mask = PV88080_M_VDD_FLT,
	},
	[PV88080_AA_IRQ_OVER_TEMP] = {
		.reg_offset = PV88080_REG_EVENT_A_OFFSET,
		.mask = PV88080_M_OVER_TEMP,
	},
	[PV88080_AA_IRQ_SEQ_RDY] = {
		.reg_offset = PV88080_REG_EVENT_A_OFFSET,
		.mask = PV88080_M_SEQ_RDY,
	},
	/* PV88080 event B register for AA/AB silicon */
	[PV88080_AA_IRQ_HVBUCK_OV] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_HVBUCK_OV,
	},
	[PV88080_AA_IRQ_HVBUCK_UV] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_HVBUCK_UV,
	},
	[PV88080_AA_IRQ_HVBUCK_SCP] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_HVBUCK_SCP,
	},
	[PV88080_AA_IRQ_BUCK1_SCP] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_BUCK1_SCP,
	},
	[PV88080_AA_IRQ_BUCK2_SCP] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_BUCK2_SCP,
	},
	[PV88080_AA_IRQ_BUCK3_SCP] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_BUCK3_SCP,
	},
	/* PV88080 event C register for AA/AB silicon */
	[PV88080_AA_IRQ_GPIO_FLAG0] = {
		.reg_offset = PV88080_REG_EVENT_C_OFFSET,
		.mask = PV88080_M_GPIO_FLAG0,
	},
	[PV88080_AA_IRQ_GPIO_FLAG1] = {
		.reg_offset = PV88080_REG_EVENT_C_OFFSET,
		.mask = PV88080_M_GPIO_FLAG1,
	},
};

static const struct regmap_irq pv88080_ba_irqs[] = {
	/* PV88080 event A register for BA/BB silicon */
	[PV88080_BA_IRQ_VDD_FLT] = {
		.reg_offset = PV88080_REG_EVENT_A_OFFSET,
		.mask = PV88080_M_VDD_FLT,
	},
	[PV88080_BA_IRQ_OVER_TEMP] = {
		.reg_offset = PV88080_REG_EVENT_A_OFFSET,
		.mask = PV88080_M_OVER_TEMP,
	},
	[PV88080_BA_IRQ_SEQ_RDY] = {
		.reg_offset = PV88080_REG_EVENT_A_OFFSET,
		.mask = PV88080_M_SEQ_RDY,
	},
	[PV88080_BA_IRQ_EXT_OT] = {
		.reg_offset = PV88080_REG_EVENT_A_OFFSET,
		.mask = PV88080_M_EXT_OT,
	},
	/* PV88080 event B register for BA/BB silicon */
	[PV88080_BA_IRQ_HVBUCK_OV] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_HVBUCK_OV,
	},
	[PV88080_BA_IRQ_HVBUCK_UV] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_HVBUCK_UV,
	},
	[PV88080_BA_IRQ_HVBUCK_SCP] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_HVBUCK_SCP,
	},
	[PV88080_BA_IRQ_BUCK1_SCP] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_BUCK1_SCP,
	},
	[PV88080_BA_IRQ_BUCK2_SCP] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_BUCK2_SCP,
	},
	[PV88080_BA_IRQ_BUCK3_SCP] = {
		.reg_offset = PV88080_REG_EVENT_B_OFFSET,
		.mask = PV88080_M_BUCK3_SCP,
	},
	/* PV88080 event C register for BA/BB silicon */
	[PV88080_BA_IRQ_GPIO_FLAG0] = {
		.reg_offset = PV88080_REG_EVENT_C_OFFSET,
		.mask = PV88080_M_GPIO_FLAG0,
	},
	[PV88080_BA_IRQ_GPIO_FLAG1] = {
		.reg_offset = PV88080_REG_EVENT_C_OFFSET,
		.mask = PV88080_M_GPIO_FLAG1,
	},
	[PV88080_BA_IRQ_BUCK1_DROP_TIMEOUT] = {
		.reg_offset = PV88080_REG_EVENT_C_OFFSET,
		.mask = PV88080_M_BUCK1_DROP_TIMEOUT,
	},
	[PV88080_BA_IRQ_BUCK2_DROP_TIMEOUT] = {
		.reg_offset = PV88080_REG_EVENT_C_OFFSET,
		.mask = PV88080_M_BUCK2_DROP_TIMEOUT,
	},
	[PB88080_BA_IRQ_BUCK3_DROP_TIMEOUT] = {
		.reg_offset = PV88080_REG_EVENT_C_OFFSET,
		.mask = PV88080_M_BUCk3_DROP_TIMEOUT,
	},
};

static struct regmap_irq_chip pv88080_irq_chip = {
	.name = "pv88080-irq",
	.num_regs = 3,
	.status_base = PV88080_REG_EVENT_A,
	.mask_base = PV88080_REG_MASK_A,
	.ack_base = PV88080_REG_EVENT_A,
	.init_ack_masked = true,
};

int pv88080_device_init(struct pv88080 *chip, unsigned int irq)
{
	struct pv88080_pdata *pdata = dev_get_platdata(chip->dev);
	int ret;

	if (pdata)
		chip->irq_base = pdata->irq_base;
	else
		chip->irq_base = 0;
	chip->irq = irq;

	if (pdata && pdata->init != NULL) {
		ret = pdata->init(chip);
		if (ret != 0) {
			dev_err(chip->dev, "Platform initialization failed\n");
			return ret;
		}
	}

	ret = regmap_write(chip->regmap, PV88080_REG_MASK_A, 0xFF);
	if (ret < 0) {
		dev_err(chip->dev,
			"Failed to mask A reg: %d\n", ret);
		return ret;
	}
	ret = regmap_write(chip->regmap, PV88080_REG_MASK_B, 0xFF);
	if (ret < 0) {
		dev_err(chip->dev,
			"Failed to mask B reg: %d\n", ret);
		return ret;
	}
	ret = regmap_write(chip->regmap, PV88080_REG_MASK_C, 0xFF);
	if (ret < 0) {
		dev_err(chip->dev, "Failed to mask C reg: %d\n", ret);
		return ret;
	}

	switch (chip->type) {
	case TYPE_PV88080_AA:
		pv88080_irq_chip.irqs = pv88080_aa_irqs;
		pv88080_irq_chip.num_irqs = ARRAY_SIZE(pv88080_aa_irqs);

		pv88080_cells[0].num_resources
				= ARRAY_SIZE(regulators_aa_resources);
		pv88080_cells[0].resources = regulators_aa_resources;
		break;
	case TYPE_PV88080_BA:
		pv88080_irq_chip.irqs = pv88080_ba_irqs;
		pv88080_irq_chip.num_irqs = ARRAY_SIZE(pv88080_ba_irqs);

		pv88080_cells[0].num_resources
				= ARRAY_SIZE(regulators_ba_resources);
		pv88080_cells[0].resources = regulators_ba_resources;
		break;
	}

	ret = regmap_add_irq_chip(chip->regmap, chip->irq,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT,
			chip->irq_base, &pv88080_irq_chip,
			&chip->irq_data);
	if (ret) {
		dev_err(chip->dev, "Failed to reguest IRQ %d: %d\n",
				chip->irq, ret);
		return ret;
	}

	ret = mfd_add_devices(chip->dev, PLATFORM_DEVID_NONE,
			pv88080_cells, ARRAY_SIZE(pv88080_cells),
			NULL, chip->irq_base, NULL);
	if (ret) {
		dev_err(chip->dev, "Cannot add MFD cells\n");
		regmap_del_irq_chip(chip->irq, chip->irq_data);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pv88080_device_init);

int pv88080_device_exit(struct pv88080 *chip)
{
	mfd_remove_devices(chip->dev);
	regmap_del_irq_chip(chip->irq, chip->irq_data);

	return 0;
}
EXPORT_SYMBOL_GPL(pv88080_device_exit);

MODULE_AUTHOR("Eric Jeong <eric.jeong.opensource@diasemi.com>");
MODULE_DESCRIPTION("MFD driver for Powerventure PV88080");
MODULE_LICENSE("GPL");

