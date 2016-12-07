/*
 * Synopsys DesignWare Ethernet Driver
 *
 * Copyright (c) 2014-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * This file is free software; you may copy, redistribute and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at
 * your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *     The Synopsys DWC ETHER XGMAC Software Driver and documentation
 *     (hereinafter "Software") is an unsupported proprietary work of Synopsys,
 *     Inc. unless otherwise expressly agreed to in writing between Synopsys
 *     and you.
 *
 *     The Software IS NOT an item of Licensed Software or Licensed Product
 *     under any End User Software License Agreement or Agreement for Licensed
 *     Product with Synopsys or any supplement thereto.  Permission is hereby
 *     granted, free of charge, to any person obtaining a copy of this software
 *     annotated with this license and the Software, to deal in the Software
 *     without restriction, including without limitation the rights to use,
 *     copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *     of the Software, and to permit persons to whom the Software is furnished
 *     to do so, subject to the following conditions:
 *
 *     The above copyright notice and this permission notice shall be included
 *     in all copies or substantial portions of the Software.
 *
 *     THIS SOFTWARE IS BEING DISTRIBUTED BY SYNOPSYS SOLELY ON AN "AS IS"
 *     BASIS AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *     TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *     PARTICULAR PURPOSE ARE HEREBY DISCLAIMED. IN NO EVENT SHALL SYNOPSYS
 *     BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *     CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *     SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *     INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *     CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *     ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *     THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __DWC_ETH_H__
#define __DWC_ETH_H__

#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <linux/workqueue.h>
#include <linux/phy.h>
#include <linux/if_vlan.h>
#include <linux/bitops.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/timecounter.h>
#include <linux/net_tstamp.h>

#define DWC_ETH_MDIO_RD_TIMEOUT		1000

/* Maximum MAC address hash table size (256 bits = 8 bytes) */
#define DWC_ETH_MAC_HASH_TABLE_SIZE	8

#define DWC_ETH_MAX_DMA_CHANNELS	16
#define DWC_ETH_MAX_QUEUES		16
#define DWC_ETH_MAX_FIFO		81920

#define DWC_ETH_DMA_INTERRUPT_MASK	0x31c7

/* Receive Side Scaling */
#define DWC_ETH_RSS_HASH_KEY_SIZE	40
#define DWC_ETH_RSS_MAX_TABLE_SIZE	256
#define DWC_ETH_RSS_LOOKUP_TABLE_TYPE	0
#define DWC_ETH_RSS_HASH_KEY_TYPE	1

#define DWC_ETH_MIN_PACKET		60
#define DWC_ETH_STD_PACKET_MTU		1500
#define DWC_ETH_MAX_STD_PACKET		1518
#define DWC_ETH_JUMBO_PACKET_MTU	9000
#define DWC_ETH_MAX_JUMBO_PACKET	9018

/* MDIO bus phy name */
#define DWC_ETH_PHY_NAME		"dwc_eth_phy"
#define DWC_ETH_PRTAD			0

/* Driver PMT macros */
#define DWC_ETH_DRIVER_CONTEXT		1
#define DWC_ETH_IOCTL_CONTEXT		2

/* Helper macro for descriptor handling
 *  Always use DWC_ETH_GET_DESC_DATA to access the descriptor data
 */
#define DWC_ETH_GET_DESC_DATA(_ring, _idx)			\
	((_ring)->desc_data_head +				\
	 ((_idx) & ((_ring)->dma_desc_count - 1)))

struct dwc_eth_pdata;

enum dwc_eth_int {
	DWC_ETH_INT_DMA_CH_SR_TI,
	DWC_ETH_INT_DMA_CH_SR_TPS,
	DWC_ETH_INT_DMA_CH_SR_TBU,
	DWC_ETH_INT_DMA_CH_SR_RI,
	DWC_ETH_INT_DMA_CH_SR_RBU,
	DWC_ETH_INT_DMA_CH_SR_RPS,
	DWC_ETH_INT_DMA_CH_SR_TI_RI,
	DWC_ETH_INT_DMA_CH_SR_FBE,
	DWC_ETH_INT_DMA_ALL,
};

struct dwc_eth_stats {
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
	u64 txoctetcount_g;
	u64 txframecount_g;
	u64 txpauseframes;
	u64 txvlanframes_g;

	/* MMC RX counters */
	u64 rxframecount_gb;
	u64 rxoctetcount_gb;
	u64 rxoctetcount_g;
	u64 rxbroadcastframes_g;
	u64 rxmulticastframes_g;
	u64 rxcrcerror;
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

	/* Extra counters */
	u64 tx_tso_packets;
	u64 rx_split_header_packets;
	u64 rx_buffer_unavailable;
};

struct dwc_eth_ring_buf {
	struct sk_buff *skb;
	dma_addr_t skb_dma;
	unsigned int skb_len;
};

/* Common Tx and Rx DMA hardware descriptor */
struct dwc_eth_dma_desc {
	__le32 desc0;
	__le32 desc1;
	__le32 desc2;
	__le32 desc3;
};

/* Page allocation related values */
struct dwc_eth_page_alloc {
	struct page *pages;
	unsigned int pages_len;
	unsigned int pages_offset;

	dma_addr_t pages_dma;
};

/* Ring entry buffer data */
struct dwc_eth_buffer_data {
	struct dwc_eth_page_alloc pa;
	struct dwc_eth_page_alloc pa_unmap;

	dma_addr_t dma_base;
	unsigned long dma_off;
	unsigned int dma_len;
};

/* Tx-related desc data */
struct dwc_eth_tx_desc_data {
	unsigned int packets;		/* BQL packet count */
	unsigned int bytes;		/* BQL byte count */
};

/* Rx-related desc data */
struct dwc_eth_rx_desc_data {
	struct dwc_eth_buffer_data hdr;	/* Header locations */
	struct dwc_eth_buffer_data buf;	/* Payload locations */

	unsigned short hdr_len;		/* Length of received header */
	unsigned short len;		/* Length of received packet */
};

struct dwc_eth_pkt_info {
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

	u32 rss_hash;
	enum pkt_hash_types rss_hash_type;
};

struct dwc_eth_desc_data {
	/* dma_desc: Virtual address of descriptor
	 *  dma_desc_addr: DMA address of descriptor
	 */
	struct dwc_eth_dma_desc *dma_desc;
	dma_addr_t dma_desc_addr;

	/* skb: Virtual address of SKB
	 *  skb_dma: DMA address of SKB data
	 *  skb_dma_len: Length of SKB DMA area
	 */
	struct sk_buff *skb;
	dma_addr_t skb_dma;
	unsigned int skb_dma_len;

	/* Tx/Rx -related data */
	struct dwc_eth_tx_desc_data tx;
	struct dwc_eth_rx_desc_data rx;

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

struct dwc_eth_ring {
	/* Per packet related information */
	struct dwc_eth_pkt_info pkt_info;

	/* Virtual/DMA addresses of DMA descriptor list and the total count */
	struct dwc_eth_dma_desc *dma_desc_head;
	dma_addr_t dma_desc_head_addr;
	unsigned int dma_desc_count;

	/* Array of descriptor data corresponding the DMA descriptor
	 * (always use the DWC_ETH_GET_DESC_DATA macro to access this data)
	 */
	struct dwc_eth_desc_data *desc_data_head;

	/* Page allocation for RX buffers */
	struct dwc_eth_page_alloc rx_hdr_pa;
	struct dwc_eth_page_alloc rx_buf_pa;

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

struct dwc_eth_channel {
	char name[16];

	/* Address of private data area for device */
	struct dwc_eth_pdata *pdata;

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

	struct dwc_eth_ring *tx_ring;
	struct dwc_eth_ring *rx_ring;
} ____cacheline_aligned;

struct dwc_eth_desc_ops {
	int (*alloc_channles_and_rings)(struct dwc_eth_pdata *pdata);
	void (*free_channels_and_rings)(struct dwc_eth_pdata *pdata);
	int (*map_tx_skb)(struct dwc_eth_channel *channel,
			  struct sk_buff *skb);
	int (*map_rx_buffer)(struct dwc_eth_pdata *pdata,
			     struct dwc_eth_ring *ring,
			struct dwc_eth_desc_data *desc_data);
	void (*unmap_desc_data)(struct dwc_eth_pdata *pdata,
				struct dwc_eth_desc_data *desc_data);
	void (*tx_desc_init)(struct dwc_eth_pdata *pdata);
	void (*rx_desc_init)(struct dwc_eth_pdata *pdata);
};

struct dwc_eth_hw_ops {
	int (*tx_complete)(struct dwc_eth_dma_desc *dma_desc);

	int (*set_mac_address)(struct dwc_eth_pdata *pdata, u8 *addr);
	int (*config_rx_mode)(struct dwc_eth_pdata *pdata);

	int (*enable_rx_csum)(struct dwc_eth_pdata *pdata);
	int (*disable_rx_csum)(struct dwc_eth_pdata *pdata);

	int (*enable_rx_vlan_stripping)(struct dwc_eth_pdata *pdata);
	int (*disable_rx_vlan_stripping)(struct dwc_eth_pdata *pdata);
	int (*enable_rx_vlan_filtering)(struct dwc_eth_pdata *pdata);
	int (*disable_rx_vlan_filtering)(struct dwc_eth_pdata *pdata);
	int (*update_vlan_hash_table)(struct dwc_eth_pdata *pdata);

	int (*read_mmd_regs)(struct dwc_eth_pdata *pdata,
			     int prtad, int mmd_reg);
	int (*write_mmd_regs)(struct dwc_eth_pdata *pdata,
			      int prtad, int mmd_reg, int mmd_data);

	int (*set_gmii_1000_speed)(struct dwc_eth_pdata *pdata);
	int (*set_gmii_2500_speed)(struct dwc_eth_pdata *pdata);
	int (*set_xgmii_10000_speed)(struct dwc_eth_pdata *pdata);
	int (*set_xlgmii_25000_speed)(struct dwc_eth_pdata *pdata);
	int (*set_xlgmii_40000_speed)(struct dwc_eth_pdata *pdata);
	int (*set_xlgmii_50000_speed)(struct dwc_eth_pdata *pdata);
	int (*set_xlgmii_100000_speed)(struct dwc_eth_pdata *pdata);

	void (*enable_tx)(struct dwc_eth_pdata *pdata);
	void (*disable_tx)(struct dwc_eth_pdata *pdata);
	void (*enable_rx)(struct dwc_eth_pdata *pdata);
	void (*disable_rx)(struct dwc_eth_pdata *pdata);

	void (*powerup_tx)(struct dwc_eth_pdata *pdata);
	void (*powerdown_tx)(struct dwc_eth_pdata *pdata);
	void (*powerup_rx)(struct dwc_eth_pdata *pdata);
	void (*powerdown_rx)(struct dwc_eth_pdata *pdata);

	int (*init)(struct dwc_eth_pdata *pdata);
	int (*exit)(struct dwc_eth_pdata *pdata);

	int (*enable_int)(struct dwc_eth_channel *channel,
			  enum dwc_eth_int int_id);
	int (*disable_int)(struct dwc_eth_channel *channel,
			   enum dwc_eth_int int_id);
	void (*dev_xmit)(struct dwc_eth_channel *channel);
	int (*dev_read)(struct dwc_eth_channel *channel);

	void (*tx_desc_init)(struct dwc_eth_channel *channel);
	void (*rx_desc_init)(struct dwc_eth_channel *channel);
	void (*tx_desc_reset)(struct dwc_eth_desc_data *desc_data);
	void (*rx_desc_reset)(struct dwc_eth_pdata *pdata,
			      struct dwc_eth_desc_data *desc_data,
			unsigned int index);
	int (*is_last_desc)(struct dwc_eth_dma_desc *dma_desc);
	int (*is_context_desc)(struct dwc_eth_dma_desc *dma_desc);
	void (*tx_start_xmit)(struct dwc_eth_channel *channel,
			      struct dwc_eth_ring *ring);

	/* For FLOW ctrl */
	int (*config_tx_flow_control)(struct dwc_eth_pdata *pdata);
	int (*config_rx_flow_control)(struct dwc_eth_pdata *pdata);

	/* For RX coalescing */
	int (*config_rx_coalesce)(struct dwc_eth_pdata *pdata);
	int (*config_tx_coalesce)(struct dwc_eth_pdata *pdata);
	unsigned int (*usec_to_riwt)(struct dwc_eth_pdata *pdata,
				     unsigned int usec);
	unsigned int (*riwt_to_usec)(struct dwc_eth_pdata *pdata,
				     unsigned int riwt);

	/* For RX and TX threshold config */
	int (*config_rx_threshold)(struct dwc_eth_pdata *pdata,
				   unsigned int val);
	int (*config_tx_threshold)(struct dwc_eth_pdata *pdata,
				   unsigned int val);

	/* For RX and TX Store and Forward Mode config */
	int (*config_rsf_mode)(struct dwc_eth_pdata *pdata,
			       unsigned int val);
	int (*config_tsf_mode)(struct dwc_eth_pdata *pdata,
			       unsigned int val);

	/* For TX DMA Operate on Second Frame config */
	int (*config_osp_mode)(struct dwc_eth_pdata *pdata);

	/* For RX and TX PBL config */
	int (*config_rx_pbl_val)(struct dwc_eth_pdata *pdata);
	int (*get_rx_pbl_val)(struct dwc_eth_pdata *pdata);
	int (*config_tx_pbl_val)(struct dwc_eth_pdata *pdata);
	int (*get_tx_pbl_val)(struct dwc_eth_pdata *pdata);
	int (*config_pblx8)(struct dwc_eth_pdata *pdata);

	/* For MMC statistics */
	void (*rx_mmc_int)(struct dwc_eth_pdata *pdata);
	void (*tx_mmc_int)(struct dwc_eth_pdata *pdata);
	void (*read_mmc_stats)(struct dwc_eth_pdata *pdata);

	/* For Timestamp config */
	int (*config_tstamp)(struct dwc_eth_pdata *pdata,
			     unsigned int mac_tscr);
	void (*update_tstamp_addend)(struct dwc_eth_pdata *pdata,
				     unsigned int addend);
	void (*set_tstamp_time)(struct dwc_eth_pdata *pdata,
				unsigned int sec,
				unsigned int nsec);
	u64 (*get_tstamp_time)(struct dwc_eth_pdata *pdata);
	u64 (*get_tx_tstamp)(struct dwc_eth_pdata *pdata);

	/* For Data Center Bridging config */
	void (*config_tc)(struct dwc_eth_pdata *pdata);
	void (*config_dcb_tc)(struct dwc_eth_pdata *pdata);
	void (*config_dcb_pfc)(struct dwc_eth_pdata *pdata);

	/* For Receive Side Scaling */
	int (*enable_rss)(struct dwc_eth_pdata *pdata);
	int (*disable_rss)(struct dwc_eth_pdata *pdata);
	int (*set_rss_hash_key)(struct dwc_eth_pdata *pdata,
				const u8 *key);
	int (*set_rss_lookup_table)(struct dwc_eth_pdata *pdata,
				    const u32 *table);
};

/* This structure contains flags that indicate what hardware features
 * or configurations are present in the device.
 */
struct dwc_eth_hw_features {
	/* HW Version */
	unsigned int version;

	/* HW Feature Register0 */
	unsigned int phyifsel;		/* PHY interface support */
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

	/* HW Feature Register1 */
	unsigned int rx_fifo_size;	/* MTL Receive FIFO Size */
	unsigned int tx_fifo_size;	/* MTL Transmit FIFO Size */
	unsigned int adv_ts_hi;		/* Advance Timestamping High Word */
	unsigned int dma_width;		/* DMA width */
	unsigned int dcb;		/* DCB Feature */
	unsigned int sph;		/* Split Header Feature */
	unsigned int tso;		/* TCP Segmentation Offload */
	unsigned int dma_debug;		/* DMA Debug Registers */
	unsigned int rss;		/* Receive Side Scaling */
	unsigned int tc_cnt;		/* Number of Traffic Classes */
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

struct dwc_eth_pdata {
	struct net_device *netdev;
	struct pci_dev *pcidev;
	struct device *dev;

	struct dwc_eth_hw_ops hw_ops;
	struct dwc_eth_hw_ops *hw2_ops;
	struct dwc_eth_desc_ops desc_ops;

	/* Device statistics */
	struct dwc_eth_stats stats;

	u32 msg_enable;

	/* MAC registers base */
	void __iomem *mac_regs;

	/* Hardware features of the device */
	struct dwc_eth_hw_features hw_feat;

	struct workqueue_struct *dev_workqueue;
	struct work_struct restart_work;

	/* AXI DMA settings */
	unsigned int coherent;
	unsigned int axdomain;
	unsigned int arcache;
	unsigned int awcache;

	/* Rings for Tx/Rx on a DMA channel */
	struct dwc_eth_channel *channel_head;
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
	unsigned int pause_autoneg;
	unsigned int tx_pause;
	unsigned int rx_pause;

	/* Device interrupt number */
	int dev_irq;
	unsigned int per_channel_irq;
	int channel_irq[DWC_ETH_MAX_DMA_CHANNELS];

	/* Netdev related settings */
	unsigned char mac_addr[ETH_ALEN];
	netdev_features_t netdev_features;
	struct napi_struct napi;

	/* Filtering support */
	unsigned long active_vlans[BITS_TO_LONGS(VLAN_N_VID)];

	/* Device clocks */
	unsigned long sysclk_rate;
	unsigned long ptpclk_rate;

	/* Keeps track of power mode */
	unsigned int power_down;

	/* Overall device lock */
	spinlock_t lock;
	/* RSS addressing mutex */
	struct mutex rss_mutex;
	/* XPCS indirect addressing mutex */
	struct mutex pcs_mutex;

	/* Receive Side Scaling settings */
	u8 rss_key[DWC_ETH_RSS_HASH_KEY_SIZE];
	u32 rss_table[DWC_ETH_RSS_MAX_TABLE_SIZE];
	u32 rss_options;

	/* MDIO settings */
	int mdio_en;
	struct module *phy_module;
	char *mii_bus_id;
	struct mii_bus *mii;
	int mdio_mmd;
	struct phy_device *phydev;
	int default_autoneg;
	int default_speed;

	/* Current PHY settings */
	phy_interface_t phy_mode;
	int phy_link;
	int phy_speed;
	unsigned int phy_tx_pause;
	unsigned int phy_rx_pause;

	/* Timestamp support */
	spinlock_t tstamp_lock;
	struct ptp_clock_info ptp_clock_info;
	struct ptp_clock *ptp_clock;
	struct hwtstamp_config tstamp_config;
	struct cyclecounter tstamp_cc;
	struct timecounter tstamp_tc;
	unsigned int tstamp_addend;
	struct work_struct tx_tstamp_work;
	struct sk_buff *tx_tstamp_skb;
	u64 tx_tstamp;

	/* DCB support */
	struct ieee_ets *ets;
	struct ieee_pfc *pfc;
	unsigned int q2tc_map[DWC_ETH_MAX_QUEUES];
	unsigned int prio2q_map[IEEE_8021QAZ_MAX_TCS];
	u8 num_tcs;

	/* Device control parameters */
	unsigned int tx_max_buf_size;
	unsigned int rx_min_buf_size;
	unsigned int rx_buf_align;
	unsigned int tx_max_desc_nr;
	unsigned int skb_alloc_size;
	unsigned int tx_desc_max_proc;
	unsigned int tx_desc_min_free;
	unsigned int rx_desc_max_dirty;
	unsigned int dma_stop_timeout;
	unsigned int max_flow_control_queues;
	unsigned int max_dma_riwt;
	unsigned int tstamp_ssinc;
	unsigned int tstamp_snsinc;
	unsigned int sph_hdsms_size;

	char drv_name[32];
	char drv_ver[32];

#ifdef CONFIG_DEBUG_FS
	struct dentry *dwc_eth_debugfs;

	unsigned int debugfs_xlgmac_reg;
	unsigned int debugfs_xlgpcs_mmd;
	unsigned int debugfs_xlgpcs_reg;
#endif
};

void dwc_eth_ptp_register(struct dwc_eth_pdata *pdata);
void dwc_eth_ptp_unregister(struct dwc_eth_pdata *pdata);
void dwc_eth_init_desc_ops(struct dwc_eth_desc_ops *desc_ops);
void dwc_eth_init_hw_ops(struct dwc_eth_hw_ops *hw_ops);
const struct net_device_ops *dwc_eth_get_netdev_ops(void);
const struct ethtool_ops *dwc_eth_get_ethtool_ops(void);
#ifdef CONFIG_DWC_ETH_DCB
const struct dcbnl_rtnl_ops *dwc_eth_get_dcbnl_ops(void);
#endif
void dwc_eth_dump_tx_desc(struct dwc_eth_pdata *pdata,
			  struct dwc_eth_ring *ring,
				unsigned int idx,
				unsigned int count,
				unsigned int flag);
void dwc_eth_dump_rx_desc(struct dwc_eth_pdata *pdata,
			  struct dwc_eth_ring *ring,
				unsigned int idx);
void dwc_eth_print_pkt(struct net_device *netdev,
		       struct sk_buff *skb, bool tx_rx);
void dwc_eth_get_all_hw_features(struct dwc_eth_pdata *pdata);
void dwc_eth_print_all_hw_features(struct dwc_eth_pdata *pdata);
int dwc_eth_powerup(struct net_device *netdev, unsigned int caller);
int dwc_eth_powerdown(struct net_device *netdev, unsigned int caller);
int dwc_eth_mdio_register(struct dwc_eth_pdata *pdata);
void dwc_eth_mdio_unregister(struct dwc_eth_pdata *pdata);

#ifdef CONFIG_DEBUG_FS
void xlgmac_debugfs_init(struct dwc_eth_pdata *pdata);
void xlgmac_debugfs_exit(struct dwc_eth_pdata *pdata);
#else
static inline void xlgmac_debugfs_init(struct dwc_eth_pdata *pdata) {}
static inline void xlgmac_debugfs_exit(struct dwc_eth_pdata *pdata) {}
#endif

/* For debug prints */
#ifdef DWC_ETH_DEBUG
#define DBGPR(fmt, args...) \
	pr_alert("[%s,%d]:" fmt, __func__, __LINE__, ## args)
#define TRACE(fmt, args...) \
	pr_alert(fmt "%s\n", ## args, __func__)
#else
#define DBGPR(x...)		do { } while (0)
#define TRACE(fmt, args...)	do { } while (0)
#endif

#endif /* __DWC_ETH_H__ */
