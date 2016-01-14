/*
 * pcie-dra7xx - Platform data for PCIe controller
 *
 * Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com/
 *
 * Authors: Kishon Vijay Abraham I <kishon@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __PCI_DRA7XX_H
#define __PCI_DRA7XX_H

/**
 * struct pci_dra7xx_platform_data - platform data specific to pci in dra7xx
 * @reset_name: name of the reset line
 * @assert_reset: callback for performing assert reset operation
 * @deassert_reset: callback for performing deassert reset operation
 */
struct pci_dra7xx_platform_data {
	const char *reset_name;

	int (*assert_reset)(struct platform_device *pdev, const char *name);
	int (*deassert_reset)(struct platform_device *pdev, const char *name);
};

#endif /* __PCI_DRA7XX_H */
