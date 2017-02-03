/*
 * Copyright (C) 2017, Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __AL_HW_UDMA_S2M_REG_H
#define __AL_HW_UDMA_S2M_REG_H

struct udma_axi_s2m {
	/* Data write master configuration */
	u32 data_wr_cfg_1;
	/* Data write master configuration */
	u32 data_wr_cfg_2;
	/* Descriptor read master configuration */
	u32 desc_rd_cfg_4;
	/* Descriptor read master configuration */
	u32 desc_rd_cfg_5;
	/* Completion  write master configuration */
	u32 comp_wr_cfg_1;
	/* Completion  write master configuration */
	u32 comp_wr_cfg_2;
	/* Data write master configuration */
	u32 data_wr_cfg;
	/* Descriptors read master configuration */
	u32 desc_rd_cfg_3;
	/* Completion descriptors write master configuration */
	u32 desc_wr_cfg_1;
	/* AXI outstanding read configuration */
	u32 ostand_cfg_rd;
	/* AXI outstanding write configuration */
	u32 ostand_cfg_wr;
	u32 rsrvd[53];
};
struct udma_s2m {
	/*
	 * DMA state
	 * 00  - No pending tasks
	 * 01 – Normal (active)
	 * 10 – Abort (error condition)
	 * 11 – Reserved
	 */
	u32 state;
	/* CPU request to change DMA state */
	u32 change_state;
	u32 rsrvd_0;
	/*
	 * S2M DMA error log mask.
	 * Each error has an interrupt controller cause bit.
	 * This register determines if these errors cause the S2M DMA to log the
	 * error condition.
	 * 0 - Log is enable
	 * 1 - Log is masked.
	 */
	u32 err_log_mask;
	u32 rsrvd_1;
	/*
	 * DMA header log
	 * Sample the packet header that caused the error
	 */
	u32 log_0;
	/*
	 * DMA header log
	 * Sample the packet header that caused the error.
	 */
	u32 log_1;
	/*
	 * DMA header log
	 * Sample the packet header that caused the error.
	 */
	u32 log_2;
	/*
	 * DMA header log
	 * Sample the packet header that caused the error
	 */
	u32 log_3;
	/* DMA clear error log */
	u32 clear_err_log;
	/* S2M stream data FIFO status */
	u32 s_data_fifo_status;
	/* S2M stream header FIFO status */
	u32 s_header_fifo_status;
	/* S2M AXI data FIFO status */
	u32 axi_data_fifo_status;
	/* S2M unack FIFO status */
	u32 unack_fifo_status;
	/* Select queue for debug */
	u32 indirect_ctrl;
	/*
	 * S2M prefetch FIFO status.
	 * Status of the selected queue in S2M_indirect_ctrl
	 */
	u32 sel_pref_fifo_status;
	/*
	 * S2M completion FIFO status.
	 * Status of the selected queue in S2M_indirect_ctrl
	 */
	u32 sel_comp_fifo_status;
	/* S2M state machine and FIFO clear control */
	u32 clear_ctrl;
	/* S2M Misc Check enable */
	u32 check_en;
	/* S2M FIFO enable control, internal */
	u32 fifo_en;
	/* Stream interface configuration */
	u32 stream_cfg;
	u32 rsrvd[43];
};
struct udma_s2m_rd {
	/* S2M descriptor prefetch configuration */
	u32 desc_pref_cfg_1;
	/* S2M descriptor prefetch configuration */
	u32 desc_pref_cfg_2;
	/* S2M descriptor prefetch configuration */
	u32 desc_pref_cfg_3;
	/* S2M descriptor prefetch configuration */
	u32 desc_pref_cfg_4;
	u32 rsrvd[12];
};
struct udma_s2m_wr {
	/* Stream data FIFO configuration */
	u32 data_cfg_1;
	/* Data write configuration */
	u32 data_cfg_2;
	u32 rsrvd[14];
};
struct udma_s2m_comp {
	/* Completion controller configuration */
	u32 cfg_1c;
	/* Completion controller configuration */
	u32 cfg_2c;
	u32 rsrvd_0;
	/* Completion controller application acknowledge configuration */
	u32 cfg_application_ack;
	u32 rsrvd[12];
};
struct udma_s2m_stat {
	u32 rsrvd_0;
	/* Number of dropped packets */
	u32 drop_pkt;
	/*
	 * Counting the net length of the data buffers [64-bit]
	 * Should be read before rx_bytes_high
	 */
	u32 rx_bytes_low;
	/*
	 * Counting the net length of the data buffers [64-bit]
	 * Should be read after tx_bytes_low (value is sampled when reading
	 * Should be read before rx_bytes_low
	 */
	u32 rx_bytes_high;
	/* Total number of descriptors read from the host memory */
	u32 prefed_desc;
	/* Number of packets written into the completion ring */
	u32 comp_pkt;
	/* Number of descriptors written into the completion ring */
	u32 comp_desc;
	/*
	 * Number of acknowledged packets.
	 * (acknowledge sent to the stream interface)
	 */
	u32 ack_pkts;
	u32 rsrvd[56];
};
struct udma_s2m_feature {
	/*
	 * S2M Feature register
	 * S2M instantiation parameters
	 */
	u32 reg_1;
	/* Reserved S2M feature register */
	u32 reg_2;
	/*
	 * S2M Feature register
	 * S2M instantiation parameters
	 */
	u32 reg_3;
	/*
	 * S2M Feature register.
	 * S2M instantiation parameters.
	 */
	u32 reg_4;
	/*
	 * S2M Feature register.
	 * S2M instantiation parameters.
	 */
	u32 reg_5;
	/* S2M Feature register. S2M instantiation parameters. */
	u32 reg_6;
	u32 rsrvd[58];
};
struct udma_s2m_q {
	u32 rsrvd_0[8];
	/* S2M Descriptor ring configuration */
	u32 cfg;
	/* S2M Descriptor ring status and information */
	u32 status;
	/* Rx Descriptor Ring Base Pointer [31:4] */
	u32 rdrbp_low;
	/* Rx Descriptor Ring Base Pointer [63:32] */
	u32 rdrbp_high;
	/*
	 * Rx Descriptor Ring Length[23:2]
	 */
	u32 rdrl;
	/* RX Descriptor Ring Head Pointer */
	u32 rdrhp;
	/* Rx Descriptor Tail Pointer increment */
	u32 rdrtp_inc;
	/* Rx Descriptor Tail Pointer */
	u32 rdrtp;
	/* RX Descriptor Current Pointer */
	u32 rdcp;
	/* Rx Completion Ring Base Pointer [31:4] */
	u32 rcrbp_low;
	/* Rx Completion Ring Base Pointer [63:32] */
	u32 rcrbp_high;
	/* Rx Completion Ring Head Pointer */
	u32 rcrhp;
	/*
	 * RX Completion Ring Head Pointer internal.
	 * (Before the coalescing FIFO)
	 */
	u32 rcrhp_internal;
	/* Completion controller configuration for the queue */
	u32 comp_cfg;
	/* Completion controller configuration for the queue */
	u32 comp_cfg_2;
	/* Packet handler configuration */
	u32 pkt_cfg;
	/* Queue QoS configuration */
	u32 qos_cfg;
	/* DMB software control */
	u32 q_sw_ctrl;
	/* Number of S2M Rx packets after completion  */
	u32 q_rx_pkt;
	u32 rsrvd[997];
};

struct udma_s2m_regs {
	u32 rsrvd_0[64];
	struct udma_axi_s2m axi_s2m;
	struct udma_s2m s2m;
	struct udma_s2m_rd s2m_rd;
	struct udma_s2m_wr s2m_wr;
	struct udma_s2m_comp s2m_comp;
	u32 rsrvd_1[80];
	struct udma_s2m_stat s2m_stat;
	struct udma_s2m_feature s2m_feature;
	u32 rsrvd_2[576];
	struct udma_s2m_q s2m_q[4];
};

/*
 * Defines the maximum number of AXI beats for a single AXI burst. This value is
 * used for the burst split decision.
 */
#define UDMA_AXI_S2M_DESC_WR_CFG_1_MAX_AXI_BEATS_MASK 0x000000FF
#define UDMA_AXI_S2M_DESC_WR_CFG_1_MAX_AXI_BEATS_SHIFT 0
/*
 * Minimum burst for writing completion descriptors.
 * (AXI beats).
 * Value must be aligned to cache lines (64 bytes).
 * Default value is 2 cache lines, 8 beats.
 */
#define UDMA_AXI_S2M_DESC_WR_CFG_1_MIN_AXI_BEATS_MASK 0x00FF0000
#define UDMA_AXI_S2M_DESC_WR_CFG_1_MIN_AXI_BEATS_SHIFT 16

/*
 * Minimum descriptor burst size when prefetch FIFO level is above the
 * descriptor prefetch threshold
 */
#define UDMA_S2M_RD_DESC_PREF_CFG_3_MIN_BURST_ABOVE_THR_MASK 0x000000F0
#define UDMA_S2M_RD_DESC_PREF_CFG_3_MIN_BURST_ABOVE_THR_SHIFT 4
/*
 * Descriptor fetch threshold.
 * Used as a threshold to determine the allowed minimum descriptor burst size.
 * (Must be at least "max_desc_per_pkt")
 */
#define UDMA_S2M_RD_DESC_PREF_CFG_3_PREF_THR_MASK 0x0000FF00
#define UDMA_S2M_RD_DESC_PREF_CFG_3_PREF_THR_SHIFT 8

/*
 * Completion descriptor size.
 * (words)
 */
#define UDMA_S2M_COMP_CFG_1C_DESC_SIZE_MASK 0x0000000F

/* Disables the completion coalescing function. */
#define UDMA_S2M_Q_COMP_CFG_DIS_COMP_COAL BIT(1)

#endif /* __AL_HW_UDMA_S2M_REG_H */
