/*
 * lpc_ich_apl.h - Intel In-Vehicle Infotainment (IVI) systems used in cars
 *                 support
 *
 * Copyright (C) 2016, Intel Corporation
 *
 * Author: Tan, Jui Nee <jui.nee.tan@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LPC_ICH_APL_H__
#define __LPC_ICH_APL_H__

#include <linux/pci.h>

#if IS_ENABLED(CONFIG_X86_INTEL_IVI)
int lpc_ich_add_gpio(struct pci_dev *dev, enum lpc_chipsets chipset);
#else /* CONFIG_X86_INTEL_IVI is not set */
static inline int lpc_ich_add_gpio(struct pci_dev *dev,
	enum lpc_chipsets chipset)
{
	return -ENODEV;
}
#endif

#endif
