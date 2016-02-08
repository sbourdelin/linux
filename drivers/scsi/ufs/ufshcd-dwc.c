/*
 * UFS Host driver for Synopsys Designware Core
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Authors: Joao Pinto <jpinto@synopsys.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "ufshcd.h"
#include "unipro.h"

#include "ufshcd-dwc.h"
#include "ufshci-dwc.h"

/**
 * ufshcd_dwc_program_clk_div()
 * This function programs the clk divider value. This value is needed to
 * provide 1 microsecond tick to unipro layer.
 * @hba: Private Structure pointer
 * @divider_val: clock divider value to be programmed
 *
 */
void ufshcd_dwc_program_clk_div(struct ufs_hba *hba, u32 divider_val)
{
	ufshcd_writel(hba, divider_val, DWC_UFS_REG_HCLKDIV);
}
EXPORT_SYMBOL(ufshcd_dwc_program_clk_div);

/**
 * ufshcd_dwc_link_is_up()
 * Check if link is up
 * @hba: private structure poitner
 *
 * Returns 0 on success, non-zero value on failure
 */
int ufshcd_dwc_link_is_up(struct ufs_hba *hba)
{
	int dme_result = 0;

	ufshcd_dme_get(hba, UIC_ARG_MIB(VS_POWERSTATE), &dme_result);

	if (dme_result == UFSHCD_LINK_IS_UP) {
		ufshcd_set_link_active(hba);
		return 0;
	}

	return 1;
}
EXPORT_SYMBOL(ufshcd_dwc_link_is_up);

/**
 * ufshcd_dwc_connection_setup()
 * This function configures both the local side (host) and the peer side
 * (device) unipro attributes to establish the connection to application/
 * cport.
 * This function is not required if the hardware is properly configured to
 * have this connection setup on reset. But invoking this function does no
 * harm and should be fine even working with any ufs device.
 *
 * @hba: pointer to drivers private data
 *
 * Returns 0 on success non-zero value on failure
 */
int ufshcd_dwc_connection_setup(struct ufs_hba *hba)
{
	int ret = 0;

	/* Local side Configuration */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(T_CONNECTIONSTATE), 0);
	if (ret)
		goto out;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(N_DEVICEID), 0);
	if (ret)
		goto out;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(N_DEVICEID_VALID), 0);
	if (ret)
		goto out;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(T_PEERDEVICEID), 1);
	if (ret)
		goto out;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(T_PEERCPORTID), 0);
	if (ret)
		goto out;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(T_TRAFFICCLASS), 0);
	if (ret)
		goto out;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(T_CPORTFLAGS), 0x6);
	if (ret)
		goto out;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(T_CPORTMODE), 1);
	if (ret)
		goto out;

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(T_CONNECTIONSTATE), 1);
	if (ret)
		goto out;


	/* Peer side Configuration */
	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(T_CONNECTIONSTATE), 0);
	if (ret)
		goto out;

	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(N_DEVICEID), 1);
	if (ret)
		goto out;

	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(N_DEVICEID_VALID), 1);
	if (ret)
		goto out;

	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(T_PEERDEVICEID), 1);
	if (ret)
		goto out;

	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(T_PEERCPORTID), 0);
	if (ret)
		goto out;

	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(T_TRAFFICCLASS), 0);
	if (ret)
		goto out;

	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(T_CPORTFLAGS), 0x6);
	if (ret)
		goto out;

	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(T_CPORTMODE), 1);
	if (ret)
		goto out;

	ret = ufshcd_dme_peer_set(hba, UIC_ARG_MIB(T_CONNECTIONSTATE), 1);
	if (ret)
		goto out;

out:
	return ret;
}

/**
 * ufshcd_dwc_setup_20bit_rmmi_lane0()
 * This function configures Synopsys MPHY 20-bit RMMI Lane 0
 * @hba: Pointer to drivers structure
 *
 * Returns 0 on success or non-zero value on failure
 */
int ufshcd_dwc_setup_20bit_rmmi_lane0(struct ufs_hba *hba)
{
	int ret = 0;

	/* TX Reference Clock 26MHz */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(TX_REFCLKFREQ,
							SELIND_LN0_TX), 0x01);
	if (ret)
		goto out;

	/* TX Configuration Clock Frequency Val; Divider setting */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(TX_CFGCLKFREQVAL,
							SELIND_LN0_TX), 0x19);
	if (ret)
		goto out;

	/* RX Configuration Clock Frequency Val; Divider setting */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RX_CFGCLKFREQVAL,
							SELIND_LN0_RX), 0x19);
	if (ret)
		goto out;

	/* TX 20-bit RMMI Interface */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGEXTRATTR,
							SELIND_LN0_TX), 0x12);
	if (ret)
		goto out;

	/* TX dither configuration */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(DITHERCTRL2,
							SELIND_LN0_TX), 0xd6);
	if (ret)
		goto out;

	/* RX Reference Clock 26MHz */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RX_REFCLKFREQ,
							SELIND_LN0_RX), 0x01);
	if (ret)
		goto out;

	/* RX 20-bit RMMI Interface */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGWIDEINLN,
							SELIND_LN0_RX), 2);
	if (ret)
		goto out;

	/* RX Squelch Detector output is routed to RX hibern8 exit signal */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXCDR8,
							SELIND_LN0_RX), 0x80);
	if (ret)
		goto out;

	/* Common block Direct Control 10 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(DIRECTCTRL10), 0x04);
	if (ret)
		goto out;

	/* Common block Direct Control 19 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(DIRECTCTRL19), 0x02);
	if (ret)
		goto out;

	/* ENARXDIRECTCFG4 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(ENARXDIRECTCFG4,
							SELIND_LN0_RX), 0x03);
	if (ret)
		goto out;

	/* CFGRXOVR8 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXOVR8,
							SELIND_LN0_RX), 0x16);
	if (ret)
		goto out;

	/* RXDIRECTCTRL2 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RXDIRECTCTRL2,
							SELIND_LN0_RX), 0x42);

	if (ret)
		goto out;

	/* ENARXDIRECTCFG3 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(ENARXDIRECTCFG3,
							SELIND_LN0_RX), 0xa4);

	if (ret)
		goto out;

	/* RXCALCTRL */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RXCALCTRL,
							SELIND_LN0_RX), 0x01);
	if (ret)
		goto out;

	/* ENARXDIRECTCFG2 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(ENARXDIRECTCFG2,
							SELIND_LN0_RX), 0x01);
	if (ret)
		goto out;

	/* CFGOVR4 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXOVR4,
							SELIND_LN0_RX), 0x28);
	if (ret)
		goto out;

	/* RXSQCTRL */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RXSQCTRL,
							SELIND_LN0_RX), 0x1E);
	if (ret)
		goto out;

	/* CFGOVR6 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXOVR6,
							SELIND_LN0_RX), 0x2f);
	if (ret)
		goto out;

	/* CBPRGPLL2 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(CBPRGPLL2), 0x00);

out:
	return ret;
}

/**
 * ufshcd_dwc_setup_20bit_rmmi_lane1()
 * This function configures Synopsys MPHY 20-bit RMMI Lane 1
 * @hba: Pointer to drivers structure
 *
 * Returns 0 on success or non-zero value on failure
 */
int ufshcd_dwc_setup_20bit_rmmi_lane1(struct ufs_hba *hba)
{
	int connected_rx_lanes = 0;
	int connected_tx_lanes = 0;
	int ret = 0;

	/* Get the available lane count */
	ufshcd_dme_get(hba, UIC_ARG_MIB(PA_CONNECTEDRXDATALANES),
			&connected_rx_lanes);
	ufshcd_dme_get(hba, UIC_ARG_MIB(PA_CONNECTEDTXDATALANES),
			&connected_tx_lanes);

	if (connected_tx_lanes == 2) {

		/* TX Reference Clock 26MHz */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(TX_REFCLKFREQ,
							SELIND_LN1_TX), 0x0d);
		if (ret)
			goto out;

		/* TX Configuration Clock Frequency Val; Divider setting */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(TX_CFGCLKFREQVAL,
							SELIND_LN1_TX), 0x19);
		if (ret)
			goto out;

		/* TX 20-bit RMMI Interface */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGEXTRATTR,
							SELIND_LN1_TX), 0x12);
		if (ret)
			goto out;

		/* TX dither configuration */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(DITHERCTRL2,
							SELIND_LN0_TX), 0xd6);
		if (ret)
			goto out;
	}

	if (connected_rx_lanes == 2) {

		/* RX Reference Clock 26MHz */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RX_REFCLKFREQ,
							SELIND_LN1_RX), 0x01);
		if (ret)
			goto out;

		/* RX Configuration Clock Frequency Val; Divider setting */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RX_CFGCLKFREQVAL,
							SELIND_LN1_RX), 0x19);
		if (ret)
			goto out;

		/* RX 20-bit RMMI Interface */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGWIDEINLN,
							SELIND_LN1_RX), 2);
		if (ret)
			goto out;

		/* RX Squelch Detector output is routed to RX hibern8 exit
		 * signal
		 */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXCDR8,
							SELIND_LN1_RX), 0x80);
		if (ret)
			goto out;

		/* ENARXDIRECTCFG4 */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(ENARXDIRECTCFG4,
							SELIND_LN1_RX), 0x03);
		if (ret)
			goto out;

		/* CFGRXOVR8 */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXOVR8,
							SELIND_LN1_RX), 0x16);
		if (ret)
			goto out;

		/* RXDIRECTCTRL2 */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RXDIRECTCTRL2,
							SELIND_LN1_RX), 0x42);
		if (ret)
			goto out;

		/* ENARXDIRECTCFG3 */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(ENARXDIRECTCFG3,
							SELIND_LN1_RX), 0xa4);
		if (ret)
			goto out;

		/* RXCALCTRL */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RXCALCTRL,
							SELIND_LN1_RX), 0x01);
		if (ret)
			goto out;

		/* ENARXDIRECTCFG2 */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(ENARXDIRECTCFG2,
							SELIND_LN1_RX), 0x01);
		if (ret)
			goto out;

		/* CFGOVR4 */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXOVR4,
							SELIND_LN1_RX), 0x28);
		if (ret)
			goto out;

		/* RXSQCTRL */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RXSQCTRL,
							SELIND_LN1_RX), 0x1E);
		if (ret)
			goto out;

		/* CFGOVR6 */
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXOVR6,
							SELIND_LN1_RX), 0x2f);
		if (ret)
			goto out;
	}

out:
	return ret;
}

/**
 * ufshcd_dwc_setup_20bit_rmmi()
 * This function configures Synopsys MPHY specific atributes (20-bit RMMI)
 * @hba: Pointer to drivers structure
 *
 * Returns 0 on success or non-zero value on failure
 */
int ufshcd_dwc_setup_20bit_rmmi(struct ufs_hba *hba)
{
	int ret = 0;

	/* Common block Tx Global Hibernate Exit */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(TX_GLOBALHIBERNATE), 0x00);
	if (ret)
		goto out;

	/* Common block Reference Clock Mode 26MHz */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(REFCLKMODE), 0x01);
	if (ret)
		goto out;

	/* Common block DCO Target Frequency MAX PWM G1:9Mpbs */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(CDIRECTCTRL6), 0xc0);
	if (ret)
		goto out;

	/* Common block TX and RX Div Factor is 4 7Mbps/20 = 350KHz */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(CBDIVFACTOR), 0x44);
	if (ret)
		goto out;

	/* Common Block DC0 Ctrl 5*/
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(CBDCOCTRL5), 0x64);
	if (ret)
		goto out;

	/* Common Block Program Tunning*/
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(CBPRGTUNING), 0x09);
	if (ret)
		goto out;

	/* Common Block Real Time Observe Select - for debugging */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(RTOBSERVESELECT), 0x00);
	if (ret)
		goto out;

	/* Lane 0 configuration*/
	ret = ufshcd_dwc_setup_20bit_rmmi_lane0(hba);
	if (ret)
		goto out;

	/* Lane 1 configuration*/
	ret = ufshcd_dwc_setup_20bit_rmmi_lane1(hba);
	if (ret)
		goto out;

out:
	return ret;
}

/**
 * ufshcd_dwc_setup_40bit_rmmi()
 * This function configures Synopsys MPHY specific atributes (40-bit RMMI)
 * @hba: Pointer to drivers structure
 *
 * Returns 0 on success or non-zero value on failure
 */
int ufshcd_dwc_setup_40bit_rmmi(struct ufs_hba *hba)
{
	int ret = 0;

	/* Common block Tx Global Hibernate Exit */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(TX_GLOBALHIBERNATE), 0x00);
	if (ret)
		goto out;

	/* Common block Reference Clock Mode 26MHz */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(REFCLKMODE), 0x01);
	if (ret)
		goto out;

	/* Common block DCO Target Frequency MAX PWM G1:7Mpbs */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(CDIRECTCTRL6), 0x80);
	if (ret)
		goto out;

	/* Common block TX and RX Div Factor is 4 7Mbps/40 = 175KHz */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(CBDIVFACTOR), 0x08);
	if (ret)
		goto out;

	/* Common Block DC0 Ctrl 5*/
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(CBDCOCTRL5), 0x64);
	if (ret)
		goto out;

	/* Common Block Program Tunning*/
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(CBPRGTUNING), 0x09);
	if (ret)
		goto out;

	/* Common Block Real Time Observe Select - for debugging */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(RTOBSERVESELECT), 0x00);
	if (ret)
		goto out;

	/* Lane 0 configuration*/

	/* TX Reference Clock 26MHz */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(TX_REFCLKFREQ,
							SELIND_LN0_TX), 0x01);
	if (ret)
		goto out;

	/* TX Configuration Clock Frequency Val; Divider setting */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(TX_CFGCLKFREQVAL,
							SELIND_LN0_TX), 0x19);
	if (ret)
		goto out;

	/* TX 40-bit RMMI Interface */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGEXTRATTR,
							SELIND_LN0_TX), 0x14);
	if (ret)
		goto out;

	/* TX dither configuration */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(DITHERCTRL2,
							SELIND_LN0_TX), 0xd6);
	if (ret)
		goto out;

	/* RX Reference Clock 26MHz */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RX_REFCLKFREQ,
							SELIND_LN0_RX), 0x01);
	if (ret)
		goto out;

	/* RX Configuration Clock Frequency Val; Divider setting */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RX_CFGCLKFREQVAL,
							SELIND_LN0_RX), 0x19);
	if (ret)
		goto out;

	/* RX 40-bit RMMI Interface */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGWIDEINLN,
							SELIND_LN0_RX), 4);
	if (ret)
		goto out;

	/* RX Squelch Detector output is routed to RX hibern8 exit signal */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXCDR8,
							SELIND_LN0_RX), 0x80);
	if (ret)
		goto out;

	/* RX Squelch Detector output is routed to RX hibern8 exit signal */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXCDR8,
							SELIND_LN0_RX), 0x80);
	if (ret)
		goto out;

	/* Common block Direct Control 10 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(DIRECTCTRL10), 0x04);
	if (ret)
		goto out;

	/* Common block Direct Control 19 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(DIRECTCTRL19), 0x02);
	if (ret)
		goto out;

	/* RX Squelch Detector output is routed to RX hibern8 exit signal */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXCDR8,
							SELIND_LN0_RX), 0x80);
	if (ret)
		goto out;

	/* ENARXDIRECTCFG4 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(ENARXDIRECTCFG4,
							SELIND_LN0_RX), 0x03);
	if (ret)
		goto out;

	/* CFGRXOVR8 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXOVR8,
							SELIND_LN0_RX), 0x16);
	if (ret)
		goto out;

	/* RXDIRECTCTRL2 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RXDIRECTCTRL2,
							SELIND_LN0_RX), 0x42);
	if (ret)
		goto out;

	/* ENARXDIRECTCFG3 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(ENARXDIRECTCFG3,
							SELIND_LN0_RX), 0xa4);
	if (ret)
		goto out;

	/* RXCALCTRL */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RXCALCTRL,
							SELIND_LN0_RX), 0x01);
	if (ret)
		goto out;

	/* ENARXDIRECTCFG2 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(ENARXDIRECTCFG2,
							SELIND_LN0_RX), 0x01);
	if (ret)
		goto out;

	/* CFGOVR4 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXOVR4,
							SELIND_LN0_RX), 0x28);
	if (ret)
		goto out;

	/* RXSQCTRL */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(RXSQCTRL,
							SELIND_LN0_RX), 0x1E);
	if (ret)
		goto out;

	/* CFGOVR6 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB_SEL(CFGRXOVR6,
							SELIND_LN0_RX), 0x2f);
	if (ret)
		goto out;

	/* CBPRGPLL2 */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(CBPRGPLL2), 0x00);
	if (ret)
		goto out;

out:
	return ret;
}

/**
 * ufshcd_dwc_setup_mphy()
 * This function configures Local (host) Synopsys MPHY specific attributes
 *
 * @hba: Pointer to drivers structure
 *
 * Returns 0 on success non-zero value on failure
 */
int ufshcd_dwc_setup_mphy(struct ufs_hba *hba)
{
	int ret = 0;

#ifdef CONFIG_SCSI_UFS_DWC_40BIT_RMMI
	dev_info(hba->dev, "Configuring MPHY 40-bit RMMI");
	ret = ufshcd_dwc_setup_40bit_rmmi(hba);
	if (ret) {
		dev_err(hba->dev, "40-bit RMMI configuration failed");
		goto out;
	}
#else
#ifdef CONFIG_SCSI_UFS_DWC_20BIT_RMMI
	dev_info(hba->dev, "Configuring MPHY 20-bit RMMI");
	ret = ufshcd_dwc_setup_20bit_rmmi(hba);
	if (ret) {
		dev_err(hba->dev, "20-bit RMMI configuration failed");
		goto out;
	}
#endif
#endif
	/* To write Shadow register bank to effective configuration block */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(VS_MPHYCFGUPDT), 0x01);
	if (ret)
		goto out;

	/* To configure Debug OMC */
	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(VS_DEBUGOMC), 0x01);

out:
	return ret;
}

/**
 * ufshcd_dwc_configuration()
 * UFS Host DWC specific configuration
 * @hba: private structure poitner
 *
 * Returns 0 on success, non-zero value on failure
 */
int ufshcd_dwc_configuration(struct ufs_hba *hba)
{
	int ret = 0;

	/* Program Clock Divider Value */
	ufshcd_dwc_program_clk_div(hba, UFSHCD_CLK_DIV_125);

#ifdef CONFIG_SCSI_UFS_DWC_MPHY_TC
	ret = ufshcd_dwc_setup_mphy(hba);
	if (ret) {
		dev_err(hba->dev, "MPHY configuration failed (%d)", ret);
		goto out;
	}
#endif
	ret = ufshcd_dme_link_startup(hba);
	if (ret) {
		dev_err(hba->dev, "Link Startup command failed (%d)", ret);
		goto out;
	}

	ret = ufshcd_dwc_link_is_up(hba);
	if (ret) {
		dev_err(hba->dev, "Link is not up");
		goto out;
	}

	ret = ufshcd_dwc_connection_setup(hba);
	if (ret) {
		dev_err(hba->dev, "Connection setup failed (%d)", ret);
		goto out;
	}

	ret = ufshcd_make_hba_operational(hba);
	if (ret) {
		dev_err(hba->dev, "HBA kick start failed (%d)", ret);
		goto out;
	}

	ret = ufshcd_verify_dev_init(hba);
	if (ret) {
		dev_err(hba->dev, "Device init failed (%d)", ret);
		goto out;
	}

	ret = ufshcd_complete_dev_init(hba);
	if (ret) {
		dev_err(hba->dev, "Device final init failed (%d)", ret);
		goto out;
	}

	ufshcd_set_ufs_dev_active(hba);
	hba->wlun_dev_clr_ua = false;

	if (hba->ufshcd_state == UFSHCD_STATE_RESET)
		scsi_unblock_requests(hba->host);

	hba->ufshcd_state = UFSHCD_STATE_OPERATIONAL;

	scsi_scan_host(hba->host);

out:
	return ret;
}
EXPORT_SYMBOL(ufshcd_dwc_configuration);
