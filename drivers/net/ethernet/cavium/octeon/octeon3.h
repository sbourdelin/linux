/*
 * Copyright (c) 2017 Cavium, Inc.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#ifndef _OCTEON3_H_
#define _OCTEON3_H_

#include <linux/netdevice.h>
#include <linux/platform_device.h>

#define MAX_NODES			2
#define NODE_MASK			(MAX_NODES - 1)
#define MAX_BGX_PER_NODE		6
#define MAX_LMAC_PER_BGX		4

#define IOBDMA_ORDERED_IO_ADDR		0xffffffffffffa200ull
#define LMTDMA_ORDERED_IO_ADDR		0xffffffffffffa400ull

#define SCRATCH_BASE			0xffffffffffff8000ull
#define PKO_LMTLINE			2ull
#define LMTDMA_SCR_OFFSET		(PKO_LMTLINE * CVMX_CACHE_LINE_SIZE)

/* Pko sub-command three bit codes (SUBDC3) */
#define PKO_SENDSUBDC_GATHER		0x1

/* Pko sub-command four bit codes (SUBDC4) */
#define PKO_SENDSUBDC_TSO		0x8
#define PKO_SENDSUBDC_FREE		0x9
#define PKO_SENDSUBDC_WORK		0xa
#define PKO_SENDSUBDC_MEM		0xc
#define PKO_SENDSUBDC_EXT		0xd

#define BGX_RX_FIFO_SIZE		(64 * 1024)
#define BGX_TX_FIFO_SIZE		(32 * 1024)

/* Registers are accessed via xkphys */
#define SET_XKPHYS			BIT_ULL(63)
#define NODE_OFFSET(node)		((node) * 0x1000000000ull)

/* Bgx register definitions */
#define BGX_BASE			0x11800e0000000ull
#define BGX_OFFSET(bgx)			(BGX_BASE + ((bgx) << 24))
#define INDEX_OFFSET(index)		((index) << 20)
#define INDEX_ADDR(n, b, i)		(SET_XKPHYS + NODE_OFFSET(n) +	       \
					 BGX_OFFSET(b) + INDEX_OFFSET(i))
#define CAM_OFFSET(mac)			((mac) << 3)
#define CAM_ADDR(n, b, m)		(INDEX_ADDR(n, b, 0) + CAM_OFFSET(m))

#define BGX_CMR_CONFIG(n, b, i)		(INDEX_ADDR(n, b, i)	      + 0x00000)
#define BGX_CMR_GLOBAL_CONFIG(n, b)	(INDEX_ADDR(n, b, 0)	      + 0x00008)
#define BGX_CMR_RX_ID_MAP(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x00028)
#define BGX_CMR_RX_BP_ON(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x00088)
#define BGX_CMR_RX_ADR_CTL(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x000a0)
#define BGX_CMR_RX_FIFO_LEN(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x000c0)
#define BGX_CMR_RX_ADRX_CAM(n, b, m)	(CAM_ADDR(n, b, m)	      + 0x00100)
#define BGX_CMR_CHAN_MSK_AND(n, b)	(INDEX_ADDR(n, b, 0)	      + 0x00200)
#define BGX_CMR_CHAN_MSK_OR(n, b)	(INDEX_ADDR(n, b, 0)	      + 0x00208)
#define BGX_CMR_TX_FIFO_LEN(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x00418)
#define BGX_CMR_TX_LMACS(n, b)		(INDEX_ADDR(n, b, 0)	      + 0x01000)

#define BGX_SPU_CONTROL1(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10000)
#define BGX_SPU_STATUS1(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10008)
#define BGX_SPU_STATUS2(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10020)
#define BGX_SPU_BX_STATUS(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10028)
#define BGX_SPU_BR_STATUS1(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10030)
#define BGX_SPU_BR_STATUS2(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10038)
#define BGX_SPU_BR_BIP_ERR_CNT(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10058)
#define BGX_SPU_BR_PMD_CONTROL(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10068)
#define BGX_SPU_BR_PMD_LP_CUP(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10078)
#define BGX_SPU_BR_PMD_LD_CUP(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10088)
#define BGX_SPU_BR_PMD_LD_REP(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10090)
#define BGX_SPU_FEC_CONTROL(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x100a0)
#define BGX_SPU_AN_CONTROL(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x100c8)
#define BGX_SPU_AN_STATUS(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x100d0)
#define BGX_SPU_AN_ADV(n, b, i)		(INDEX_ADDR(n, b, i)	      + 0x100d8)
#define BGX_SPU_MISC_CONTROL(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x10218)
#define BGX_SPU_INT(n, b, i)		(INDEX_ADDR(n, b, i)	      + 0x10220)
#define BGX_SPU_DBG_CONTROL(n, b)	(INDEX_ADDR(n, b, 0)	      + 0x10300)

#define BGX_SMU_RX_INT(n, b, i)		(INDEX_ADDR(n, b, i)	      + 0x20000)
#define BGX_SMU_RX_FRM_CTL(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x20008)
#define BGX_SMU_RX_JABBER(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x20018)
#define BGX_SMU_RX_CTL(n, b, i)		(INDEX_ADDR(n, b, i)	      + 0x20030)
#define BGX_SMU_TX_APPEND(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x20100)
#define BGX_SMU_TX_MIN_PKT(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x20118)
#define BGX_SMU_TX_INT(n, b, i)		(INDEX_ADDR(n, b, i)	      + 0x20140)
#define BGX_SMU_TX_CTL(n, b, i)		(INDEX_ADDR(n, b, i)	      + 0x20160)
#define BGX_SMU_TX_THRESH(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x20168)
#define BGX_SMU_CTRL(n, b, i)		(INDEX_ADDR(n, b, i)	      + 0x20200)

#define BGX_GMP_PCS_MR_CONTROL(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x30000)
#define BGX_GMP_PCS_MR_STATUS(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x30008)
#define BGX_GMP_PCS_AN_ADV(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x30010)
#define BGX_GMP_PCS_LINK_TIMER(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x30040)
#define BGX_GMP_PCS_SGM_AN_ADV(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x30068)
#define BGX_GMP_PCS_MISC_CTL(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x30078)
#define BGX_GMP_GMI_PRT_CFG(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x38010)
#define BGX_GMP_GMI_RX_FRM_CTL(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x38018)
#define BGX_GMP_GMI_RX_JABBER(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x38038)
#define BGX_GMP_GMI_TX_THRESH(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x38210)
#define BGX_GMP_GMI_TX_APPEND(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x38218)
#define BGX_GMP_GMI_TX_SLOT(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x38220)
#define BGX_GMP_GMI_TX_BURST(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x38228)
#define BGX_GMP_GMI_TX_MIN_PKT(n, b, i)	(INDEX_ADDR(n, b, i)	      + 0x38240)
#define BGX_GMP_GMI_TX_SGMII_CTL(n, b, i) (INDEX_ADDR(n, b, i)	      + 0x38300)

/* XCV register definitions */
#define XCV_BASE			0x11800db000000ull
#define SET_XCV_BASE(node)		(SET_XKPHYS + NODE_OFFSET(node) +      \
					 XCV_BASE)
#define XCV_RESET(node)			(SET_XCV_BASE(node)	       + 0x0000)
#define XCV_DLL_CTL(node)		(SET_XCV_BASE(node)	       + 0x0010)
#define XCV_COMP_CTL(node)		(SET_XCV_BASE(node)	       + 0x0020)
#define XCV_CTL(node)			(SET_XCV_BASE(node)	       + 0x0030)
#define XCV_INT(node)			(SET_XCV_BASE(node)	       + 0x0040)
#define XCV_INBND_STATUS(node)		(SET_XCV_BASE(node)	       + 0x0080)
#define XCV_BATCH_CRD_RET(node)		(SET_XCV_BASE(node)	       + 0x0100)

/* Gser register definitions */
#define GSER_BASE			0x1180090000000ull
#define GSER_OFFSET(gser)		(GSER_BASE + ((gser) << 24))
#define GSER_LANE_OFFSET(lane)		((lane) << 20)
#define GSER_LANE_ADDR(n, g, l)		(SET_XKPHYS + NODE_OFFSET(n) +	       \
					 GSER_OFFSET(g) + GSER_LANE_OFFSET(l))
#define GSER_PHY_CTL(n, g)		(GSER_LANE_ADDR(n, g, 0)     + 0x000000)
#define GSER_CFG(n, g)			(GSER_LANE_ADDR(n, g, 0)     + 0x000080)
#define GSER_LANE_MODE(n, g)		(GSER_LANE_ADDR(n, g, 0)     + 0x000118)
#define GSER_RX_EIE_DETSTS(n, g)	(GSER_LANE_ADDR(n, g, 0)     + 0x000150)
#define GSER_LANE_LBERT_CFG(n, g, l)	(GSER_LANE_ADDR(n, g, l)     + 0x4c0020)
#define GSER_LANE_PCS_CTLIFC_0(n, g, l)	(GSER_LANE_ADDR(n, g, l)     + 0x4c0060)
#define GSER_LANE_PCS_CTLIFC_2(n, g, l)	(GSER_LANE_ADDR(n, g, l)     + 0x4c0070)

/* Odd gser registers */
#define GSER_LANE_OFFSET_1(lane)	((lane) << 7)
#define GSER_LANE_ADDR_1(n, g, l)	(SET_XKPHYS + NODE_OFFSET(n) +	       \
					 GSER_OFFSET(g) + GSER_LANE_OFFSET_1(l))

#define GSER_BR_RX_CTL(n, g, l)		(GSER_LANE_ADDR_1(n, g, l)   + 0x000400)
#define GSER_BR_RX_EER(n, g, l)		(GSER_LANE_ADDR_1(n, g, l)   + 0x000418)

#define GSER_LANE_OFFSET_2(mode)	((mode) << 5)
#define GSER_LANE_ADDR_2(n, g, m)	(SET_XKPHYS + NODE_OFFSET(n) +	       \
					 GSER_OFFSET(g) + GSER_LANE_OFFSET_2(m))

#define GSER_LANE_P_MODE_1(n, g, m)	(GSER_LANE_ADDR_2(n, g, m)   + 0x4e0048)

#define DPI_BASE			0x1df0000000000ull
#define DPI_ADDR(n)			(SET_XKPHYS + NODE_OFFSET(n) + DPI_BASE)
#define DPI_CTL(n)			(DPI_ADDR(n)                  + 0x00040)

enum octeon3_mac_type {
	BGX_MAC,
	SRIO_MAC
};

enum octeon3_src_type {
	QLM,
	XCV
};

struct mac_platform_data {
	enum octeon3_mac_type	mac_type;
	int			numa_node;
	int			interface;
	int			port;
	enum octeon3_src_type	src_type;
};

struct bgx_port_netdev_priv {
	struct bgx_port_priv *bgx_priv;
};

/* Remove this define to use these enums after the last cvmx code references are
 * gone.
 */
/* PKO_MEMDSZ_E */
enum pko_memdsz_e {
	MEMDSZ_B64 = 0,
	MEMDSZ_B32 = 1,
	MEMDSZ_B16 = 2,
	MEMDSZ_B8 = 3
};

/* PKO_MEMALG_E */
enum pko_memalg_e {
	MEMALG_SET = 0,
	MEMALG_SETTSTMP = 1,
	MEMALG_SETRSLT = 2,
	MEMALG_ADD = 8,
	MEMALG_SUB = 9,
	MEMALG_ADDLEN = 0xA,
	MEMALG_SUBLEN = 0xB,
	MEMALG_ADDMBUF = 0xC,
	MEMALG_SUBMBUF = 0xD
};

/* PKO_QUERY_RTN_S[DQSTATUS] */
enum pko_query_dqstatus {
	PKO_DQSTATUS_PASS = 0,
	PKO_DQSTATUS_BADSTATE = 0x8,
	PKO_DQSTATUS_NOFPABUF = 0x9,
	PKO_DQSTATUS_NOPKOBUF = 0xA,
	PKO_DQSTATUS_FAILRTNPTR = 0xB,
	PKO_DQSTATUS_ALREADY = 0xC,
	PKO_DQSTATUS_NOTCREATED = 0xD,
	PKO_DQSTATUS_NOTEMPTY = 0xE,
	PKO_DQSTATUS_SENDPKTDROP = 0xF
};

union wqe_word0 {
	u64 u64;
	struct {
		__BITFIELD_FIELD(u64 rsvd_0:4,
		__BITFIELD_FIELD(u64 aura:12,
		__BITFIELD_FIELD(u64 rsvd_1:1,
		__BITFIELD_FIELD(u64 apad:3,
		__BITFIELD_FIELD(u64 channel:12,
		__BITFIELD_FIELD(u64 bufs:8,
		__BITFIELD_FIELD(u64 style:8,
		__BITFIELD_FIELD(u64 rsvd_2:10,
		__BITFIELD_FIELD(u64 pknd:6,
		;)))))))))
	};
};

union wqe_word1 {
	u64 u64;
	struct {
		__BITFIELD_FIELD(u64 len:16,
		__BITFIELD_FIELD(u64 rsvd_0:2,
		__BITFIELD_FIELD(u64 rsvd_1:2,
		__BITFIELD_FIELD(u64 grp:10,
		__BITFIELD_FIELD(u64 tag_type:2,
		__BITFIELD_FIELD(u64 tag:32,
		;))))))
	};
};

union wqe_word2 {
	u64 u64;
	struct {
		__BITFIELD_FIELD(u64 software:1,
		__BITFIELD_FIELD(u64 lg_hdr_type:5,
		__BITFIELD_FIELD(u64 lf_hdr_type:5,
		__BITFIELD_FIELD(u64 le_hdr_type:5,
		__BITFIELD_FIELD(u64 ld_hdr_type:5,
		__BITFIELD_FIELD(u64 lc_hdr_type:5,
		__BITFIELD_FIELD(u64 lb_hdr_type:5,
		__BITFIELD_FIELD(u64 is_la_ether:1,
		__BITFIELD_FIELD(u64 rsvd_0:8,
		__BITFIELD_FIELD(u64 vlan_valid:1,
		__BITFIELD_FIELD(u64 vlan_stacked:1,
		__BITFIELD_FIELD(u64 stat_inc:1,
		__BITFIELD_FIELD(u64 pcam_flag4:1,
		__BITFIELD_FIELD(u64 pcam_flag3:1,
		__BITFIELD_FIELD(u64 pcam_flag2:1,
		__BITFIELD_FIELD(u64 pcam_flag1:1,
		__BITFIELD_FIELD(u64 is_frag:1,
		__BITFIELD_FIELD(u64 is_l3_bcast:1,
		__BITFIELD_FIELD(u64 is_l3_mcast:1,
		__BITFIELD_FIELD(u64 is_l2_bcast:1,
		__BITFIELD_FIELD(u64 is_l2_mcast:1,
		__BITFIELD_FIELD(u64 is_raw:1,
		__BITFIELD_FIELD(u64 err_level:3,
		__BITFIELD_FIELD(u64 err_code:8,
		;))))))))))))))))))))))))
	};
};

union buf_ptr {
	u64 u64;
	struct {
		__BITFIELD_FIELD(u64 size:16,
		__BITFIELD_FIELD(u64 packet_outside_wqe:1,
		__BITFIELD_FIELD(u64 rsvd0:5,
		__BITFIELD_FIELD(u64 addr:42,
		;))))
	};
};

union wqe_word4 {
	u64 u64;
	struct {
		__BITFIELD_FIELD(u64 ptr_vlan:8,
		__BITFIELD_FIELD(u64 ptr_layer_g:8,
		__BITFIELD_FIELD(u64 ptr_layer_f:8,
		__BITFIELD_FIELD(u64 ptr_layer_e:8,
		__BITFIELD_FIELD(u64 ptr_layer_d:8,
		__BITFIELD_FIELD(u64 ptr_layer_c:8,
		__BITFIELD_FIELD(u64 ptr_layer_b:8,
		__BITFIELD_FIELD(u64 ptr_layer_a:8,
		;))))))))
	};
};

struct wqe {
	union wqe_word0	word0;
	union wqe_word1	word1;
	union wqe_word2	word2;
	union buf_ptr	packet_ptr;
	union wqe_word4	word4;
	u64		wqe_data[11];
};

enum port_mode {
	PORT_MODE_DISABLED,
	PORT_MODE_SGMII,
	PORT_MODE_RGMII,
	PORT_MODE_XAUI,
	PORT_MODE_RXAUI,
	PORT_MODE_XLAUI,
	PORT_MODE_XFI,
	PORT_MODE_10G_KR,
	PORT_MODE_40G_KR4
};

enum lane_mode {
	R_25G_REFCLK100,
	R_5G_REFCLK100,
	R_8G_REFCLK100,
	R_125G_REFCLK15625_KX,
	R_3125G_REFCLK15625_XAUI,
	R_103125G_REFCLK15625_KR,
	R_125G_REFCLK15625_SGMII,
	R_5G_REFCLK15625_QSGMII,
	R_625G_REFCLK15625_RXAUI,
	R_25G_REFCLK125,
	R_5G_REFCLK125,
	R_8G_REFCLK125
};

struct port_status {
	int	link;
	int	duplex;
	int	speed;
};

static inline u64 oct_csr_read(u64 addr)
{
	return __raw_readq((void __iomem *)addr);
}

static inline void oct_csr_write(u64 data, u64 addr)
{
	__raw_writeq(data, (void __iomem *)addr);
}

extern int ilk0_lanes;
extern int ilk1_lanes;

void bgx_nexus_load(void);

int bgx_port_allocate_pknd(int node);
int bgx_port_get_pknd(int node, int bgx, int index);
enum port_mode bgx_port_get_mode(int node, int bgx, int index);
int bgx_port_get_qlm(int node, int bgx, int index);
void bgx_port_set_netdev(struct device *dev, struct net_device *netdev);
int bgx_port_enable(struct net_device *netdev);
int bgx_port_disable(struct net_device *netdev);
const u8 *bgx_port_get_mac(struct net_device *netdev);
void bgx_port_set_rx_filtering(struct net_device *netdev);
int bgx_port_change_mtu(struct net_device *netdev, int new_mtu);
int bgx_port_ethtool_get_link_ksettings(struct net_device *netdev,
					struct ethtool_link_ksettings *cmd);
int bgx_port_ethtool_get_settings(struct net_device *netdev,
				  struct ethtool_cmd *cmd);
int bgx_port_ethtool_set_settings(struct net_device *netdev,
				  struct ethtool_cmd *cmd);
int bgx_port_ethtool_nway_reset(struct net_device *netdev);
int bgx_port_do_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd);

void bgx_port_mix_assert_reset(struct net_device *netdev, int mix, bool v);

int octeon3_pki_vlan_init(int node);
int octeon3_pki_cluster_init(int node, struct platform_device *pdev);
int octeon3_pki_ltype_init(int node);
int octeon3_pki_enable(int node);
int octeon3_pki_port_init(int node, int aura, int grp, int skip, int mb_size,
			  int pknd, int num_rx_cxt);
int octeon3_pki_get_stats(int node, int pknd, u64 *packets, u64 *octets,
			  u64 *dropped);
int octeon3_pki_set_ptp_skip(int node, int pknd, int skip);
int octeon3_pki_port_shutdown(int node, int pknd);
void octeon3_pki_shutdown(int node);

void octeon3_sso_pass1_limit(int node, int grp);
int octeon3_sso_init(int node, int aura);
void octeon3_sso_shutdown(int node, int aura);
int octeon3_sso_alloc_grp(int node, int grp);
int octeon3_sso_alloc_grp_range(int node, int req_grp, int req_cnt,
				bool use_last_avail, int *grp);
void octeon3_sso_free_grp(int node, int grp);
void octeon3_sso_free_grp_range(int node, int *grp, int req_cnt);
void octeon3_sso_irq_set(int node, int grp, bool en);

int octeon3_pko_interface_init(int node, int interface, int index,
			       enum octeon3_mac_type mac_type, int ipd_port);
int octeon3_pko_activate_dq(int node, int dq, int cnt);
int octeon3_pko_get_fifo_size(int node, int interface, int index,
			      enum octeon3_mac_type mac_type);
int octeon3_pko_set_mac_options(int node, int interface, int index,
				enum octeon3_mac_type mac_type, bool fcs_en,
				bool pad_en, int fcs_sop_off);
int octeon3_pko_init_global(int node, int aura);
int octeon3_pko_interface_uninit(int node, const int *dq, int num_dq);
int octeon3_pko_exit_global(int node);

#endif /* _OCTEON3_H_ */
