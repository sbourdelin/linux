/*
 * Atmel AT91 common AIC (Advanced Interrupt Controller) header file
 *
 *  Copyright (C) 2004 SAN People
 *  Copyright (C) 2004 ATMEL
 *  Copyright (C) Rick Bronson
 *  Copyright (C) 2014 Free Electrons
 *
 *  Author: Boris BREZILLON <boris.brezillon@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __IRQ_ATMEL_AIC_COMMON_H
#define __IRQ_ATMEL_AIC_COMMON_H

#define AIC_IRQS_PER_CHIP	32

int aic_common_set_type(struct irq_data *d, unsigned type, unsigned *val);

struct irq_domain *__init aic_common_of_init(struct device_node *node,
					     const char *name, int nirqs);

#endif /* __IRQ_ATMEL_AIC_COMMON_H */
