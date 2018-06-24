/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c)  2018 Intel Corporation */

#ifndef _E1000_MAC_H_
#define _E1000_MAC_H_

#include "e1000_hw.h"
#include "e1000_defines.h"

#ifndef E1000_REMOVED
#define E1000_REMOVED(a) (0)
#endif /* E1000_REMOVED */

/* forward declaration */
s32  igc_disable_pcie_master(struct e1000_hw *hw);
void igc_init_rx_addrs(struct e1000_hw *hw, u16 rar_count);
s32  igc_setup_link(struct e1000_hw *hw);
void igc_clear_hw_cntrs_base(struct e1000_hw *hw);
s32 igc_get_auto_rd_done(struct e1000_hw *hw);
void igc_put_hw_semaphore(struct e1000_hw *hw);

s32  igc_get_bus_info_pcie(struct e1000_hw *hw);

#endif
