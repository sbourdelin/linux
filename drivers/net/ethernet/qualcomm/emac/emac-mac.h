/* Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* EMAC DMA HW engine uses three rings:
 * Tx:
 *   TPD: Transmit Packet Descriptor ring.
 * Rx:
 *   RFD: Receive Free Descriptor ring.
 *     Ring of descriptors with empty buffers to be filled by Rx HW.
 *   RRD: Receive Return Descriptor ring.
 *     Ring of descriptors with buffers filled with received data.
 */

#ifndef _EMAC_HW_H_
#define _EMAC_HW_H_

/* EMAC_CSR register offsets */
#define EMAC_EMAC_WRAPPER_CSR1                                0x000000
#define EMAC_EMAC_WRAPPER_CSR2                                0x000004
#define EMAC_EMAC_WRAPPER_TX_TS_LO                            0x000104
#define EMAC_EMAC_WRAPPER_TX_TS_HI                            0x000108
#define EMAC_EMAC_WRAPPER_TX_TS_INX                           0x00010c

/* DMA Order Settings */
enum emac_dma_order {
	emac_dma_ord_in = 1,
	emac_dma_ord_enh = 2,
	emac_dma_ord_out = 4
};

enum emac_mac_speed {
	emac_mac_speed_0 = 0,
	emac_mac_speed_10_100 = 1,
	emac_mac_speed_1000 = 2
};

enum emac_dma_req_block {
	emac_dma_req_128 = 0,
	emac_dma_req_256 = 1,
	emac_dma_req_512 = 2,
	emac_dma_req_1024 = 3,
	emac_dma_req_2048 = 4,
	emac_dma_req_4096 = 5
};

/* Returns the value of bits idx...idx+n_bits */
#define BITS_MASK(idx, n_bits) (((((unsigned long)1) << (n_bits)) - 1) << (idx))
#define BITS_GET(val, idx, n_bits) (((val) & BITS_MASK(idx, n_bits)) >> idx)
#define BITS_SET(val, idx, n_bits, new_val)				\
	((val) = (((val) & (~BITS_MASK(idx, n_bits))) |			\
		 (((new_val) << (idx)) & BITS_MASK(idx, n_bits))))

/* RRD (Receive Return Descriptor) */
struct emac_rrd {
	u32	word[6];

/* number of RFD */
#define RRD_NOR(rrd)			BITS_GET((rrd)->word[0], 16, 4)
/* start consumer index of rfd-ring */
#define RRD_SI(rrd)			BITS_GET((rrd)->word[0], 20, 12)
/* vlan-tag (CVID, CFI and PRI) */
#define RRD_CVALN_TAG(rrd)		BITS_GET((rrd)->word[2], 0, 16)
/* length of the packet */
#define RRD_PKT_SIZE(rrd)		BITS_GET((rrd)->word[3], 0, 14)
/* L4(TCP/UDP) checksum failed */
#define RRD_L4F(rrd)			BITS_GET((rrd)->word[3], 14, 1)
/* vlan tagged */
#define RRD_CVTAG(rrd)			BITS_GET((rrd)->word[3], 16, 1)
/* When set, indicates that the descriptor is updated by the IP core.
 * When cleared, indicates that the descriptor is invalid.
 */
#define RRD_UPDT(rrd)			BITS_GET((rrd)->word[3], 31, 1)
#define RRD_UPDT_SET(rrd, val)		BITS_SET((rrd)->word[3], 31, 1, val)
/* timestamp low */
#define RRD_TS_LOW(rrd)			BITS_GET((rrd)->word[4], 0, 30)
/* timestamp high */
#define RRD_TS_HI(rrd)			((rrd)->word[5])
};

/* RFD (Receive Free Descriptor) */
union emac_rfd {
	u64	addr;
	u32	word[2];
};

/* TPD (Transmit Packet Descriptor) */
struct emac_tpd {
	u32				word[4];

/* Number of bytes of the transmit packet. (include 4-byte CRC) */
#define TPD_BUF_LEN_SET(tpd, val)	BITS_SET((tpd)->word[0], 0, 16, val)
/* Custom Checksum Offload: When set, ask IP core to offload custom checksum */
#define TPD_CSX_SET(tpd, val)		BITS_SET((tpd)->word[1], 8, 1, val)
/* TCP Large Send Offload: When set, ask IP core to do offload TCP Large Send */
#define TPD_LSO(tpd)			BITS_GET((tpd)->word[1], 12, 1)
#define TPD_LSO_SET(tpd, val)		BITS_SET((tpd)->word[1], 12, 1, val)
/*  Large Send Offload Version: When set, indicates this is an LSOv2
 * (for both IPv4 and IPv6). When cleared, indicates this is an LSOv1
 * (only for IPv4).
 */
#define TPD_LSOV_SET(tpd, val)		BITS_SET((tpd)->word[1], 13, 1, val)
/* IPv4 packet: When set, indicates this is an  IPv4 packet, this bit is only
 * for LSOV2 format.
 */
#define TPD_IPV4_SET(tpd, val)		BITS_SET((tpd)->word[1], 16, 1, val)
/* 0: Ethernet   frame (DA+SA+TYPE+DATA+CRC)
 * 1: IEEE 802.3 frame (DA+SA+LEN+DSAP+SSAP+CTL+ORG+TYPE+DATA+CRC)
 */
#define TPD_TYP_SET(tpd, val)		BITS_SET((tpd)->word[1], 17, 1, val)
/* Low-32bit Buffer Address */
#define TPD_BUFFER_ADDR_L_SET(tpd, val)	((tpd)->word[2] = (val))
/* CVLAN Tag to be inserted if INS_VLAN_TAG is set, CVLAN TPID based on global
 * register configuration.
 */
#define TPD_CVLAN_TAG_SET(tpd, val)	BITS_SET((tpd)->word[3], 0, 16, val)
/*  Insert CVlan Tag: When set, ask MAC to insert CVLAN TAG to outgoing packet
 */
#define TPD_INSTC_SET(tpd, val)		BITS_SET((tpd)->word[3], 17, 1, val)
/* High-14bit Buffer Address, So, the 64b-bit address is
 * {DESC_CTRL_11_TX_DATA_HIADDR[17:0],(register) BUFFER_ADDR_H, BUFFER_ADDR_L}
 */
#define TPD_BUFFER_ADDR_H_SET(tpd, val)	BITS_SET((tpd)->word[3], 18, 13, val)
/* Format D. Word offset from the 1st byte of this packet to start to calculate
 * the custom checksum.
 */
#define TPD_PAYLOAD_OFFSET_SET(tpd, val) BITS_SET((tpd)->word[1], 0, 8, val)
/*  Format D. Word offset from the 1st byte of this packet to fill the custom
 * checksum to
 */
#define TPD_CXSUM_OFFSET_SET(tpd, val)	BITS_SET((tpd)->word[1], 18, 8, val)

/* Format C. TCP Header offset from the 1st byte of this packet. (byte unit) */
#define TPD_TCPHDR_OFFSET_SET(tpd, val)	BITS_SET((tpd)->word[1], 0, 8, val)
/* Format C. MSS (Maximum Segment Size) got from the protocol layer. (byte unit)
 */
#define TPD_MSS_SET(tpd, val)		BITS_SET((tpd)->word[1], 18, 13, val)
/* packet length in ext tpd */
#define TPD_PKT_LEN_SET(tpd, val)	((tpd)->word[2] = (val))
};

/* emac_ring_header represents a single, contiguous block of DMA space
 * mapped for the three descriptor rings (tpd, rfd, rrd)
 */
struct emac_ring_header {
	void			*v_addr;	/* virtual address */
	dma_addr_t		p_addr;		/* physical address */
	size_t			size;		/* length in bytes */
	size_t			used;
};

/* emac_buffer is wrapper around a pointer to a socket buffer
 * so a DMA handle can be stored along with the skb
 */
struct emac_buffer {
	struct sk_buff		*skb;	/* socket buffer */
	u16			length;	/* rx buffer length */
	dma_addr_t		dma;
};

/* receive free descriptor (rfd) ring */
struct emac_rfd_ring {
	struct emac_buffer	*rfbuff;
	u32 __iomem		*v_addr;	/* virtual address */
	dma_addr_t		p_addr;		/* physical address */
	u64			size;		/* length in bytes */
	u32			count;		/* number of desc in the ring */
	u32			produce_idx;
	u32			process_idx;
	u32			consume_idx;	/* unused */
};

/* Receive Return Desciptor (RRD) ring */
struct emac_rrd_ring {
	u32 __iomem		*v_addr;	/* virtual address */
	dma_addr_t		p_addr;		/* physical address */
	u64			size;		/* length in bytes */
	u32			count;		/* number of desc in the ring */
	u32			produce_idx;	/* unused */
	u32			consume_idx;
};

/* Rx queue */
struct emac_rx_queue {
	struct net_device	*netdev;	/* netdev ring belongs to */
	struct emac_rrd_ring	rrd;
	struct emac_rfd_ring	rfd;
	struct napi_struct	napi;

	u16			que_idx;	/* index in multi rx queues*/
	u16			produce_reg;
	u32			produce_mask;
	u8			produce_shft;

	u16			process_reg;
	u32			process_mask;
	u8			process_shft;

	u16			consume_reg;
	u32			consume_mask;
	u8			consume_shft;

	u32			intr;
	struct emac_irq		*irq;
};

/* Transimit Packet Descriptor (tpd) ring */
struct emac_tpd_ring {
	struct emac_buffer	*tpbuff;
	u32 __iomem		*v_addr;	/* virtual address */
	dma_addr_t		p_addr;		/* physical address */

	u64			size;		/* length in bytes */
	u32			count;		/* number of desc in the ring */
	u32			produce_idx;
	u32			consume_idx;
	u32			last_produce_idx;
};

/* Tx queue */
struct emac_tx_queue {
	struct emac_tpd_ring	tpd;

	u16			que_idx;	/* for multiqueue management */
	u16			max_packets;	/* max packets per interrupt */
	u16			produce_reg;
	u32			produce_mask;
	u8			produce_shft;

	u16			consume_reg;
	u32			consume_mask;
	u8			consume_shft;
};

/* HW tx timestamp */
struct emac_tx_ts {
	u32			ts_idx;
	u32			sec;
	u32			ns;
};

/* Tx timestamp statistics */
struct emac_tx_ts_stats {
	u32			tx;
	u32			rx;
	u32			deliver;
	u32			drop;
	u32			lost;
	u32			timeout;
	u32			sched;
	u32			poll;
	u32			tx_poll;
};

struct emac_adapter;

int  emac_mac_up(struct emac_adapter *adpt);
void emac_mac_down(struct emac_adapter *adpt, bool reset);
void emac_mac_reset(struct emac_adapter *adpt);
void emac_mac_start(struct emac_adapter *adpt);
void emac_mac_stop(struct emac_adapter *adpt);
void emac_mac_addr_clear(struct emac_adapter *adpt, u8 *addr);
void emac_mac_pm(struct emac_adapter *adpt, u32 speed, bool wol_en, bool rx_en);
void emac_mac_mode_config(struct emac_adapter *adpt);
void emac_mac_wol_config(struct emac_adapter *adpt, u32 wufc);
void emac_mac_rx_process(struct emac_adapter *adpt, struct emac_rx_queue *rx_q,
			 int *num_pkts, int max_pkts);
int emac_mac_tx_buf_send(struct emac_adapter *adpt, struct emac_tx_queue *tx_q,
			 struct sk_buff *skb);
void emac_mac_tx_process(struct emac_adapter *adpt, struct emac_tx_queue *tx_q);
void emac_mac_rx_tx_ring_init_all(struct platform_device *pdev,
				  struct emac_adapter *adpt);
int  emac_mac_rx_tx_rings_alloc_all(struct emac_adapter *adpt);
void emac_mac_rx_tx_rings_free_all(struct emac_adapter *adpt);
void emac_mac_tx_ts_periodic_routine(struct work_struct *work);
void emac_mac_multicast_addr_clear(struct emac_adapter *adpt);
void emac_mac_multicast_addr_set(struct emac_adapter *adpt, u8 *addr);

#endif /*_EMAC_HW_H_*/
