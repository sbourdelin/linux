/*
 * File:	pci-of.c
 * Purpose:	Provide PCI PM/wakeup support in OF systems
 *
 * Copyright (C) 2017 Google, Inc.
 *	Brian Norris <briannorris@chromium.org>
 *
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_wakeirq.h>
#include "pci.h"

static bool of_pci_power_manageable(struct pci_dev *dev)
{
	return false;
}

static int of_pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
	return -ENOSYS;
}

static pci_power_t of_pci_get_power_state(struct pci_dev *dev)
{
	return PCI_UNKNOWN;
}

static pci_power_t of_pci_choose_state(struct pci_dev *pdev)
{
	return PCI_POWER_ERROR;
}

static int of_pci_wakeup(struct pci_dev *dev, bool enable)
{
	if (enable)
		dev_pm_enable_wake_irq(&dev->dev);
	else
		dev_pm_disable_wake_irq(&dev->dev);
	return 0;
}

static bool of_pci_need_resume(struct pci_dev *dev)
{
	return false;
}

static const struct pci_platform_pm_ops of_pci_platform_pm = {
	.is_manageable = of_pci_power_manageable,
	.set_state = of_pci_set_power_state,
	.get_state = of_pci_get_power_state,
	.choose_state = of_pci_choose_state,
	.set_wakeup = of_pci_wakeup,
	.need_resume = of_pci_need_resume,
};

static int __init of_pci_init(void)
{
	if (!acpi_disabled)
		return 0;

	pci_set_platform_pm(&of_pci_platform_pm);

	return 0;
}
arch_initcall(of_pci_init);
