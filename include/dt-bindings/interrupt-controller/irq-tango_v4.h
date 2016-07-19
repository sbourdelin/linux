/*
 * Copyright (C) 2014 Sebastian Frias <sf84@laposte.net>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef _DT_BINDINGS_INTERRUPT_CONTROLLER_SIGMA_SMP_TANGO_V4_H
#define _DT_BINDINGS_INTERRUPT_CONTROLLER_SIGMA_SMP_TANGO_V4_H

#define SIGMA_HWIRQ (0xAA)
#define SIGMA_SWIRQ (0x55)

#define SIGMA_MAX_IRQGROUPS (16)

#define SIGMA_IRQGROUP_KEY (0x80)

/* NOTE: SW IRQs have their own IRQ group reserved, that's why there are
 * only (SIGMA_MAX_IRQGROUPS - 1)=15 groups listed below and available
 */
#define SIGMA_IRQGROUP_1  (SIGMA_IRQGROUP_KEY + 0x1)
#define SIGMA_IRQGROUP_2  (SIGMA_IRQGROUP_KEY + 0x2)
#define SIGMA_IRQGROUP_3  (SIGMA_IRQGROUP_KEY + 0x3)
#define SIGMA_IRQGROUP_4  (SIGMA_IRQGROUP_KEY + 0x4)
#define SIGMA_IRQGROUP_5  (SIGMA_IRQGROUP_KEY + 0x5)
#define SIGMA_IRQGROUP_6  (SIGMA_IRQGROUP_KEY + 0x6)
#define SIGMA_IRQGROUP_7  (SIGMA_IRQGROUP_KEY + 0x7)
#define SIGMA_IRQGROUP_8  (SIGMA_IRQGROUP_KEY + 0x8)
#define SIGMA_IRQGROUP_9  (SIGMA_IRQGROUP_KEY + 0x9)
#define SIGMA_IRQGROUP_10 (SIGMA_IRQGROUP_KEY + 0xa)
#define SIGMA_IRQGROUP_11 (SIGMA_IRQGROUP_KEY + 0xb)
#define SIGMA_IRQGROUP_12 (SIGMA_IRQGROUP_KEY + 0xc)
#define SIGMA_IRQGROUP_13 (SIGMA_IRQGROUP_KEY + 0xd)
#define SIGMA_IRQGROUP_14 (SIGMA_IRQGROUP_KEY + 0xe)
#define SIGMA_IRQGROUP_15 (SIGMA_IRQGROUP_KEY + 0xf)

#endif
