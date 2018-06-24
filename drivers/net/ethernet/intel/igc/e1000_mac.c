// SPDX-License-Identifier: GPL-2.0
/* Copyright (c)  2018 Intel Corporation */

#include <linux/pci.h>
#include <linux/delay.h>

#include "e1000_mac.h"
#include "e1000_hw.h"

/* forward declaration */
static s32 igc_set_default_fc(struct e1000_hw *hw);
static s32 igc_set_fc_watermarks(struct e1000_hw *hw);

/**
 *  igc_get_bus_info_pcie - Get PCIe bus information
 *  @hw: pointer to the HW structure
 *
 *  Determines and stores the system bus information for a particular
 *  network interface.  The following bus information is determined and stored:
 *  bus speed, bus width, type (PCIe), and PCIe function.
 **/
s32 igc_get_bus_info_pcie(struct e1000_hw *hw)
{
	struct e1000_bus_info *bus = &hw->bus;
	u16 pcie_link_status;
	s32 ret_val;
	u32 reg = 0;

	bus->type = e1000_bus_type_pci_express;

	ret_val = igc_read_pcie_cap_reg(hw, PCI_EXP_LNKSTA,
					&pcie_link_status);
	if (ret_val) {
		bus->width = e1000_bus_width_unknown;
		bus->speed = e1000_bus_speed_unknown;
	} else {
		switch (pcie_link_status & PCI_EXP_LNKSTA_CLS) {
		case PCI_EXP_LNKSTA_CLS_2_5GB:
			bus->speed = e1000_bus_speed_2500;
			break;
		case PCI_EXP_LNKSTA_CLS_5_0GB:
			bus->speed = e1000_bus_speed_5000;
			break;
		default:
			bus->speed = e1000_bus_speed_unknown;
			break;
		}

		bus->width = (enum e1000_bus_width)((pcie_link_status &
						     PCI_EXP_LNKSTA_NLW) >>
						     PCI_EXP_LNKSTA_NLW_SHIFT);
	}

	reg = rd32(E1000_STATUS);
	bus->func = (reg & E1000_STATUS_FUNC_MASK) >> E1000_STATUS_FUNC_SHIFT;

	return 0;
}

/**
 *  igc_disable_pcie_master - Disables PCI-express master access
 *  @hw: pointer to the HW structure
 *
 *  Returns 0 (0) if successful, else returns -10
 *  (-E1000_ERR_MASTER_REQUESTS_PENDING) if master disable bit has not caused
 *  the master requests to be disabled.
 *
 *  Disables PCI-Express master access and verifies there are no pending
 *  requests.
 **/
s32 igc_disable_pcie_master(struct e1000_hw *hw)
{
	u32 ctrl;
	s32 timeout = MASTER_DISABLE_TIMEOUT;
	s32 ret_val = 0;

	if (hw->bus.type != e1000_bus_type_pci_express)
		goto out;

	ctrl = rd32(E1000_CTRL);
	ctrl |= E1000_CTRL_GIO_MASTER_DISABLE;
	wr32(E1000_CTRL, ctrl);

	while (timeout) {
		if (!(rd32(E1000_STATUS) &
		    E1000_STATUS_GIO_MASTER_ENABLE))
			break;
		usleep_range(2000, 3000);
		timeout--;
	}

	if (!timeout) {
		hw_dbg("Master requests are pending.\n");
		ret_val = -E1000_ERR_MASTER_REQUESTS_PENDING;
		goto out;
	}

out:
	return ret_val;
}

/**
 *  igc_init_rx_addrs - Initialize receive address's
 *  @hw: pointer to the HW structure
 *  @rar_count: receive address registers
 *
 *  Setups the receive address registers by setting the base receive address
 *  register to the devices MAC address and clearing all the other receive
 *  address registers to 0.
 **/
void igc_init_rx_addrs(struct e1000_hw *hw, u16 rar_count)
{
	u32 i;
	u8 mac_addr[ETH_ALEN] = {0};

	/* Setup the receive address */
	hw_dbg("Programming MAC Address into RAR[0]\n");

	hw->mac.ops.rar_set(hw, hw->mac.addr, 0);

	/* Zero out the other (rar_entry_count - 1) receive addresses */
	hw_dbg("Clearing RAR[1-%u]\n", rar_count - 1);
	for (i = 1; i < rar_count; i++)
		hw->mac.ops.rar_set(hw, mac_addr, i);
}

/**
 *  igc_setup_link - Setup flow control and link settings
 *  @hw: pointer to the HW structure
 *
 *  Determines which flow control settings to use, then configures flow
 *  control.  Calls the appropriate media-specific link configuration
 *  function.  Assuming the adapter has a valid link partner, a valid link
 *  should be established.  Assumes the hardware has previously been reset
 *  and the transmitter and receiver are not enabled.
 **/
s32 igc_setup_link(struct e1000_hw *hw)
{
	s32 ret_val = 0;

	/* In the case of the phy reset being blocked, we already have a link.
	 * We do not need to set it up again.
	 */
	if (igc_check_reset_block(hw))
		goto out;

	/* If requested flow control is set to default, set flow control
	 * based on the EEPROM flow control settings.
	 */
	if (hw->fc.requested_mode == e1000_fc_default) {
		ret_val = igc_set_default_fc(hw);
		if (ret_val)
			goto out;
	}

	/* We want to save off the original Flow Control configuration just
	 * in case we get disconnected and then reconnected into a different
	 * hub or switch with different Flow Control capabilities.
	 */
	hw->fc.current_mode = hw->fc.requested_mode;

	hw_dbg("After fix-ups FlowControl is now = %x\n", hw->fc.current_mode);

	/* Call the necessary media_type subroutine to configure the link. */
	ret_val = hw->mac.ops.setup_physical_interface(hw);
	if (ret_val)
		goto out;

	/* Initialize the flow control address, type, and PAUSE timer
	 * registers to their default values.  This is done even if flow
	 * control is disabled, because it does not hurt anything to
	 * initialize these registers.
	 */
	hw_dbg("Initializing the Flow Control address, type and timer regs\n");
	wr32(E1000_FCT, FLOW_CONTROL_TYPE);
	wr32(E1000_FCAH, FLOW_CONTROL_ADDRESS_HIGH);
	wr32(E1000_FCAL, FLOW_CONTROL_ADDRESS_LOW);

	wr32(E1000_FCTTV, hw->fc.pause_time);

	ret_val = igc_set_fc_watermarks(hw);

out:

	return ret_val;
}

/**
 *  igc_set_default_fc - Set flow control default values
 *  @hw: pointer to the HW structure
 *
 *  Read the EEPROM for the default values for flow control and store the
 *  values.
 **/
static s32 igc_set_default_fc(struct e1000_hw *hw)
{
	hw->fc.requested_mode = e1000_fc_full;
	return 0;
}

/**
 *  igc_force_mac_fc - Force the MAC's flow control settings
 *  @hw: pointer to the HW structure
 *
 *  Force the MAC's flow control settings.  Sets the TFCE and RFCE bits in the
 *  device control register to reflect the adapter settings.  TFCE and RFCE
 *  need to be explicitly set by software when a copper PHY is used because
 *  autonegotiation is managed by the PHY rather than the MAC.  Software must
 *  also configure these bits when link is forced on a fiber connection.
 **/
s32 igc_force_mac_fc(struct e1000_hw *hw)
{
	u32 ctrl;
	s32 ret_val = 0;

	ctrl = rd32(E1000_CTRL);

	/* Because we didn't get link via the internal auto-negotiation
	 * mechanism (we either forced link or we got link via PHY
	 * auto-neg), we have to manually enable/disable transmit an
	 * receive flow control.
	 *
	 * The "Case" statement below enables/disable flow control
	 * according to the "hw->fc.current_mode" parameter.
	 *
	 * The possible values of the "fc" parameter are:
	 *      0:  Flow control is completely disabled
	 *      1:  Rx flow control is enabled (we can receive pause
	 *          frames but not send pause frames).
	 *      2:  Tx flow control is enabled (we can send pause frames
	 *          frames but we do not receive pause frames).
	 *      3:  Both Rx and TX flow control (symmetric) is enabled.
	 *  other:  No other values should be possible at this point.
	 */
	hw_dbg("hw->fc.current_mode = %u\n", hw->fc.current_mode);

	switch (hw->fc.current_mode) {
	case e1000_fc_none:
		ctrl &= (~(E1000_CTRL_TFCE | E1000_CTRL_RFCE));
		break;
	case e1000_fc_rx_pause:
		ctrl &= (~E1000_CTRL_TFCE);
		ctrl |= E1000_CTRL_RFCE;
		break;
	case e1000_fc_tx_pause:
		ctrl &= (~E1000_CTRL_RFCE);
		ctrl |= E1000_CTRL_TFCE;
		break;
	case e1000_fc_full:
		ctrl |= (E1000_CTRL_TFCE | E1000_CTRL_RFCE);
		break;
	default:
		hw_dbg("Flow control param set incorrectly\n");
		ret_val = -E1000_ERR_CONFIG;
		goto out;
	}

	wr32(E1000_CTRL, ctrl);

out:
	return ret_val;
}

/**
 *  igc_set_fc_watermarks - Set flow control high/low watermarks
 *  @hw: pointer to the HW structure
 *
 *  Sets the flow control high/low threshold (watermark) registers.  If
 *  flow control XON frame transmission is enabled, then set XON frame
 *  tansmission as well.
 **/
static s32 igc_set_fc_watermarks(struct e1000_hw *hw)
{
	s32 ret_val = 0;
	u32 fcrtl = 0, fcrth = 0;

	/* Set the flow control receive threshold registers.  Normally,
	 * these registers will be set to a default threshold that may be
	 * adjusted later by the driver's runtime code.  However, if the
	 * ability to transmit pause frames is not enabled, then these
	 * registers will be set to 0.
	 */
	if (hw->fc.current_mode & e1000_fc_tx_pause) {
		/* We need to set up the Receive Threshold high and low water
		 * marks as well as (optionally) enabling the transmission of
		 * XON frames.
		 */
		fcrtl = hw->fc.low_water;
		if (hw->fc.send_xon)
			fcrtl |= E1000_FCRTL_XONE;

		fcrth = hw->fc.high_water;
	}
	wr32(E1000_FCRTL, fcrtl);
	wr32(E1000_FCRTH, fcrth);

	return ret_val;
}

/**
 *  igc_clear_hw_cntrs_base - Clear base hardware counters
 *  @hw: pointer to the HW structure
 *
 *  Clears the base hardware counters by reading the counter registers.
 **/
void igc_clear_hw_cntrs_base(struct e1000_hw *hw)
{
	rd32(E1000_CRCERRS);
	rd32(E1000_SYMERRS);
	rd32(E1000_MPC);
	rd32(E1000_SCC);
	rd32(E1000_ECOL);
	rd32(E1000_MCC);
	rd32(E1000_LATECOL);
	rd32(E1000_COLC);
	rd32(E1000_DC);
	rd32(E1000_SEC);
	rd32(E1000_RLEC);
	rd32(E1000_XONRXC);
	rd32(E1000_XONTXC);
	rd32(E1000_XOFFRXC);
	rd32(E1000_XOFFTXC);
	rd32(E1000_FCRUC);
	rd32(E1000_GPRC);
	rd32(E1000_BPRC);
	rd32(E1000_MPRC);
	rd32(E1000_GPTC);
	rd32(E1000_GORCL);
	rd32(E1000_GORCH);
	rd32(E1000_GOTCL);
	rd32(E1000_GOTCH);
	rd32(E1000_RNBC);
	rd32(E1000_RUC);
	rd32(E1000_RFC);
	rd32(E1000_ROC);
	rd32(E1000_RJC);
	rd32(E1000_TORL);
	rd32(E1000_TORH);
	rd32(E1000_TOTL);
	rd32(E1000_TOTH);
	rd32(E1000_TPR);
	rd32(E1000_TPT);
	rd32(E1000_MPTC);
	rd32(E1000_BPTC);

	rd32(E1000_PRC64);
	rd32(E1000_PRC127);
	rd32(E1000_PRC255);
	rd32(E1000_PRC511);
	rd32(E1000_PRC1023);
	rd32(E1000_PRC1522);
	rd32(E1000_PTC64);
	rd32(E1000_PTC127);
	rd32(E1000_PTC255);
	rd32(E1000_PTC511);
	rd32(E1000_PTC1023);
	rd32(E1000_PTC1522);

	rd32(E1000_ALGNERRC);
	rd32(E1000_RXERRC);
	rd32(E1000_TNCRS);
	rd32(E1000_CEXTERR);
	rd32(E1000_TSCTC);
	rd32(E1000_TSCTFC);

	rd32(E1000_MGTPRC);
	rd32(E1000_MGTPDC);
	rd32(E1000_MGTPTC);

	rd32(E1000_IAC);
	rd32(E1000_ICRXOC);

	rd32(E1000_ICRXPTC);
	rd32(E1000_ICRXATC);
	rd32(E1000_ICTXPTC);
	rd32(E1000_ICTXATC);
	rd32(E1000_ICTXQEC);
	rd32(E1000_ICTXQMTC);
	rd32(E1000_ICRXDMTC);

	rd32(E1000_CBTMPC);
	rd32(E1000_HTDPMC);
	rd32(E1000_CBRMPC);
	rd32(E1000_RPTHC);
	rd32(E1000_HGPTC);
	rd32(E1000_HTCBDPC);
	rd32(E1000_HGORCL);
	rd32(E1000_HGORCH);
	rd32(E1000_HGOTCL);
	rd32(E1000_HGOTCH);
	rd32(E1000_LENERRS);
}

/**
 *  igc_rar_set - Set receive address register
 *  @hw: pointer to the HW structure
 *  @addr: pointer to the receive address
 *  @index: receive address array register
 *
 *  Sets the receive address array register at index to the address passed
 *  in by addr.
 **/
void igc_rar_set(struct e1000_hw *hw, u8 *addr, u32 index)
{
	u32 rar_low, rar_high;

	/* HW expects these in little endian so we reverse the byte order
	 * from network order (big endian) to little endian
	 */
	rar_low = ((u32)addr[0] |
		   ((u32)addr[1] << 8) |
		   ((u32)addr[2] << 16) | ((u32)addr[3] << 24));

	rar_high = ((u32)addr[4] | ((u32)addr[5] << 8));

	/* If MAC address zero, no need to set the AV bit */
	if (rar_low || rar_high)
		rar_high |= E1000_RAH_AV;

	/* Some bridges will combine consecutive 32-bit writes into
	 * a single burst write, which will malfunction on some parts.
	 * The flushes avoid this.
	 */
	wr32(E1000_RAL(index), rar_low);
	wrfl();
	wr32(E1000_RAH(index), rar_high);
	wrfl();
}

/**
 *  igc_check_for_copper_link - Check for link (Copper)
 *  @hw: pointer to the HW structure
 *
 *  Checks to see of the link status of the hardware has changed.  If a
 *  change in link status has been detected, then we read the PHY registers
 *  to get the current speed/duplex if link exists.
 **/
s32 igc_check_for_copper_link(struct e1000_hw *hw)
{
	struct e1000_mac_info *mac = &hw->mac;
	s32 ret_val;
	bool link;

	/* We only want to go out to the PHY registers to see if Auto-Neg
	 * has completed and/or if our link status has changed.  The
	 * get_link_status flag is set upon receiving a Link Status
	 * Change or Rx Sequence Error interrupt.
	 */
	if (!mac->get_link_status) {
		ret_val = 0;
		goto out;
	}

	/* First we want to see if the MII Status Register reports
	 * link.  If so, then we want to get the current speed/duplex
	 * of the PHY.
	 */
	ret_val = igc_phy_has_link(hw, 1, 0, &link);

	if (ret_val)
		goto out;

	if (!link)
		goto out; /* No link detected */

	mac->get_link_status = false;

	/* Check if there was DownShift, must be checked
	 * immediately after link-up
	 */
	igc_check_downshift(hw);

	/* If we are forcing speed/duplex, then we simply return since
	 * we have already determined whether we have link or not.
	 */
	if (!mac->autoneg) {
		ret_val = -E1000_ERR_CONFIG;
		goto out;
	}

	/* Auto-Neg is enabled.  Auto Speed Detection takes care
	 * of MAC speed/duplex configuration.  So we only need to
	 * configure Collision Distance in the MAC.
	 */
	igc_config_collision_dist(hw);

	/* Configure Flow Control now that Auto-Neg has completed.
	 * First, we need to restore the desired flow control
	 * settings because we may have had to re-autoneg with a
	 * different link partner.
	 */
	ret_val = igc_config_fc_after_link_up(hw);
	if (ret_val)
		hw_dbg("Error configuring flow control\n");

out:
	return ret_val;
}

/**
 *  igc_config_collision_dist - Configure collision distance
 *  @hw: pointer to the HW structure
 *
 *  Configures the collision distance to the default value and is used
 *  during link setup. Currently no func pointer exists and all
 *  implementations are handled in the generic version of this function.
 **/
void igc_config_collision_dist(struct e1000_hw *hw)
{
	u32 tctl;

	tctl = rd32(E1000_TCTL);

	tctl &= ~E1000_TCTL_COLD;
	tctl |= E1000_COLLISION_DISTANCE << E1000_COLD_SHIFT;

	wr32(E1000_TCTL, tctl);
	wrfl();
}

/**
 *  igc_config_fc_after_link_up - Configures flow control after link
 *  @hw: pointer to the HW structure
 *
 *  Checks the status of auto-negotiation after link up to ensure that the
 *  speed and duplex were not forced.  If the link needed to be forced, then
 *  flow control needs to be forced also.  If auto-negotiation is enabled
 *  and did not fail, then we configure flow control based on our link
 *  partner.
 **/
s32 igc_config_fc_after_link_up(struct e1000_hw *hw)
{
	struct e1000_mac_info *mac = &hw->mac;
	s32 ret_val = 0;
	u16 mii_status_reg, mii_nway_adv_reg, mii_nway_lp_ability_reg;
	u16 speed, duplex;

	/* Check for the case where we have fiber media and auto-neg failed
	 * so we had to force link.  In this case, we need to force the
	 * configuration of the MAC to match the "fc" parameter.
	 */
	if (mac->autoneg_failed) {
		if (hw->phy.media_type == e1000_media_type_copper)
			ret_val = igc_force_mac_fc(hw);
	}

	if (ret_val) {
		hw_dbg("Error forcing flow control settings\n");
		goto out;
	}

	/* Check for the case where we have copper media and auto-neg is
	 * enabled.  In this case, we need to check and see if Auto-Neg
	 * has completed, and if so, how the PHY and link partner has
	 * flow control configured.
	 */
	if (hw->phy.media_type == e1000_media_type_copper && mac->autoneg) {
		/* Read the MII Status Register and check to see if AutoNeg
		 * has completed.  We read this twice because this reg has
		 * some "sticky" (latched) bits.
		 */
		ret_val = hw->phy.ops.read_reg(hw, PHY_STATUS,
					       &mii_status_reg);
		if (ret_val)
			goto out;
		ret_val = hw->phy.ops.read_reg(hw, PHY_STATUS,
					       &mii_status_reg);
		if (ret_val)
			goto out;

		if (!(mii_status_reg & MII_SR_AUTONEG_COMPLETE)) {
			hw_dbg("Copper PHY and Auto Neg has not completed.\n");
			goto out;
		}

		/* The AutoNeg process has completed, so we now need to
		 * read both the Auto Negotiation Advertisement
		 * Register (Address 4) and the Auto_Negotiation Base
		 * Page Ability Register (Address 5) to determine how
		 * flow control was negotiated.
		 */
		ret_val = hw->phy.ops.read_reg(hw, PHY_AUTONEG_ADV,
					       &mii_nway_adv_reg);
		if (ret_val)
			goto out;
		ret_val = hw->phy.ops.read_reg(hw, PHY_LP_ABILITY,
					       &mii_nway_lp_ability_reg);
		if (ret_val)
			goto out;
		/* Two bits in the Auto Negotiation Advertisement Register
		 * (Address 4) and two bits in the Auto Negotiation Base
		 * Page Ability Register (Address 5) determine flow control
		 * for both the PHY and the link partner.  The following
		 * table, taken out of the IEEE 802.3ab/D6.0 dated March 25,
		 * 1999, describes these PAUSE resolution bits and how flow
		 * control is determined based upon these settings.
		 * NOTE:  DC = Don't Care
		 *
		 *   LOCAL DEVICE  |   LINK PARTNER
		 * PAUSE | ASM_DIR | PAUSE | ASM_DIR | NIC Resolution
		 *-------|---------|-------|---------|--------------------
		 *   0   |    0    |  DC   |   DC    | e1000_fc_none
		 *   0   |    1    |   0   |   DC    | e1000_fc_none
		 *   0   |    1    |   1   |    0    | e1000_fc_none
		 *   0   |    1    |   1   |    1    | e1000_fc_tx_pause
		 *   1   |    0    |   0   |   DC    | e1000_fc_none
		 *   1   |   DC    |   1   |   DC    | e1000_fc_full
		 *   1   |    1    |   0   |    0    | e1000_fc_none
		 *   1   |    1    |   0   |    1    | e1000_fc_rx_pause
		 *
		 * Are both PAUSE bits set to 1?  If so, this implies
		 * Symmetric Flow Control is enabled at both ends.  The
		 * ASM_DIR bits are irrelevant per the spec.
		 *
		 * For Symmetric Flow Control:
		 *
		 *   LOCAL DEVICE  |   LINK PARTNER
		 * PAUSE | ASM_DIR | PAUSE | ASM_DIR | Result
		 *-------|---------|-------|---------|--------------------
		 *   1   |   DC    |   1   |   DC    | E1000_fc_full
		 *
		 */
		if ((mii_nway_adv_reg & NWAY_AR_PAUSE) &&
		    (mii_nway_lp_ability_reg & NWAY_LPAR_PAUSE)) {
			/* Now we need to check if the user selected RX ONLY
			 * of pause frames.  In this case, we had to advertise
			 * FULL flow control because we could not advertise RX
			 * ONLY. Hence, we must now check to see if we need to
			 * turn OFF  the TRANSMISSION of PAUSE frames.
			 */
			if (hw->fc.requested_mode == e1000_fc_full) {
				hw->fc.current_mode = e1000_fc_full;
				hw_dbg("Flow Control = FULL.\n");
			} else {
				hw->fc.current_mode = e1000_fc_rx_pause;
				hw_dbg("Flow Control = RX PAUSE frames only.\n");
			}
		}

		/* For receiving PAUSE frames ONLY.
		 *
		 *   LOCAL DEVICE  |   LINK PARTNER
		 * PAUSE | ASM_DIR | PAUSE | ASM_DIR | Result
		 *-------|---------|-------|---------|--------------------
		 *   0   |    1    |   1   |    1    | e1000_fc_tx_pause
		 */
		else if (!(mii_nway_adv_reg & NWAY_AR_PAUSE) &&
			 (mii_nway_adv_reg & NWAY_AR_ASM_DIR) &&
			 (mii_nway_lp_ability_reg & NWAY_LPAR_PAUSE) &&
			 (mii_nway_lp_ability_reg & NWAY_LPAR_ASM_DIR)) {
			hw->fc.current_mode = e1000_fc_tx_pause;
			hw_dbg("Flow Control = TX PAUSE frames only.\n");
		}
		/* For transmitting PAUSE frames ONLY.
		 *
		 *   LOCAL DEVICE  |   LINK PARTNER
		 * PAUSE | ASM_DIR | PAUSE | ASM_DIR | Result
		 *-------|---------|-------|---------|--------------------
		 *   1   |    1    |   0   |    1    | e1000_fc_rx_pause
		 */
		else if ((mii_nway_adv_reg & NWAY_AR_PAUSE) &&
			 (mii_nway_adv_reg & NWAY_AR_ASM_DIR) &&
			 !(mii_nway_lp_ability_reg & NWAY_LPAR_PAUSE) &&
			 (mii_nway_lp_ability_reg & NWAY_LPAR_ASM_DIR)) {
			hw->fc.current_mode = e1000_fc_rx_pause;
			hw_dbg("Flow Control = RX PAUSE frames only.\n");
		}
		/* Per the IEEE spec, at this point flow control should be
		 * disabled.  However, we want to consider that we could
		 * be connected to a legacy switch that doesn't advertise
		 * desired flow control, but can be forced on the link
		 * partner.  So if we advertised no flow control, that is
		 * what we will resolve to.  If we advertised some kind of
		 * receive capability (Rx Pause Only or Full Flow Control)
		 * and the link partner advertised none, we will configure
		 * ourselves to enable Rx Flow Control only.  We can do
		 * this safely for two reasons:  If the link partner really
		 * didn't want flow control enabled, and we enable Rx, no
		 * harm done since we won't be receiving any PAUSE frames
		 * anyway.  If the intent on the link partner was to have
		 * flow control enabled, then by us enabling RX only, we
		 * can at least receive pause frames and process them.
		 * This is a good idea because in most cases, since we are
		 * predominantly a server NIC, more times than not we will
		 * be asked to delay transmission of packets than asking
		 * our link partner to pause transmission of frames.
		 */
		else if ((hw->fc.requested_mode == e1000_fc_none) ||
			 (hw->fc.requested_mode == e1000_fc_tx_pause) ||
			 (hw->fc.strict_ieee)) {
			hw->fc.current_mode = e1000_fc_none;
			hw_dbg("Flow Control = NONE.\n");
		} else {
			hw->fc.current_mode = e1000_fc_rx_pause;
			hw_dbg("Flow Control = RX PAUSE frames only.\n");
		}

		/* Now we need to do one last check...  If we auto-
		 * negotiated to HALF DUPLEX, flow control should not be
		 * enabled per IEEE 802.3 spec.
		 */
		ret_val = hw->mac.ops.get_speed_and_duplex(hw, &speed, &duplex);
		if (ret_val) {
			hw_dbg("Error getting link speed and duplex\n");
			goto out;
		}

		if (duplex == HALF_DUPLEX)
			hw->fc.current_mode = e1000_fc_none;

		/* Now we call a subroutine to actually force the MAC
		 * controller to use the correct flow control settings.
		 */
		ret_val = igc_force_mac_fc(hw);
		if (ret_val) {
			hw_dbg("Error forcing flow control settings\n");
			goto out;
		}
	}

out:
	return 0;
}

/**
 *  igc_get_auto_rd_done - Check for auto read completion
 *  @hw: pointer to the HW structure
 *
 *  Check EEPROM for Auto Read done bit.
 **/
s32 igc_get_auto_rd_done(struct e1000_hw *hw)
{
	s32 i = 0;
	s32 ret_val = 0;

	while (i < AUTO_READ_DONE_TIMEOUT) {
		if (rd32(E1000_EECD) & E1000_EECD_AUTO_RD)
			break;
		usleep_range(1000, 2000);
		i++;
	}

	if (i == AUTO_READ_DONE_TIMEOUT) {
		hw_dbg("Auto read by HW from NVM has not completed.\n");
		ret_val = -E1000_ERR_RESET;
		goto out;
	}

out:
	return ret_val;
}

/**
 *  igc_get_speed_and_duplex_copper - Retrieve current speed/duplex
 *  @hw: pointer to the HW structure
 *  @speed: stores the current speed
 *  @duplex: stores the current duplex
 *
 *  Read the status register for the current speed/duplex and store the current
 *  speed and duplex for copper connections.
 **/
s32 igc_get_speed_and_duplex_copper(struct e1000_hw *hw, u16 *speed,
				    u16 *duplex)
{
	u32 status;

	status = rd32(E1000_STATUS);
	if (status & E1000_STATUS_SPEED_1000) {
		/* For I225, STATUS will indicate 1G speed in both 1 Gbps
		 * and 2.5 Gbps link modes. An additional bit is used
		 * to differentiate between 1 Gbps and 2.5 Gbps.
		 */
		if (hw->mac.type == e1000_i225 &&
		    (status & E1000_STATUS_SPEED_2500)) {
			*speed = SPEED_2500;
			hw_dbg("2500 Mbs, ");
		} else {
			*speed = SPEED_1000;
			hw_dbg("1000 Mbs, ");
		}
	} else if (status & E1000_STATUS_SPEED_100) {
		*speed = SPEED_100;
		hw_dbg("100 Mbs, ");
	} else {
		*speed = SPEED_10;
		hw_dbg("10 Mbs, ");
	}

	if (status & E1000_STATUS_FD) {
		*duplex = FULL_DUPLEX;
		hw_dbg("Full Duplex\n");
	} else {
		*duplex = HALF_DUPLEX;
		hw_dbg("Half Duplex\n");
	}

	return 0;
}

/**
 *  igc_put_hw_semaphore - Release hardware semaphore
 *  @hw: pointer to the HW structure
 *
 *  Release hardware semaphore used to access the PHY or NVM
 **/
void igc_put_hw_semaphore(struct e1000_hw *hw)
{
	u32 swsm;

	swsm = rd32(E1000_SWSM);

	swsm &= ~(E1000_SWSM_SMBI | E1000_SWSM_SWESMBI);

	wr32(E1000_SWSM, swsm);
}

/**
 *  igc_enable_mng_pass_thru - Enable processing of ARP's
 *  @hw: pointer to the HW structure
 *
 *  Verifies the hardware needs to leave interface enabled so that frames can
 *  be directed to and from the management interface.
 **/
bool igc_enable_mng_pass_thru(struct e1000_hw *hw)
{
	u32 manc;
	u32 fwsm, factps;
	bool ret_val = false;

	if (!hw->mac.asf_firmware_present)
		goto out;

	manc = rd32(E1000_MANC);

	if (!(manc & E1000_MANC_RCV_TCO_EN))
		goto out;

	if (hw->mac.arc_subsystem_valid) {
		fwsm = rd32(E1000_FWSM);
		factps = rd32(E1000_FACTPS);

		if (!(factps & E1000_FACTPS_MNGCG) &&
		    ((fwsm & E1000_FWSM_MODE_MASK) ==
		    (e1000_mng_mode_pt << E1000_FWSM_MODE_SHIFT))) {
			ret_val = true;
			goto out;
		}
	} else {
		if ((manc & E1000_MANC_SMBUS_EN) &&
		    !(manc & E1000_MANC_ASF_EN)) {
			ret_val = true;
			goto out;
		}
	}

out:
	return ret_val;
}
