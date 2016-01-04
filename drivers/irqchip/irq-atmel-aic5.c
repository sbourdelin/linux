/*
 * Atmel AT91 AIC5 (Advanced Interrupt Controller) driver
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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/bitmap.h>
#include <linux/types.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irqdomain.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/io.h>

#include <asm/exception.h>
#include <asm/mach/irq.h>

/* Number of irq lines managed by AIC */
#define NR_AIC5_IRQS	128

#define AT91_AIC5_SSR		0x0
#define AT91_AIC5_INTSEL_MSK	(0x7f << 0)

#define AT91_AIC5_SMR			0x4

#define AT91_AIC5_SVR			0x8
#define AT91_AIC5_IVR			0x10
#define AT91_AIC5_FVR			0x14
#define AT91_AIC5_ISR			0x18

#define AT91_AIC5_IPR0			0x20
#define AT91_AIC5_IPR1			0x24
#define AT91_AIC5_IPR2			0x28
#define AT91_AIC5_IPR3			0x2c
#define AT91_AIC5_IMR			0x30
#define AT91_AIC5_CISR			0x34

#define AT91_AIC5_IECR			0x40
#define AT91_AIC5_IDCR			0x44
#define AT91_AIC5_ICCR			0x48
#define AT91_AIC5_ISCR			0x4c
#define AT91_AIC5_EOICR			0x38
#define AT91_AIC5_SPU			0x3c
#define AT91_AIC5_DCR			0x6c

#define AT91_AIC5_FFER			0x50
#define AT91_AIC5_FFDR			0x54
#define AT91_AIC5_FFSR			0x58
