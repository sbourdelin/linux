/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c)  2018 Intel Corporation */

#ifndef _E1000_HW_H_
#define _E1000_HW_H_

#include <linux/types.h>
#include <linux/if_ether.h>
#include "e1000_regs.h"
#include "e1000_defines.h"
#include "e1000_mac.h"
#include "e1000_i225.h"

#define E1000_DEV_ID_I225_LM			0x15F2
#define E1000_DEV_ID_I225_V			0x15F3

/* Forward declaration */
struct e1000_hw;

/* Function pointers for the MAC. */
struct e1000_mac_operations {
};

enum e1000_mac_type {
	e1000_undefined = 0,
	e1000_i225,
	e1000_num_macs  /* List is 1-based, so subtract 1 for true count. */
};

enum e1000_phy_type {
	e1000_phy_unknown = 0,
	e1000_phy_none,
	e1000_phy_i225,
};

enum e1000_bus_type {
	e1000_bus_type_unknown = 0,
	e1000_bus_type_pci_express,
	e1000_bus_type_reserved
};

enum e1000_bus_speed {
	e1000_bus_speed_unknown = 0,
	e1000_bus_speed_2500,
	e1000_bus_speed_5000,
	e1000_bus_speed_reserved
};

enum e1000_bus_width {
	e1000_bus_width_unknown = 0,
	e1000_bus_width_pcie_x1,
	e1000_bus_width_pcie_x2,
	e1000_bus_width_pcie_x4 = 4,
	e1000_bus_width_pcie_x8 = 8,
	e1000_bus_width_reserved
};

struct e1000_mac_info {
	struct e1000_mac_operations ops;

	u8 addr[ETH_ALEN];
	u8 perm_addr[ETH_ALEN];

	enum e1000_mac_type type;

	u32 collision_delta;
	u32 ledctl_default;
	u32 ledctl_mode1;
	u32 ledctl_mode2;
	u32 mc_filter_type;
	u32 tx_packet_delta;
	u32 txcw;

	u16 mta_reg_count;
	u16 uta_reg_count;

	/* Maximum size of the MTA register table in all supported adapters */
#define MAX_MTA_REG 128
	u32 mta_shadow[MAX_MTA_REG];
	u16 rar_entry_count;

	u8  forced_speed_duplex;

	bool adaptive_ifs;
	bool has_fwsm;
	bool arc_subsystem_valid;

	bool autoneg;
	bool autoneg_failed;
};

struct e1000_bus_info {
	enum e1000_bus_type type;
	enum e1000_bus_speed speed;
	enum e1000_bus_width width;

	u16 func;
	u16 pci_cmd_word;
};

struct e1000_hw {
	void *back;

	u8 *hw_addr;
	u8 *flash_address;
	unsigned long io_base;

	struct e1000_mac_info  mac;

	struct e1000_bus_info bus;

	u16 device_id;
	u16 subsystem_vendor_id;
	u16 subsystem_device_id;
	u16 vendor_id;

	u8  revision_id;
};

/* These functions must be implemented by drivers */
s32  igc_read_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value);
s32  igc_write_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value);
void igc_read_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value);
void igc_write_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value);

#endif /* _E1000_HW_H_ */
