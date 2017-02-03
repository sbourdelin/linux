/*
 * Copyright (C) 2015, Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __AL_HW_UDMA_M2S_REG_H
#define __AL_HW_UDMA_M2S_REG_H

#include <linux/types.h>

struct udma_axi_m2s {
	/* Completion write master configuration */
	u32 comp_wr_cfg_1;
	/* Completion write master configuration */
	u32 comp_wr_cfg_2;
	/* Data read master configuration */
	u32 data_rd_cfg_1;
	/* Data read master configuration */
	u32 data_rd_cfg_2;
	/* Descriptor read master configuration */
	u32 desc_rd_cfg_1;
	/* Descriptor read master configuration */
	u32 desc_rd_cfg_2;
	/* Data read master configuration */
	u32 data_rd_cfg;
	/* Descriptors read master configuration */
	u32 desc_rd_cfg_3;
	/* Descriptors write master configuration (completion) */
	u32 desc_wr_cfg_1;
	/* AXI outstanding  configuration */
	u32 ostand_cfg;
	u32 rsrvd[54];
};
struct udma_m2s {
	/*
	 * DMA state.
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
	 * M2S DMA error log mask.
	 * Each error has an interrupt controller cause bit.
	 * This register determines if these errors cause the M2S DMA to log the
	 * error condition.
	 * 0 - Log is enabled.
	 * 1 - Log is masked.
	 */
	u32 err_log_mask;
	u32 rsrvd_1;
	/*
	 * DMA header log.
	 * Sample the packet header that caused the error.
	 */
	u32 log_0;
	/*
	 * DMA header log.
	 * Sample the packet header that caused the error.
	 */
	u32 log_1;
	/*
	 * DMA header log.
	 * Sample the packet header that caused the error.
	 */
	u32 log_2;
	/*
	 * DMA header log.
	 * Sample the packet header that caused the error.
	 */
	u32 log_3;
	/* DMA clear error log */
	u32 clear_err_log;
	/* M2S data FIFO status */
	u32 data_fifo_status;
	/* M2S header FIFO status */
	u32 header_fifo_status;
	/* M2S unack FIFO status */
	u32 unack_fifo_status;
	/* Select queue for debug */
	u32 indirect_ctrl;
	/*
	 * M2S prefetch FIFO status.
	 * Status of the selected queue in M2S_indirect_ctrl
	 */
	u32 sel_pref_fifo_status;
	/*
	 * M2S completion FIFO status.
	 * Status of the selected queue in M2S_indirect_ctrl
	 */
	u32 sel_comp_fifo_status;
	/*
	 * M2S rate limit status.
	 * Status of the selected queue in M2S_indirect_ctrl
	 */
	u32 sel_rate_limit_status;
	/*
	 * M2S DWRR scheduler status.
	 * Status of the selected queue in M2S_indirect_ctrl
	 */
	u32 sel_dwrr_status;
	/* M2S state machine and FIFO clear control */
	u32 clear_ctrl;
	/* Misc Check enable */
	u32 check_en;
	/* M2S FIFO enable control, internal */
	u32 fifo_en;
	/* M2S packet length configuration */
	u32 cfg_len;
	/* Stream interface configuration */
	u32 stream_cfg;
	u32 rsrvd[41];
};
struct udma_m2s_rd {
	/* M2S descriptor prefetch configuration */
	u32 desc_pref_cfg_1;
	/* M2S descriptor prefetch configuration */
	u32 desc_pref_cfg_2;
	/* M2S descriptor prefetch configuration */
	u32 desc_pref_cfg_3;
	u32 rsrvd_0;
	/* Data burst read configuration */
	u32 data_cfg;
	u32 rsrvd[11];
};
struct udma_m2s_dwrr {
	/* Tx DMA DWRR scheduler configuration */
	u32 cfg_sched;
	/* Token bucket rate limit control */
	u32 ctrl_deficit_cnt;
	u32 rsrvd[14];
};
struct udma_m2s_rate_limiter {
	/* Token bucket rate limit configuration */
	u32 gen_cfg;
	/*
	 * Token bucket rate limit control.
	 * Controls the cycle counters.
	 */
	u32 ctrl_cycle_cnt;
	/*
	 * Token bucket rate limit control.
	 * Controls the token bucket counter.
	 */
	u32 ctrl_token;
	u32 rsrvd[13];
};

struct udma_rlimit_common {
	/* Token bucket configuration */
	u32 cfg_1s;
	/* Token bucket rate limit configuration */
	u32 cfg_cycle;
	/* Token bucket rate limit configuration */
	u32 cfg_token_size_1;
	/* Token bucket rate limit configuration */
	u32 cfg_token_size_2;
	/* Token bucket rate limit configuration */
	u32 sw_ctrl;
	/*
	 * Mask the different types of rate limiter.
	 * 0 - Rate limit is active.
	 * 1 - Rate limit is masked.
	 */
	u32 mask;
};

struct udma_m2s_stream_rate_limiter {
	struct udma_rlimit_common rlimit;
	u32 rsrvd[10];
};
struct udma_m2s_comp {
	/* Completion controller configuration */
	u32 cfg_1c;
	/* Completion controller coalescing configuration */
	u32 cfg_coal;
	/* Completion controller application acknowledge configuration */
	u32 cfg_application_ack;
	u32 rsrvd[61];
};
struct udma_m2s_stat {
	/* Statistics counters configuration */
	u32 cfg_st;
	/* Counting number of descriptors with First-bit set. */
	u32 tx_pkt;
	/*
	 *  Counting the net length of the data buffers [64-bit]
	 * Should be read before tx_bytes_high
	 */
	u32 tx_bytes_low;
	/*
	 *  Counting the net length of the data buffers [64-bit],
	 * Should be read after tx_bytes_low (value is sampled when reading
	 * Should be read before tx_bytes_low
	 */
	u32 tx_bytes_high;
	/* Total number of descriptors read from the host memory */
	u32 prefed_desc;
	/* Number of packets read from the unack FIFO */
	u32 comp_pkt;
	/* Number of descriptors written into the completion ring */
	u32 comp_desc;
	/*
	 *  Number of acknowledged packets.
	 * (acknowledge received from the stream interface)
	 */
	u32 ack_pkts;
	u32 rsrvd[56];
};
struct udma_m2s_feature {
	/*
	 *  M2S Feature register.
	 * M2S instantiation parameters
	 */
	u32 reg_1;
	/* Reserved M2S feature register */
	u32 reg_2;
	/*
	 *  M2S Feature register.
	 * M2S instantiation parameters
	 */
	u32 reg_3;
	/*
	 *  M2S Feature register.
	 * M2S instantiation parameters
	 */
	u32 reg_4;
	/*
	 *  M2S Feature register.
	 * M2S instantiation parameters
	 */
	u32 reg_5;
	u32 rsrvd[59];
};
struct udma_m2s_q {
	u32 rsrvd_0[8];
	/* M2S descriptor ring configuration */
	u32 cfg;
	/* M2S descriptor ring status and information */
	u32 status;
	/* TX Descriptor Ring Base Pointer [31:4] */
	u32 tdrbp_low;
	/* TX Descriptor Ring Base Pointer [63:32] */
	u32 tdrbp_high;
	/*
	 *  TX Descriptor Ring Length[23:2]
	 */
	u32 tdrl;
	/* TX Descriptor Ring Head Pointer */
	u32 tdrhp;
	/* Tx Descriptor Tail Pointer increment */
	u32 tdrtp_inc;
	/* Tx Descriptor Tail Pointer */
	u32 tdrtp;
	/* TX Descriptor Current Pointer */
	u32 tdcp;
	/* Tx Completion Ring Base Pointer [31:4] */
	u32 tcrbp_low;
	/* TX Completion Ring Base Pointer [63:32] */
	u32 tcrbp_high;
	/* TX Completion Ring Head Pointer */
	u32 tcrhp;
	/*
	 *  Tx Completion Ring Head Pointer internal (Before the
	 * coalescing FIFO)
	 */
	u32 tcrhp_internal;
	u32 rsrvd_1[3];
	/* Rate limit configuration */
	struct udma_rlimit_common rlimit;
	u32 rsrvd_2[2];
	/* DWRR scheduler configuration */
	u32 dwrr_cfg_1;
	/* DWRR scheduler configuration */
	u32 dwrr_cfg_2;
	/* DWRR scheduler configuration */
	u32 dwrr_cfg_3;
	/* DWRR scheduler software control */
	u32 dwrr_sw_ctrl;
	u32 rsrvd_3[4];
	/* Completion controller configuration */
	u32 comp_cfg;
	u32 rsrvd_4[3];
	/* SW control  */
	u32 q_sw_ctrl;
	u32 rsrvd_5[3];
	/* Number of M2S Tx packets after the scheduler */
	u32 q_tx_pkt;
	u32 rsrvd[975];
};

struct udma_m2s_regs {
	u32 rsrvd_0[64];
	struct udma_axi_m2s axi_m2s;
	struct udma_m2s m2s;
	struct udma_m2s_rd m2s_rd;
	struct udma_m2s_dwrr m2s_dwrr;
	struct udma_m2s_rate_limiter m2s_rate_limiter;
	struct udma_m2s_stream_rate_limiter m2s_stream_rate_limiter;
	struct udma_m2s_comp m2s_comp;
	struct udma_m2s_stat m2s_stat;
	struct udma_m2s_feature m2s_feature;
	u32 rsrvd_1[576];
	struct udma_m2s_q m2s_q[4];
};


/* Completion control */
#define UDMA_M2S_STATE_COMP_CTRL_MASK 0x00000003
#define UDMA_M2S_STATE_COMP_CTRL_SHIFT 0
/* Stream interface */
#define UDMA_M2S_STATE_STREAM_IF_MASK 0x00000030
#define UDMA_M2S_STATE_STREAM_IF_SHIFT 4
/* Data read control */
#define UDMA_M2S_STATE_DATA_RD_CTRL_MASK 0x00000300
#define UDMA_M2S_STATE_DATA_RD_CTRL_SHIFT 8
/* Descriptor prefetch */
#define UDMA_M2S_STATE_DESC_PREF_MASK 0x00003000
#define UDMA_M2S_STATE_DESC_PREF_SHIFT 12

/* Start normal operation */
#define UDMA_M2S_CHANGE_STATE_NORMAL BIT(0)
/* Stop normal operation */
#define UDMA_M2S_CHANGE_STATE_DIS    BIT(1)
/*
 * Stop all machines.
 * (Prefetch, scheduling, completion and stream interface)
 */
#define UDMA_M2S_CHANGE_STATE_ABORT  BIT(2)

/* Maximum packet size for the M2S */
#define UDMA_M2S_CFG_LEN_MAX_PKT_SIZE_MASK 0x000FFFFF
/*
 * Length encoding for 64K.
 * 0 - length 0x0000 = 0
 * 1 - length 0x0000 = 64k
 */
#define UDMA_M2S_CFG_LEN_ENCODE_64K  BIT(24)

/* Maximum number of descriptors per packet */
#define UDMA_M2S_RD_DESC_PREF_CFG_2_MAX_DESC_PER_PKT_MASK 0x0000001F
#define UDMA_M2S_RD_DESC_PREF_CFG_2_MAX_DESC_PER_PKT_SHIFT 0
/*
 * Minimum descriptor burst size when prefetch FIFO level is above the
 * descriptor prefetch threshold
 */
#define UDMA_M2S_RD_DESC_PREF_CFG_3_MIN_BURST_ABOVE_THR_MASK 0x000000F0
#define UDMA_M2S_RD_DESC_PREF_CFG_3_MIN_BURST_ABOVE_THR_SHIFT 4
/*
 * Descriptor fetch threshold.
 * Used as a threshold to determine the allowed minimum descriptor burst size.
 * (Must be at least max_desc_per_pkt)
 */
#define UDMA_M2S_RD_DESC_PREF_CFG_3_PREF_THR_MASK 0x0000FF00
#define UDMA_M2S_RD_DESC_PREF_CFG_3_PREF_THR_SHIFT 8

/*
 * Maximum number of data beats in the data read FIFO.
 * Defined based on data FIFO size
 * (default FIFO size 2KB → 128 beats)
 */
#define UDMA_M2S_RD_DATA_CFG_DATA_FIFO_DEPTH_MASK 0x000003FF
#define UDMA_M2S_RD_DATA_CFG_DATA_FIFO_DEPTH_SHIFT 0

/*
 * Enable operation of this queue.
 * Start prefetch.
 */
#define UDMA_M2S_Q_CFG_EN_PREF       BIT(16)
/*
 * Enable operation of this queue.
 * Start scheduling.
 */
#define UDMA_M2S_Q_CFG_EN_SCHEDULING BIT(17)

/*
 * M2S Descriptor Ring Base address [31:4].
 * Value of the base address of the M2S descriptor ring
 * [3:0] - 0 - 16B alignment is enforced
 * ([11:4] should be 0 for 4KB alignment)
 */
#define UDMA_M2S_Q_TDRBP_LOW_ADDR_MASK 0xFFFFFFF0

/*
 * M2S Descriptor Ring Base address [31:4].
 * Value of the base address of the M2S descriptor ring
 * [3:0] - 0 - 16B alignment is enforced
 * ([11:4] should be 0 for 4KB alignment)
 * NOTE:
 * Length of the descriptor ring (in descriptors) associated with the ring base
 * address. Ends at maximum burst size alignment.
 */
#define UDMA_M2S_Q_TCRBP_LOW_ADDR_MASK 0xFFFFFFF0

/*
 * Mask the internal pause mechanism for DMB.
 * (Data Memory Barrier).
 */
#define UDMA_M2S_Q_RATE_LIMIT_MASK_INTERNAL_PAUSE_DMB BIT(2)

/* Enable writing to the completion ring */
#define UDMA_M2S_Q_COMP_CFG_EN_COMP_RING_UPDATE BIT(0)
/* Disable the completion coalescing function. */
#define UDMA_M2S_Q_COMP_CFG_DIS_COMP_COAL BIT(1)

#endif /* __AL_HW_UDMA_M2S_REG_H */
