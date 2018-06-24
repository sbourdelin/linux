/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c)  2018 Intel Corporation */

#ifndef _E1000_PHY_H_
#define _E1000_PHY_H_

#include "e1000_mac.h"

s32  igc_check_reset_block(struct e1000_hw *hw);
s32  igc_phy_hw_reset(struct e1000_hw *hw);
s32  igc_get_phy_id(struct e1000_hw *hw);
s32  igc_phy_has_link(struct e1000_hw *hw, u32 iterations,
		      u32 usec_interval, bool *success);
s32 igc_check_downshift(struct e1000_hw *hw);
void igc_power_up_phy_copper(struct e1000_hw *hw);
void igc_power_down_phy_copper(struct e1000_hw *hw);

s32  igc_write_phy_reg_gpy(struct e1000_hw *hw, u32 offset, u16 data);
s32  igc_read_phy_reg_gpy(struct e1000_hw *hw, u32 offset, u16 *data);

#endif
