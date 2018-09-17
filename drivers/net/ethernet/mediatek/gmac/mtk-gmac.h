/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018 MediaTek Inc.
 */
#ifndef __MTK_GMAC_H__
#define __MTK_GMAC_H__

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clocksource.h>
#include <linux/dma-mapping.h>
#include <linux/gpio.h>
#include <linux/if_vlan.h>
#include <linux/netdevice.h>
#include <linux/net_tstamp.h>
#include <linux/phy.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/timecounter.h>
#include <linux/timer.h>
#include <linux/time64.h>
#include <linux/workqueue.h>

#include "mtk-gmac-desc.h"
#include "mtk-gmac-reg.h"

#define GMAC_DRV_NAME			"mtk-gmac"
#define GMAC_DRV_VERSION		"1.0.0"
#define GMAC_DRV_DESC			"MediaTek GMAC Driver"

/* Descriptor related parameters */
#define GMAC_TX_DESC_CNT		1024
#define GMAC_TX_DESC_MIN_FREE		(GMAC_TX_DESC_CNT >> 3)
#define GMAC_TX_DESC_MAX_PROC		(GMAC_TX_DESC_CNT >> 1)
#define GMAC_RX_DESC_CNT		1024
#define GMAC_RX_DESC_MAX_DIRTY		(GMAC_RX_DESC_CNT >> 3)

/* Descriptors required for maximum contiguous TSO/GSO packet */
#define GMAC_TX_MAX_SPLIT	((GSO_MAX_SIZE / GMAC_TX_MAX_BUF_SIZE) + 1)

/* Maximum possible descriptors needed for a SKB */
#define GMAC_TX_MAX_DESC_NR	(MAX_SKB_FRAGS + GMAC_TX_MAX_SPLIT + 2)

#define GMAC_TX_MAX_BUF_SIZE	(SZ_16K - 1)
#define GMAC_RX_MIN_BUF_SIZE	(ETH_FRAME_LEN + ETH_FCS_LEN + VLAN_HLEN)
#define GMAC_RX_BUF_ALIGN	64

#define GMAC_MAX_FIFO			81920

#define GMAC_MAX_DMA_CHANNELS		8
#define GMAC_DMA_STOP_TIMEOUT		5
#define GMAC_DMA_INTERRUPT_MASK		0x31c7

/* Default coalescing parameters */
#define GMAC_INIT_DMA_TX_USECS		1000
#define GMAC_INIT_DMA_TX_FRAMES		25
#define GMAC_INIT_DMA_RX_USECS		30
#define GMAC_INIT_DMA_RX_FRAMES		25
#define GMAC_MAX_DMA_RIWT		0xff
#define GMAC_MIN_DMA_RIWT		0x01

/* Flow control queue count */
#define GMAC_MAX_FLOW_CONTROL_QUEUES	8

/* System clock is axi clk */
#define GMAC_SYSCLOCK			(273000000 / 2)

/* Maximum MAC address hash table size (256 bits = 8 bytes) */
#define GMAC_MAC_HASH_TABLE_SIZE	8

/* Helper macro for descriptor handling
 *  Always use GMAC_GET_DESC_DATA to access the descriptor data
 */
#define GMAC_GET_DESC_DATA(ring, idx) ({				\
	typeof(ring) _ring = (ring);					\
	((_ring)->desc_data_head +					\
	 ((idx) & ((_ring)->dma_desc_count - 1)));			\
})

struct gmac_pdata;

enum gmac_clks_map {
	GMAC_CLK_AXI_DRAM,
	GMAC_CLK_APB_REG,
	GMAC_CLK_MAC_EXT,
	GMAC_CLK_PTP,
	GMAC_CLK_PTP_PARENT,
	GMAC_CLK_PTP_TOP,
	GMAC_CLK_MAX
};

enum gmac_int {
	GMAC_INT_DMA_CH_SR_TI,
	GMAC_INT_DMA_CH_SR_TPS,
	GMAC_INT_DMA_CH_SR_TBU,
	GMAC_INT_DMA_CH_SR_RI,
	GMAC_INT_DMA_CH_SR_RBU,
	GMAC_INT_DMA_CH_SR_RPS,
	GMAC_INT_DMA_CH_SR_TI_RI,
	GMAC_INT_DMA_CH_SR_FBE,
	GMAC_INT_DMA_ALL,
};

struct gmac_stats {
	/* MMC TX counters */
	u64 txoctetcount_gb;
	u64 txframecount_gb;
	u64 txbroadcastframes_g;
	u64 txmulticastframes_g;
	u64 tx64octets_gb;
	u64 tx65to127octets_gb;
	u64 tx128to255octets_gb;
	u64 tx256to511octets_gb;
	u64 tx512to1023octets_gb;
	u64 tx1024tomaxoctets_gb;
	u64 txunicastframes_gb;
	u64 txmulticastframes_gb;
	u64 txbroadcastframes_gb;
	u64 txunderflowerror;
	u64 txsinglecol_g;
	u64 txmulticol_g;
	u64 txdeferred;
	u64 txlatecol;
	u64 txexesscol;
	u64 txcarriererror;
	u64 txoctetcount_g;
	u64 txframecount_g;
	u64 txexcessdef;
	u64 txpauseframes;
	u64 txvlanframes_g;
	u64 txosizeframe_g;
	u64 txlpiusec;
	u64 txlpitran;

	/* MMC RX counters */
	u64 rxframecount_gb;
	u64 rxoctetcount_gb;
	u64 rxoctetcount_g;
	u64 rxbroadcastframes_g;
	u64 rxmulticastframes_g;
	u64 rxcrcerror;
	u64 rxalignerror;
	u64 rxrunterror;
	u64 rxjabbererror;
	u64 rxundersize_g;
	u64 rxoversize_g;
	u64 rx64octets_gb;
	u64 rx65to127octets_gb;
	u64 rx128to255octets_gb;
	u64 rx256to511octets_gb;
	u64 rx512to1023octets_gb;
	u64 rx1024tomaxoctets_gb;
	u64 rxunicastframes_g;
	u64 rxlengtherror;
	u64 rxoutofrangetype;
	u64 rxpauseframes;
	u64 rxfifooverflow;
	u64 rxvlanframes_gb;
	u64 rxwatchdogerror;
	u64 rxreceiveerror;
	u64 rxctrlframes_g;
	u64 rxlpiusec;
	u64 rxlpitran;

	/* MMC RXIPC counters */
	u64 rxipv4_g;
	u64 rxipv4hderr;
	u64 rxipv4nopay;
	u64 rxipv4frag;
	u64 rxipv4udsbl;
	u64 rxipv6octets_g;
	u64 rxipv6hderroctets;
	u64 rxipv6nopayoctets;
	u64 rxudp_g;
	u64 rxudperr;
	u64 rxtcp_g;
	u64 rxtcperr;
	u64 rxicmp_g;
	u64 rxicmperr;
	u64 rxipv4octets_g;
	u64 rxipv4hderroctets;
	u64 rxipv4nopayoctets;
	u64 rxipv4fragoctets;
	u64 rxipv4udsbloctets;
	u64 rxipv6_g;
	u64 rxipv6hderr;
	u64 rxipv6nopay;
	u64 rxudpoctets_g;
	u64 rxudperroctets;
	u64 rxtcpoctets_g;
	u64 rxtcperroctets;
	u64 rxicmpoctets_g;
	u64 rxicmperroctets;

	/* Extra counters */
	u64 tx_tso_packets;
	u64 rx_split_header_packets;
	u64 tx_process_stopped;
	u64 rx_process_stopped;
	u64 tx_buffer_unavailable;
	u64 rx_buffer_unavailable;
	u64 fatal_bus_error;
	u64 tx_vlan_packets;
	u64 rx_vlan_packets;
	u64 tx_timestamp_packets;
	u64 rx_timestamp_packets;
	u64 napi_poll_isr;
	u64 napi_poll_txtimer;
};

struct gmac_ring_buf {
	struct sk_buff *skb;
	dma_addr_t skb_dma;
	unsigned int skb_len;
};

/* Common Tx and Rx DMA hardware descriptor */
struct gmac_dma_desc {
	u32 desc0;
	u32 desc1;
	u32 desc2;
	u32 desc3;
};

/* TxRx-related desc data */
struct gmac_trx_desc_data {
	unsigned int packets;		/* BQL packet count */
	unsigned int bytes;		/* BQL byte count */
};

struct gmac_pkt_info {
	struct sk_buff *skb;

	unsigned int attributes;

	unsigned int errors;

	/* descriptors needed for this packet */
	unsigned int desc_count;
	unsigned int length;

	unsigned int tx_packets;
	unsigned int tx_bytes;

	unsigned int header_len;
	unsigned int tcp_header_len;
	unsigned int tcp_payload_len;
	unsigned short mss;

	unsigned short vlan_ctag;

	u64 rx_tstamp;
};

struct gmac_desc_data {
	/* dma_desc: Virtual address of descriptor
	 *  dma_desc_addr: DMA address of descriptor
	 */
	struct gmac_dma_desc *dma_desc;
	dma_addr_t dma_desc_addr;

	/* skb: Virtual address of SKB
	 *  skb_dma: DMA address of SKB data
	 *  skb_dma_len: Length of SKB DMA area
	 */
	struct sk_buff *skb;
	dma_addr_t skb_dma;
	unsigned int skb_dma_len;

	/* Tx/Rx -related data */
	struct gmac_trx_desc_data trx;

	unsigned int mapped_as_page;

	/* Incomplete receive save location.  If the budget is exhausted
	 * or the last descriptor (last normal descriptor or a following
	 * context descriptor) has not been DMA'd yet the current state
	 * of the receive processing needs to be saved.
	 */
	unsigned int state_saved;
	struct {
		struct sk_buff *skb;
		unsigned int len;
		unsigned int error;
	} state;
};

struct gmac_ring {
	/* Per packet related information */
	struct gmac_pkt_info pkt_info;

	/* Virtual/DMA addresses of DMA descriptor list and the total count */
	struct gmac_dma_desc *dma_desc_head;
	dma_addr_t dma_desc_head_addr;
	unsigned int dma_desc_count;

	/* Array of descriptor data corresponding the DMA descriptor
	 * (always use the GMAC_GET_DESC_DATA macro to access this data)
	 */
	struct gmac_desc_data *desc_data_head;

	/* Ring index values
	 *  cur   - Tx: index of descriptor to be used for current transfer
	 *          Rx: index of descriptor to check for packet availability
	 *  dirty - Tx: index of descriptor to check for transfer complete
	 *          Rx: index of descriptor to check for buffer reallocation
	 */
	unsigned int cur;
	unsigned int dirty;

	/* Coalesce frame count used for interrupt bit setting */
	unsigned int coalesce_count;

	union {
		struct {
			unsigned int xmit_more;
			unsigned int queue_stopped;
			unsigned short cur_mss;
			unsigned short cur_vlan_ctag;
		} tx;
	};
} ____cacheline_aligned;

struct gmac_channel {
	char name[16];

	/* Address of private data area for device */
	struct gmac_pdata *pdata;

	/* Queue index and base address of queue's DMA registers */
	unsigned int queue_index;
	void __iomem *dma_regs;

	/* Per channel interrupt irq number */
	int dma_irq;
	char dma_irq_name[IFNAMSIZ + 32];

	/* Netdev related settings */
	struct napi_struct napi;

	unsigned int saved_ier;

	unsigned int tx_timer_active;
	struct timer_list tx_timer;

	struct gmac_ring *tx_ring;
	struct gmac_ring *rx_ring;
} ____cacheline_aligned;

struct gmac_desc_ops {
	int (*alloc_channles_and_rings)(struct gmac_pdata *pdata);
	void (*free_channels_and_rings)(struct gmac_pdata *pdata);
	int (*map_tx_skb)(struct gmac_channel *channel,
			  struct sk_buff *skb);
	int (*map_rx_buffer)(struct gmac_pdata *pdata,
			     struct gmac_ring *ring,
			struct gmac_desc_data *desc_data);
	void (*unmap_desc_data)(struct gmac_pdata *pdata,
				struct gmac_desc_data *desc_data,
				unsigned int tx_rx);
	void (*tx_desc_init)(struct gmac_pdata *pdata);
	void (*rx_desc_init)(struct gmac_pdata *pdata);
};

struct gmac_hw_ops {
	int (*init)(struct gmac_pdata *pdata);
	int (*exit)(struct gmac_pdata *pdata);

	int (*tx_complete)(struct gmac_dma_desc *dma_desc);

	void (*enable_tx)(struct gmac_pdata *pdata);
	void (*disable_tx)(struct gmac_pdata *pdata);
	void (*enable_rx)(struct gmac_pdata *pdata);
	void (*disable_rx)(struct gmac_pdata *pdata);

	int (*enable_int)(struct gmac_channel *channel,
			  enum gmac_int int_id);
	int (*disable_int)(struct gmac_channel *channel,
			   enum gmac_int int_id);
	void (*dev_xmit)(struct gmac_channel *channel);
	int (*dev_read)(struct gmac_channel *channel);

	int (*set_mac_address)(struct gmac_pdata *pdata, u8 *addr,
			       unsigned int idx);
	int (*config_rx_mode)(struct gmac_pdata *pdata);
	int (*enable_rx_csum)(struct gmac_pdata *pdata);
	int (*disable_rx_csum)(struct gmac_pdata *pdata);

	/* For MII speed configuration */
	int (*set_gmii_10_speed)(struct gmac_pdata *pdata);
	int (*set_gmii_100_speed)(struct gmac_pdata *pdata);
	int (*set_gmii_1000_speed)(struct gmac_pdata *pdata);

	int (*set_full_duplex)(struct gmac_pdata *pdata);
	int (*set_half_duplex)(struct gmac_pdata *pdata);

	/* For descriptor related operation */
	void (*tx_desc_init)(struct gmac_channel *channel);
	void (*rx_desc_init)(struct gmac_channel *channel);
	void (*tx_desc_reset)(struct gmac_desc_data *desc_data);
	void (*rx_desc_reset)(struct gmac_pdata *pdata,
			      struct gmac_desc_data *desc_data,
			      unsigned int index);
	int (*is_last_desc)(struct gmac_dma_desc *dma_desc);
	int (*is_context_desc)(struct gmac_dma_desc *dma_desc);
	void (*tx_start_xmit)(struct gmac_channel *channel,
			      struct gmac_ring *ring);

	/* For Flow Control */
	int (*config_tx_flow_control)(struct gmac_pdata *pdata);
	int (*config_rx_flow_control)(struct gmac_pdata *pdata);

	/* For Vlan related config */
	int (*enable_rx_vlan_stripping)(struct gmac_pdata *pdata);
	int (*disable_rx_vlan_stripping)(struct gmac_pdata *pdata);
	int (*enable_rx_vlan_filtering)(struct gmac_pdata *pdata);
	int (*disable_rx_vlan_filtering)(struct gmac_pdata *pdata);
	int (*update_vlan_hash_table)(struct gmac_pdata *pdata);
	int (*update_vlan)(struct gmac_pdata *pdata);

	/* For RX coalescing */
	int (*config_rx_coalesce)(struct gmac_pdata *pdata);
	int (*config_tx_coalesce)(struct gmac_pdata *pdata);
	unsigned int (*usec_to_riwt)(struct gmac_pdata *pdata,
				     unsigned int usec);
	unsigned int (*riwt_to_usec)(struct gmac_pdata *pdata,
				     unsigned int riwt);

	/* For RX and TX threshold config */
	int (*config_rx_threshold)(struct gmac_pdata *pdata,
				   unsigned int val);
	int (*config_tx_threshold)(struct gmac_pdata *pdata,
				   unsigned int val);

	/* For RX and TX Store and Forward Mode config */
	int (*config_rsf_mode)(struct gmac_pdata *pdata,
			       unsigned int val);
	int (*config_tsf_mode)(struct gmac_pdata *pdata,
			       unsigned int val);

	/* For TX DMA Operate on Second Frame config */
	int (*config_osp_mode)(struct gmac_pdata *pdata);

	/* For RX and TX PBL config */
	int (*config_rx_pbl_val)(struct gmac_pdata *pdata);
	int (*config_tx_pbl_val)(struct gmac_pdata *pdata);
	int (*config_pblx8)(struct gmac_pdata *pdata);

	/* For MMC statistics */
	void (*rxipc_mmc_int)(struct gmac_pdata *pdata);
	void (*rx_mmc_int)(struct gmac_pdata *pdata);
	void (*tx_mmc_int)(struct gmac_pdata *pdata);
	void (*read_mmc_stats)(struct gmac_pdata *pdata);

	void (*config_hw_timestamping)(struct gmac_pdata *pdata,
				       u32 data);
	void (*config_sub_second_increment)(struct gmac_pdata *pdata,
					    u32 ptp_clock,
					    u32 *ssinc);
	int (*init_systime)(struct gmac_pdata *pdata, u32 sec, u32 nsec);
	int (*config_addend)(struct gmac_pdata *pdata, u32 addend);
	int (*adjust_systime)(struct gmac_pdata *pdata,
			      u32 sec,
			      u32 nsec,
			      int add_sub);
	void (*get_systime)(struct gmac_pdata *pdata, u64 *systime);
	void (*get_tx_hwtstamp)(struct gmac_pdata *pdata,
				struct gmac_dma_desc *desc,
				struct sk_buff *skb);

};

/* This structure contains flags that indicate what hardware features
 * or configurations are present in the device.
 */
struct gmac_hw_features {
	/* HW Version */
	unsigned int version;

	/* HW Feature Register0 */
	unsigned int mii;		/* 10/100Mbps support */
	unsigned int gmii;		/* 1000Mbps support */
	unsigned int hd;		/* Half Duplex support */
	unsigned int pcs;		/* TBI/SGMII/RTBI PHY interface */
	unsigned int vlhash;		/* VLAN Hash Filter */
	unsigned int sma;		/* SMA(MDIO) Interface */
	unsigned int rwk;		/* PMT remote wake-up packet */
	unsigned int mgk;		/* PMT magic packet */
	unsigned int mmc;		/* RMON module */
	unsigned int aoe;		/* ARP Offload */
	unsigned int ts;		/* IEEE 1588-2008 Advanced Timestamp */
	unsigned int eee;		/* Energy Efficient Ethernet */
	unsigned int tx_coe;		/* Tx Checksum Offload */
	unsigned int rx_coe;		/* Rx Checksum Offload */
	unsigned int addn_mac;		/* Additional MAC Addresses */
	unsigned int ts_src;		/* Timestamp Source */
	unsigned int sa_vlan_ins;	/* Source Address or VLAN Insertion */
	unsigned int phyifsel;		/* PHY interface support */

	/* HW Feature Register1 */
	unsigned int rx_fifo_size;	/* MTL Receive FIFO Size */
	unsigned int tx_fifo_size;	/* MTL Transmit FIFO Size */
	unsigned int one_step_en;	/* One-Step Timingstamping Enable */
	unsigned int ptp_offload;	/* PTP Offload Enable */
	unsigned int adv_ts_hi;		/* Advance Timestamping High Word */
	unsigned int dma_width;		/* DMA width */
	unsigned int dcb;		/* DCB Feature */
	unsigned int sph;		/* Split Header Feature */
	unsigned int tso;		/* TCP Segmentation Offload */
	unsigned int dma_debug;		/* DMA Debug Registers */
	unsigned int av;		/* Audio-Vedio Bridge Option */
	unsigned int rav;		/* Rx Side AV Feature */
	unsigned int pouost;		/* One-Step for PTP over UDP/IP */
	unsigned int hash_table_size;	/* Hash Table Size */
	unsigned int l3l4_filter_num;	/* Number of L3-L4 Filters */

	/* HW Feature Register2 */
	unsigned int rx_q_cnt;		/* Number of MTL Receive Queues */
	unsigned int tx_q_cnt;		/* Number of MTL Transmit Queues */
	unsigned int rx_ch_cnt;		/* Number of DMA Receive Channels */
	unsigned int tx_ch_cnt;		/* Number of DMA Transmit Channels */
	unsigned int pps_out_num;	/* Number of PPS outputs */
	unsigned int aux_snap_num;	/* Number of Aux snapshot inputs */
};

struct plat_gmac_data {
	struct regmap *infra_regmap, *peri_regmap;
	struct clk *clks[GMAC_CLK_MAX];
	struct device_node *np;
	int phy_mode;
	void (*gmac_set_interface)(struct plat_gmac_data *plat);
	void (*gmac_set_delay)(struct plat_gmac_data *plat);
	int (*gmac_clk_enable)(struct plat_gmac_data *plat);
	void (*gmac_clk_disable)(struct plat_gmac_data *plat);
};

struct gmac_resources {
	void __iomem *base_addr;
	const char *mac_addr;
	int irq;
	int phy_rst;
};

struct gmac_pdata {
	struct net_device *netdev;
	struct device *dev;

	struct plat_gmac_data *plat;

	struct gmac_hw_ops hw_ops;
	struct gmac_desc_ops desc_ops;

	/* Device statistics */
	struct gmac_stats stats;

	u32 msg_enable;

	/* MAC registers base */
	void __iomem *mac_regs;

	/* phydev */
	struct mii_bus *mii;
	struct phy_device *phydev;
	int phyaddr;
	int bus_id;

	/* Hardware features of the device */
	struct gmac_hw_features hw_feat;

	struct work_struct restart_work;

	/* Rings for Tx/Rx on a DMA channel */
	struct gmac_channel *channel_head;
	unsigned int channel_count;
	unsigned int tx_ring_count;
	unsigned int rx_ring_count;
	unsigned int tx_desc_count;
	unsigned int rx_desc_count;
	unsigned int tx_q_count;
	unsigned int rx_q_count;

	/* Tx/Rx common settings */
	unsigned int pblx8;

	/* Tx settings */
	unsigned int tx_sf_mode;
	unsigned int tx_threshold;
	unsigned int tx_pbl;
	unsigned int tx_osp_mode;

	/* Rx settings */
	unsigned int rx_sf_mode;
	unsigned int rx_threshold;
	unsigned int rx_pbl;
	unsigned int rx_sph;

	/* Tx coalescing settings */
	unsigned int tx_usecs;
	unsigned int tx_frames;

	/* Rx coalescing settings */
	unsigned int rx_riwt;
	unsigned int rx_usecs;
	unsigned int rx_frames;

	/* Current Rx buffer size */
	unsigned int rx_buf_size;

	/* Flow control settings */
	unsigned int tx_pause;
	unsigned int rx_pause;

	unsigned int max_addr_reg_cnt;

	/* Device interrupt number */
	int phy_rst;
	int dev_irq;
	unsigned int per_channel_irq;
	int channel_irq[GMAC_MAX_DMA_CHANNELS];

	/* Netdev related settings */
	unsigned char mac_addr[ETH_ALEN];
	netdev_features_t netdev_features;
	struct napi_struct napi;

	/* Filtering support */
	unsigned long active_vlans[BITS_TO_LONGS(VLAN_N_VID)];
	int vlan_weight;

	/* Device clocks */
	unsigned long sysclk_rate;

	/* DMA width */
	unsigned int dma_width;

	/* HW timestamping */
	unsigned char hwts_tx_en;
	unsigned char hwts_rx_en;
	unsigned long ptpclk_rate, ptptop_rate;
	unsigned int ptp_divider;
	struct ptp_clock_info ptp_clock_info;
	struct ptp_clock *ptp_clock;
	u64 default_addend;
	/* protects registers access */
	spinlock_t ptp_lock;

	int phy_speed;
	int duplex;

	char drv_name[32];
	char drv_ver[32];
};

void gmac_init_desc_ops(struct gmac_desc_ops *desc_ops);
void gmac_init_hw_ops(struct gmac_hw_ops *hw_ops);
const struct net_device_ops *gmac_get_netdev_ops(void);
const struct ethtool_ops *gmac_get_ethtool_ops(void);
void gmac_dump_tx_desc(struct gmac_pdata *pdata,
		       struct gmac_ring *ring,
		       unsigned int idx,
		       unsigned int count,
		       unsigned int flag);
void gmac_dump_rx_desc(struct gmac_pdata *pdata,
		       struct gmac_ring *ring,
		       unsigned int idx);
void gmac_print_pkt(struct net_device *netdev,
		    struct sk_buff *skb, bool tx_rx);
int gmac_drv_probe(struct device *dev,
		   struct plat_gmac_data *plat,
		   struct gmac_resources *res);
int gmac_drv_remove(struct device *dev);

int mdio_register(struct net_device *ndev);
void mdio_unregister(struct net_device *ndev);

/* For debug prints */
#ifdef GMAC_DEBUG
#define GMAC_PR(fmt, args...) \
	pr_alert("[%s,%d]:" fmt, __func__, __LINE__, ## args)
#else
#define GMAC_PR(x...)		do { } while (0)
#endif

#endif /* __MTK_GMAC_H__ */
