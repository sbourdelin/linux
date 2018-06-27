/* SPDX-License-Identifier: GPL-2.0 */
/* Octeon III BGX Ethernet Driver
 *
 * Copyright (C) 2018 Cavium, Inc.
 */
#ifndef _OCTEON3_H_
#define _OCTEON3_H_

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>

#include <asm/octeon/octeon.h>

#include "octeon3-bgx.h"
#include "octeon3-pki.h"
#include "octeon3-pko.h"
#include "octeon3-sso.h"

#define MAX_CORES			48
#define MAX_NODES			2
#define NODE_MASK			(MAX_NODES - 1)
#define MAX_BGX_PER_NODE		6
#define MAX_LMAC_PER_BGX		4

#define IOBDMA_ORDERED_IO_ADDR		0xffffffffffffa200ull
#define LMTDMA_ORDERED_IO_ADDR		0xffffffffffffa400ull
#define SCRATCH_BASE_ADDR		0xffffffffffff8000ull

#define PKO_LMTLINE			2ull
#define LMTDMA_SCR_OFFSET		(PKO_LMTLINE * CVMX_CACHE_LINE_SIZE)
#define PKO_LMTDMA_SCRADDR_SHIFT	3

/* Registers are accessed via xkphys */
#define SET_XKPHYS			BIT_ULL(63)
#define NODE_OFFSET(node)		((node) * 0x1000000000ull)

/* DPI registers */
#define DPI_BASE			0x1df0000000000ull
#define DPI_ADDR(n)			(DPI_BASE + SET_XKPHYS + NODE_OFFSET(n))
#define DPI_CTL(n)			(DPI_ADDR(n) + 0x00040)

/* Gser register definitions */
#define GSER_BASE			0x1180090000000ull
#define GSER_ADDR(n, gser)		(GSER_BASE + SET_XKPHYS +	       \
					 NODE_OFFSET(n) + ((gser) << 24))
#define GSER_LANE_OFFSET(lane)		((lane) << 20)
#define GSER_LANE_OFFSET1(lane)		((lane) << 7)
#define GSER_LANE_OFFSET2(lane)		((lane) << 5)

#define GSER_LANE_ADDR(n, g, l)		(GSER_ADDR(n, g) + GSER_LANE_OFFSET(l))
#define GSER_LANE_ADDR1(n, g, l)	(GSER_ADDR(n, g) + GSER_LANE_OFFSET1(l))
#define GSER_LANE_ADDR2(n, g, l)	(GSER_ADDR(n, g) + GSER_LANE_OFFSET2(l))

#define GSER_PHY_CTL(n, g)		(GSER_LANE_ADDR(n, g, 0) + 0x000000)
#define GSER_CFG(n, g)			(GSER_LANE_ADDR(n, g, 0) + 0x000080)
#define GSER_LANE_MODE(n, g)		(GSER_LANE_ADDR(n, g, 0) + 0x000118)
#define GSER_RX_EIE_DETSTS(n, g)	(GSER_LANE_ADDR(n, g, 0) + 0x000150)
#define GSER_LANE_LBERT_CFG(n, g, l)	(GSER_LANE_ADDR(n, g, l) + 0x4c0020)
#define GSER_LANE_PCS_CTLIFC_0(n, g, l)	(GSER_LANE_ADDR(n, g, l) + 0x4c0060)
#define GSER_LANE_PCS_CTLIFC_2(n, g, l)	(GSER_LANE_ADDR(n, g, l) + 0x4c0070)
#define GSER_BR_RX_CTL(n, g, l)		(GSER_LANE_ADDR1(n, g, l) + 0x000400)
#define GSER_BR_RX_EER(n, g, l)		(GSER_LANE_ADDR1(n, g, l) + 0x000418)
#define GSER_LANE_P_MODE_1(n, g, m)	(GSER_LANE_ADDR2(n, g, m) + 0x4e0048)

/* Gser register bitfields */
#define GSER_PHY_CTL_PHY_RESET		BIT(1)
#define GSER_PHY_CTL_PHY_PD		BIT(0)
#define GSER_CFG_BGX			BIT(2)
#define GSER_LANE_MODE_LMODE_MASK	GENMASK_ULL(3, 0)
#define GSER_RX_EIE_DETSTS_CDRLCK_SHIFT	8
#define GSER_LANE_LBERT_CFG_LBERT_PM_EN	BIT(6)
#define GSER_LANE_PCS_CTLIFC_0_CFG_TX_COEFF_REQ_OVRRD_VAL	BIT(12)
#define GSER_LANE_PCS_CTLIFC_2_CTLIFC_OVRRD_REQ			BIT(15)
#define GSER_LANE_PCS_CTLIFC_2_CFG_TX_COEFF_REQ_OVRRD_EN	BIT(7)
#define GSER_BR_RX_CTL_RXT_EER		BIT(15)
#define GSER_BR_RX_CTL_RXT_ESV		BIT(14)
#define GSER_BR_RX_CTL_RXT_SWM		BIT(2)
#define GSER_BR_RX_EER_RXT_EER		BIT(15)
#define GSER_BR_RX_EER_RXT_ESV		BIT(14)
#define GSER_LANE_P_MODE_1_VMA_MM	BIT(14)

/* XCV register definitions */
#define XCV_BASE			0x11800db000000ull
#define XCV_ADDR(n)			(XCV_BASE + SET_XKPHYS + NODE_OFFSET(n))

#define XCV_RESET(n)			(XCV_ADDR(n) + 0x0000)
#define XCV_DLL_CTL(n)			(XCV_ADDR(n) + 0x0010)
#define XCV_COMP_CTL(n)			(XCV_ADDR(n) + 0x0020)
#define XCV_CTL(n)			(XCV_ADDR(n) + 0x0030)
#define XCV_INT(n)			(XCV_ADDR(n) + 0x0040)
#define XCV_INBND_STATUS(n)		(XCV_ADDR(n) + 0x0080)
#define XCV_BATCH_CRD_RET(n)		(XCV_ADDR(n) + 0x0100)

/* XCV register bitfields */
#define XCV_RESET_ENABLE		BIT(63)
#define XCV_RESET_CLKRST		BIT(15)
#define XCV_RESET_DLLRST		BIT(11)
#define XCV_RESET_COMP			BIT(7)
#define XCV_RESET_TX_PKT_RST_N		BIT(3)
#define XCV_RESET_TX_DAT_RST_N		BIT(2)
#define XCV_RESET_RX_PKT_RST_N		BIT(1)
#define XCV_RESET_RX_DAT_RST_N		BIT(0)
#define XCV_DLL_CTL_CLKRX_BYP		BIT(23)
#define XCV_DLL_CTL_CLKRX_SET_MASK	GENMASK_ULL(22, 16)
#define XCV_DLL_CTL_CLKTX_BYP		BIT(15)
#define XCV_DLL_CTL_REFCLK_SEL_MASK	GENMASK_ULL(1, 0)
#define XCV_COMP_CTL_DRV_BYP		BIT(63)
#define XCV_CTL_LPBK_INT		BIT(2)
#define XCV_CTL_SPEED_MASK		GENMASK_ULL(1, 0)
#define XCV_BATCH_CRD_RET_CRD_RET	BIT(0)

enum octeon3_mac_type {
	BGX_MAC,
	SRIO_MAC
};

enum octeon3_src_type {
	QLM,
	XCV
};

struct mac_platform_data {
	enum octeon3_mac_type mac_type;
	enum octeon3_src_type src_type;
	int interface;
	int numa_node;
	int port;
};

struct bgx_port_netdev_priv {
	struct bgx_port_priv *bgx_priv;
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
	union buf_ptr packet_ptr;
	union wqe_word4	word4;
	u64 wqe_data[11];
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
	int link;
	int duplex;
	int speed;
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
int octeon3_sso_alloc_groups(int node, int *groups, int cnt, int start);
void octeon3_sso_free_groups(int node, int *groups, int cnt);
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
