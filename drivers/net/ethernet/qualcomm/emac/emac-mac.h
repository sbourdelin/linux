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
#define EMAC_EMAC_WRAPPER_CSR3                                0x000008
#define EMAC_EMAC_WRAPPER_CSR5                                0x000010
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

/* RRD (Receive Return Descriptor) */
union emac_rrd {
	struct {
		/* 32bit word 0 */
		u32  xsum:16;
		u32  nor:4;       /* number of RFD */
		u32  si:12;       /* start index of rfd-ring */
		/* 32bit word 1 */
		u32  hash;
		/* 32bit word 2 */
		u32  cvlan_tag:16; /* vlan-tag */
		u32  reserved:8;
		u32  ptp_timestamp:1;
		u32  rss_cpu:3;   /* CPU number used by RSS */
		u32  rss_flag:4;  /* rss_flag 0, TCP(IPv6) flag for RSS hash alg
				   * rss_flag 1, IPv6 flag for RSS hash algrithm
				   * rss_flag 2, TCP(IPv4) flag for RSS hash alg
				   * rss_flag 3, IPv4 flag for RSS hash algrithm
				   */
		/* 32bit word 3 */
		u32  pkt_len:14;  /* length of the packet */
		u32  l4f:1;       /* L4(TCP/UDP) checksum failed */
		u32  ipf:1;       /* IP checksum failed */
		u32  cvlan_flag:1; /* vlan tagged */
		u32  pid:3;
		u32  res:1;       /* received error summary */
		u32  crc:1;       /* crc error */
		u32  fae:1;       /* frame alignment error */
		u32  trunc:1;     /* truncated packet, larger than MTU */
		u32  runt:1;      /* runt packet */
		u32  icmp:1;      /* incomplete packet due to insufficient
				   * rx-desc
				   */
		u32  bar:1;       /* broadcast address received */
		u32  mar:1;       /* multicast address received */
		u32  type:1;      /* ethernet type */
		u32  fov:1;       /* fifo overflow */
		u32  lene:1;      /* length error */
		u32  update:1;    /* update */

		/* 32bit word 4 */
		u32 ts_low:30;
		u32 __unused__:2;
		/* 32bit word 5 */
		u32 ts_high;
	} genr;
	u32	word[6];
};

/* RFD (Receive Free Descriptor) */
union emac_rfd {
	u64	addr;
	u32	word[2];
};

/* general parameter format of Transmit Packet Descriptor */
struct emac_tpd_general {
	/* 32bit word 0 */
	u32  buffer_len:16; /* include 4-byte CRC */
	u32  svlan_tag:16;
	/* 32bit word 1 */
	u32  l4hdr_offset:8; /* l4 header offset to the 1st byte of packet */
	u32  c_csum:1;
	u32  ip_csum:1;
	u32  tcp_csum:1;
	u32  udp_csum:1;
	u32  lso:1;
	u32  lso_v2:1;
	u32  svtagged:1;   /* vlan-id tagged already */
	u32  ins_svtag:1;  /* insert vlan tag */
	u32  ipv4:1;       /* ipv4 packet */
	u32  type:1;       /* type of packet (ethernet_ii(0) or snap(1)) */
	u32  reserve:12;
	u32  epad:1;       /* even byte padding when this packet */
	u32  last_frag:1;  /* last fragment(buffer) of the packet */
	/* 32bit word 2 */
	u32  addr_lo;
	/* 32bit word 3 */
	u32  cvlan_tag:16;
	u32  cvtagged:1;
	u32  ins_cvtag:1;
	u32  addr_hi:13;
	u32  tstmp_sav:1;
};

/* Custom checksum parameter format of Transmit Packet Descriptor */
struct emac_tpd_checksum {
	/* 32bit word 0 */
	u32  buffer_len:16;
	u32  svlan_tag:16;
	/* 32bit word 1 */
	u32  payld_offset:8; /* payload offset to the 1st byte of packet */
	u32  c_csum:1;       /* do custom checksum offload */
	u32  ip_csum:1;      /* do ip(v4) header checksum offload */
	u32  tcp_csum:1;     /* do tcp checksum offload, both ipv4 and ipv6 */
	u32  udp_csum:1;     /* do udp checksum offlaod, both ipv4 and ipv6 */
	u32  lso:1;
	u32  lso_v2:1;
	u32  svtagged:1;     /* vlan-id tagged already */
	u32  ins_svtag:1;    /* insert vlan tag */
	u32  ipv4:1;         /* ipv4 packet */
	u32  type:1;         /* type of packet (ethernet_ii(0) or snap(1)) */
	u32  cxsum_offset:8; /* checksum offset to the 1st byte of packet */
	u32  reserve:4;
	u32  epad:1;         /* even byte padding when this packet */
	u32  last_frag:1;    /* last fragment(buffer) of the packet */
	/* 32bit word 2 */
	u32  addr_lo;
	/* 32bit word 3 */
	u32  cvlan_tag:16;
	u32  cvtagged:1;
	u32  ins_cvtag:1;
	u32  addr_hi:14;
};

/* TCP Segmentation Offload (v1/v2) of Transmit Packet Descriptor  */
struct emac_tpd_tso {
	/* 32bit word 0 */
	u32  buffer_len:16; /* include 4-byte CRC */
	u32  svlan_tag:16;
	/* 32bit word 1 */
	u32  tcphdr_offset:8; /* tcp hdr offset to the 1st byte of packet */
	u32  c_csum:1;
	u32  ip_csum:1;
	u32  tcp_csum:1;
	u32  udp_csum:1;
	u32  lso:1;        /* do tcp large send (ipv4 only) */
	u32  lso_v2:1;     /* must be 0 in this format */
	u32  svtagged:1;   /* vlan-id tagged already */
	u32  ins_svtag:1;  /* insert vlan tag */
	u32  ipv4:1;       /* ipv4 packet */
	u32  type:1;       /* type of packet (ethernet_ii(1) or snap(0)) */
	u32  mss:13;       /* mss if do tcp large send */
	u32  last_frag:1;  /* last fragment(buffer) of the packet */
	/* 32bit word 2 & 3 */
	u64  pkt_len:32;   /* packet length in ext tpd */
	u64  reserve:32;
};

/* TPD (Transmit Packet Descriptor) */
union emac_tpd {
	struct emac_tpd_general		genr;
	struct emac_tpd_checksum	csum;
	struct emac_tpd_tso		tso;
	u32				word[4];
};

/* emac_ring_header represents a single, contiguous block of DMA space
 * mapped for the three descriptor rings (tpd, rfd, rrd)
 */
struct emac_ring_header {
	void			*v_addr;	/* virtual address */
	dma_addr_t		p_addr;		/* physical address */
	unsigned int		size;		/* length in bytes */
	unsigned int		used;
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
