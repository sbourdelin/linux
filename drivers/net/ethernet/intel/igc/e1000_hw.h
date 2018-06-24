/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c)  2018 Intel Corporation */

#ifndef _E1000_HW_H_
#define _E1000_HW_H_

#include <linux/types.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>

#include "e1000_regs.h"
#include "e1000_defines.h"
#include "e1000_mac.h"
#include "e1000_nvm.h"
#include "e1000_i225.h"
#include "e1000_base.h"

#define E1000_DEV_ID_I225_LM			0x15F2
#define E1000_DEV_ID_I225_V			0x15F3

/* Forward declaration */
struct e1000_hw;

/* Function pointers for the MAC. */
struct e1000_mac_operations {
	s32 (*check_for_link)(struct e1000_hw *hw);
	s32 (*reset_hw)(struct e1000_hw *hw);
	s32 (*init_hw)(struct e1000_hw *hw);
	s32 (*setup_physical_interface)(struct e1000_hw *hw);
	void (*rar_set)(struct e1000_hw *hw, u8 *address, u32 index);
	s32 (*read_mac_addr)(struct e1000_hw *hw);
	s32 (*get_speed_and_duplex)(struct e1000_hw *hw, u16 *speed,
				    u16 *duplex);
	s32 (*acquire_swfw_sync)(struct e1000_hw *hw, u16 mask);
	void (*release_swfw_sync)(struct e1000_hw *hw, u16 mask);
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

enum e1000_nvm_type {
	e1000_nvm_unknown = 0,
	e1000_nvm_flash_hw,
	e1000_nvm_invm,
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

struct e1000_info {
	s32 (*get_invariants)(struct e1000_hw *hw);
	struct e1000_mac_operations *mac_ops;
	const struct e1000_phy_operations *phy_ops;
	struct e1000_nvm_operations *nvm_ops;
};

extern const struct e1000_info e1000_base_info;

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
	bool get_link_status;
};

struct e1000_nvm_operations {
	s32 (*acquire)(struct e1000_hw *hw);
	s32 (*read)(struct e1000_hw *hw, u16 offset, u16 words, u16 *data);
	void (*release)(struct e1000_hw *hw);
	s32 (*write)(struct e1000_hw *hw, u16 offset, u16 i, u16 *data);
	s32 (*update)(struct e1000_hw *hw);
	s32 (*validate)(struct e1000_hw *hw);
	s32 (*valid_led_default)(struct e1000_hw *hw, u16 *data);
};

struct e1000_nvm_info {
	struct e1000_nvm_operations ops;
	enum e1000_nvm_type type;

	u32 flash_bank_size;
	u32 flash_base_addr;

	u16 word_size;
	u16 delay_usec;
	u16 address_bits;
	u16 opcode_bits;
	u16 page_size;
};

struct e1000_bus_info {
	enum e1000_bus_type type;
	enum e1000_bus_speed speed;
	enum e1000_bus_width width;

	u16 func;
	u16 pci_cmd_word;
};

enum e1000_fc_mode {
	e1000_fc_none = 0,
	e1000_fc_rx_pause,
	e1000_fc_tx_pause,
	e1000_fc_full,
	e1000_fc_default = 0xFF
};

struct e1000_fc_info {
	u32 high_water;     /* Flow control high-water mark */
	u32 low_water;      /* Flow control low-water mark */
	u16 pause_time;     /* Flow control pause timer */
	bool send_xon;      /* Flow control send XON */
	bool strict_ieee;   /* Strict IEEE mode */
	enum e1000_fc_mode current_mode; /* Type of flow control */
	enum e1000_fc_mode requested_mode;
};

struct e1000_dev_spec_base {
	bool global_device_reset;
	bool eee_disable;
	bool clear_semaphore_once;
	bool module_plugged;
	u8 media_port;
};

struct e1000_hw {
	void *back;

	u8 *hw_addr;
	u8 *flash_address;
	unsigned long io_base;

	struct e1000_mac_info  mac;
	struct e1000_fc_info   fc;
	struct e1000_nvm_info  nvm;

	struct e1000_bus_info bus;

	union {
		struct e1000_dev_spec_base	_base;
	} dev_spec;

	u16 device_id;
	u16 subsystem_vendor_id;
	u16 subsystem_device_id;
	u16 vendor_id;

	u8  revision_id;
};

/* Statistics counters collected by the MAC */
struct e1000_hw_stats {
	u64 crcerrs;
	u64 algnerrc;
	u64 symerrs;
	u64 rxerrc;
	u64 mpc;
	u64 scc;
	u64 ecol;
	u64 mcc;
	u64 latecol;
	u64 colc;
	u64 dc;
	u64 tncrs;
	u64 sec;
	u64 cexterr;
	u64 rlec;
	u64 xonrxc;
	u64 xontxc;
	u64 xoffrxc;
	u64 xofftxc;
	u64 fcruc;
	u64 prc64;
	u64 prc127;
	u64 prc255;
	u64 prc511;
	u64 prc1023;
	u64 prc1522;
	u64 gprc;
	u64 bprc;
	u64 mprc;
	u64 gptc;
	u64 gorc;
	u64 gotc;
	u64 rnbc;
	u64 ruc;
	u64 rfc;
	u64 roc;
	u64 rjc;
	u64 mgprc;
	u64 mgpdc;
	u64 mgptc;
	u64 tor;
	u64 tot;
	u64 tpr;
	u64 tpt;
	u64 ptc64;
	u64 ptc127;
	u64 ptc255;
	u64 ptc511;
	u64 ptc1023;
	u64 ptc1522;
	u64 mptc;
	u64 bptc;
	u64 tsctc;
	u64 tsctfc;
	u64 iac;
	u64 icrxptc;
	u64 icrxatc;
	u64 ictxptc;
	u64 ictxatc;
	u64 ictxqec;
	u64 ictxqmtc;
	u64 icrxdmtc;
	u64 icrxoc;
	u64 cbtmpc;
	u64 htdpmc;
	u64 cbrdpc;
	u64 cbrmpc;
	u64 rpthc;
	u64 hgptc;
	u64 htcbdpc;
	u64 hgorc;
	u64 hgotc;
	u64 lenerrs;
	u64 scvpc;
	u64 hrmpc;
	u64 doosync;
	u64 o2bgptc;
	u64 o2bspc;
	u64 b2ospc;
	u64 b2ogprc;
};

struct net_device *igc_get_hw_dev(struct e1000_hw *hw);
#define hw_dbg(format, arg...) \
	netdev_dbg(igc_get_hw_dev(hw), format, ##arg)

/* These functions must be implemented by drivers */
s32  igc_read_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value);
s32  igc_write_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value);
void igc_read_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value);
void igc_write_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value);

#endif /* _E1000_HW_H_ */
