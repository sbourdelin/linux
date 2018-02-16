// SPDX-License-Identifier: GPL-2.0
/**
 * dwc3-pci.h - PCI Specific glue layer header
 *
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 *	    Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 */

#ifndef __DWC3_PCI_H
#define __DWC3_PCI_H

#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>

/**
 * struct dwc3_pci - Driver private structure
 * @dwc3: child dwc3 platform_device
 * @pci: our link to PCI bus
 * @guid: _DSM GUID
 * @has_dsm_for_pm: true for devices which need to run _DSM on runtime PM
 * @properties: null terminated array of device properties
 * @property_array_size: property array size
 */
struct dwc3_pci {
	struct platform_device *dwc3;
	struct pci_dev *pci;

	guid_t guid;

	unsigned int has_dsm_for_pm:1;
	struct work_struct wakeup_work;

	struct property_entry *properties;
	int property_array_size;
};

int dwc3_pci_add_one_property(struct dwc3_pci *dwc,
			      struct property_entry property);
int dwc3_pci_add_properties(struct dwc3_pci *dwc,
			    struct property_entry *properties);
int dwc3_pci_add_platform_device(struct dwc3_pci *dwc);

#endif /* __DWC3_PCI_H */
