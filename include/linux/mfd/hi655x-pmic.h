/*
 * Head file for hi655x pmic
 *
 * Copyright (c) 2015 Hisilicon.
 *
 * Fei Wang <w.f@huawei.com>
 * Chen Feng <puck.chen@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __HI655X_PMIC_H
#define __HI655X_PMIC_H

/* Hi655x registers are mapped to memory bus in 4 bytes stride */
#define HI655X_REG_TO_BUS_ADDR(x)        ((x) << 2)

#define HI655X_BITS                      (8)

/*numb of sub-interrupt*/
#define HI655X_NR_IRQ                    (32)

#define HI655X_IRQ_STAT_BASE             (0x003)
#define HI655X_IRQ_MASK_BASE             (0x007)
#define HI655X_IRQ_ARRAY                 (4)
#define HI655X_IRQ_MASK                  (0x0ff)
#define HI655X_IRQ_CLR                   (0x0ff)
#define HI655X_VER_REG                   (0x000)
#define HI655X_VER_REG                   (0x000)
#define HI655X_REG_MAX                   (0x000)

#define PMU_VER_START                    (0x010)
#define PMU_VER_END                      (0x038)
#define ANA_IRQM_REG0                    (0x1b5)

struct hi655x_pmic {
	struct resource *res;
	struct device *dev;
	struct regmap *regmap;
	spinlock_t ssi_hw_lock;
	struct clk *clk;
	struct irq_domain *domain;
	int irq;
	int gpio;
	unsigned int irqs[HI655X_NR_IRQ];
	unsigned int ver;
};
#endif
