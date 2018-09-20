/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Header file for PNI RM3100 driver
 *
 * Copyright (C) 2018 Song Qiang <songqiang1304521@gmail.com>
 */

#ifndef RM3100_CORE_H
#define RM3100_CORE_H

#include <linux/module.h>
#include <linux/regmap.h>

#define RM_REG_REV_ID		0x36

/* Cycle Count Registers MSBs and LSBs. */
#define RM_REG_CCXM		0x04
#define RM_REG_CCXL		0x05
#define RM_REG_CCYM		0x06
#define RM_REG_CCYL		0x07
#define RM_REG_CCZM		0x08
#define RM_REG_CCZL		0x09

/* Single Measurement Mode register. */
#define RM_REG_POLL		0x00
#define RM_POLL_PMX		BIT(4)
#define RM_POLL_PMY		BIT(5)
#define RM_POLL_PMZ		BIT(6)

/* Continues Measurement Mode register. */
#define RM_REG_CMM		0x01
#define RM_CMM_START		BIT(0)
#define RM_CMM_DRDM		BIT(2)
#define RM_CMM_PMX		BIT(4)
#define RM_CMM_PMY		BIT(5)
#define RM_CMM_PMZ		BIT(6)

/* TiMe Rate Configuration register. */
#define RM_REG_TMRC		0x0B
#define RM_TMRC_OFFSET		0x92

/* Result Status register. */
#define RM_REG_STATUS		0x34
#define RM_STATUS_DRDY		BIT(7)

/* Measurement result registers. */
#define RM_REG_MX2		0x24
#define RM_REG_MX1		0x25
#define RM_REG_MX0		0x26
#define RM_REG_MY2		0x27
#define RM_REG_MY1		0x28
#define RM_REG_MY0		0x29
#define RM_REG_MZ2		0x2a
#define RM_REG_MZ1		0x2b
#define RM_REG_MZ0		0x2c

#define RM_REG_HSHAKE		0x35

#define RM_W_REG_START		RM_REG_POLL
#define RM_W_REG_END		RM_REG_REV_ID
#define RM_R_REG_START		RM_REG_POLL
#define RM_R_REG_END		RM_REG_HSHAKE
#define RM_V_REG_START		RM_REG_MX2
#define RM_V_REG_END		RM_REG_HSHAKE

/* Built-In Self Test reigister. */
#define RM_REG_BIST		0x33

struct rm3100_data {
	struct device *dev;
	struct regmap *regmap;
	struct completion measuring_done;
	bool use_interrupt;

	int conversion_time;

	/* To protect consistency of every measurement and sampling
	 * frequency change operations.
	 */
	struct mutex lock;
};

extern const struct regmap_access_table rm3100_readable_table;
extern const struct regmap_access_table rm3100_writable_table;
extern const struct regmap_access_table rm3100_volatile_table;

int rm3100_common_probe(struct device *dev, struct regmap *regmap, int irq);
int rm3100_common_remove(struct device *dev);

#endif /* RM3100_CORE_H */
