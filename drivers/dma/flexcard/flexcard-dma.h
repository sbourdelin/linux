/*
 * Eberspächer Flexcard PMC II Carrier Board PCI Driver - DMA controller
 *
 * Copyright (c) 2014 - 2016, Linutronix GmbH
 * Author: Benedikt Spranger <b.spranger@linutronix.de>
 *         Holger Dengler <dengler@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */
#ifndef __FLEXCARD_DMA_H
#define __FLEXCARD_DMA_H

#define FLEXCARD_DMA_BUF_SIZE		0x200000
#define FLEXCARD_DMA_BUF_MASK		(FLEXCARD_DMA_BUF_SIZE - 1)

#define FLEXCARD_DMA_CTRL_DMA_ENA	(1 << 0)
#define FLEXCARD_DMA_CTRL_MAN_ENA	(1 << 1)
#define FLEXCARD_DMA_CTRL_STOP_REQ	(1 << 16)
#define FLEXCARD_DMA_CTRL_DMA_IDLE	(1 << 17)
#define FLEXCARD_DMA_CTRL_RST_DMA	(1 << 31)

#define FLEXCARD_DMA_STAT_BUSY		(1 << 15)
#define FLEXCARD_DMA_STAT_OFL		(1 << 31)

#define FLEXCARD_MAX_PAKET_SIZE		0x200

#define FLEXCARD_BUF_HEADER_LEN_SHIFT	15
#define FLEXCARD_BUF_HEADER_LEN_MASK	0xfe

#define FLEXCARD_CANIF_OFFSET		0x20

struct flexcard_dma_reg {
	u32 dma_ctrl;
	u32 dma_stat;
	u32 r1[2];
	u64 dma_cba;
	u32 dma_cbs;
	u32 dma_txr;
	u32 dma_irer;
	u32 dma_irsr;
	u32 r2[10];
	u32 dma_cbcr;
	u32 dma_cblr;
	u32 r3[2];
	u32 dma_itcr;
	u32 dma_itr;
	u32 r4[2];
	u32 dma_wptr;
	u32 dma_rptr;
	u32 r5[2];
} __packed;

struct flexcard_dma {
	int irq;
	int irq_ovr;
	u32 rptr;
	void *buf;
	dma_addr_t phys;
	int nr_eray;
	struct flexcard_dma_reg __iomem *reg;
};

enum fc_packet_type {
	FC_PACKET_TYPE_INFO = 1,
	FC_PACKET_TYPE_FLEXRAY_FRAME = 2,
	FC_PACKET_TYPE_ERROR = 3,
	FC_PACKET_TYPE_STATUS = 4,
	FC_PACKET_TYPE_TRIGGER = 5,
	FC_PACKET_TYPE_TX_ACK = 6,
	FC_PACKET_TYPE_NMV_VECTOR = 7,
	FC_PACKET_TYPE_NOTIFICATION = 8,
	FC_PACKET_TYPE_TRIGGER_EX = 9,
	FC_PACKET_TYPE_CAN = 10,
	FC_PACKET_TYPE_CAN_ERROR = 11,
};

struct fc_packet {
	__u32 type;
	__u32 p_packet;
	__u32 p_next_packet;
} __packed;

struct fc_info_packet {
	__u32 current_cycle;
	__u32 timestamp;
	__u32 offset_rate_correction;
	__u32 pta_ccf_count;
	__u32 cc;
} __packed;

struct fc_flexray_frame {
	__u32 header;
	__u32 header_crc;
	__u32 pdata;
	__u32 channel;
	__u32 frame_crc;
	__u32 timestamp;
	__u32 cc;
} __packed;

struct fc_error_packet {
	__u32 flag;
	__u32 timestamp;
	__u32 cycle_count;
	__u64 additional_info;
	__u32 cc;
	__u32 reserved;
} __packed;

struct fc_status_packet {
	__u32 flag;
	__u32 timestamp;
	__u32 cycle_count;
	__u32 additional_info;
	__u32 cc;
	__u32 reserved[2];
} __packed;

struct fc_tx_ack_packet {
	__u32 bufferid;
	__u32 timestamp;
	__u32 cycle_count;
	__u32 header;
	__u32 header_crc;
	__u32 pdata;
	__u32 channel;
	__u32 cc;
} __packed;

struct fc_nm_vector_packet {
	__u32 timestamp;
	__u32 cycle_count;
	__u32 nmv_vector_length;
	__u32 nmv_vector[3];
	__u32 cc;
	__u32 reserved;
} __packed;

struct fc_notification_packet {
	__u32 timestamp;
	__u32 sequence_count;
	__u32 reserved;
} __packed;

struct fc_trigger_ex_info_packet {
	__u32 condition;
	__u32 timestamp;
	__u32 sequence_count;
	__u32 reserved1;
	__u64 performance_counter;
	__u32 edge;
	__u32 trigger_line;
	__u32 reserved[4];
} __packed;

struct fc_can_packet {
	__u32 id;
	__u32 timestamp;
	__u32 flags;
	__u32 reserved;
	__u32 cc;
	__u8 data[8];
} __packed;

struct fc_can_error_packet {
	__u32 type;
	__u32 state;
	__u32 timestamp;
	__u32 rx_error_counter;
	__u32 tx_error_counter;
	__u32 cc;
	__u32 reserved[2];
} __packed;

enum fc_can_cc_state {
	fc_can_state_unknown = 0,
	fc_can_state_config,
	fc_can_state_normalActive,
	fc_can_state_warning,
	fc_can_state_error_passive,
	fc_can_state_bus_off,
};

enum fc_can_error_type {
	fc_can_error_none = 0,
	fc_can_error_stuff,
	fc_can_error_form,
	fc_can_error_acknowledge,
	fc_can_error_bit1,
	fc_can_error_bit0,
	fc_can_error_crc,
	fc_can_error_parity,
};

union fc_packet_types {
	struct fc_info_packet		info_packet;
	struct fc_flexray_frame		flexray_frame;
	struct fc_error_packet		error_packet;
	struct fc_status_packet		status_packet;
	struct fc_tx_ack_packet		tx_ack_packet;
	struct fc_nm_vector_packet	nm_vector_packet;
	struct fc_notification_packet	notification_packet;
	struct fc_trigger_ex_info_packet ex_info_packet;
	struct fc_can_packet		can_packet;
	struct fc_can_error_packet	can_error_packet;
};

struct fc_packet_buf {
	struct  fc_packet	header;
	union   fc_packet_types	packet;
} __packed;

u32 flexcard_parse_packet(struct fc_packet_buf *pb, u32 avail,
			  struct flexcard_dma *dma);

#endif /* __FLEXCARD_DMA_H */
