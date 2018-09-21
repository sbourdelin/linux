// SPDX-License-Identifier: GPL-2.0
/* Copyright (c)  2018 Intel Corporation */

#include <linux/pci.h>
#include "igc_hw.h"

/**
 *  igc_get_bus_info_pcie - Get PCIe bus information
 *  @hw: pointer to the HW structure
 *
 *  Determines and stores the system bus information for a particular
 *  network interface.  The following bus information is determined and stored:
 *  bus speed, bus width, type (PCIe), and PCIe function.
 **/
s32 igc_get_bus_info_pcie(struct igc_hw *hw)
{
	struct igc_bus_info *bus = &hw->bus;
	u16 pcie_link_status;
	s32 ret_val;
	u32 reg = 0;

	bus->type = igc_bus_type_pci_express;

	ret_val = igc_read_pcie_cap_reg(hw, PCI_EXP_LNKSTA,
					&pcie_link_status);
	if (ret_val) {
		bus->width = igc_bus_width_unknown;
		bus->speed = igc_bus_speed_unknown;
	} else {
		switch (pcie_link_status & PCI_EXP_LNKSTA_CLS) {
		case PCI_EXP_LNKSTA_CLS_2_5GB:
			bus->speed = igc_bus_speed_2500;
			break;
		case PCI_EXP_LNKSTA_CLS_5_0GB:
			bus->speed = igc_bus_speed_5000;
			break;
		default:
			bus->speed = igc_bus_speed_unknown;
			break;
		}

		bus->width = (enum igc_bus_width)((pcie_link_status &
						     PCI_EXP_LNKSTA_NLW) >>
						     PCI_EXP_LNKSTA_NLW_SHIFT);
	}

	reg = rd32(IGC_STATUS);
	bus->func = (reg & IGC_STATUS_FUNC_MASK) >> IGC_STATUS_FUNC_SHIFT;

	return 0;
}
