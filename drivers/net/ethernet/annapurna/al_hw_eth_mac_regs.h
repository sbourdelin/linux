/*
 * Copyright (C) 2017, Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __AL_HW_ETH_MAC_REGS_H__
#define __AL_HW_ETH_MAC_REGS_H__

/*
* Unit Registers
*/

struct al_eth_mac_1g_stats {
	u32 reserved1[2];
	u32 aFramesTransmittedOK;
	u32 aFramesReceivedOK;
	u32 aFrameCheckSequenceErrors;
	u32 aAlignmentErrors;
	u32 aOctetsTransmittedOK;
	u32 aOctetsReceivedOK;
	u32 aPAUSEMACCtrlFramesTransmitted;
	u32 aPAUSEMACCtrlFramesReceived;
	u32 ifInErrors;
	u32 ifOutErrors;
	u32 ifInUcastPkts;
	u32 ifInMulticastPkts;
	u32 ifInBroadcastPkts;
	u32 reserved2;
	u32 ifOutUcastPkts;
	u32 ifOutMulticastPkts;
	u32 ifOutBroadcastPkts;
	u32 etherStatsDropEvents;
	u32 etherStatsOctets;
	u32 etherStatsPkts;
	u32 etherStatsUndersizePkts;
	u32 etherStatsOversizePkts;
	u32 etherStatsPkts64Octets;
	u32 etherStatsPkts65to127Octets;
	u32 etherStatsPkts128to255Octets;
	u32 etherStatsPkts256to511Octets;
	u32 etherStatsPkts512to1023Octets;
	u32 etherStatsPkts1024to1518Octets;
	u32 etherStatsPkts1519toX;
	u32 etherStatsJabbers;
	u32 etherStatsFragments;
	u32 reserved3[71];
};

struct al_eth_mac_1g {
	u32 rev;
	u32 scratch;
	u32 cmd_cfg;
	u32 mac_0;

	u32 mac_1;
	u32 frm_len;
	u32 pause_quant;
	u32 rx_section_empty;

	u32 rx_section_full;
	u32 tx_section_empty;
	u32 tx_section_full;
	u32 rx_almost_empty;

	u32 rx_almost_full;
	u32 tx_almost_empty;
	u32 tx_almost_full;
	u32 mdio_addr0;

	u32 mdio_addr1;
	u32 Reserved[5];

	u32 reg_stat;
	u32 tx_ipg_len;

	struct al_eth_mac_1g_stats stats;

	u32 phy_regs_base;
	u32 Reserved2[127];
};

struct al_eth_mac_10g_stats_v2 {
	u32 aFramesTransmittedOK;
	u32 reserved1;
	u32 aFramesReceivedOK;
	u32 reserved2;
	u32 aFrameCheckSequenceErrors;
	u32 reserved3;
	u32 aAlignmentErrors;
	u32 reserved4;
	u32 aPAUSEMACCtrlFramesTransmitted;
	u32 reserved5;
	u32 aPAUSEMACCtrlFramesReceived;
	u32 reserved6;
	u32 aFrameTooLongErrors;
	u32 reserved7;
	u32 aInRangeLengthErrors;
	u32 reserved8;
	u32 VLANTransmittedOK;
	u32 reserved9;
	u32 VLANReceivedOK;
	u32 reserved10;
	u32 ifOutOctetsL;
	u32 ifOutOctetsH;
	u32 ifInOctetsL;
	u32 ifInOctetsH;
	u32 ifInUcastPkts;
	u32 reserved11;
	u32 ifInMulticastPkts;
	u32 reserved12;
	u32 ifInBroadcastPkts;
	u32 reserved13;
	u32 ifOutErrors;
	u32 reserved14[3];
	u32 ifOutUcastPkts;
	u32 reserved15;
	u32 ifOutMulticastPkts;
	u32 reserved16;
	u32 ifOutBroadcastPkts;
	u32 reserved17;
	u32 etherStatsDropEvents;
	u32 reserved18;
	u32 etherStatsOctets;
	u32 reserved19;
	u32 etherStatsPkts;
	u32 reserved20;
	u32 etherStatsUndersizePkts;
	u32 reserved21;
	u32 etherStatsPkts64Octets;
	u32 reserved22;
	u32 etherStatsPkts65to127Octets;
	u32 reserved23;
	u32 etherStatsPkts128to255Octets;
	u32 reserved24;
	u32 etherStatsPkts256to511Octets;
	u32 reserved25;
	u32 etherStatsPkts512to1023Octets;
	u32 reserved26;
	u32 etherStatsPkts1024to1518Octets;
	u32 reserved27;
	u32 etherStatsPkts1519toX;
	u32 reserved28;
	u32 etherStatsOversizePkts;
	u32 reserved29;
	u32 etherStatsJabbers;
	u32 reserved30;
	u32 etherStatsFragments;
	u32 reserved31;
	u32 ifInErrors;
	u32 reserved32[91];
};

struct al_eth_mac_10g_stats_v3_rx {
	u32 etherStatsOctets;
	u32 reserved2;
	u32 ifOctetsL;
	u32 ifOctetsH;
	u32 aAlignmentErrors;
	u32 reserved4;
	u32 aPAUSEMACCtrlFrames;
	u32 reserved5;
	u32 FramesOK;
	u32 reserved6;
	u32 CRCErrors;
	u32 reserved7;
	u32 VLANOK;
	u32 reserved8;
	u32 ifInErrors;
	u32 reserved9;
	u32 ifInUcastPkts;
	u32 reserved10;
	u32 ifInMulticastPkts;
	u32 reserved11;
	u32 ifInBroadcastPkts;
	u32 reserved12;
	u32 etherStatsDropEvents;
	u32 reserved13;
	u32 etherStatsPkts;
	u32 reserved14;
	u32 etherStatsUndersizePkts;
	u32 reserved15;
	u32 etherStatsPkts64Octets;
	u32 reserved16;
	u32 etherStatsPkts65to127Octets;
	u32 reserved17;
	u32 etherStatsPkts128to255Octets;
	u32 reserved18;
	u32 etherStatsPkts256to511Octets;
	u32 reserved19;
	u32 etherStatsPkts512to1023Octets;
	u32 reserved20;
	u32 etherStatsPkts1024to1518Octets;
	u32 reserved21;
	u32 etherStatsPkts1519toMax;
	u32 reserved22;
	u32 etherStatsOversizePkts;
	u32 reserved23;
	u32 etherStatsJabbers;
	u32 reserved24;
	u32 etherStatsFragments;
	u32 reserved25;
	u32 aMACControlFramesReceived;
	u32 reserved26;
	u32 aFrameTooLong;
	u32 reserved27;
	u32 aInRangeLengthErrors;
	u32 reserved28;
	u32 reserved29[10];
};

struct al_eth_mac_10g_stats_v3_tx {
	u32 etherStatsOctets;
	u32 reserved30;
	u32 ifOctetsL;
	u32 ifOctetsH;
	u32 aAlignmentErrors;
	u32 reserved32;
	u32 aPAUSEMACCtrlFrames;
	u32 reserved33;
	u32 FramesOK;
	u32 reserved34;
	u32 CRCErrors;
	u32 reserved35;
	u32 VLANOK;
	u32 reserved36;
	u32 ifOutErrors;
	u32 reserved37;
	u32 ifUcastPkts;
	u32 reserved38;
	u32 ifMulticastPkts;
	u32 reserved39;
	u32 ifBroadcastPkts;
	u32 reserved40;
	u32 etherStatsDropEvents;
	u32 reserved41;
	u32 etherStatsPkts;
	u32 reserved42;
	u32 etherStatsUndersizePkts;
	u32 reserved43;
	u32 etherStatsPkts64Octets;
	u32 reserved44;
	u32 etherStatsPkts65to127Octets;
	u32 reserved45;
	u32 etherStatsPkts128to255Octets;
	u32 reserved46;
	u32 etherStatsPkts256to511Octets;
	u32 reserved47;
	u32 etherStatsPkts512to1023Octets;
	u32 reserved48;
	u32 etherStatsPkts1024to1518Octets;
	u32 reserved49;
	u32 etherStatsPkts1519toTX_MTU;
	u32 reserved50;
	u32 reserved51[4];
	u32 aMACControlFrames;
	u32 reserved52[15];
};

struct al_eth_mac_10g_stats_v3 {
	u32 reserved1[32];

	struct al_eth_mac_10g_stats_v3_rx rx;
	struct al_eth_mac_10g_stats_v3_tx tx;
};

union al_eth_mac_10g_stats {
	struct al_eth_mac_10g_stats_v2	v2;
	struct al_eth_mac_10g_stats_v3	v3;
};

struct al_eth_mac_10g {
	u32 rev;
	u32 scratch;
	u32 cmd_cfg;
	u32 mac_0;

	u32 mac_1;
	u32 frm_len;
	u32 Reserved;
	u32 rx_fifo_sections;

	u32 tx_fifo_sections;
	u32 rx_fifo_almost_f_e;
	u32 tx_fifo_almost_f_e;
	u32 hashtable_load;

	u32 mdio_cfg_status;
	u16 mdio_cmd;
	u16 reserved1;
	u16 mdio_data;
	u16 reserved2;
	u16 mdio_regaddr;
	u16 reserved3;

	u32 status;
	u32 tx_ipg_len;
	u32 Reserved1[3];

	u32 cl01_pause_quanta;
	u32 cl23_pause_quanta;
	u32 cl45_pause_quanta;

	u32 cl67_pause_quanta;
	u32 cl01_quanta_thresh;
	u32 cl23_quanta_thresh;
	u32 cl45_quanta_thresh;

	u32 cl67_quanta_thresh;
	u32 rx_pause_status;
	u32 Reserved2;
	u32 ts_timestamp;

	union al_eth_mac_10g_stats stats;

	u32 control;
	u32 status_reg;
	u32 phy_id[2];

	u32 dev_ability;
	u32 partner_ability;
	u32 an_expansion;
	u32 device_np;

	u32 partner_np;
	u32 Reserved4[9];

	u32 link_timer_lo;
	u32 link_timer_hi;

	u32 if_mode;

	u32 Reserved5[43];
};

struct al_eth_mac_gen {
	/*  Ethernet Controller Version */
	u32 version;
	u32 rsrvd_0[2];
	/* MAC selection configuration */
	u32 cfg;
	/* 10/100/1000 MAC external configuration */
	u32 mac_1g_cfg;
	/* 10/100/1000 MAC status */
	u32 mac_1g_stat;
	/* RGMII external configuration */
	u32 rgmii_cfg;
	/* RGMII status */
	u32 rgmii_stat;
	/* 1/2.5/10G MAC external configuration */
	u32 mac_10g_cfg;
	/* 1/2.5/10G MAC status */
	u32 mac_10g_stat;
	/* XAUI PCS configuration */
	u32 xaui_cfg;
	/* XAUI PCS status */
	u32 xaui_stat;
	/* RXAUI PCS configuration */
	u32 rxaui_cfg;
	/* RXAUI PCS status */
	u32 rxaui_stat;
	/* Signal detect configuration */
	u32 sd_cfg;
	/* MDIO control register for MDIO interface 1 */
	u32 mdio_ctrl_1;
	/* MDIO information register for MDIO interface 1 */
	u32 mdio_1;
	/* MDIO control register for MDIO interface 2 */
	u32 mdio_ctrl_2;
	/* MDIO information register for MDIO interface 2 */
	u32 mdio_2;
	/* XGMII 32 to 64 data FIFO control */
	u32 xgmii_dfifo_32_64;
	/* Reserved 1 out */
	u32 mac_res_1_out;
	/* XGMII 64 to 32 data FIFO control */
	u32 xgmii_dfifo_64_32;
	/* Reserved 1 in */
	u32 mac_res_1_in;
	/* SerDes TX FIFO control */
	u32 sd_fifo_ctrl;
	/* SerDes TX FIFO status */
	u32 sd_fifo_stat;
	/* SerDes in/out selection */
	u32 mux_sel;
	/* Clock configuration */
	u32 clk_cfg;
	u32 rsrvd_1;
	/* LOS and SD selection */
	u32 los_sel;
	/* RGMII selection configuration */
	u32 rgmii_sel;
	/* Ethernet LED configuration */
	u32 led_cfg;
	u32 rsrvd[33];
};

struct al_eth_mac_kr {
	/* PCS register file address */
	u32 pcs_addr;
	/* PCS register file data */
	u32 pcs_data;
	/* AN register file address */
	u32 an_addr;
	/* AN register file data */
	u32 an_data;
	/* PMA register file address */
	u32 pma_addr;
	/* PMA register file data */
	u32 pma_data;
	/* MTIP register file address */
	u32 mtip_addr;
	/* MTIP register file data */
	u32 mtip_data;
	/* KR PCS config  */
	u32 pcs_cfg;
	/* KR PCS status  */
	u32 pcs_stat;
	u32 rsrvd[54];
};

struct al_eth_mac_sgmii {
	/* PCS register file address */
	u32 reg_addr;
	/* PCS register file data */
	u32 reg_data;
	/* PCS clock divider configuration */
	u32 clk_div;
	/* PCS Status */
	u32 link_stat;
	u32 rsrvd[60];
};

struct al_eth_mac_stat {
	/* Receive rate matching error */
	u32 match_fault;
	/* EEE, number of times the MAC went into low power mode */
	u32 eee_in;
	/* EEE, number of times the MAC went out of low power mode */
	u32 eee_out;
	/*
	 * 40G PCS,
	 * FEC corrected error indication
	 */
	u32 v3_pcs_40g_ll_cerr_0;
	/*
	 * 40G PCS,
	 * FEC corrected error indication
	 */
	u32 v3_pcs_40g_ll_cerr_1;
	/*
	 * 40G PCS,
	 * FEC corrected error indication
	 */
	u32 v3_pcs_40g_ll_cerr_2;
	/*
	 * 40G PCS,
	 * FEC corrected error indication
	 */
	u32 v3_pcs_40g_ll_cerr_3;
	/*
	 * 40G PCS,
	 * FEC uncorrectable error indication
	 */
	u32 v3_pcs_40g_ll_ncerr_0;
	/*
	 * 40G PCS,
	 * FEC uncorrectable error indication
	 */
	u32 v3_pcs_40g_ll_ncerr_1;
	/*
	 * 40G PCS,
	 * FEC uncorrectable error indication
	 */
	u32 v3_pcs_40g_ll_ncerr_2;
	/*
	 * 40G PCS,
	 * FEC uncorrectable error indication
	 */
	u32 v3_pcs_40g_ll_ncerr_3;
	/*
	 * 10G_LL PCS,
	 * FEC corrected error indication
	 */
	u32 v3_pcs_10g_ll_cerr;
	/*
	 * 10G_LL PCS,
	 * FEC uncorrectable error indication
	 */
	u32 v3_pcs_10g_ll_ncerr;
	u32 rsrvd[51];
};

struct al_eth_mac_stat_lane {
	/* Character error */
	u32 char_err;
	/* Disparity error */
	u32 disp_err;
	/* Comma detection */
	u32 pat;
	u32 rsrvd[13];
};

struct al_eth_mac_gen_v3 {
	/* ASYNC FIFOs control */
	u32 afifo_ctrl;
	/* TX ASYNC FIFO configuration */
	u32 tx_afifo_cfg_1;
	/* TX ASYNC FIFO configuration */
	u32 tx_afifo_cfg_2;
	/* TX ASYNC FIFO configuration */
	u32 tx_afifo_cfg_3;
	/* TX ASYNC FIFO configuration */
	u32 tx_afifo_cfg_4;
	/* TX ASYNC FIFO configuration */
	u32 tx_afifo_cfg_5;
	/* RX ASYNC FIFO configuration */
	u32 rx_afifo_cfg_1;
	/* RX ASYNC FIFO configuration */
	u32 rx_afifo_cfg_2;
	/* RX ASYNC FIFO configuration */
	u32 rx_afifo_cfg_3;
	/* RX ASYNC FIFO configuration */
	u32 rx_afifo_cfg_4;
	/* RX ASYNC FIFO configuration */
	u32 rx_afifo_cfg_5;
	/* MAC selection configuration */
	u32 mac_sel;
	/* 10G LL MAC configuration */
	u32 mac_10g_ll_cfg;
	/* 10G LL MAC control */
	u32 mac_10g_ll_ctrl;
	/* 10G LL PCS configuration */
	u32 pcs_10g_ll_cfg;
	/* 10G LL PCS status */
	u32 pcs_10g_ll_status;
	/* 40G LL PCS configuration */
	u32 pcs_40g_ll_cfg;
	/* 40G LL PCS status */
	u32 pcs_40g_ll_status;
	/* PCS 40G  register file address */
	u32 pcs_40g_ll_addr;
	/* PCS 40G register file data */
	u32 pcs_40g_ll_data;
	/* 40G LL MAC configuration */
	u32 mac_40g_ll_cfg;
	/* 40G LL MAC status */
	u32 mac_40g_ll_status;
	/* Preamble configuration (high [55:32]) */
	u32 preamble_cfg_high;
	/* Preamble configuration (low [31:0]) */
	u32 preamble_cfg_low;
	/* MAC 40G register file address */
	u32 mac_40g_ll_addr;
	/* MAC 40G register file data */
	u32 mac_40g_ll_data;
	/* 40G LL MAC control */
	u32 mac_40g_ll_ctrl;
	/* PCS 40G  register file address */
	u32 pcs_40g_fec_91_ll_addr;
	/* PCS 40G register file data */
	u32 pcs_40g_fec_91_ll_data;
	/* 40G LL PCS EEE configuration */
	u32 pcs_40g_ll_eee_cfg;
	/* 40G LL PCS EEE status */
	u32 pcs_40g_ll_eee_status;
	/*
	 * SERDES 32-bit interface shift configuration (when swap is
	 * enabled)
	 */
	u32 serdes_32_tx_shift;
	/*
	 * SERDES 32-bit interface shift configuration (when swap is
	 * enabled)
	 */
	u32 serdes_32_rx_shift;
	/*
	 * SERDES 32-bit interface bit selection
	 */
	u32 serdes_32_tx_sel;
	/*
	 * SERDES 32-bit interface bit selection
	 */
	u32 serdes_32_rx_sel;
	/* AN/LT wrapper  control */
	u32 an_lt_ctrl;
	/* AN/LT wrapper  register file address */
	u32 an_lt_0_addr;
	/* AN/LT wrapper register file data */
	u32 an_lt_0_data;
	/* AN/LT wrapper  register file address */
	u32 an_lt_1_addr;
	/* AN/LT wrapper register file data */
	u32 an_lt_1_data;
	/* AN/LT wrapper  register file address */
	u32 an_lt_2_addr;
	/* AN/LT wrapper register file data */
	u32 an_lt_2_data;
	/* AN/LT wrapper  register file address */
	u32 an_lt_3_addr;
	/* AN/LT wrapper register file data */
	u32 an_lt_3_data;
	/* External SERDES control */
	u32 ext_serdes_ctrl;
	/* spare bits */
	u32 spare;
	u32 rsrvd[18];
};

struct al_eth_mac_regs {
	struct al_eth_mac_1g mac_1g;
	struct al_eth_mac_10g mac_10g;
	u32 rsrvd_0[64];
	struct al_eth_mac_gen gen;
	struct al_eth_mac_kr kr;
	struct al_eth_mac_sgmii sgmii;
	struct al_eth_mac_stat stat;
	struct al_eth_mac_stat_lane stat_lane[4];
	struct al_eth_mac_gen_v3 gen_v3;
};

/* cmd_cfg */
#define ETH_1G_MAC_CMD_CFG_TX_ENA	BIT(0)
#define ETH_1G_MAC_CMD_CFG_RX_ENA	BIT(1)
/* enable Half Duplex */
#define ETH_1G_MAC_CMD_CFG_HD_EN	BIT(10)
/* enable 1G speed */
#define ETH_1G_MAC_CMD_CFG_1G_SPD	BIT(3)
/* enable 10M speed */
#define ETH_1G_MAC_CMD_CFG_10M_SPD	BIT(25)

/* cmd_cfg */
#define ETH_10G_MAC_CMD_CFG_TX_ENA				BIT(0)
#define ETH_10G_MAC_CMD_CFG_RX_ENA				BIT(1)

/* mdio_cfg_status */
#define ETH_10G_MAC_MDIO_CFG_HOLD_TIME_MASK	0x0000001c
#define ETH_10G_MAC_MDIO_CFG_HOLD_TIME_SHIFT	2

#define ETH_10G_MAC_MDIO_CFG_HOLD_TIME_7_CLK	3

/* control */
#define ETH_10G_MAC_CONTROL_AN_EN_MASK	0x00001000

/* if_mode */
#define ETH_10G_MAC_IF_MODE_SGMII_EN_MASK	0x00000001
#define ETH_10G_MAC_IF_MODE_SGMII_AN_MASK	0x00000002
#define ETH_10G_MAC_IF_MODE_SGMII_SPEED_MASK	0x0000000c
#define ETH_10G_MAC_IF_MODE_SGMII_SPEED_SHIFT	2
#define ETH_10G_MAC_IF_MODE_SGMII_DUPLEX_MASK	0x00000010
#define ETH_10G_MAC_IF_MODE_SGMII_DUPLEX_SHIFT	4

#define ETH_10G_MAC_IF_MODE_SGMII_SPEED_10M	0
#define ETH_10G_MAC_IF_MODE_SGMII_SPEED_100M	1
#define ETH_10G_MAC_IF_MODE_SGMII_SPEED_1G	2

#define ETH_10G_MAC_IF_MODE_SGMII_DUPLEX_FULL	0
#define ETH_10G_MAC_IF_MODE_SGMII_DUPLEX_HALF	1

/*
 * Selection of the input for the "set_1000" input of the RGMII converter
 * 0 - From MAC
 * 1 - From register set_1000_def (automatic speed selection)
 */
#define ETH_MAC_GEN_RGMII_CFG_SET_1000_SEL BIT(0)
/*
 * Selection of the input for the "set_10" input of the RGMII converter:
 * 0 - From MAC
 * 1 - From register set_10_def (automatic speed selection)
 */
#define ETH_MAC_GEN_RGMII_CFG_SET_10_SEL BIT(4)
/* Enable automatic speed selection (based on PHY in-band status information) */
#define ETH_MAC_GEN_RGMII_CFG_ENA_AUTO   BIT(8)

#define ETH_MAC_GEN_MUX_SEL_KR_IN_MASK   0x0000C000

/*
 * LED source selection:
 * 0 – Default reg
 * 1 – Rx activity
 * 2 – Tx activity
 * 3 – Rx | Tx activity
 * 4-9 – SGMII LEDs
 */
#define ETH_MAC_GEN_LED_CFG_SEL_MASK     0x0000000F

/* turn the led on/off based on default value field (ETH_MAC_GEN_LED_CFG_DEF) */
#define ETH_MAC_GEN_LED_CFG_SEL_DEFAULT_REG	0

/* LED default value */
#define ETH_MAC_GEN_LED_CFG_DEF          BIT(4)

#define ETH_MAC_SGMII_REG_ADDR_CTRL_REG	0x0
#define ETH_MAC_SGMII_REG_ADDR_IF_MODE_REG 0x14

#define ETH_MAC_SGMII_REG_DATA_CTRL_AN_ENABLE			BIT(12)
#define ETH_MAC_SGMII_REG_DATA_IF_MODE_SGMII_EN			BIT(0)
#define ETH_MAC_SGMII_REG_DATA_IF_MODE_SGMII_AN			BIT(1)
#define ETH_MAC_SGMII_REG_DATA_IF_MODE_SGMII_SPEED_10		0x0
#define ETH_MAC_SGMII_REG_DATA_IF_MODE_SGMII_SPEED_100		0x1
#define ETH_MAC_SGMII_REG_DATA_IF_MODE_SGMII_SPEED_1000		0x2
#define ETH_MAC_SGMII_REG_DATA_IF_MODE_SGMII_DUPLEX		BIT(4)

/* command config */
#define ETH_MAC_GEN_V3_MAC_40G_COMMAND_CONFIG_ADDR	0x00000008
#define ETH_MAC_GEN_V3_MAC_40G_COMMAND_CONFIG_TX_ENA	BIT(0)
#define ETH_MAC_GEN_V3_MAC_40G_COMMAND_CONFIG_RX_ENA	BIT(1)
#define ETH_MAC_GEN_V3_MAC_40G_COMMAND_CONFIG_PFC_MODE	BIT(19)

/* frame length */
#define ETH_MAC_GEN_V3_MAC_40G_FRM_LENGTH_ADDR		0x00000014

#define ETH_MAC_GEN_V3_MAC_40G_CL01_PAUSE_QUANTA_ADDR	0x00000054
#define ETH_MAC_GEN_V3_MAC_40G_CL23_PAUSE_QUANTA_ADDR	0x00000058
#define ETH_MAC_GEN_V3_MAC_40G_CL45_PAUSE_QUANTA_ADDR	0x0000005C
#define ETH_MAC_GEN_V3_MAC_40G_CL67_PAUSE_QUANTA_ADDR	0x00000060
#define ETH_MAC_GEN_V3_MAC_40G_CL01_QUANTA_THRESH_ADDR	0x00000064
#define ETH_MAC_GEN_V3_MAC_40G_CL23_QUANTA_THRESH_ADDR	0x00000068
#define ETH_MAC_GEN_V3_MAC_40G_CL45_QUANTA_THRESH_ADDR	0x0000006C
#define ETH_MAC_GEN_V3_MAC_40G_CL67_QUANTA_THRESH_ADDR	0x00000070

/* spare */
#define ETH_MAC_GEN_V3_SPARE_CHICKEN_DISABLE_TIMESTAMP_STRETCH BIT(0)

#endif /* __AL_HW_ETH_MAC_REGS_H__ */
