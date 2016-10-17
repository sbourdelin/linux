/*
 * Copyright (C) 2012 Thomas Petazzoni
 *
 * Thomas Petazzoni <thomas.petazzoni@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef _LINUX_IRQCHIP_H
#define _LINUX_IRQCHIP_H

#include <linux/acpi.h>
#include <linux/of.h>

/*
 * This macro must be used by the different irqchip drivers to declare
 * the association between their DT compatible string and their
 * initialization function.
 *
 * @name: name that must be unique accross all IRQCHIP_DECLARE of the
 * same file.
 * @compstr: compatible string of the irqchip driver
 * @fn: initialization function
 */
#define IRQCHIP_DECLARE(name, compat, fn) OF_DECLARE_2(irqchip, name, compat, fn)

/*
 * This macro must be used by the different irqchip drivers to declare
 * the association between their version and their initialization function.
 * Two syntaxes are supported depending on the table where the irqchip device
 * is declared:
 *
 * - MADT irqchip syntax, which requires the following five arguments:
 *
 * @name: name that must be unique accross all IRQCHIP_ACPI_DECLARE of the
 * same file.
 * @subtable: Subtable to be identified in MADT
 * @validate: Function to be called on that subtable to check its validity.
 *            Can be NULL.
 * @data: data to be checked by the validate function.
 * @fn: initialization function
 *
 * - DSDT irqchip syntax, which requires the following three arguments:
 *
 * @name: name that must be unique across all IRQCHIP_ACPI_DECLARE of the
 * same file.
 * @hid: _HID of the DSDT device
 * @fn: initialization function
 */

#define IRQCHIP_ACPI_DECLARE(...)					\
	__IRQCHIP_ACPI_DECLARE(__VA_ARGS__, MADT, _unused, DSDT)(__VA_ARGS__)

#ifdef CONFIG_IRQCHIP
void irqchip_init(void);
#else
static inline void irqchip_init(void) {}
#endif

#endif
