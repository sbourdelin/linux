/*
 * Copyright (C) 2017, Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __AL_HW_EC_REG_H
#define __AL_HW_EC_REG_H

struct al_ec_gen {
	/*  Ethernet controller Version */
	u32 version;
	/*  Enable modules operation. */
	u32 en;
	/*  Enable FIFO operation on the EC side. */
	u32 fifo_en;
	/*  General L2 configuration for the Ethernet controlle */
	u32 l2;
	/*  Configure protocol index values */
	u32 cfg_i;
	/*  Configure protocol index values (extended protocols */
	u32 cfg_i_ext;
	/*  Enable modules operation (extended operations). */
	u32 en_ext;
	u32 rsrvd[9];
};

struct al_ec_mac {
	/*  General configuration of the MAC side of the Ethern */
	u32 gen;
	/*  Minimum packet size  */
	u32 min_pkt;
	/*  Maximum packet size  */
	u32 max_pkt;
	u32 rsrvd[13];
};

struct al_ec_rxf {
	/*  Rx FIFO input controller configuration 1 */
	u32 cfg_1;
	/*  Rx FIFO input controller configuration 2 */
	u32 cfg_2;
	/*  Threshold to start reading packet from the Rx FIFO */
	u32 rd_fifo;
	/*  Threshold to stop writing packet to the Rx FIFO */
	u32 wr_fifo;
	/*  Threshold to stop writing packet to the loopback FI */
	u32 lb_fifo;
	/*  Rx FIFO input controller loopback FIFO configuratio */
	u32 cfg_lb;
	/*  Configuration for dropping packet at the FIFO outpu */
	u32 out_drop;
	u32 rsrvd[25];
};

struct al_ec_epe {
	/*  Ethernet parsing engine configuration 1 */
	u32 parse_cfg;
	/*  Protocol index action table address */
	u32 act_table_addr;
	/*  Protocol index action table data */
	u32 act_table_data_1;
	/*  Protocol index action table data */
	u32 act_table_data_2;
	/*  Protocol index action table data */
	u32 act_table_data_3;
	/*  Protocol index action table data */
	u32 act_table_data_4;
	/*  Protocol index action table data */
	u32 act_table_data_5;
	/*  Protocol index action table data */
	u32 act_table_data_6;
	/*  Input result vector, default values for parser inpu */
	u32 res_def;
	/*  Result input vector selection */
	u32 res_in;
	u32 rsrvd[6];
};

struct al_ec_epe_res {
	/*  Parser result vector pointer */
	u32 p1;
	/*  Parser result vector pointer */
	u32 p2;
	/*  Parser result vector pointer */
	u32 p3;
	/*  Parser result vector pointer */
	u32 p4;
	/*  Parser result vector pointer */
	u32 p5;
	/*  Parser result vector pointer */
	u32 p6;
	/*  Parser result vector pointer */
	u32 p7;
	/*  Parser result vector pointer */
	u32 p8;
	/*  Parser result vector pointer */
	u32 p9;
	/*  Parser result vector pointer */
	u32 p10;
	/*  Parser result vector pointer */
	u32 p11;
	/*  Parser result vector pointer */
	u32 p12;
	/*  Parser result vector pointer */
	u32 p13;
	/*  Parser result vector pointer */
	u32 p14;
	/*  Parser result vector pointer */
	u32 p15;
	/*  Parser result vector pointer */
	u32 p16;
	/*  Parser result vector pointer */
	u32 p17;
	/*  Parser result vector pointer */
	u32 p18;
	/*  Parser result vector pointer */
	u32 p19;
	/*  Parser result vector pointer */
	u32 p20;
	u32 rsrvd[12];
};

struct al_ec_epe_h {
	/*  Header length, support for header length table for  */
	u32 hdr_len;
};

struct al_ec_epe_p {
	/*  Data  for comparison */
	u32 comp_data;
	/*  Mask for comparison */
	u32 comp_mask;
	/*  Compare control */
	u32 comp_ctrl;
	u32 rsrvd[4];
};

struct al_ec_epe_a {
	/*  Protocol index action register */
	u32 prot_act;
};

struct al_ec_rfw {
	/*  Tuple (4/2) Hash configuration */
	u32 thash_cfg_1;
	/*  Tuple (4/2) Hash configuration */
	u32 thash_cfg_2;
	/*  MAC Hash configuration */
	u32 mhash_cfg_1;
	/*  MAC Hash configuration */
	u32 mhash_cfg_2;
	/*  MAC Hash configuration */
	u32 hdr_split;
	/*  Masking the errors described in  register rxf_drop  */
	u32 meta_err;
	/*  Configuration for generating the MetaData for the R */
	u32 meta;
	/*  Configuration for generating the MetaData for the R */
	u32 filter;
	/*  4 tupple hash table address */
	u32 thash_table_addr;
	/*  4 tupple hash table data */
	u32 thash_table_data;
	/*  MAC hash table address */
	u32 mhash_table_addr;
	/*  MAC hash table data */
	u32 mhash_table_data;
	/*  VLAN table address */
	u32 vid_table_addr;
	/*  VLAN table data */
	u32 vid_table_data;
	/*  VLAN p-bits table address */
	u32 pbits_table_addr;
	/*  VLAN p-bits table data */
	u32 pbits_table_data;
	/*  DSCP table address */
	u32 dscp_table_addr;
	/*  DSCP table data */
	u32 dscp_table_data;
	/*  TC table address */
	u32 tc_table_addr;
	/*  TC table data */
	u32 tc_table_data;
	/*  Control table address */
	u32 ctrl_table_addr;
	/*  Control table data */
	u32 ctrl_table_data;
	/*  Forwarding output configuration */
	u32 out_cfg;
	/*  Flow steering mechanism, Table address */
	u32 fsm_table_addr;
	/*  Flow steering mechanism, Table data */
	u32 fsm_table_data;
	/*  Selection of data to be used in packet forwarding0  */
	u32 ctrl_sel;
	/*  Default VLAN data, used for untagged packets */
	u32 default_vlan;
	/*  Default HASH output values */
	u32 default_hash;
	/*  Default override values, if a packet was filtered b */
	u32 default_or;
	/*  Latched information when a drop condition occurred */
	u32 drop_latch;
	/*  Check sum calculation configuration */
	u32 checksum;
	/*  LRO offload engine configuration register */
	u32 lro_cfg_1;
	/*  LRO offload engine Check rules configurations for I */
	u32 lro_check_ipv4;
	/*  LRO offload engine IPv4 values configuration */
	u32 lro_ipv4;
	/*  LRO offload engine Check rules configurations for I */
	u32 lro_check_ipv6;
	/*  LRO offload engine IPv6 values configuration */
	u32 lro_ipv6;
	/*  LRO offload engine Check rules configurations for T */
	u32 lro_check_tcp;
	/*  LRO offload engine IPv6 values configuration */
	u32 lro_tcp;
	/*  LRO offload engine Check rules configurations for U */
	u32 lro_check_udp;
	/*  LRO offload engine Check rules configurations for U */
	u32 lro_check_l2;
	/*  LRO offload engine Check rules configurations for U */
	u32 lro_check_gen;
	/*  Rules for storing packet information into the cache */
	u32 lro_store;
	/*  VLAN table default */
	u32 vid_table_def;
	/*  Control table default */
	u32 ctrl_table_def;
	/*  Additional configuration 0 */
	u32 cfg_a_0;
	/*  Tuple (4/2) Hash configuration (extended for RoCE a */
	u32 thash_cfg_3;
	/*  Tuple (4/2) Hash configuration , mask for the input */
	u32 thash_mask_outer_ipv6;
	/*  Tuple (4/2) Hash configuration , mask for the input */
	u32 thash_mask_outer;
	/*  Tuple (4/2) Hash configuration , mask for the input */
	u32 thash_mask_inner_ipv6;
	/*  Tuple (4/2) Hash configuration , mask for the input */
	u32 thash_mask_inner;
	u32 rsrvd[10];
};

struct al_ec_rfw_udma {
	/*  Per UDMA default configuration */
	u32 def_cfg;
};

struct al_ec_rfw_hash {
	/*  key configuration (320 bits) */
	u32 key;
};

struct al_ec_rfw_priority {
	/*  Priority to queue mapping configuration */
	u32 queue;
};

struct al_ec_rfw_default {
	/*  Default forwarding configuration options */
	u32 opt_1;
};

struct al_ec_fwd_mac {
	/*  MAC address data [31:0] */
	u32 data_l;
	/*  MAC address data [15:0] */
	u32 data_h;
	/*  MAC address mask [31:0] */
	u32 mask_l;
	/*  MAC address mask [15:0] */
	u32 mask_h;
	/*  MAC compare control */
	u32 ctrl;
};

struct al_ec_msw {
	/*  Configuration for unicast packets */
	u32 uc;
	/*  Configuration for multicast packets */
	u32 mc;
	/*  Configuration for broadcast packets */
	u32 bc;
	u32 rsrvd[3];
};

struct al_ec_tso {
	/*  Input configuration */
	u32 in_cfg;
	/*  MetaData default cache table address */
	u32 cache_table_addr;
	/*  MetaData default cache table data */
	u32 cache_table_data_1;
	/*  MetaData default cache table data */
	u32 cache_table_data_2;
	/*  MetaData default cache table data */
	u32 cache_table_data_3;
	/*  MetaData default cache table data */
	u32 cache_table_data_4;
	/*  TCP control bit operation for first segment */
	u32 ctrl_first;
	/*  TCP control bit operation for middle segments  */
	u32 ctrl_middle;
	/*  TCP control bit operation for last segment */
	u32 ctrl_last;
	/*  Additional TSO configurations */
	u32 cfg_add_0;
	/*  TSO configuration for tunnelled packets */
	u32 cfg_tunnel;
	u32 rsrvd[13];
};

struct al_ec_tso_sel {
	/*  MSS value */
	u32 mss;
};

struct al_ec_tpe {
	/*  Parsing configuration */
	u32 parse;
	u32 rsrvd[15];
};

struct al_ec_tpm_udma {
	/*  Default VLAN data */
	u32 vlan_data;
	/*  UDMA MAC SA information for spoofing */
	u32 mac_sa_1;
	/*  UDMA MAC SA information for spoofing */
	u32 mac_sa_2;
};

struct al_ec_tpm_sel {
	/*  Ethertype values for VLAN modification */
	u32 etype;
};

struct al_ec_tfw {
	/*  Tx FIFO Wr configuration */
	u32 tx_wr_fifo;
	/*  VLAN table address */
	u32 tx_vid_table_addr;
	/*  VLAN table data */
	u32 tx_vid_table_data;
	/*  Tx FIFO Rd configuration */
	u32 tx_rd_fifo;
	/*  Tx FIFO Rd configuration, checksum insertion */
	u32 tx_checksum;
	/*  Tx forwarding general configuration register */
	u32 tx_gen;
	/*  Tx spoofing configuration */
	u32 tx_spf;
	/*  TX data FIFO status */
	u32 data_fifo;
	/*  Tx control FIFO status */
	u32 ctrl_fifo;
	/*  Tx header FIFO status */
	u32 hdr_fifo;
	u32 rsrvd[14];
};

struct al_ec_tfw_udma {
	/*  Default GMDA output bitmap for unicast packet */
	u32 uc_udma;
	/*  Default GMDA output bitmap for multicast packet */
	u32 mc_udma;
	/*  Default GMDA output bitmap for broadcast packet */
	u32 bc_udma;
	/*  Tx spoofing configuration */
	u32 spf_cmd;
	/*  Forwarding decision control */
	u32 fwd_dec;
	u32 rsrvd;
};

struct al_ec_tmi {
	/*  Forward packets back to the Rx data path for local  */
	u32 tx_cfg;
	u32 rsrvd[3];
};

struct al_ec_efc {
	/*  Mask of pause_on  [7:0] for the Ethernet controller */
	u32 ec_pause;
	/*  Mask of Ethernet controller Almost Full indication  */
	u32 ec_xoff;
	/*  Mask for generating XON indication pulse */
	u32 xon;
	/*  Mask for generating GPIO output XOFF indication fro */
	u32 gpio;
	/*  Rx FIFO threshold for generating the Almost Full in */
	u32 rx_fifo_af;
	/*  Rx FIFO threshold for generating the Almost Full in */
	u32 rx_fifo_hyst;
	/*  Rx FIFO threshold for generating the Almost Full in */
	u32 stat;
	/*  XOFF timer for the 1G MACSets the interval (in SB_C */
	u32 xoff_timer_1g;
	/*  PFC force flow control generation */
	u32 ec_pfc;
	u32 rsrvd[3];
};

struct al_ec_fc_udma {
	/*  Mask of "pause_on"  [0] for all queues */
	u32 q_pause_0;
	/*  Mask of "pause_on"  [1] for all queues */
	u32 q_pause_1;
	/*  Mask of "pause_on"  [2] for all queues */
	u32 q_pause_2;
	/*  Mask of "pause_on"  [3] for all queues */
	u32 q_pause_3;
	/*  Mask of "pause_on"  [4] for all queues */
	u32 q_pause_4;
	/*  Mask of "pause_on"  [5] for all queues */
	u32 q_pause_5;
	/*  Mask of "pause_on"  [6] for all queues */
	u32 q_pause_6;
	/*  Mask of "pause_on"  [7] for all queues */
	u32 q_pause_7;
	/*  Mask of external GPIO input pause [0] for all queue */
	u32 q_gpio_0;
	/*  Mask of external GPIO input pause [1] for all queue */
	u32 q_gpio_1;
	/*  Mask of external GPIO input pause [2] for all queue */
	u32 q_gpio_2;
	/*  Mask of external GPIO input pause [3] for all queue */
	u32 q_gpio_3;
	/*  Mask of external GPIO input [4] for all queues */
	u32 q_gpio_4;
	/*  Mask of external GPIO input [5] for all queues */
	u32 q_gpio_5;
	/*  Mask of external GPIO input [6] for all queues */
	u32 q_gpio_6;
	/*  Mask of external GPIO input [7] for all queues */
	u32 q_gpio_7;
	/*  Mask of "pause_on"  [7:0] for the UDMA stream inter */
	u32 s_pause;
	/*  Mask of Rx Almost Full indication for generating XO */
	u32 q_xoff_0;
	/*  Mask of Rx Almost Full indication for generating XO */
	u32 q_xoff_1;
	/*  Mask of Rx Almost Full indication for generating XO */
	u32 q_xoff_2;
	/*  Mask of Rx Almost Full indication for generating XO */
	u32 q_xoff_3;
	/*  Mask of Rx Almost Full indication for generating XO */
	u32 q_xoff_4;
	/*  Mask of Rx Almost Full indication for generating XO */
	u32 q_xoff_5;
	/*  Mask of Rx Almost Full indication for generating XO */
	u32 q_xoff_6;
	/*  Mask of Rx Almost Full indication for generating XO */
	u32 q_xoff_7;
	u32 rsrvd[7];
};

struct al_ec_tpg_rpa_res {
	/*  NOT used */
	u32 not_used;
	u32 rsrvd[63];
};

struct al_ec_eee {
	/*  EEE configuration */
	u32 cfg_e;
	/*  Number of clocks to get into EEE mode. */
	u32 pre_cnt;
	/*  Number of clocks to stop MAC EEE mode after getting */
	u32 post_cnt;
	/*  Number of clocks to stop the Tx MAC interface after */
	u32 stop_cnt;
	/*  EEE status */
	u32 stat_eee;
	u32 rsrvd[59];
};

struct al_ec_stat {
	/*  Rx Frequency adjust FIFO input  packets */
	u32 faf_in_rx_pkt;
	/*  Rx Frequency adjust FIFO input short error packets */
	u32 faf_in_rx_short;
	/*  Rx Frequency adjust FIFO input  long error packets */
	u32 faf_in_rx_long;
	/*  Rx Frequency adjust FIFO output  packets */
	u32 faf_out_rx_pkt;
	/*  Rx Frequency adjust FIFO output short error packets */
	u32 faf_out_rx_short;
	/*  Rx Frequency adjust FIFO output long error packets */
	u32 faf_out_rx_long;
	/*  Rx Frequency adjust FIFO output  drop packets */
	u32 faf_out_drop;
	/*  Number of packets written into the Rx FIFO (without */
	u32 rxf_in_rx_pkt;
	/*  Number of error packets written into the Rx FIFO (w */
	u32 rxf_in_fifo_err;
	/*  Number of packets written into the loopback FIFO (w */
	u32 lbf_in_rx_pkt;
	/*  Number of error packets written into the loopback F */
	u32 lbf_in_fifo_err;
	/*  Number of packets read from Rx FIFO 1 */
	u32 rxf_out_rx_1_pkt;
	/*  Number of packets read from Rx FIFO 2 (loopback FIF */
	u32 rxf_out_rx_2_pkt;
	/*  Rx FIFO output drop packets from FIFO 1 */
	u32 rxf_out_drop_1_pkt;
	/*  Rx FIFO output drop packets from FIFO 2 (loopback) */
	u32 rxf_out_drop_2_pkt;
	/*  Rx Parser 1, input packet counter */
	u32 rpe_1_in_rx_pkt;
	/*  Rx Parser 1, output packet counter */
	u32 rpe_1_out_rx_pkt;
	/*  Rx Parser 2, input packet counter */
	u32 rpe_2_in_rx_pkt;
	/*  Rx Parser 2, output packet counter */
	u32 rpe_2_out_rx_pkt;
	/*  Rx Parser 3 (MACsec), input packet counter */
	u32 rpe_3_in_rx_pkt;
	/*  Rx Parser 3 (MACsec), output packet counter */
	u32 rpe_3_out_rx_pkt;
	/*  Tx parser, input packet counter */
	u32 tpe_in_tx_pkt;
	/*  Tx parser, output packet counter */
	u32 tpe_out_tx_pkt;
	/*  Tx packet modification, input packet counter */
	u32 tpm_tx_pkt;
	/*  Tx forwarding input packet counter */
	u32 tfw_in_tx_pkt;
	/*  Tx forwarding input packet counter */
	u32 tfw_out_tx_pkt;
	/*  Rx forwarding input packet counter */
	u32 rfw_in_rx_pkt;
	/*  Rx Forwarding, packet with VLAN command drop indica */
	u32 rfw_in_vlan_drop;
	/*  Rx Forwarding, packets with parse drop indication */
	u32 rfw_in_parse_drop;
	/*  Rx Forwarding, multicast packets */
	u32 rfw_in_mc;
	/*  Rx Forwarding, broadcast packets */
	u32 rfw_in_bc;
	/*  Rx Forwarding, tagged packets */
	u32 rfw_in_vlan_exist;
	/*  Rx Forwarding, untagged packets */
	u32 rfw_in_vlan_nexist;
	/*  Rx Forwarding, packets with MAC address drop indica */
	u32 rfw_in_mac_drop;
	/*  Rx Forwarding, packets with undetected MAC address */
	u32 rfw_in_mac_ndet_drop;
	/*  Rx Forwarding, packets with drop indication from th */
	u32 rfw_in_ctrl_drop;
	/*  Rx Forwarding, packets with L3_protocol_index drop  */
	u32 rfw_in_prot_i_drop;
	/*  EEE, number of times the system went into EEE state */
	u32 eee_in;
	u32 rsrvd[90];
};

struct al_ec_stat_udma {
	/*  Rx forwarding output packet counter */
	u32 rfw_out_rx_pkt;
	/*  Rx forwarding output drop packet counter */
	u32 rfw_out_drop;
	/*  Multi-stream write, number of Rx packets */
	u32 msw_in_rx_pkt;
	/*  Multi-stream write, number of dropped packets at SO */
	u32 msw_drop_q_full;
	/*  Multi-stream write, number of dropped packets at SO */
	u32 msw_drop_sop;
	/*  Multi-stream write, number of dropped packets at EO */
	u32 msw_drop_eop;
	/*  Multi-stream write, number of packets written to th */
	u32 msw_wr_eop;
	/*  Multi-stream write, number of packets read from the */
	u32 msw_out_rx_pkt;
	/*  Number of transmitted packets without TSO enabled */
	u32 tso_no_tso_pkt;
	/*  Number of transmitted packets with TSO enabled */
	u32 tso_tso_pkt;
	/*  Number of TSO segments that were generated */
	u32 tso_seg_pkt;
	/*  Number of TSO segments that required padding */
	u32 tso_pad_pkt;
	/*  Tx Packet modification, MAC SA spoof error  */
	u32 tpm_tx_spoof;
	/*  Tx MAC interface, input packet counter */
	u32 tmi_in_tx_pkt;
	/*  Tx MAC interface, number of packets forwarded to th */
	u32 tmi_out_to_mac;
	/*  Tx MAC interface, number of packets forwarded to th */
	u32 tmi_out_to_rx;
	/*  Tx MAC interface, number of transmitted bytes */
	u32 tx_q0_bytes;
	/*  Tx MAC interface, number of transmitted bytes */
	u32 tx_q1_bytes;
	/*  Tx MAC interface, number of transmitted bytes */
	u32 tx_q2_bytes;
	/*  Tx MAC interface, number of transmitted bytes */
	u32 tx_q3_bytes;
	/*  Tx MAC interface, number of transmitted packets */
	u32 tx_q0_pkts;
	/*  Tx MAC interface, number of transmitted packets */
	u32 tx_q1_pkts;
	/*  Tx MAC interface, number of transmitted packets */
	u32 tx_q2_pkts;
	/*  Tx MAC interface, number of transmitted packets */
	u32 tx_q3_pkts;
	u32 rsrvd[40];
};

struct al_ec_msp {
	/*  Ethernet parsing engine configuration 1 */
	u32 p_parse_cfg;
	/*  Protocol index action table address */
	u32 p_act_table_addr;
	/*  Protocol index action table data */
	u32 p_act_table_data_1;
	/*  Protocol index action table data */
	u32 p_act_table_data_2;
	/*  Protocol index action table data */
	u32 p_act_table_data_3;
	/*  Protocol index action table data */
	u32 p_act_table_data_4;
	/*  Protocol index action table data */
	u32 p_act_table_data_5;
	/*  Protocol index action table data */
	u32 p_act_table_data_6;
	/*  Input result vector, default values for parser inpu */
	u32 p_res_def;
	/*  Result input vector selection */
	u32 p_res_in;
	u32 rsrvd[6];
};

struct al_ec_msp_p {
	/*  Header length, support for header length table for  */
	u32 h_hdr_len;
};

struct al_ec_msp_c {
	/*  Data  for comparison */
	u32 p_comp_data;
	/*  Mask for comparison */
	u32 p_comp_mask;
	/*  Compare control */
	u32 p_comp_ctrl;
	u32 rsrvd[4];
};

struct al_ec_wol {
	/*  WoL enable configuration,Packet forwarding and inte */
	u32 wol_en;
	/*  Password for magic_password packet detection - bits */
	u32 magic_pswd_l;
	/*  Password for magic+password packet detection -  47: */
	u32 magic_pswd_h;
	/*  Configured L3 Destination IP address for WoL IPv6 p */
	u32 ipv6_dip_word0;
	/*  Configured L3 Destination IP address for WoL IPv6 p */
	u32 ipv6_dip_word1;
	/*  Configured L3 Destination IP address for WoL IPv6 p */
	u32 ipv6_dip_word2;
	/*  Configured L3 Destination IP address for WoL IPv6 p */
	u32 ipv6_dip_word3;
	/*  Configured L3 Destination IP address for WoL IPv4 p */
	u32 ipv4_dip;
	/*  Configured EtherType for WoL EtherType_da/EtherType */
	u32 ethertype;
	u32 rsrvd[7];
};

struct al_ec_pth {
	/*  System time counter (Time of Day) */
	u32 system_time_seconds;
	/*  System time subseconds in a second (MSBs) */
	u32 system_time_subseconds_msb;
	/*  System time subseconds in a second (LSBs) */
	u32 system_time_subseconds_lsb;
	/*  Clock period in femtoseconds (MSB) */
	u32 clock_period_msb;
	/*  Clock period in femtoseconds (LSB) */
	u32 clock_period_lsb;
	/*  Control register for internal updates to the system */
	u32 int_update_ctrl;
	/*  Value to update system_time_seconds with */
	u32 int_update_seconds;
	/*  Value to update system_time_subseconds_msb with */
	u32 int_update_subseconds_msb;
	/*  Value to update system_time_subseconds_lsb with */
	u32 int_update_subseconds_lsb;
	/*  Control register for external updates to the system */
	u32 ext_update_ctrl;
	/*  Value to update system_time_seconds with */
	u32 ext_update_seconds;
	/*  Value to update system_time_subseconds_msb with */
	u32 ext_update_subseconds_msb;
	/*  Value to update system_time_subseconds_lsb with */
	u32 ext_update_subseconds_lsb;
	/*  This value represents the APB transaction delay fro */
	u32 read_compensation_subseconds_msb;
	/*  This value represents the APB transaction delay fro */
	u32 read_compensation_subseconds_lsb;
	/*  This value is used for two purposes:1 */
	u32 int_write_compensation_subseconds_msb;
	/*  This value is used for two purposes:1 */
	u32 int_write_compensation_subseconds_lsb;
	/*  This value represents the number of cycles it for a */
	u32 ext_write_compensation_subseconds_msb;
	/*  This value represents the number of cycles it for a */
	u32 ext_write_compensation_subseconds_lsb;
	/*  Value to be added to system_time before transferrin */
	u32 sync_compensation_subseconds_msb;
	/*  Value to be added to system_time before transferrin */
	u32 sync_compensation_subseconds_lsb;
	u32 rsrvd[11];
};

struct al_ec_pth_egress {
	/*  Control register for egress trigger #k */
	u32 trigger_ctrl;
	/*  threshold for next egress trigger (#k) - secondsWri */
	u32 trigger_seconds;
	/*  Threshold for next egress trigger (#k) - subseconds */
	u32 trigger_subseconds_msb;
	/*  threshold for next egress trigger (#k) - subseconds */
	u32 trigger_subseconds_lsb;
	/*  External output pulse width (subseconds_msb)(Atomic */
	u32 pulse_width_subseconds_msb;
	/*  External output pulse width (subseconds_lsb)(Atomic */
	u32 pulse_width_subseconds_lsb;
	u32 rsrvd[2];
};

struct al_ec_pth_db {
	/*  timestamp[k], in resolution of 2^18 femtosec =~ 0 */
	u32 ts;
	/*  Timestamp entry is valid */
	u32 qual;
	u32 rsrvd[4];
};

struct al_ec_gen_v3 {
	/*  Bypass enable */
	u32 bypass;
	/*  Rx Completion descriptor */
	u32 rx_comp_desc;
	/*  general configuration */
	u32 conf;
	u32 rsrvd[13];
};

struct al_ec_tfw_v3 {
	/*  Generic protocol detect Cam compare table address */
	u32 tx_gpd_cam_addr;
	/*  Tx Generic protocol detect Cam compare data_1 (low) */
	u32 tx_gpd_cam_data_1;
	/*  Tx Generic protocol detect Cam compare data_2 (high */
	u32 tx_gpd_cam_data_2;
	/*  Tx Generic protocol detect Cam compare mask_1 (low) */
	u32 tx_gpd_cam_mask_1;
	/*  Tx Generic protocol detect Cam compare mask_1 (high */
	u32 tx_gpd_cam_mask_2;
	/*  Tx Generic protocol detect Cam compare control */
	u32 tx_gpd_cam_ctrl;
	/*  Tx Generic crc parameters legacy */
	u32 tx_gcp_legacy;
	/*  Tx Generic crc prameters table address */
	u32 tx_gcp_table_addr;
	/*  Tx Generic crc prameters table general */
	u32 tx_gcp_table_gen;
	/*  Tx Generic crc parametrs tabel mask word 1 */
	u32 tx_gcp_table_mask_1;
	/*  Tx Generic crc parametrs tabel mask word 2 */
	u32 tx_gcp_table_mask_2;
	/*  Tx Generic crc parametrs tabel mask word 3 */
	u32 tx_gcp_table_mask_3;
	/*  Tx Generic crc parametrs tabel mask word 4 */
	u32 tx_gcp_table_mask_4;
	/*  Tx Generic crc parametrs tabel mask word 5 */
	u32 tx_gcp_table_mask_5;
	/*  Tx Generic crc parametrs tabel mask word 6 */
	u32 tx_gcp_table_mask_6;
	/*  Tx Generic crc parametrs tabel crc init */
	u32 tx_gcp_table_crc_init;
	/*  Tx Generic crc parametrs tabel result configuration */
	u32 tx_gcp_table_res;
	/*  Tx Generic crc parameters table alu opcode */
	u32 tx_gcp_table_alu_opcode;
	/*  Tx Generic crc parameters table alu opsel */
	u32 tx_gcp_table_alu_opsel;
	/*  Tx Generic crc parameters table alu constant value */
	u32 tx_gcp_table_alu_val;
	/*  Tx CRC/Checksum replace */
	u32 crc_csum_replace;
	/*  CRC/Checksum replace table address */
	u32 crc_csum_replace_table_addr;
	/*  CRC/Checksum replace table */
	u32 crc_csum_replace_table;
	u32 rsrvd[9];
};

struct al_ec_rfw_v3 {
	/*  Rx Generic protocol detect Cam compare table address */
	u32 rx_gpd_cam_addr;
	/*  Rx Generic protocol detect Cam compare data_1 (low) */
	u32 rx_gpd_cam_data_1;
	/*  Rx Generic protocol detect Cam compare data_2 (high */
	u32 rx_gpd_cam_data_2;
	/*  Rx Generic protocol detect Cam compare mask_1 (low) */
	u32 rx_gpd_cam_mask_1;
	/*  Rx Generic protocol detect Cam compare mask_1 (high */
	u32 rx_gpd_cam_mask_2;
	/*  Rx Generic protocol detect Cam compare control */
	u32 rx_gpd_cam_ctrl;
	/*  Generic protocol detect Parser result vector pointe */
	u32 gpd_p1;
	/*  Generic protocol detect Parser result vector pointe */
	u32 gpd_p2;
	/*  Generic protocol detect Parser result vector pointe */
	u32 gpd_p3;
	/*  Generic protocol detect Parser result vector pointe */
	u32 gpd_p4;
	/*  Generic protocol detect Parser result vector pointe */
	u32 gpd_p5;
	/*  Generic protocol detect Parser result vector pointe */
	u32 gpd_p6;
	/*  Generic protocol detect Parser result vector pointe */
	u32 gpd_p7;
	/*  Generic protocol detect Parser result vector pointe */
	u32 gpd_p8;
	/*  Rx Generic crc parameters legacy */
	u32 rx_gcp_legacy;
	/*  Rx Generic crc prameters table address */
	u32 rx_gcp_table_addr;
	/*  Rx Generic crc prameters table general */
	u32 rx_gcp_table_gen;
	/*  Rx Generic crc parametrs tabel mask word 1 */
	u32 rx_gcp_table_mask_1;
	/*  Rx Generic crc parametrs tabel mask word 2 */
	u32 rx_gcp_table_mask_2;
	/*  Rx Generic crc parametrs tabel mask word 3 */
	u32 rx_gcp_table_mask_3;
	/*  Rx Generic crc parametrs tabel mask word 4 */
	u32 rx_gcp_table_mask_4;
	/*  Rx Generic crc parametrs tabel mask word 5 */
	u32 rx_gcp_table_mask_5;
	/*  Rx Generic crc parametrs tabel mask word 6 */
	u32 rx_gcp_table_mask_6;
	/*  Rx Generic crc parametrs tabel crc init */
	u32 rx_gcp_table_crc_init;
	/*  Rx Generic crc parametrs tabel result configuration */
	u32 rx_gcp_table_res;
	/*  Rx Generic crc  parameters table alu opcode */
	u32 rx_gcp_table_alu_opcode;
	/*  Rx Generic crc  parameters table alu opsel */
	u32 rx_gcp_table_alu_opsel;
	/*  Rx Generic crc  parameters table alu constant value */
	u32 rx_gcp_table_alu_val;
	/*  Generic crc engin parameters alu Parser result vect */
	u32 rx_gcp_alu_p1;
	/*  Generic crc engine parameters alu Parser result vec */
	u32 rx_gcp_alu_p2;
	/*  Header split control table address */
	u32 hs_ctrl_table_addr;
	/*  Header split control table */
	u32 hs_ctrl_table;
	/*  Header split control alu opcode */
	u32 hs_ctrl_table_alu_opcode;
	/*  Header split control alu opsel */
	u32 hs_ctrl_table_alu_opsel;
	/*  Header split control alu constant value */
	u32 hs_ctrl_table_alu_val;
	/*  Header split control configuration */
	u32 hs_ctrl_cfg;
	/*  Header split control alu Parser result vector point */
	u32 hs_ctrl_alu_p1;
	/*  Header split control alu Parser result vector point */
	u32 hs_ctrl_alu_p2;
	u32 rsrvd[26];
};

struct al_ec_crypto {
	/*  Tx inline crypto configuration */
	u32 tx_config;
	/*  Rx inline crypto configuration */
	u32 rx_config;
	/*  reserved FFU */
	u32 tx_override;
	/*  reserved FFU */
	u32 rx_override;
	/*  inline XTS alpha [31:0] */
	u32 xts_alpha_1;
	/*  inline XTS alpha [63:32] */
	u32 xts_alpha_2;
	/*  inline XTS alpha [95:64] */
	u32 xts_alpha_3;
	/*  inline XTS alpha [127:96] */
	u32 xts_alpha_4;
	/*  inline XTS sector ID increment [31:0] */
	u32 xts_sector_id_1;
	/*  inline XTS sector ID increment [63:32] */
	u32 xts_sector_id_2;
	/*  inline XTS sector ID increment [95:64] */
	u32 xts_sector_id_3;
	/*  inline XTS sector ID increment [127:96] */
	u32 xts_sector_id_4;
	/*  IV formation configuration */
	u32 tx_enc_iv_construction;
	/*  IV formation configuration */
	u32 rx_enc_iv_construction;
	/*  IV formation configuration */
	u32 rx_enc_iv_map;
	/*
	 * effectively shorten shift-registers used for eop-pkt-trim, in order
	 * to improve performance.  Each value must be built of consecutive 1's
	 * (bypassed regs), and then consecutive 0's (non-bypassed regs)
	 */
	u32 tx_pkt_trim_len;
	/*
	 * effectively shorten shift-registers used for eop-pkt-trim, in order
	 * to improve performance.  Each value must be built of consecutive 1's
	 * (bypassed regs), and then consecutive 0's (non-bypassed regs)
	 */
	u32 rx_pkt_trim_len;
	/*  reserved FFU */
	u32 tx_reserved;
	/*  reserved FFU */
	u32 rx_reserved;
	u32 rsrvd[13];
};

struct al_ec_crypto_perf_cntr {
	u32 total_tx_pkts;
	u32 total_rx_pkts;
	u32 total_tx_secured_pkts;
	u32 total_rx_secured_pkts;
	u32 total_tx_secured_pkts_cipher_mode;
	u32 total_tx_secured_pkts_cipher_mode_cmpr;
	u32 total_rx_secured_pkts_cipher_mode;
	u32 total_rx_secured_pkts_cipher_mode_cmpr;
	u32 total_tx_secured_bytes_low;
	u32 total_tx_secured_bytes_high;
	u32 total_rx_secured_bytes_low;
	u32 total_rx_secured_bytes_high;
	u32 total_tx_sign_calcs;
	u32 total_rx_sign_calcs;
	u32 total_tx_sign_errs;
	u32 total_rx_sign_errs;
};

struct al_ec_crypto_tx_tid {
	/*  tid_default_entry */
	u32 def_val;
};

struct al_ec_regs {
	u32 rsrvd_0[32];
	struct al_ec_gen gen;
	struct al_ec_mac mac;
	struct al_ec_rxf rxf;
	struct al_ec_epe epe[2];
	struct al_ec_epe_res epe_res;
	struct al_ec_epe_h epe_h[32];
	struct al_ec_epe_p epe_p[32];
	struct al_ec_epe_a epe_a[32];
	struct al_ec_rfw rfw;
	struct al_ec_rfw_udma rfw_udma[4];
	struct al_ec_rfw_hash rfw_hash[10];
	struct al_ec_rfw_priority rfw_priority[8];
	struct al_ec_rfw_default rfw_default[8];
	struct al_ec_fwd_mac fwd_mac[32];
	struct al_ec_msw msw;
	struct al_ec_tso tso;
	struct al_ec_tso_sel tso_sel[8];
	struct al_ec_tpe tpe;
	struct al_ec_tpm_udma tpm_udma[4];
	struct al_ec_tpm_sel tpm_sel[4];
	struct al_ec_tfw tfw;
	struct al_ec_tfw_udma tfw_udma[4];
	struct al_ec_tmi tmi;
	struct al_ec_efc efc;
	struct al_ec_fc_udma fc_udma[4];
	struct al_ec_tpg_rpa_res tpg_rpa_res;
	struct al_ec_eee eee;
	struct al_ec_stat stat;
	struct al_ec_stat_udma stat_udma[4];
	struct al_ec_msp msp;
	struct al_ec_msp_p msp_p[32];
	struct al_ec_msp_c msp_c[32];
	u32 rsrvd_1[16];
	struct al_ec_wol wol;
	u32 rsrvd_2[80];
	struct al_ec_pth pth;
	struct al_ec_pth_egress pth_egress[8];
	struct al_ec_pth_db pth_db[16];
	u32 rsrvd_3[416];
	struct al_ec_gen_v3 gen_v3;
	struct al_ec_tfw_v3 tfw_v3;
	struct al_ec_rfw_v3 rfw_v3;
	struct al_ec_crypto crypto;
	struct al_ec_crypto_perf_cntr crypto_perf_cntr[2];
	u32 rsrvd_4[48];
	struct al_ec_crypto_tx_tid crypto_tx_tid[8];
};

/* Selection between descriptor caching options (WORD selection) */
#define EC_GEN_EN_EXT_CACHE_WORD_SPLIT   BIT(20)

/* Drop indication for the selected protocol index */
#define EC_EPE_A_PROT_ACT_DROP           BIT(0)

/* Enable SIP/DIP swap if SIP<DIP */
#define EC_RFW_THASH_CFG_1_ENABLE_IP_SWAP BIT(16)
/* Enable PORT swap if SPORT<DPORT */
#define EC_RFW_THASH_CFG_1_ENABLE_PORT_SWAP BIT(17)

/* Selects how to calculate the L3 header length when L3 is IpPv */
#define EC_RFW_META_L3_LEN_CALC          BIT(4)

/* Number of MetaData at the end of the packet1 - One MetaData b */
#define EC_RFW_OUT_CFG_META_CNT_MASK     0x00000003
/* Enable packet drop */
#define EC_RFW_OUT_CFG_DROP_EN           BIT(2)

/* Select the header that will be used for the checksum when a t */
#define EC_RFW_CHECKSUM_HDR_SEL          BIT(1)

/* Default data selection 0 - Default value 1 - Table data out */
#define EC_RFW_CTRL_TABLE_DEF_SEL        BIT(20)

/* Drop indication */
#define EC_FWD_MAC_CTRL_RX_VAL_DROP		BIT(0)

/* UDMA selection */
#define EC_FWD_MAC_CTRL_RX_VAL_UDMA_MASK	0x000000078
#define EC_FWD_MAC_CTRL_RX_VAL_UDMA_SHIFT	3

/* queue number */
#define EC_FWD_MAC_CTRL_RX_VAL_QID_MASK		0x00000180
#define EC_FWD_MAC_CTRL_RX_VAL_QID_SHIFT	7

/* Entry is valid for Rx forwarding engine. */
#define EC_FWD_MAC_CTRL_RX_VALID         BIT(15)
/* Control value for Tx forwarding engine */
#define EC_FWD_MAC_CTRL_TX_VAL_MASK      0x001F0000
#define EC_FWD_MAC_CTRL_TX_VAL_SHIFT     16
/* Entry is valid for Tx forwarding engine. */
#define EC_FWD_MAC_CTRL_TX_VALID         BIT(31)

/* MSS selection option:0 - MSS value is selected using MSS_sel  */
#define EC_TSO_CFG_ADD_0_MSS_SEL         BIT(0)

/* Enable TSO with tunnelling */
#define EC_TSO_CFG_TUNNEL_EN_TUNNEL_TSO  BIT(0)
/* Enable outer UDP checksum update */
#define EC_TSO_CFG_TUNNEL_EN_UDP_CHKSUM  BIT(8)
/* Enable outer UDP length update */
#define EC_TSO_CFG_TUNNEL_EN_UDP_LEN     BIT(9)
/* Enable outer Ip6  length update */
#define EC_TSO_CFG_TUNNEL_EN_IPV6_PLEN   BIT(10)
/* Enable outer IPv4 checksum update */
#define EC_TSO_CFG_TUNNEL_EN_IPV4_CHKSUM BIT(11)
/* Enable outer IPv4 Identification update */
#define EC_TSO_CFG_TUNNEL_EN_IPV4_IDEN   BIT(12)
/* Enable outer IPv4 length update */
#define EC_TSO_CFG_TUNNEL_EN_IPV4_TLEN   BIT(13)

/* Swap output byte order */
#define EC_TMI_TX_CFG_SWAP_BYTES         BIT(0)
/* Enable forwarding to the Rx data path. */
#define EC_TMI_TX_CFG_EN_FWD_TO_RX       BIT(1)
/* Mask 2 for XOFF [7:0] Mask 2 for sampled Almost Full indicati */
#define EC_EFC_EC_XOFF_MASK_2_SHIFT      8

/* Mask 1 for generating XON pulse, masking XOFF [0] */
#define EC_EFC_XON_MASK_1                BIT(0)
/* Mask 2 for generating XON pulse, masking Almost Full indicati */
#define EC_EFC_XON_MASK_2                BIT(1)

/* Threshold high */
#define EC_EFC_RX_FIFO_HYST_TH_HIGH_SHIFT 16

#endif /* __AL_HW_EC_REG_H */
