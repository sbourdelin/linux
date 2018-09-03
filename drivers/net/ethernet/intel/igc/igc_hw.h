/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c)  2018 Intel Corporation */

#ifndef _IGC_HW_H_
#define _IGC_HW_H_

#include <linux/types.h>
#include <linux/if_ether.h>
#include "igc_regs.h"
#include "igc_defines.h"
#include "igc_mac.h"
#include "igc_i225.h"

#define IGC_DEV_ID_I225_LM			0x15F2
#define IGC_DEV_ID_I225_V			0x15F3

/* forward declaration */
struct igc_hw;

/* Function pointers for the MAC. */
struct igc_mac_operations {
};

enum igc_mac_type {
	igc_undefined = 0,
	igc_i225,
	igc_num_macs  /* List is 1-based, so subtract 1 for true count. */
};

enum igc_phy_type {
	igc_phy_unknown = 0,
	igc_phy_none,
	igc_phy_i225,
};

enum igc_bus_type {
	igc_bus_type_unknown = 0,
	igc_bus_type_pci_express,
	igc_bus_type_reserved
};

enum igc_bus_speed {
	igc_bus_speed_unknown = 0,
	igc_bus_speed_2500,
	igc_bus_speed_5000,
	igc_bus_speed_reserved
};

enum igc_bus_width {
	igc_bus_width_unknown = 0,
	igc_bus_width_pcie_x1 = 1,
	igc_bus_width_pcie_x2 = 2,
	igc_bus_width_pcie_x4 = 4,
	igc_bus_width_pcie_x8 = 8,
	igc_bus_width_reserved
};

struct igc_mac_info {
	struct igc_mac_operations ops;

	u8 addr[ETH_ALEN];
	u8 perm_addr[ETH_ALEN];

	enum igc_mac_type type;

	u32 collision_delta;
	u32 ledctl_default;
	u32 ledctl_mode1;
	u32 ledctl_mode2;
	u32 mc_filter_type;
	u32 tx_packet_delta;
	u32 txcw;

	u16 mta_reg_count;
	u16 uta_reg_count;

	u16 rar_entry_count;

	u8  forced_speed_duplex;

	bool adaptive_ifs;
	bool has_fwsm;
	bool arc_subsystem_valid;

	bool autoneg;
	bool autoneg_failed;
	bool get_link_status;
};

struct igc_bus_info {
	enum igc_bus_type type;
	enum igc_bus_speed speed;
	enum igc_bus_width width;

	u16 func;
	u16 pci_cmd_word;
};

struct igc_hw {
	void *back;

	u8 __iomem *hw_addr;
	unsigned long io_base;

	struct igc_mac_info  mac;

	struct igc_bus_info bus;

	u16 device_id;
	u16 subsystem_vendor_id;
	u16 subsystem_device_id;
	u16 vendor_id;

	u8  revision_id;
};

/* These functions must be implemented by drivers */
s32  igc_read_pcie_cap_reg(struct igc_hw *hw, u32 reg, u16 *value);
s32  igc_write_pcie_cap_reg(struct igc_hw *hw, u32 reg, u16 *value);
void igc_read_pci_cfg(struct igc_hw *hw, u32 reg, u16 *value);
void igc_write_pci_cfg(struct igc_hw *hw, u32 reg, u16 *value);

#endif /* _IGC_HW_H_ */
