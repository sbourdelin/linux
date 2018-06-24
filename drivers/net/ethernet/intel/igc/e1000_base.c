// SPDX-License-Identifier: GPL-2.0
/* Copyright (c)  2018 Intel Corporation */

#include <linux/delay.h>

#include "e1000_hw.h"
#include "e1000_i225.h"
#include "e1000_mac.h"
#include "e1000_base.h"
#include "igc.h"

/* forward declaration */
static s32 igc_get_invariants_base(struct e1000_hw *);
static s32 igc_check_for_link_base(struct e1000_hw *);
static s32 igc_acquire_phy_base(struct e1000_hw *);
static void igc_release_phy_base(struct e1000_hw *);
static s32 igc_get_phy_id_base(struct e1000_hw *);
static s32 igc_init_hw_base(struct e1000_hw *);
static s32 igc_reset_hw_base(struct e1000_hw *);
static s32 igc_set_pcie_completion_timeout(struct e1000_hw *hw);
static s32 igc_read_mac_addr_base(struct e1000_hw *hw);

/**
 *  igc_get_phy_id_base - Retrieve PHY addr and id
 *  @hw: pointer to the HW structure
 *
 *  Retrieves the PHY address and ID for both PHY's which do and do not use
 *  sgmi interface.
 **/
static s32 igc_get_phy_id_base(struct e1000_hw *hw)
{
	s32  ret_val = 0;

	ret_val = igc_get_phy_id(hw);

	return ret_val;
}

/**
 *  igc_init_nvm_params_base - Init NVM func ptrs.
 *  @hw: pointer to the HW structure
 **/
static s32 igc_init_nvm_params_base(struct e1000_hw *hw)
{
	struct e1000_nvm_info *nvm = &hw->nvm;
	u32 eecd = rd32(E1000_EECD);
	u16 size;

	size = (u16)((eecd & E1000_EECD_SIZE_EX_MASK) >>
		     E1000_EECD_SIZE_EX_SHIFT);

	/* Added to a constant, "size" becomes the left-shift value
	 * for setting word_size.
	 */
	size += NVM_WORD_SIZE_BASE_SHIFT;

	/* Just in case size is out of range, cap it to the largest
	 * EEPROM size supported
	 */
	if (size > 15)
		size = 15;

	nvm->word_size = BIT(size);
	nvm->opcode_bits = 8;
	nvm->delay_usec = 1;

	nvm->page_size = eecd & E1000_EECD_ADDR_BITS ? 32 : 8;
	nvm->address_bits = eecd & E1000_EECD_ADDR_BITS ?
			    16 : 8;

	if (nvm->word_size == BIT(15))
		nvm->page_size = 128;

	return 0;
}

/**
 *  igc_init_mac_params_base - Init MAC func ptrs.
 *  @hw: pointer to the HW structure
 **/
static s32 igc_init_mac_params_base(struct e1000_hw *hw)
{
	struct e1000_mac_info *mac = &hw->mac;
	struct e1000_dev_spec_base *dev_spec = &hw->dev_spec._base;

	/* Set mta register count */
	mac->mta_reg_count = 128;
	mac->rar_entry_count = E1000_RAR_ENTRIES;

	/* reset */
	mac->ops.reset_hw = igc_reset_hw_base;

	mac->ops.acquire_swfw_sync = igc_acquire_swfw_sync_i225;
	mac->ops.release_swfw_sync = igc_release_swfw_sync_i225;

	/* Allow a single clear of the SW semaphore on I225 */
	if (mac->type == e1000_i225)
		dev_spec->clear_semaphore_once = true;

	return 0;
}

/**
 *  igc_init_phy_params_base - Init PHY func ptrs.
 *  @hw: pointer to the HW structure
 **/
static s32 igc_init_phy_params_base(struct e1000_hw *hw)
{
	struct e1000_phy_info *phy = &hw->phy;
	s32 ret_val = 0;
	u32 ctrl_ext;

	if (hw->phy.media_type != e1000_media_type_copper) {
		phy->type = e1000_phy_none;
		goto out;
	}

	phy->autoneg_mask       = AUTONEG_ADVERTISE_SPEED_DEFAULT_2500;
	phy->reset_delay_us     = 100;

	ctrl_ext = rd32(E1000_CTRL_EXT);

	/* set lan id */
	hw->bus.func = (rd32(E1000_STATUS) & E1000_STATUS_FUNC_MASK) >>
			E1000_STATUS_FUNC_SHIFT;

	/* Make sure the PHY is in a good state. Several people have reported
	 * firmware leaving the PHY's page select register set to something
	 * other than the default of zero, which causes the PHY ID read to
	 * access something other than the intended register.
	 */
	ret_val = hw->phy.ops.reset(hw);
	if (ret_val) {
		hw_dbg("Error resetting the PHY.\n");
		goto out;
	}

	ret_val = igc_get_phy_id_base(hw);
	if (ret_val)
		return ret_val;

	/* Verify phy id and set remaining function pointers */
	switch (phy->id) {
	case I225_I_PHY_ID:
		phy->type	= e1000_phy_i225;
		break;
	default:
		ret_val = -E1000_ERR_PHY;
		goto out;
	}

out:
	return ret_val;
}

static s32 igc_get_invariants_base(struct e1000_hw *hw)
{
	s32 ret_val = 0;
	u32 ctrl_ext = 0;
	u32 link_mode = 0;

	ctrl_ext = rd32(E1000_CTRL_EXT);
	link_mode = ctrl_ext & E1000_CTRL_EXT_LINK_MODE_MASK;

	/* mac initialization and operations */
	ret_val = igc_init_mac_params_base(hw);
	if (ret_val)
		goto out;

	/* NVM initialization */
	ret_val = igc_init_nvm_params_base(hw);
	switch (hw->mac.type) {
	case e1000_i225:
		ret_val = igc_init_nvm_params_i225(hw);
		break;
	default:
		break;
	}

	/* setup PHY parameters */
	ret_val = igc_init_phy_params_base(hw);
	if (ret_val)
		goto out;

out:
	return ret_val;
}

/**
 *  igc_acquire_phy_base - Acquire rights to access PHY
 *  @hw: pointer to the HW structure
 *
 *  Acquire access rights to the correct PHY.  This is a
 *  function pointer entry point called by the api module.
 **/
static s32 igc_acquire_phy_base(struct e1000_hw *hw)
{
	u16 mask = E1000_SWFW_PHY0_SM;

	return hw->mac.ops.acquire_swfw_sync(hw, mask);
}

/**
 *  igc_release_phy_base - Release rights to access PHY
 *  @hw: pointer to the HW structure
 *
 *  A wrapper to release access rights to the correct PHY.  This is a
 *  function pointer entry point called by the api module.
 **/
static void igc_release_phy_base(struct e1000_hw *hw)
{
	u16 mask = E1000_SWFW_PHY0_SM;

	hw->mac.ops.release_swfw_sync(hw, mask);
}

/**
 *  igc_get_link_up_info_base - Get link speed/duplex info
 *  @hw: pointer to the HW structure
 *  @speed: stores the current speed
 *  @duplex: stores the current duplex
 *
 *  This is a wrapper function, if using the serial gigabit media independent
 *  interface, use PCS to retrieve the link speed and duplex information.
 *  Otherwise, use the generic function to get the link speed and duplex info.
 **/
static s32 igc_get_link_up_info_base(struct e1000_hw *hw, u16 *speed,
				     u16 *duplex)
{
	s32 ret_val;

	ret_val = igc_get_speed_and_duplex_copper(hw, speed, duplex);

	return ret_val;
}

/**
 *  igc_check_for_link_base - Check for link
 *  @hw: pointer to the HW structure
 *
 *  If sgmii is enabled, then use the pcs register to determine link, otherwise
 *  use the generic interface for determining link.
 **/
static s32 igc_check_for_link_base(struct e1000_hw *hw)
{
	s32 ret_val = 0;

	ret_val = igc_check_for_copper_link(hw);

	return ret_val;
}

/**
 *  igc_init_hw_base - Initialize hardware
 *  @hw: pointer to the HW structure
 *
 *  This inits the hardware readying it for operation.
 **/
static s32 igc_init_hw_base(struct e1000_hw *hw)
{
	struct e1000_mac_info *mac = &hw->mac;
	s32 ret_val = 0;
	u16 i, rar_count = mac->rar_entry_count;

	/* Setup the receive address */
	igc_init_rx_addrs(hw, rar_count);

	/* Zero out the Multicast HASH table */
	hw_dbg("Zeroing the MTA\n");
	for (i = 0; i < mac->mta_reg_count; i++)
		array_wr32(E1000_MTA, i, 0);

	/* Zero out the Unicast HASH table */
	hw_dbg("Zeroing the UTA\n");
	for (i = 0; i < mac->uta_reg_count; i++)
		array_wr32(E1000_UTA, i, 0);

	/* Setup link and flow control */
	ret_val = igc_setup_link(hw);

	/* Clear all of the statistics registers (clear on read).  It is
	 * important that we do this after we have tried to establish link
	 * because the symbol error count will increment wildly if there
	 * is no link.
	 */
	igc_clear_hw_cntrs_base(hw);

	return ret_val;
}

/**
 *  igc_read_mac_addr_base - Read device MAC address
 *  @hw: pointer to the HW structure
 **/
static s32 igc_read_mac_addr_base(struct e1000_hw *hw)
{
	s32 ret_val = 0;

	ret_val = igc_read_mac_addr(hw);

	return ret_val;
}

/**
 *  igc_power_down_phy_copper_base - Remove link during PHY power down
 *  @hw: pointer to the HW structure
 *
 *  In the case of a PHY power down to save power, or to turn off link during a
 *  driver unload, or wake on lan is not enabled, remove the link.
 **/
void igc_power_down_phy_copper_base(struct e1000_hw *hw)
{
	/* If the management interface is not enabled, then power down */
	if (!(igc_enable_mng_pass_thru(hw) || igc_check_reset_block(hw)))
		igc_power_down_phy_copper(hw);
}

/**
 *  igc_rx_fifo_flush_base - Clean rx fifo after Rx enable
 *  @hw: pointer to the HW structure
 *
 *  After Rx enable, if manageability is enabled then there is likely some
 *  bad data at the start of the fifo and possibly in the DMA fifo.  This
 *  function clears the fifos and flushes any packets that came in as rx was
 *  being enabled.
 **/
void igc_rx_fifo_flush_base(struct e1000_hw *hw)
{
	u32 rctl, rlpml, rxdctl[4], rfctl, temp_rctl, rx_enabled;
	int i, ms_wait;

	/* disable IPv6 options as per hardware errata */
	rfctl = rd32(E1000_RFCTL);
	rfctl |= E1000_RFCTL_IPV6_EX_DIS;
	wr32(E1000_RFCTL, rfctl);

	if (!(rd32(E1000_MANC) & E1000_MANC_RCV_TCO_EN))
		return;

	/* Disable all Rx queues */
	for (i = 0; i < 4; i++) {
		rxdctl[i] = rd32(E1000_RXDCTL(i));
		wr32(E1000_RXDCTL(i),
		     rxdctl[i] & ~E1000_RXDCTL_QUEUE_ENABLE);
	}
	/* Poll all queues to verify they have shut down */
	for (ms_wait = 0; ms_wait < 10; ms_wait++) {
		usleep_range(1000, 2000);
		rx_enabled = 0;
		for (i = 0; i < 4; i++)
			rx_enabled |= rd32(E1000_RXDCTL(i));
		if (!(rx_enabled & E1000_RXDCTL_QUEUE_ENABLE))
			break;
	}

	if (ms_wait == 10)
		pr_debug("Queue disable timed out after 10ms\n");

	/* Clear RLPML, RCTL.SBP, RFCTL.LEF, and set RCTL.LPE so that all
	 * incoming packets are rejected.  Set enable and wait 2ms so that
	 * any packet that was coming in as RCTL.EN was set is flushed
	 */
	wr32(E1000_RFCTL, rfctl & ~E1000_RFCTL_LEF);

	rlpml = rd32(E1000_RLPML);
	wr32(E1000_RLPML, 0);

	rctl = rd32(E1000_RCTL);
	temp_rctl = rctl & ~(E1000_RCTL_EN | E1000_RCTL_SBP);
	temp_rctl |= E1000_RCTL_LPE;

	wr32(E1000_RCTL, temp_rctl);
	wr32(E1000_RCTL, temp_rctl | E1000_RCTL_EN);
	wrfl();
	usleep_range(2000, 3000);

	/* Enable Rx queues that were previously enabled and restore our
	 * previous state
	 */
	for (i = 0; i < 4; i++)
		wr32(E1000_RXDCTL(i), rxdctl[i]);
	wr32(E1000_RCTL, rctl);
	wrfl();

	wr32(E1000_RLPML, rlpml);
	wr32(E1000_RFCTL, rfctl);

	/* Flush receive errors generated by workaround */
	rd32(E1000_ROC);
	rd32(E1000_RNBC);
	rd32(E1000_MPC);
}

static struct e1000_mac_operations e1000_mac_ops_base = {
	.init_hw		= igc_init_hw_base,
	.check_for_link		= igc_check_for_link_base,
	.rar_set		= igc_rar_set,
	.read_mac_addr		=  igc_read_mac_addr_base,
	.get_speed_and_duplex	= igc_get_link_up_info_base,
};

static const struct e1000_phy_operations e1000_phy_ops_base = {
	.acquire		= igc_acquire_phy_base,
	.release		= igc_release_phy_base,
	.reset			= igc_phy_hw_reset,
	.read_reg		= igc_read_phy_reg_gpy,
	.write_reg		= igc_write_phy_reg_gpy,
};

const struct e1000_info e1000_base_info = {
	.get_invariants	= igc_get_invariants_base,
	.mac_ops	= &e1000_mac_ops_base,
	.phy_ops	= &e1000_phy_ops_base,
};

/**
 *  igc_reset_hw_base - Reset hardware
 *  @hw: pointer to the HW structure
 *
 *  This resets the hardware into a known state.  This is a
 *  function pointer entry point called by the api module.
 **/
static s32 igc_reset_hw_base(struct e1000_hw *hw)
{
	u32 ctrl;
	s32 ret_val;

	/* Prevent the PCI-E bus from sticking if there is no TLP connection
	 * on the last TLP read/write transaction when MAC is reset.
	 */
	ret_val = igc_disable_pcie_master(hw);
	if (ret_val)
		hw_dbg("PCI-E Master disable polling has failed.\n");

	/* set the completion timeout for interface */
	ret_val = igc_set_pcie_completion_timeout(hw);
	if (ret_val)
		hw_dbg("PCI-E Set completion timeout has failed.\n");

	hw_dbg("Masking off all interrupts\n");
	wr32(E1000_IMC, 0xffffffff);

	wr32(E1000_RCTL, 0);
	wr32(E1000_TCTL, E1000_TCTL_PSP);
	wrfl();

	usleep_range(10000, 20000);

	ctrl = rd32(E1000_CTRL);

	hw_dbg("Issuing a global reset to MAC\n");
	wr32(E1000_CTRL, ctrl | E1000_CTRL_RST);

	ret_val = igc_get_auto_rd_done(hw);
	if (ret_val) {
		/* When auto config read does not complete, do not
		 * return with an error. This can happen in situations
		 * where there is no eeprom and prevents getting link.
		 */
		hw_dbg("Auto Read Done did not complete\n");
	}

	/* Clear any pending interrupt events. */
	wr32(E1000_IMC, 0xffffffff);
	rd32(E1000_ICR);

	return ret_val;
}

/**
 *  igc_set_pcie_completion_timeout - set pci-e completion timeout
 *  @hw: pointer to the HW structure
 *
 *  The defaults for 82575 and 82576 should be in the range of 50us to 50ms,
 *  however the hardware default for these parts is 500us to 1ms which is less
 *  than the 10ms recommended by the pci-e spec.  To address this we need to
 *  increase the value to either 10ms to 200ms for capability version 1 config,
 *  or 16ms to 55ms for version 2.
 **/
static s32 igc_set_pcie_completion_timeout(struct e1000_hw *hw)
{
	u32 gcr = rd32(E1000_GCR);
	s32 ret_val = 0;
	u16 pcie_devctl2;

	/* only take action if timeout value is defaulted to 0 */
	if (gcr & E1000_GCR_CMPL_TMOUT_MASK)
		goto out;

	/* if capabilities version is type 1 we can write the
	 * timeout of 10ms to 200ms through the GCR register
	 */
	if (!(gcr & E1000_GCR_CAP_VER2)) {
		gcr |= E1000_GCR_CMPL_TMOUT_10ms;
		goto out;
	}

	/* for version 2 capabilities we need to write the config space
	 * directly in order to set the completion timeout value for
	 * 16ms to 55ms
	 */
	ret_val = igc_read_pcie_cap_reg(hw, PCIE_DEVICE_CONTROL2,
					&pcie_devctl2);
	if (ret_val)
		goto out;

	pcie_devctl2 |= PCIE_DEVICE_CONTROL2_16ms;

	ret_val = igc_write_pcie_cap_reg(hw, PCIE_DEVICE_CONTROL2,
					 &pcie_devctl2);
out:
	/* disable completion timeout resend */
	gcr &= ~E1000_GCR_CMPL_TMOUT_RESEND;

	wr32(E1000_GCR, gcr);
	return ret_val;
}
