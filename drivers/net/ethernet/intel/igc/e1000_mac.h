/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c)  2018 Intel Corporation */

#ifndef _E1000_MAC_H_
#define _E1000_MAC_H_

#include "e1000_hw.h"
#include "e1000_phy.h"
#include "e1000_defines.h"

#ifndef E1000_REMOVED
#define E1000_REMOVED(a) (0)
#endif /* E1000_REMOVED */

/* forward declaration */
s32 igc_check_for_copper_link(struct e1000_hw *hw);
s32 igc_config_fc_after_link_up(struct e1000_hw *hw);
s32 igc_force_mac_fc(struct e1000_hw *hw);

s32 igc_disable_pcie_master(struct e1000_hw *hw);
void igc_init_rx_addrs(struct e1000_hw *hw, u16 rar_count);
s32 igc_setup_link(struct e1000_hw *hw);
void igc_clear_hw_cntrs_base(struct e1000_hw *hw);
s32 igc_get_auto_rd_done(struct e1000_hw *hw);
void igc_put_hw_semaphore(struct e1000_hw *hw);
void igc_rar_set(struct e1000_hw *hw, u8 *addr, u32 index);

void igc_config_collision_dist(struct e1000_hw *hw);

s32 igc_get_bus_info_pcie(struct e1000_hw *hw);
s32 igc_get_speed_and_duplex_copper(struct e1000_hw *hw, u16 *speed,
				    u16 *duplex);

bool igc_enable_mng_pass_thru(struct e1000_hw *hw);

enum e1000_mng_mode {
	e1000_mng_mode_none = 0,
	e1000_mng_mode_asf,
	e1000_mng_mode_pt,
	e1000_mng_mode_ipmi,
	e1000_mng_mode_host_if_only
};

#endif
