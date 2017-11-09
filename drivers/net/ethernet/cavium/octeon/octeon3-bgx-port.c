// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2017 Cavium, Inc.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>

#include <asm/octeon/octeon.h>

#include "octeon3.h"

struct bgx_port_priv {
	int node;
	int bgx;
	int index; /* Port index on BGX block*/
	enum port_mode mode;
	int pknd;
	int qlm;
	const u8 *mac_addr;
	struct phy_device *phydev;
	struct device_node *phy_np;
	int phy_mode;
	bool mode_1000basex;
	bool bgx_as_phy;
	struct net_device *netdev;
	struct mutex lock;	/* Serializes delayed work */
	struct port_status (*get_link)(struct bgx_port_priv *priv);
	int (*set_link)(struct bgx_port_priv *priv, struct port_status status);
	struct port_status last_status;
	struct delayed_work dwork;
	bool work_queued;
};

/* lmac_pknd keeps track of the port kinds assigned to the lmacs */
static int lmac_pknd[MAX_NODES][MAX_BGX_PER_NODE][MAX_LMAC_PER_BGX];

static struct workqueue_struct *check_state_wq;
static DEFINE_MUTEX(check_state_wq_mutex);

int bgx_port_get_qlm(int node, int bgx, int index)
{
	u64	data;
	int	qlm = -1;

	if (OCTEON_IS_MODEL(OCTEON_CN78XX)) {
		if (bgx < 2) {
			data = oct_csr_read(BGX_CMR_GLOBAL_CONFIG(node, bgx));
			if (data & 1)
				qlm = bgx + 2;
			else
				qlm = bgx;
		} else {
			qlm = bgx + 2;
		}
	} else if (OCTEON_IS_MODEL(OCTEON_CN73XX)) {
		if (bgx < 2) {
			qlm = bgx + 2;
		} else {
			/* Ports on bgx2 can be connected to qlm5 or qlm6 */
			if (index < 2)
				qlm = 5;
			else
				qlm = 6;
		}
	} else if (OCTEON_IS_MODEL(OCTEON_CNF75XX)) {
		/* Ports on bgx0 can be connected to qlm4 or qlm5 */
		if (index < 2)
			qlm = 4;
		else
			qlm = 5;
	}

	return qlm;
}
EXPORT_SYMBOL(bgx_port_get_qlm);

/* Returns the mode of the bgx port */
enum port_mode bgx_port_get_mode(int node, int bgx, int index)
{
	enum port_mode	mode;
	u64		data;

	data = oct_csr_read(BGX_CMR_CONFIG(node, bgx, index));

	switch ((data >> 8) & 7) {
	case 0:
		mode = PORT_MODE_SGMII;
		break;
	case 1:
		mode = PORT_MODE_XAUI;
		break;
	case 2:
		mode = PORT_MODE_RXAUI;
		break;
	case 3:
		data = oct_csr_read(BGX_SPU_BR_PMD_CONTROL(node, bgx, index));
		/* The use of training differentiates 10G_KR from xfi */
		if (data & BIT(1))
			mode = PORT_MODE_10G_KR;
		else
			mode = PORT_MODE_XFI;
		break;
	case 4:
		data = oct_csr_read(BGX_SPU_BR_PMD_CONTROL(node, bgx, index));
		/* The use of training differentiates 40G_KR4 from xlaui */
		if (data & BIT(1))
			mode = PORT_MODE_40G_KR4;
		else
			mode = PORT_MODE_XLAUI;
		break;
	case 5:
		mode = PORT_MODE_RGMII;
		break;
	default:
		mode = PORT_MODE_DISABLED;
		break;
	}

	return mode;
}
EXPORT_SYMBOL(bgx_port_get_mode);

int bgx_port_allocate_pknd(int node)
{
	struct global_resource_tag	tag;
	char				buf[16];
	int				pknd;

	strncpy((char *)&tag.lo, "cvm_pknd", 8);
	snprintf(buf, 16, "_%d......", node);
	memcpy(&tag.hi, buf, 8);

	res_mgr_create_resource(tag, 64);
	pknd = res_mgr_alloc(tag, -1, false);
	if (pknd < 0) {
		pr_err("bgx-port: Failed to allocate pknd\n");
		return -ENODEV;
	}

	return pknd;
}
EXPORT_SYMBOL(bgx_port_allocate_pknd);

int bgx_port_get_pknd(int node, int bgx, int index)
{
	return lmac_pknd[node][bgx][index];
}
EXPORT_SYMBOL(bgx_port_get_pknd);

/* GSER-20075 */
static void bgx_port_gser_20075(struct bgx_port_priv	*priv,
				int			qlm,
				int			lane)
{
	u64	data;
	u64	addr;

	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X) &&
	    (lane == -1 || lane == 3)) {
		/* Enable software control */
		addr = GSER_BR_RX_CTL(priv->node, qlm, 3);
		data = oct_csr_read(addr);
		data |= BIT(2);
		oct_csr_write(data, addr);

		/* Clear the completion flag */
		addr = GSER_BR_RX_EER(priv->node, qlm, 3);
		data = oct_csr_read(addr);
		data &= ~BIT(14);
		oct_csr_write(data, addr);

		/* Initiate a new request on lane 2 */
		if (lane == 3) {
			addr = GSER_BR_RX_EER(priv->node, qlm, 2);
			data = oct_csr_read(addr);
			data |= BIT(15);
			oct_csr_write(data, addr);
		}
	}
}

static void bgx_common_init_pknd(struct bgx_port_priv *priv)
{
	u64	data;
	int	num_ports;

	/* Setup pkind */
	priv->pknd = bgx_port_allocate_pknd(priv->node);
	lmac_pknd[priv->node][priv->bgx][priv->index] = priv->pknd;
	data = oct_csr_read(BGX_CMR_RX_ID_MAP(priv->node, priv->bgx, priv->index));
	data &= ~GENMASK_ULL(7, 0);
	data |= priv->pknd;
	if (OCTEON_IS_MODEL(OCTEON_CN73XX)) {
		/* Change the default reassembly id (max allowed is 14) */
		data &= ~GENMASK_ULL(14, 8);
		data |= ((4 * priv->bgx) + 2 + priv->index) << 8;
	}
	oct_csr_write(data, BGX_CMR_RX_ID_MAP(priv->node, priv->bgx, priv->index));

	/* Set backpressure channel mask AND/OR registers */
	data = oct_csr_read(BGX_CMR_CHAN_MSK_AND(priv->node, priv->bgx));
	data |= 0xffff << (16 * priv->index);
	oct_csr_write(data, BGX_CMR_CHAN_MSK_AND(priv->node, priv->bgx));

	data = oct_csr_read(BGX_CMR_CHAN_MSK_OR(priv->node, priv->bgx));
	data |= 0xffff << (16 * priv->index);
	oct_csr_write(data, BGX_CMR_CHAN_MSK_OR(priv->node, priv->bgx));

	/* Rx back pressure watermark:
	 * Set to 1/4 of the available lmacs buffer (in multiple of 16 bytes)
	 */
	data = oct_csr_read(BGX_CMR_TX_LMACS(priv->node, priv->bgx));
	num_ports = data & 7;
	data = BGX_RX_FIFO_SIZE / (num_ports * 4 * 16);
	oct_csr_write(data, BGX_CMR_RX_BP_ON(priv->node, priv->bgx, priv->index));
}

static int bgx_xgmii_hardware_init(struct bgx_port_priv *priv)
{
	u64	clock_mhz;
	u64	data;
	u64	ctl;

	/* Set TX Threshold */
	data = 0x20;
	oct_csr_write(data, BGX_GMP_GMI_TX_THRESH(priv->node, priv->bgx, priv->index));

	data = oct_csr_read(BGX_GMP_PCS_MISC_CTL(priv->node, priv->bgx, priv->index));
	data &= ~(BIT(8) | BIT(9));
	if (priv->mode_1000basex)
		data |= BIT(8);
	if (priv->bgx_as_phy)
		data |= BIT(9);
	oct_csr_write(data, BGX_GMP_PCS_MISC_CTL(priv->node, priv->bgx, priv->index));

	data = oct_csr_read(BGX_GMP_PCS_LINK_TIMER(priv->node, priv->bgx, priv->index));
	clock_mhz = octeon_get_io_clock_rate() / 1000000;
	if (priv->mode_1000basex)
		data = (10000ull * clock_mhz) >> 10;
	else
		data = (1600ull * clock_mhz) >> 10;
	oct_csr_write(data, BGX_GMP_PCS_LINK_TIMER(priv->node, priv->bgx, priv->index));

	if (priv->mode_1000basex) {
		data = oct_csr_read(BGX_GMP_PCS_AN_ADV(priv->node, priv->bgx, priv->index));
		data &= ~(GENMASK_ULL(13, 12) | GENMASK_ULL(8, 7));
		data |= 3 << 7;
		data |= BIT(6) | BIT(5);
		oct_csr_write(data, BGX_GMP_PCS_AN_ADV(priv->node, priv->bgx, priv->index));
	} else if (priv->bgx_as_phy) {
		data = oct_csr_read(BGX_GMP_PCS_SGM_AN_ADV(priv->node, priv->bgx, priv->index));
		data |= BIT(12);
		data &= ~(GENMASK_ULL(11, 10));
		data |= 2 << 10;
		oct_csr_write(data, BGX_GMP_PCS_SGM_AN_ADV(priv->node, priv->bgx, priv->index));
	}

	data = oct_csr_read(BGX_GMP_GMI_TX_APPEND(priv->node, priv->bgx, priv->index));
	ctl = oct_csr_read(BGX_GMP_GMI_TX_SGMII_CTL(priv->node, priv->bgx, priv->index));
	ctl &= ~BIT(0);
	ctl |= (data & BIT(0)) ? 0 : 1;
	oct_csr_write(ctl, BGX_GMP_GMI_TX_SGMII_CTL(priv->node, priv->bgx, priv->index));

	if (priv->mode == PORT_MODE_RGMII) {
		/* Disable XCV interface when initialized */
		data = oct_csr_read(XCV_RESET(priv->node));
		data &= ~(BIT(63) | BIT(3) | BIT(1));
		oct_csr_write(data, XCV_RESET(priv->node));
	}

	return 0;
}

int bgx_get_tx_fifo_size(struct bgx_port_priv *priv)
{
	u64	data;
	int	num_ports;

	data = oct_csr_read(BGX_CMR_TX_LMACS(priv->node, priv->bgx));
	num_ports = data & 7;

	switch (num_ports) {
	case 1:
		return BGX_TX_FIFO_SIZE;
	case 2:
		return BGX_TX_FIFO_SIZE / 2;
	case 3:
	case 4:
		return BGX_TX_FIFO_SIZE / 4;
	default:
		return 0;
	}
}

static int bgx_xaui_hardware_init(struct bgx_port_priv *priv)
{
	u64	data;
	u64	clock_mhz;
	u64	tx_fifo_size;

	if (octeon_is_simulation()) {
		/* Enable the port */
		data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
		data |= BIT(15);
		oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
	} else {
		/* Reset the port */
		data = oct_csr_read(BGX_SPU_CONTROL1(priv->node, priv->bgx, priv->index));
		data |= BIT(15);
		oct_csr_write(data, BGX_SPU_CONTROL1(priv->node, priv->bgx, priv->index));

		/* Wait for reset to complete */
		udelay(1);
		data = oct_csr_read(BGX_SPU_CONTROL1(priv->node, priv->bgx, priv->index));
		if (data & BIT(15)) {
			netdev_err(priv->netdev,
				   "BGX%d:%d: SPU stuck in reset\n", priv->bgx, priv->node);
			return -1;
		}

		/* Reset the SerDes lanes */
		data = oct_csr_read(BGX_SPU_CONTROL1(priv->node, priv->bgx, priv->index));
		data |= BIT(11);
		oct_csr_write(data, BGX_SPU_CONTROL1(priv->node, priv->bgx, priv->index));

		/* Disable packet reception */
		data = oct_csr_read(BGX_SPU_MISC_CONTROL(priv->node, priv->bgx, priv->index));
		data |= BIT(12);
		oct_csr_write(data, BGX_SPU_MISC_CONTROL(priv->node, priv->bgx, priv->index));

		/* Clear/disable interrupts */
		data = oct_csr_read(BGX_SMU_RX_INT(priv->node, priv->bgx, priv->index));
		oct_csr_write(data, BGX_SMU_RX_INT(priv->node, priv->bgx, priv->index));
		data = oct_csr_read(BGX_SMU_TX_INT(priv->node, priv->bgx, priv->index));
		oct_csr_write(data, BGX_SMU_TX_INT(priv->node, priv->bgx, priv->index));
		data = oct_csr_read(BGX_SPU_INT(priv->node, priv->bgx, priv->index));
		oct_csr_write(data, BGX_SPU_INT(priv->node, priv->bgx, priv->index));

		if ((priv->mode == PORT_MODE_10G_KR ||
		     priv->mode == PORT_MODE_40G_KR4) &&
		    !OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X)) {
			oct_csr_write(0, BGX_SPU_BR_PMD_LP_CUP(priv->node, priv->bgx, priv->index));
			oct_csr_write(0, BGX_SPU_BR_PMD_LD_CUP(priv->node, priv->bgx, priv->index));
			oct_csr_write(0, BGX_SPU_BR_PMD_LD_REP(priv->node, priv->bgx, priv->index));
			data = oct_csr_read(BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
			data |= BIT(1);
			oct_csr_write(data, BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
		}
	}

	data = oct_csr_read(BGX_SMU_TX_APPEND(priv->node, priv->bgx, priv->index));
	data |= BIT(3);
	oct_csr_write(data, BGX_SMU_TX_APPEND(priv->node, priv->bgx, priv->index));

	if (!octeon_is_simulation()) {
		/* Disable fec */
		data = oct_csr_read(BGX_SPU_FEC_CONTROL(priv->node, priv->bgx, priv->index));
		data &= ~BIT(0);
		oct_csr_write(data, BGX_SPU_FEC_CONTROL(priv->node, priv->bgx, priv->index));

		/* Disable/configure auto negotiation */
		data = oct_csr_read(BGX_SPU_AN_CONTROL(priv->node, priv->bgx, priv->index));
		data &= ~(BIT(13) | BIT(12));
		oct_csr_write(data, BGX_SPU_AN_CONTROL(priv->node, priv->bgx, priv->index));

		data = oct_csr_read(BGX_SPU_AN_ADV(priv->node, priv->bgx, priv->index));
		data &= ~(BIT(47) | BIT(26) | BIT(25) | BIT(22) | BIT(21) |
			  BIT(13) | BIT(12));
		data |= BIT(46);
		if (priv->mode == PORT_MODE_40G_KR4)
			data |= BIT(24);
		else
			data &= ~BIT(24);
		if (priv->mode == PORT_MODE_10G_KR)
			data |= BIT(23);
		else
			data &= ~BIT(23);
		oct_csr_write(data, BGX_SPU_AN_ADV(priv->node, priv->bgx, priv->index));

		data = oct_csr_read(BGX_SPU_DBG_CONTROL(priv->node, priv->bgx));
		data |= BIT(29);
		if (priv->mode == PORT_MODE_10G_KR ||
		    priv->mode == PORT_MODE_40G_KR4)
			data |= BIT(18);
		else
			data &= ~BIT(18);
		oct_csr_write(data, BGX_SPU_DBG_CONTROL(priv->node, priv->bgx));

		/* Enable the port */
		data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
		data |= BIT(15);
		oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));

		if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X) && priv->index) {
			/* BGX-22429 */
			data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, 0));
			data |= BIT(15);
			oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, 0));
		}
	}

	data = oct_csr_read(BGX_SPU_CONTROL1(priv->node, priv->bgx, priv->index));
	data &= ~BIT(11);
	oct_csr_write(data, BGX_SPU_CONTROL1(priv->node, priv->bgx, priv->index));

	data = oct_csr_read(BGX_SMU_TX_CTL(priv->node, priv->bgx, priv->index));
	data |= BIT(0);
	data &= ~BIT(1);
	oct_csr_write(data, BGX_SMU_TX_CTL(priv->node, priv->bgx, priv->index));

	clock_mhz = octeon_get_io_clock_rate() / 1000000;
	data = oct_csr_read(BGX_SPU_DBG_CONTROL(priv->node, priv->bgx));
	data &= ~GENMASK_ULL(43, 32);
	data |= (clock_mhz - 1) << 32;
	oct_csr_write(data, BGX_SPU_DBG_CONTROL(priv->node, priv->bgx));

	/* Fifo in 16-byte words */
	tx_fifo_size = bgx_get_tx_fifo_size(priv);
	tx_fifo_size >>= 4;
	oct_csr_write(tx_fifo_size - 10, BGX_SMU_TX_THRESH(priv->node, priv->bgx, priv->index));

	if (priv->mode == PORT_MODE_RXAUI && priv->phy_np) {
		data = oct_csr_read(BGX_SPU_MISC_CONTROL(priv->node, priv->bgx, priv->index));
		data |= BIT(10);
		oct_csr_write(data, BGX_SPU_MISC_CONTROL(priv->node, priv->bgx, priv->index));
	}

	/* Some PHYs take up to 250ms to stabilize */
	if (!octeon_is_simulation())
		usleep_range(250000, 300000);

	return 0;
}

/* Configure/initialize a bgx port. */
static int bgx_port_init(struct bgx_port_priv *priv)
{
	u64	data;
	int	rc = 0;

	/* GSER-20956 */
	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X) &&
	    (priv->mode == PORT_MODE_10G_KR ||
	     priv->mode == PORT_MODE_XFI ||
	     priv->mode == PORT_MODE_40G_KR4 ||
	     priv->mode == PORT_MODE_XLAUI)) {
		/* Disable link training */
		data = oct_csr_read(BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
		data &= ~(1 << 1);
		oct_csr_write(data, BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
	}

	bgx_common_init_pknd(priv);

	if (priv->mode == PORT_MODE_SGMII ||
	    priv->mode == PORT_MODE_RGMII)
		rc = bgx_xgmii_hardware_init(priv);
	else
		rc = bgx_xaui_hardware_init(priv);

	return rc;
}

static int bgx_port_get_qlm_speed(struct bgx_port_priv	*priv,
				  int			qlm)
{
	enum lane_mode	lmode;
	u64		data;

	data = oct_csr_read(GSER_LANE_MODE(priv->node, qlm));
	lmode = data & 0xf;

	switch (lmode) {
	case R_25G_REFCLK100:
		return 2500;
	case R_5G_REFCLK100:
		return 5000;
	case R_8G_REFCLK100:
		return 8000;
	case R_125G_REFCLK15625_KX:
		return 1250;
	case R_3125G_REFCLK15625_XAUI:
		return 3125;
	case R_103125G_REFCLK15625_KR:
		return 10312;
	case R_125G_REFCLK15625_SGMII:
		return 1250;
	case R_5G_REFCLK15625_QSGMII:
		return 5000;
	case R_625G_REFCLK15625_RXAUI:
		return 6250;
	case R_25G_REFCLK125:
		return 2500;
	case R_5G_REFCLK125:
		return 5000;
	case R_8G_REFCLK125:
		return 8000;
	default:
		return 0;
	}
}

static struct port_status bgx_port_get_sgmii_link(struct bgx_port_priv *priv)
{
	struct port_status	status;
	int			speed;

	/* The simulator always uses a 1Gbps full duplex port */
	if (octeon_is_simulation()) {
		status.link = 1;
		status.duplex = DUPLEX_FULL;
		status.speed = 1000;
	} else {
		/* Use the qlm speed */
		speed = bgx_port_get_qlm_speed(priv, priv->qlm);
		status.link = 1;
		status.duplex = DUPLEX_FULL;
		status.speed = speed * 8 / 10;
	}

	return status;
}

static int bgx_port_xgmii_set_link_up(struct bgx_port_priv *priv)
{
	u64	data;
	int	timeout;

	if (!octeon_is_simulation()) {
		/* PCS reset sequence */
		data = oct_csr_read(BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));
		data |= BIT(15);
		oct_csr_write(data, BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));

		/* Wait for reset to complete */
		udelay(1);
		data = oct_csr_read(BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));
		if (data & BIT(15)) {
			netdev_err(priv->netdev,
				   "BGX%d:%d: PCS stuck in reset\n", priv->bgx, priv->node);
			return -1;
		}
	}

	/* Autonegotiation */
	if (priv->phy_np) {
		data = oct_csr_read(BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));
		data |= BIT(9);
		if (priv->mode != PORT_MODE_RGMII)
			data |= BIT(12);
		else
			data &= ~BIT(12);
		data &= ~BIT(11);
		oct_csr_write(data, BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));
	} else {
		data = oct_csr_read(BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));
		data |= BIT(6);
		data &= ~(BIT(13) | BIT(12) | BIT(11));
		oct_csr_write(data, BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));
	}

	data = oct_csr_read(BGX_GMP_PCS_MISC_CTL(priv->node, priv->bgx, priv->index));
	data &= ~(BIT(9) | BIT(8));
	if (priv->mode_1000basex)
		data |= BIT(8);
	if (priv->bgx_as_phy)
		data |= BIT(9);
	oct_csr_write(data, BGX_GMP_PCS_MISC_CTL(priv->node, priv->bgx, priv->index));

	/* Wait for autonegotiation to complete */
	if (!octeon_is_simulation() && !priv->bgx_as_phy &&
	    priv->mode != PORT_MODE_RGMII) {
		timeout = 10000;
		do {
			data = oct_csr_read(BGX_GMP_PCS_MR_STATUS(priv->node, priv->bgx, priv->index));
			if (data & BIT(5))
				break;
			timeout--;
			udelay(1);
		} while (timeout);
		if (!timeout) {
			netdev_err(priv->netdev, "BGX%d:%d: AN timeout\n", priv->bgx, priv->node);
			return -1;
		}
	}

	return 0;
}

static void bgx_port_rgmii_set_link_down(struct bgx_port_priv *priv)
{
	u64	data;
	int	rx_fifo_len;

	data = oct_csr_read(XCV_RESET(priv->node));
	data &= ~BIT(1);
	oct_csr_write(data, XCV_RESET(priv->node));
	/* Is this read really needed? TODO */
	data = oct_csr_read(XCV_RESET(priv->node));

	/* Wait for 2 MTUs */
	mdelay(10);

	data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
	data &= ~BIT(14);
	oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));

	/* Wait for the rx and tx fifos to drain */
	do {
		data = oct_csr_read(BGX_CMR_RX_FIFO_LEN(priv->node, priv->bgx, priv->index));
		rx_fifo_len = data & 0x1fff;
		data = oct_csr_read(BGX_CMR_TX_FIFO_LEN(priv->node, priv->bgx, priv->index));
	} while (rx_fifo_len > 0 || !(data & BIT(13)));

	data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
	data &= ~BIT(13);
	oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));

	data = oct_csr_read(XCV_RESET(priv->node));
	data &= ~BIT(3);
	oct_csr_write(data, XCV_RESET(priv->node));

	data = oct_csr_read(BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));
	data |= BIT(11);
	oct_csr_write(data, BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));
}

static void bgx_port_sgmii_set_link_down(struct bgx_port_priv *priv)
{
	u64	data;

	data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
	data &= ~(BIT(14) | BIT(13));
	oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));

	data = oct_csr_read(BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));
	data &= ~BIT(12);
	oct_csr_write(data, BGX_GMP_PCS_MR_CONTROL(priv->node, priv->bgx, priv->index));

	data = oct_csr_read(BGX_GMP_PCS_MISC_CTL(priv->node, priv->bgx, priv->index));
	data |= BIT(11);
	oct_csr_write(data, BGX_GMP_PCS_MISC_CTL(priv->node, priv->bgx, priv->index));
	data = oct_csr_read(BGX_GMP_PCS_MISC_CTL(priv->node, priv->bgx, priv->index));
}

static int bgx_port_sgmii_set_link_speed(struct bgx_port_priv *priv, struct port_status status)
{
	u64	data;
	u64	prtx;
	u64	miscx;
	int	timeout;

	data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
	data &= ~(BIT(14) | BIT(13));
	oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));

	timeout = 10000;
	do {
		prtx = oct_csr_read(BGX_GMP_GMI_PRT_CFG(priv->node, priv->bgx, priv->index));
		if (prtx & BIT(13) && prtx & BIT(12))
			break;
		timeout--;
		udelay(1);
	} while (timeout);
	if (!timeout) {
		netdev_err(priv->netdev, "BGX%d:%d: GMP idle timeout\n", priv->bgx, priv->node);
		return -1;
	}

	prtx = oct_csr_read(BGX_GMP_GMI_PRT_CFG(priv->node, priv->bgx, priv->index));
	miscx = oct_csr_read(BGX_GMP_PCS_MISC_CTL(priv->node, priv->bgx, priv->index));
	if (status.link) {
		miscx &= ~BIT(11);
		if (status.duplex == DUPLEX_FULL)
			prtx |= BIT(2);
		else
			prtx &= ~BIT(2);
	} else {
		miscx |= BIT(11);
	}

	switch (status.speed) {
	case 10:
		prtx &= ~(BIT(3) | BIT(1));
		prtx |= BIT(8);
		miscx &= ~GENMASK_ULL(6, 0);
		miscx |= 25;
		oct_csr_write(64, BGX_GMP_GMI_TX_SLOT(priv->node, priv->bgx, priv->index));
		oct_csr_write(0, BGX_GMP_GMI_TX_BURST(priv->node, priv->bgx, priv->index));
		break;
	case 100:
		prtx &= ~(BIT(8) | BIT(3) | BIT(1));
		miscx &= ~GENMASK_ULL(6, 0);
		miscx |= 5;
		oct_csr_write(64, BGX_GMP_GMI_TX_SLOT(priv->node, priv->bgx, priv->index));
		oct_csr_write(0, BGX_GMP_GMI_TX_BURST(priv->node, priv->bgx, priv->index));
		break;
	case 1000:
		prtx |= (BIT(3) | BIT(1));
		prtx &= ~BIT(8);
		miscx &= ~GENMASK_ULL(6, 0);
		miscx |= 1;
		oct_csr_write(512, BGX_GMP_GMI_TX_SLOT(priv->node, priv->bgx, priv->index));
		if (status.duplex == DUPLEX_FULL)
			oct_csr_write(0, BGX_GMP_GMI_TX_BURST(priv->node, priv->bgx, priv->index));
		else
			oct_csr_write(8192, BGX_GMP_GMI_TX_BURST(priv->node, priv->bgx, priv->index));
		break;
	default:
		break;
	}

	oct_csr_write(miscx, BGX_GMP_PCS_MISC_CTL(priv->node, priv->bgx, priv->index));
	oct_csr_write(prtx, BGX_GMP_GMI_PRT_CFG(priv->node, priv->bgx, priv->index));
	/* This read verifies the write completed */
	prtx = oct_csr_read(BGX_GMP_GMI_PRT_CFG(priv->node, priv->bgx, priv->index));

	data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
	data |= (BIT(14) | BIT(13));
	oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));

	return 0;
}

static int bgx_port_rgmii_set_link_speed(struct bgx_port_priv *priv, struct port_status status)
{
	u64	data;
	int	speed;
	bool	speed_changed = false;
	bool	int_lpbk = false;
	bool	do_credits;

	switch (status.speed) {
	case 10:
		speed = 0;
		break;
	case 100:
		speed = 1;
		break;
	case 1000:
	default:
		speed = 2;
		break;
	}

	/* Do credits if link came up */
	data = oct_csr_read(XCV_RESET(priv->node));
	do_credits = status.link && !(data & BIT(63));

	/* Was there a speed change */
	data = oct_csr_read(XCV_CTL(priv->node));
	if ((data & GENMASK_ULL(1, 0)) != speed)
		speed_changed = true;

	/* Clear clkrst when in internal loopback */
	if (data & BIT(2)) {
		int_lpbk = true;
		data = oct_csr_read(XCV_RESET(priv->node));
		data &= ~BIT(15);
		oct_csr_write(data, XCV_RESET(priv->node));
	}

	/* Link came up or there was a speed change */
	data = oct_csr_read(XCV_RESET(priv->node));
	if (status.link && (!(data & BIT(63)) || speed_changed)) {
		data |= BIT(63);
		oct_csr_write(data, XCV_RESET(priv->node));

		data = oct_csr_read(XCV_CTL(priv->node));
		data &= ~GENMASK_ULL(1, 0);
		data |= speed;
		oct_csr_write(data, XCV_CTL(priv->node));

		data = oct_csr_read(XCV_DLL_CTL(priv->node));
		data |= BIT(23);
		data &= ~GENMASK_ULL(22, 16);
		data &= ~BIT(15);
		oct_csr_write(data, XCV_DLL_CTL(priv->node));

		data = oct_csr_read(XCV_DLL_CTL(priv->node));
		data &= ~GENMASK_ULL(1, 0);
		oct_csr_write(data, XCV_DLL_CTL(priv->node));

		data = oct_csr_read(XCV_RESET(priv->node));
		data &= ~BIT(11);
		oct_csr_write(data, XCV_RESET(priv->node));

		usleep_range(10, 100);

		data = oct_csr_read(XCV_COMP_CTL(priv->node));
		data &= ~BIT(63);
		oct_csr_write(data, XCV_COMP_CTL(priv->node));

		data = oct_csr_read(XCV_RESET(priv->node));
		data |= BIT(7);
		oct_csr_write(data, XCV_RESET(priv->node));

		data = oct_csr_read(XCV_RESET(priv->node));
		if (int_lpbk)
			data &= ~BIT(15);
		else
			data |= BIT(15);
		oct_csr_write(data, XCV_RESET(priv->node));

		data = oct_csr_read(XCV_RESET(priv->node));
		data |= BIT(2) | BIT(0);
		oct_csr_write(data, XCV_RESET(priv->node));
	}

	data = oct_csr_read(XCV_RESET(priv->node));
	if (status.link)
		data |= BIT(3) | BIT(1);
	else
		data &= ~(BIT(3) | BIT(1));
	oct_csr_write(data, XCV_RESET(priv->node));

	if (!status.link) {
		mdelay(10);
		oct_csr_write(0, XCV_RESET(priv->node));
	}

	/* Grant pko tx credits */
	if (do_credits) {
		data = oct_csr_read(XCV_BATCH_CRD_RET(priv->node));
		data |= BIT(0);
		oct_csr_write(data, XCV_BATCH_CRD_RET(priv->node));
	}

	return 0;
}

static int bgx_port_set_xgmii_link(struct bgx_port_priv *priv,
				   struct port_status status)
{
	u64	data;
	int	rc = 0;

	if (status.link) {
		/* Link up */
		data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
		data |= BIT(15);
		oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));

		/* BGX-22429 */
		if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X) && priv->index) {
			data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, 0));
			data |= BIT(15);
			oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, 0));
		}

		rc = bgx_port_xgmii_set_link_up(priv);
		if (rc)
			return rc;
		rc = bgx_port_sgmii_set_link_speed(priv, status);
		if (rc)
			return rc;
		if (priv->mode == PORT_MODE_RGMII)
			rc = bgx_port_rgmii_set_link_speed(priv, status);
	} else {
		/* Link down */
		if (priv->mode == PORT_MODE_RGMII) {
			bgx_port_rgmii_set_link_down(priv);
			rc = bgx_port_sgmii_set_link_speed(priv, status);
			if (rc)
				return rc;
			rc = bgx_port_rgmii_set_link_speed(priv, status);
		} else {
			bgx_port_sgmii_set_link_down(priv);
		}
	}

	return rc;
}

static struct port_status bgx_port_get_xaui_link(struct bgx_port_priv *priv)
{
	struct port_status	status;
	int			speed;
	int			lanes;
	u64			data;

	status.link = 0;
	status.duplex = DUPLEX_HALF;
	status.speed = 0;

	/* Get the link state */
	data = oct_csr_read(BGX_SMU_TX_CTL(priv->node, priv->bgx, priv->index));
	data &= GENMASK_ULL(5, 4);
	if (!data) {
		data = oct_csr_read(BGX_SMU_RX_CTL(priv->node, priv->bgx, priv->index));
		data &= GENMASK_ULL(1, 0);
		if (!data) {
			data = oct_csr_read(BGX_SPU_STATUS1(priv->node, priv->bgx, priv->index));
			if (data & BIT(2))
				status.link = 1;
		}
	}

	if (status.link) {
		/* Always full duplex */
		status.duplex = DUPLEX_FULL;

		/* Speed */
		speed = bgx_port_get_qlm_speed(priv, priv->qlm);
		data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
		switch ((data >> 8) & 7) {
		default:
		case 1:
			speed = (speed * 8 + 5) / 10;
			lanes = 4;
			break;
		case 2:
			speed = (speed * 8 + 5) / 10;
			lanes = 2;
			break;
		case 3:
			speed = (speed * 64 + 33) / 66;
			lanes = 1;
			break;
		case 4:
			if (speed == 6250)
				speed = 6445;
			speed = (speed * 64 + 33) / 66;
			lanes = 4;
			break;
		}

		speed *= lanes;
		status.speed = speed;
	}

	return status;
}

static int bgx_port_init_xaui_an(struct bgx_port_priv *priv)
{
	u64	data;

	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X)) {
		data = oct_csr_read(BGX_SPU_INT(priv->node, priv->bgx, priv->index));
		/* If autonegotiation is no good */
		if (!(data & BIT(11))) {
			data = BIT(12) | BIT(11) | BIT(10);
			oct_csr_write(data, BGX_SPU_INT(priv->node, priv->bgx, priv->index));

			data = oct_csr_read(BGX_SPU_AN_CONTROL(priv->node, priv->bgx, priv->index));
			data |= BIT(9);
			oct_csr_write(data, BGX_SPU_AN_CONTROL(priv->node, priv->bgx, priv->index));
			return -1;
		}
	} else {
		data = oct_csr_read(BGX_SPU_AN_STATUS(priv->node, priv->bgx, priv->index));
		/* If autonegotiation hasn't completed */
		if (!(data & BIT(5))) {
			data = oct_csr_read(BGX_SPU_AN_CONTROL(priv->node, priv->bgx, priv->index));
			data |= BIT(9);
			oct_csr_write(data, BGX_SPU_AN_CONTROL(priv->node, priv->bgx, priv->index));
			return -1;
		}
	}

	return 0;
}

static void bgx_port_xaui_start_training(struct bgx_port_priv *priv)
{
	u64	data;

	data = BIT(14) | BIT(13);
	oct_csr_write(data, BGX_SPU_INT(priv->node, priv->bgx, priv->index));

	/* BGX-20968 */
	oct_csr_write(0, BGX_SPU_BR_PMD_LP_CUP(priv->node, priv->bgx, priv->index));
	oct_csr_write(0, BGX_SPU_BR_PMD_LD_CUP(priv->node, priv->bgx, priv->index));
	oct_csr_write(0, BGX_SPU_BR_PMD_LD_REP(priv->node, priv->bgx, priv->index));
	data = oct_csr_read(BGX_SPU_AN_CONTROL(priv->node, priv->bgx, priv->index));
	data &= ~BIT(12);
	oct_csr_write(data, BGX_SPU_AN_CONTROL(priv->node, priv->bgx, priv->index));
	udelay(1);

	data = oct_csr_read(BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
	data |= BIT(1);
	oct_csr_write(data, BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
	udelay(1);

	data = oct_csr_read(BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
	data |= BIT(0);
	oct_csr_write(data, BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
}

static int bgx_port_gser_27882(struct bgx_port_priv *priv)
{
	u64	data;
	u64	addr;
	int	timeout;

	timeout = 200;
	do {
		data = oct_csr_read(GSER_RX_EIE_DETSTS(priv->node, priv->qlm));
		if (data & (1 << (priv->index + 8)))
			break;
		timeout--;
		udelay(1);
	} while (timeout);
	if (!timeout)
		return -1;

	addr = GSER_LANE_PCS_CTLIFC_0(priv->node, priv->qlm, priv->index);
	data = oct_csr_read(addr);
	data |= BIT(12);
	oct_csr_write(data, addr);

	addr = GSER_LANE_PCS_CTLIFC_2(priv->node, priv->qlm, priv->index);
	data = oct_csr_read(addr);
	data |= BIT(7);
	oct_csr_write(data, addr);

	data = oct_csr_read(addr);
	data |= BIT(15);
	oct_csr_write(data, addr);

	data = oct_csr_read(addr);
	data &= ~BIT(7);
	oct_csr_write(data, addr);

	data = oct_csr_read(addr);
	data |= BIT(15);
	oct_csr_write(data, addr);

	return 0;
}

static void bgx_port_xaui_restart_training(struct bgx_port_priv *priv)
{
	u64	data;

	data = BIT(14) | BIT(13);
	oct_csr_write(data, BGX_SPU_INT(priv->node, priv->bgx, priv->index));
	usleep_range(1700, 2000);

	/* BGX-20968 */
	oct_csr_write(0, BGX_SPU_BR_PMD_LP_CUP(priv->node, priv->bgx, priv->index));
	oct_csr_write(0, BGX_SPU_BR_PMD_LD_CUP(priv->node, priv->bgx, priv->index));
	oct_csr_write(0, BGX_SPU_BR_PMD_LD_REP(priv->node, priv->bgx, priv->index));

	/* Restart training */
	data = oct_csr_read(BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
	data |= BIT(0);
	oct_csr_write(data, BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
}

static int bgx_port_get_max_qlm_lanes(int qlm)
{
	if (OCTEON_IS_MODEL(OCTEON_CN73XX))
		return (qlm < 4) ? 4 : 2;
	else if (OCTEON_IS_MODEL(OCTEON_CNF75XX))
		return 2;
	return 4;
}

static int bgx_port_qlm_rx_equalization(struct bgx_port_priv *priv, int qlm, int lane)
{
	u64	data;
	u64	addr;
	u64	lmode;
	int	max_lanes = bgx_port_get_max_qlm_lanes(qlm);
	int	lane_mask = lane == -1 ? ((1 << max_lanes) - 1) : (1 << lane);
	int	timeout;
	int	i;
	int	rc = 0;

	/* Nothing to do for qlms in reset */
	data = oct_csr_read(GSER_PHY_CTL(priv->node, qlm));
	if (data & (BIT(0) | BIT(1)))
		return -1;

	for (i = 0; i < max_lanes; i++) {
		if (!(i & lane_mask))
			continue;

		addr = GSER_LANE_LBERT_CFG(priv->node, qlm, i);
		data = oct_csr_read(addr);
		/* Rx equalization can't be completed while pattern matcher is
		 * enabled because it causes errors.
		 */
		if (data & BIT(6))
			return -1;
	}

	lmode = oct_csr_read(GSER_LANE_MODE(priv->node, qlm));
	lmode &= 0xf;
	addr = GSER_LANE_P_MODE_1(priv->node, qlm, lmode);
	data = oct_csr_read(addr);
	/* Don't complete rx equalization if in VMA manual mode */
	if (data & BIT(14))
		return 0;

	/* Apply rx equalization for speed > 6250 */
	if (bgx_port_get_qlm_speed(priv, qlm) < 6250)
		return 0;

	/* Wait until rx data is valid (CDRLOCK) */
	timeout = 500;
	addr = GSER_RX_EIE_DETSTS(priv->node, qlm);
	do {
		data = oct_csr_read(addr);
		data >>= 8;
		data &= lane_mask;
		if (data == lane_mask)
			break;
		timeout--;
		udelay(1);
	} while (timeout);
	if (!timeout) {
		pr_debug("QLM%d:%d: CDRLOCK timeout\n", qlm, priv->node);
		return -1;
	}

	bgx_port_gser_20075(priv, qlm, lane);

	for (i = 0; i < max_lanes; i++) {
		if (!(i & lane_mask))
			continue;
		/* Skip lane 3 on 78p1.x due to gser-20075. Handled above */
		if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X) && i == 3)
			continue;

		/* Enable software control */
		addr = GSER_BR_RX_CTL(priv->node, qlm, i);
		data = oct_csr_read(addr);
		data |= BIT(2);
		oct_csr_write(data, addr);

		/* Clear the completion flag */
		addr = GSER_BR_RX_EER(priv->node, qlm, i);
		data = oct_csr_read(addr);
		data &= ~BIT(14);
		data |= BIT(15);
		oct_csr_write(data, addr);
	}

	/* Wait for rx equalization to complete */
	for (i = 0; i < max_lanes; i++) {
		if (!(i & lane_mask))
			continue;

		timeout = 250000;
		addr = GSER_BR_RX_EER(priv->node, qlm, i);
		do {
			data = oct_csr_read(addr);
			if (data & BIT(14))
				break;
			timeout--;
			udelay(1);
		} while (timeout);
		if (!timeout) {
			pr_debug("QLM%d:%d: RXT_ESV timeout\n",
				 qlm, priv->node);
			rc = -1;
		}

		/* Switch back to hardware control */
		addr = GSER_BR_RX_CTL(priv->node, qlm, i);
		data = oct_csr_read(addr);
		data &= ~BIT(2);
		oct_csr_write(data, addr);
	}

	return rc;
}

static int bgx_port_xaui_equalization(struct bgx_port_priv *priv)
{
	u64	data;
	int	lane;

	/* Nothing to do for loopback mode */
	data = oct_csr_read(BGX_SPU_CONTROL1(priv->node, priv->bgx,
					     priv->index));
	if (data & BIT(14))
		return 0;

	if (priv->mode == PORT_MODE_XAUI || priv->mode == PORT_MODE_XLAUI) {
		if (bgx_port_qlm_rx_equalization(priv, priv->qlm, -1))
			return -1;

		/* BGX2 of 73xx uses 2 dlms */
		if (OCTEON_IS_MODEL(OCTEON_CN73XX) && priv->bgx == 2) {
			if (bgx_port_qlm_rx_equalization(priv, priv->qlm + 1, -1))
				return -1;
		}
	} else if (priv->mode == PORT_MODE_RXAUI) {
		/* Rxaui always uses 2 lanes */
		if (bgx_port_qlm_rx_equalization(priv, priv->qlm, -1))
			return -1;
	} else if (priv->mode == PORT_MODE_XFI) {
		lane = priv->index;
		if ((OCTEON_IS_MODEL(OCTEON_CN73XX) && priv->qlm == 6) ||
		    (OCTEON_IS_MODEL(OCTEON_CNF75XX) && priv->qlm == 5))
			lane -= 2;

		if (bgx_port_qlm_rx_equalization(priv, priv->qlm, lane))
			return -1;
	}

	return 0;
}

static int bgx_port_init_xaui_link(struct bgx_port_priv *priv)
{
	u64	data;
	int	use_training = 0;
	int	use_ber = 0;
	int	timeout;
	int	rc = 0;

	if (priv->mode == PORT_MODE_10G_KR || priv->mode == PORT_MODE_40G_KR4)
		use_training = 1;

	if (!octeon_is_simulation() &&
	    (priv->mode == PORT_MODE_XFI || priv->mode == PORT_MODE_XLAUI ||
	     priv->mode == PORT_MODE_10G_KR || priv->mode == PORT_MODE_40G_KR4))
		use_ber = 1;

	data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
	data &= ~(BIT(14) | BIT(13));
	oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));

	data = oct_csr_read(BGX_SPU_MISC_CONTROL(priv->node, priv->bgx, priv->index));
	data |= BIT(12);
	oct_csr_write(data, BGX_SPU_MISC_CONTROL(priv->node, priv->bgx, priv->index));

	if (!octeon_is_simulation()) {
		data = oct_csr_read(BGX_SPU_AN_CONTROL(priv->node, priv->bgx, priv->index));
		/* Restart autonegotiation */
		if (data & BIT(12)) {
			rc = bgx_port_init_xaui_an(priv);
			if (rc)
				return rc;
		}

		if (use_training) {
			data = oct_csr_read(BGX_SPU_BR_PMD_CONTROL(priv->node, priv->bgx, priv->index));
			/* Check if training is enabled */
			if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X) &&
			    !(data & BIT(1))) {
				bgx_port_xaui_start_training(priv);
				return -1;
			}

			if (OCTEON_IS_MODEL(OCTEON_CN73XX) ||
			    OCTEON_IS_MODEL(OCTEON_CNF75XX) ||
			    OCTEON_IS_MODEL(OCTEON_CN78XX))
				bgx_port_gser_27882(priv);

			data = oct_csr_read(BGX_SPU_INT(priv->node, priv->bgx, priv->index));

			/* Restart training if it failed */
			if ((data & BIT(14)) &&
			    !OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X)) {
				bgx_port_xaui_restart_training(priv);
				return -1;
			}

			if (!(data & BIT(13))) {
				pr_debug("Waiting for link training\n");
				return -1;
			}
		} else {
			bgx_port_xaui_equalization(priv);
		}

		/* Wait until the reset is complete */
		timeout = 10000;
		do {
			data = oct_csr_read(BGX_SPU_CONTROL1(priv->node, priv->bgx, priv->index));
			if (!(data & BIT(15)))
				break;
			timeout--;
			udelay(1);
		} while (timeout);
		if (!timeout) {
			pr_debug("BGX%d:%d:%d: Reset timeout\n", priv->bgx,
				 priv->index, priv->node);
			return -1;
		}

		if (use_ber) {
			timeout = 10000;
			do {
				data =
				oct_csr_read(BGX_SPU_BR_STATUS1(priv->node, priv->bgx, priv->index));
				if (data & BIT(0))
					break;
				timeout--;
				udelay(1);
			} while (timeout);
			if (!timeout) {
				pr_debug("BGX%d:%d:%d: BLK_LOCK timeout\n",
					 priv->bgx, priv->index, priv->node);
				return -1;
			}
		} else {
			timeout = 10000;
			do {
				data =
				oct_csr_read(BGX_SPU_BX_STATUS(priv->node, priv->bgx, priv->index));
				if (data & BIT(12))
					break;
				timeout--;
				udelay(1);
			} while (timeout);
			if (!timeout) {
				pr_debug("BGX%d:%d:%d: Lanes align timeout\n",
					 priv->bgx, priv->index, priv->node);
				return -1;
			}
		}

		if (use_ber) {
			data = oct_csr_read(BGX_SPU_BR_STATUS2(priv->node, priv->bgx, priv->index));
			data |= BIT(15);
			oct_csr_write(data, BGX_SPU_BR_STATUS2(priv->node, priv->bgx, priv->index));
		}

		data = oct_csr_read(BGX_SPU_STATUS2(priv->node, priv->bgx, priv->index));
		data |= BIT(10);
		oct_csr_write(data, BGX_SPU_STATUS2(priv->node, priv->bgx, priv->index));

		data = oct_csr_read(BGX_SPU_STATUS2(priv->node, priv->bgx, priv->index));
		if (data & BIT(10)) {
			if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X) &&
			    use_training)
				bgx_port_xaui_restart_training(priv);
			return -1;
		}

		/* Wait for mac rx to be ready */
		timeout = 10000;
		do {
			data = oct_csr_read(BGX_SMU_RX_CTL(priv->node, priv->bgx, priv->index));
			data &= GENMASK_ULL(1, 0);
			if (!data)
				break;
			timeout--;
			udelay(1);
		} while (timeout);
		if (!timeout) {
			pr_debug("BGX%d:%d:%d: mac ready timeout\n",
				 priv->bgx, priv->index, priv->node);
			return -1;
		}

		/* Wait for bgx rx to be idle */
		timeout = 10000;
		do {
			data = oct_csr_read(BGX_SMU_CTRL(priv->node, priv->bgx, priv->index));
			if (data & BIT(0))
				break;
			timeout--;
			udelay(1);
		} while (timeout);
		if (!timeout) {
			pr_debug("BGX%d:%d:%d: rx idle timeout\n",
				 priv->bgx, priv->index, priv->node);
			return -1;
		}

		/* Wait for gmx tx to be idle */
		timeout = 10000;
		do {
			data = oct_csr_read(BGX_SMU_CTRL(priv->node, priv->bgx, priv->index));
			if (data & BIT(1))
				break;
			timeout--;
			udelay(1);
		} while (timeout);
		if (!timeout) {
			pr_debug("BGX%d:%d:%d: tx idle timeout\n",
				 priv->bgx, priv->index, priv->node);
			return -1;
		}

		/* Check rcvflt is still be 0 */
		data = oct_csr_read(BGX_SPU_STATUS2(priv->node, priv->bgx, priv->index));
		if (data & BIT(10)) {
			pr_debug("BGX%d:%d:%d: receive fault\n",
				 priv->bgx, priv->index, priv->node);
			return -1;
		}

		/* Receive link is latching low. Force it high and verify it */
		data = oct_csr_read(BGX_SPU_STATUS1(priv->node, priv->bgx, priv->index));
		data |= BIT(2);
		oct_csr_write(data, BGX_SPU_STATUS1(priv->node, priv->bgx, priv->index));
		timeout = 10000;
		do {
			data = oct_csr_read(BGX_SPU_STATUS1(priv->node, priv->bgx, priv->index));
			if (data & BIT(2))
				break;
			timeout--;
			udelay(1);
		} while (timeout);
		if (!timeout) {
			pr_debug("BGX%d:%d:%d: rx link down\n",
				 priv->bgx, priv->index, priv->node);
			return -1;
		}
	}

	if (use_ber) {
		/* Read error counters to clear */
		data = oct_csr_read(BGX_SPU_BR_BIP_ERR_CNT(priv->node, priv->bgx, priv->index));
		data = oct_csr_read(BGX_SPU_BR_STATUS2(priv->node, priv->bgx, priv->index));

		/* Verify latch lock is set */
		if (!(data & BIT(15))) {
			pr_debug("BGX%d:%d:%d: latch lock lost\n",
				 priv->bgx, priv->index, priv->node);
			return -1;
		}

		/* LATCHED_BER is cleared by writing 1 to it */
		if (data & BIT(14))
			oct_csr_write(data, BGX_SPU_BR_STATUS2(priv->node, priv->bgx, priv->index));

		usleep_range(1500, 2000);
		data = oct_csr_read(BGX_SPU_BR_STATUS2(priv->node, priv->bgx, priv->index));
		if (data & BIT(14)) {
			pr_debug("BGX%d:%d:%d: BER test failed\n",
				 priv->bgx, priv->index, priv->node);
			return -1;
		}
	}

	/* Enable packet transmit and receive */
	data = oct_csr_read(BGX_SPU_MISC_CONTROL(priv->node, priv->bgx, priv->index));
	data &= ~BIT(12);
	oct_csr_write(data, BGX_SPU_MISC_CONTROL(priv->node, priv->bgx, priv->index));
	data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
	data |= BIT(14) | BIT(13);
	oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));

	return 0;
}

static int bgx_port_set_xaui_link(struct bgx_port_priv *priv,
				  struct port_status status)
{
	u64	data;
	bool	smu_tx_ok = false;
	bool	smu_rx_ok = false;
	bool	spu_link_ok = false;
	int	rc = 0;

	/* Initialize hardware if link is up but hardware is not happy */
	if (status.link) {
		data = oct_csr_read(BGX_SMU_TX_CTL(priv->node, priv->bgx, priv->index));
		data &= GENMASK_ULL(5, 4);
		smu_tx_ok = data == 0;

		data = oct_csr_read(BGX_SMU_RX_CTL(priv->node, priv->bgx, priv->index));
		data &= GENMASK_ULL(1, 0);
		smu_rx_ok = data == 0;

		data = oct_csr_read(BGX_SPU_STATUS1(priv->node, priv->bgx, priv->index));
		data &= BIT(2);
		spu_link_ok = data == BIT(2);

		if (!smu_tx_ok || !smu_rx_ok || !spu_link_ok)
			rc = bgx_port_init_xaui_link(priv);
	}

	return rc;
}

static struct bgx_port_priv *bgx_port_netdev2priv(struct net_device *netdev)
{
	struct bgx_port_netdev_priv *nd_priv = netdev_priv(netdev);

	return nd_priv->bgx_priv;
}

void bgx_port_set_netdev(struct device *dev, struct net_device *netdev)
{
	struct bgx_port_priv *priv = dev_get_drvdata(dev);

	if (netdev) {
		struct bgx_port_netdev_priv *nd_priv = netdev_priv(netdev);

		nd_priv->bgx_priv = priv;
	}

	priv->netdev = netdev;
}
EXPORT_SYMBOL(bgx_port_set_netdev);

int bgx_port_ethtool_get_link_ksettings(struct net_device *netdev,
					struct ethtool_link_ksettings *cmd)
{
	struct bgx_port_priv	*priv = bgx_port_netdev2priv(netdev);

	if (priv->phydev) {
		phy_ethtool_ksettings_get(priv->phydev, cmd);
		return 0;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(bgx_port_ethtool_get_link_ksettings);

int bgx_port_ethtool_set_settings(struct net_device	*netdev,
				  struct ethtool_cmd	*cmd)
{
	struct bgx_port_priv *p = bgx_port_netdev2priv(netdev);

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (p->phydev)
		return phy_ethtool_sset(p->phydev, cmd);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(bgx_port_ethtool_set_settings);

int bgx_port_ethtool_nway_reset(struct net_device *netdev)
{
	struct bgx_port_priv *p = bgx_port_netdev2priv(netdev);

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (p->phydev)
		return phy_start_aneg(p->phydev);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(bgx_port_ethtool_nway_reset);

const u8 *bgx_port_get_mac(struct net_device *netdev)
{
	struct bgx_port_priv *priv = bgx_port_netdev2priv(netdev);

	return priv->mac_addr;
}
EXPORT_SYMBOL(bgx_port_get_mac);

int bgx_port_do_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	struct bgx_port_priv *p = bgx_port_netdev2priv(netdev);

	if (p->phydev)
		return phy_mii_ioctl(p->phydev, ifr, cmd);
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(bgx_port_do_ioctl);

static void bgx_port_write_cam(struct bgx_port_priv	*priv,
			       int			cam,
			       const u8			*mac)
{
	u64	m = 0;
	int	i;

	if (mac) {
		for (i = 0; i < 6; i++)
			m |= (((u64)mac[i]) << ((5 - i) * 8));
		m |= BIT(48);
	}

	m |= (u64)priv->index << 52;
	oct_csr_write(m, BGX_CMR_RX_ADRX_CAM(priv->node, priv->bgx, priv->index * 8 + cam));
}

/* Set MAC address for the net_device that is attached. */
void bgx_port_set_rx_filtering(struct net_device *netdev)
{
	u64	data;
	struct bgx_port_priv *priv = bgx_port_netdev2priv(netdev);
	int available_cam_entries, current_cam_entry;
	struct netdev_hw_addr *ha;

	available_cam_entries = 8;
	data = 0;
	data |= BIT(0); /* Accept all Broadcast*/

	if ((netdev->flags & IFF_PROMISC) || netdev->uc.count > 7) {
		data &= ~BIT(3); /* Reject CAM match */
		available_cam_entries = 0;
	} else {
		/* One CAM entry for the primary address, leaves seven
		 * for the secondary addresses.
		 */
		data |= BIT(3); /* Accept CAM match */
		available_cam_entries = 7 - netdev->uc.count;
	}

	if (netdev->flags & IFF_PROMISC) {
		data |= 1 << 1; /* Accept all Multicast */
	} else {
		if (netdev->flags & IFF_MULTICAST) {
			if ((netdev->flags & IFF_ALLMULTI) ||
			    netdev_mc_count(netdev) > available_cam_entries)
				data |= 1 << 1; /* Accept all Multicast */
			else
				data |= 2 << 1; /* Accept all Mcast via CAM */
		}
	}
	current_cam_entry = 0;
	if (data & BIT(3)) {
		bgx_port_write_cam(priv, current_cam_entry, netdev->dev_addr);
		current_cam_entry++;
		netdev_for_each_uc_addr(ha, netdev) {
			bgx_port_write_cam(priv, current_cam_entry, ha->addr);
			current_cam_entry++;
		}
	}
	if (((data & GENMASK_ULL(2, 1)) >> 1) == 2) {
		/* Accept all Multicast via CAM */
		netdev_for_each_mc_addr(ha, netdev) {
			bgx_port_write_cam(priv, current_cam_entry, ha->addr);
			current_cam_entry++;
		}
	}
	while (current_cam_entry < 8) {
		bgx_port_write_cam(priv, current_cam_entry, NULL);
		current_cam_entry++;
	}
	oct_csr_write(data, BGX_CMR_RX_ADR_CTL(priv->node, priv->bgx,
					       priv->index));
}
EXPORT_SYMBOL(bgx_port_set_rx_filtering);

static void bgx_port_adjust_link(struct net_device *netdev)
{
	struct bgx_port_priv	*priv = bgx_port_netdev2priv(netdev);
	int			link_changed = 0;
	unsigned int		link;
	unsigned int		speed;
	unsigned int		duplex;

	mutex_lock(&priv->lock);

	if (!priv->phydev->link && priv->last_status.link)
		link_changed = -1;

	if (priv->phydev->link &&
	    (priv->last_status.link != priv->phydev->link ||
	     priv->last_status.duplex != priv->phydev->duplex ||
	     priv->last_status.speed != priv->phydev->speed))
		link_changed = 1;

	link = priv->phydev->link;
	priv->last_status.link = priv->phydev->link;

	speed = priv->phydev->speed;
	priv->last_status.speed = priv->phydev->speed;

	duplex = priv->phydev->duplex;
	priv->last_status.duplex = priv->phydev->duplex;

	mutex_unlock(&priv->lock);

	if (link_changed != 0) {
		struct port_status status;

		if (link_changed > 0) {
			netdev_info(netdev, "Link is up - %d/%s\n",
				    priv->phydev->speed,
				    priv->phydev->duplex == DUPLEX_FULL ?
				    "Full" : "Half");
		} else {
			netdev_info(netdev, "Link is down\n");
		}
		status.link = link ? 1 : 0;
		status.duplex = duplex;
		status.speed = speed;
		if (!link) {
			netif_carrier_off(netdev);
			 /* Let TX drain. FIXME check that it is drained. */
			mdelay(50);
		}
		priv->set_link(priv, status);
		if (link)
			netif_carrier_on(netdev);
	}
}

static void bgx_port_check_state(struct work_struct *work)
{
	struct bgx_port_priv	*priv;
	struct port_status	status;

	priv = container_of(work, struct bgx_port_priv, dwork.work);

	status = priv->get_link(priv);

	if (!status.link &&
	    priv->mode != PORT_MODE_SGMII && priv->mode != PORT_MODE_RGMII)
		bgx_port_init_xaui_link(priv);

	if (priv->last_status.link != status.link) {
		priv->last_status.link = status.link;
		if (status.link)
			netdev_info(priv->netdev, "Link is up - %d/%s\n",
				    status.speed,
				    status.duplex == DUPLEX_FULL ? "Full" : "Half");
		else
			netdev_info(priv->netdev, "Link is down\n");
	}

	mutex_lock(&priv->lock);
	if (priv->work_queued)
		queue_delayed_work(check_state_wq, &priv->dwork, HZ);
	mutex_unlock(&priv->lock);
}

int bgx_port_enable(struct net_device *netdev)
{
	struct bgx_port_priv	*priv = bgx_port_netdev2priv(netdev);
	u64			data;
	struct port_status	status;
	bool			dont_use_phy;

	if (priv->mode == PORT_MODE_SGMII || priv->mode == PORT_MODE_RGMII) {
		/* 1G */
		data = oct_csr_read(BGX_GMP_GMI_TX_APPEND(priv->node, priv->bgx, priv->index));
		data |= BIT(2) | BIT(1);
		oct_csr_write(data, BGX_GMP_GMI_TX_APPEND(priv->node, priv->bgx, priv->index));

		/* Packets are padded (without FCS) to MIN_SIZE + 1 in SGMII */
		data = 60 - 1;
		oct_csr_write(data, BGX_GMP_GMI_TX_MIN_PKT(priv->node, priv->bgx, priv->index));
	} else {
		/* 10G or higher */
		data = oct_csr_read(BGX_SMU_TX_APPEND(priv->node, priv->bgx, priv->index));
		data |= BIT(2) | BIT(1);
		oct_csr_write(data, BGX_SMU_TX_APPEND(priv->node, priv->bgx, priv->index));

		/* Packets are padded(with FCS) to MIN_SIZE  in non-SGMII */
		data = 60 + 4;
		oct_csr_write(data, BGX_SMU_TX_MIN_PKT(priv->node, priv->bgx, priv->index));
	}

	switch (priv->mode) {
	case PORT_MODE_XLAUI:
	case PORT_MODE_XFI:
	case PORT_MODE_10G_KR:
	case PORT_MODE_40G_KR4:
		dont_use_phy = true;
		break;
	default:
		dont_use_phy = false;
		break;
	}

	if (!priv->phy_np || dont_use_phy) {
		status = priv->get_link(priv);
		priv->set_link(priv, status);
		netif_carrier_on(netdev);

		mutex_lock(&check_state_wq_mutex);
		if (!check_state_wq) {
			check_state_wq =
				alloc_workqueue("check_state_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
		}
		mutex_unlock(&check_state_wq_mutex);
		if (!check_state_wq)
			return -ENOMEM;

		mutex_lock(&priv->lock);
		INIT_DELAYED_WORK(&priv->dwork, bgx_port_check_state);
		queue_delayed_work(check_state_wq, &priv->dwork, 0);
		priv->work_queued = true;
		mutex_unlock(&priv->lock);

		netdev_info(priv->netdev, "Link is not ready\n");

	} else {
		priv->phydev = of_phy_connect(netdev, priv->phy_np,
					      bgx_port_adjust_link, 0, priv->phy_mode);
		if (!priv->phydev)
			return -ENODEV;

		netif_carrier_off(netdev);

		if (priv->phydev)
			phy_start_aneg(priv->phydev);
	}

	return 0;
}
EXPORT_SYMBOL(bgx_port_enable);

int bgx_port_disable(struct net_device *netdev)
{
	struct bgx_port_priv	*priv = bgx_port_netdev2priv(netdev);
	struct port_status	status;

	if (priv->phydev) {
		phy_stop(priv->phydev);
		phy_disconnect(priv->phydev);
	}
	priv->phydev = NULL;

	netif_carrier_off(netdev);
	memset(&status, 0, sizeof(status));
	priv->last_status.link = 0;
	priv->set_link(priv, status);

	mutex_lock(&priv->lock);
	if (priv->work_queued) {
		cancel_delayed_work_sync(&priv->dwork);
		priv->work_queued = false;
	}
	mutex_unlock(&priv->lock);

	return 0;
}
EXPORT_SYMBOL(bgx_port_disable);

int bgx_port_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct bgx_port_priv *priv = bgx_port_netdev2priv(netdev);
	int max_frame;

	if (new_mtu < 60 || new_mtu > 65392) {
		netdev_warn(netdev, "Maximum MTU supported is 65392\n");
		return -EINVAL;
	}

	netdev->mtu = new_mtu;

	max_frame = round_up(new_mtu + ETH_HLEN + ETH_FCS_LEN, 8);

	if (priv->mode == PORT_MODE_SGMII || priv->mode == PORT_MODE_RGMII) {
		/* 1G */
		oct_csr_write(max_frame, BGX_GMP_GMI_RX_JABBER(priv->node, priv->bgx, priv->index));
	} else {
		/* 10G or higher */
		oct_csr_write(max_frame, BGX_SMU_RX_JABBER(priv->node, priv->bgx, priv->index));
	}

	return 0;
}
EXPORT_SYMBOL(bgx_port_change_mtu);

void bgx_port_mix_assert_reset(struct net_device *netdev, int mix, bool v)
{
	struct bgx_port_priv *priv = bgx_port_netdev2priv(netdev);
	u64 mask = 1ull << (3 + (mix & 1));
	u64 data;

	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X) && v) {
		/* Need to disable the mix before resetting the bgx-mix
		 * interface as not doing so confuses the other already up
		 * lmacs.
		 */
		data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
		data &= ~BIT(11);
		oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
	}

	data = oct_csr_read(BGX_CMR_GLOBAL_CONFIG(priv->node, priv->bgx));
	if (v)
		data |= mask;
	else
		data &= ~mask;
	oct_csr_write(data, BGX_CMR_GLOBAL_CONFIG(priv->node, priv->bgx));

	if (OCTEON_IS_MODEL(OCTEON_CN78XX_PASS1_X) && !v) {
		data = oct_csr_read(BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
		data |= BIT(11);
		oct_csr_write(data, BGX_CMR_CONFIG(priv->node, priv->bgx, priv->index));
	}
}
EXPORT_SYMBOL(bgx_port_mix_assert_reset);

static int bgx_port_probe(struct platform_device *pdev)
{
	u64 addr;
	const u8 *mac;
	const __be32 *reg;
	u32 index;
	int rc;
	struct bgx_port_priv *priv;
	int numa_node;

	reg = of_get_property(pdev->dev.parent->of_node, "reg", NULL);
	addr = of_translate_address(pdev->dev.parent->of_node, reg);
	mac = of_get_mac_address(pdev->dev.of_node);

	numa_node = (addr >> 36) & 0x7;

	rc = of_property_read_u32(pdev->dev.of_node, "reg", &index);
	if (rc)
		return -ENODEV;
	priv = kzalloc_node(sizeof(*priv), GFP_KERNEL, numa_node);
	if (!priv)
		return -ENOMEM;
	priv->phy_np = of_parse_phandle(pdev->dev.of_node, "phy-handle", 0);
	priv->phy_mode = of_get_phy_mode(pdev->dev.of_node);
	/* If phy-mode absent, default to SGMII. */
	if (priv->phy_mode < 0)
		priv->phy_mode = PHY_INTERFACE_MODE_SGMII;

	if (priv->phy_mode == PHY_INTERFACE_MODE_1000BASEX)
		priv->mode_1000basex = true;

	if (of_phy_is_fixed_link(pdev->dev.of_node))
		priv->bgx_as_phy = true;

	mutex_init(&priv->lock);
	priv->node = numa_node;
	priv->bgx = (addr >> 24) & 0xf;
	priv->index = index;
	if (mac)
		priv->mac_addr = mac;

	priv->qlm = bgx_port_get_qlm(priv->node, priv->bgx, priv->index);
	priv->mode = bgx_port_get_mode(priv->node, priv->bgx, priv->index);

	switch (priv->mode) {
	case PORT_MODE_SGMII:
	case PORT_MODE_RGMII:
		priv->get_link = bgx_port_get_sgmii_link;
		priv->set_link = bgx_port_set_xgmii_link;
		break;
	case PORT_MODE_XAUI:
	case PORT_MODE_RXAUI:
	case PORT_MODE_XLAUI:
	case PORT_MODE_XFI:
	case PORT_MODE_10G_KR:
	case PORT_MODE_40G_KR4:
		priv->get_link = bgx_port_get_xaui_link;
		priv->set_link = bgx_port_set_xaui_link;
		break;
	default:
		goto err;
	}

	dev_set_drvdata(&pdev->dev, priv);

	bgx_port_init(priv);

	dev_info(&pdev->dev, "Probed\n");
	return 0;
 err:
	kfree(priv);
	return rc;
}

static int bgx_port_remove(struct platform_device *pdev)
{
	struct bgx_port_priv *priv = dev_get_drvdata(&pdev->dev);

	kfree(priv);
	return 0;
}

static void bgx_port_shutdown(struct platform_device *pdev)
{
}

static const struct of_device_id bgx_port_match[] = {
	{
		.compatible = "cavium,octeon-7890-bgx-port",
	},
	{
		.compatible = "cavium,octeon-7360-xcv",
	},
	{},
};
MODULE_DEVICE_TABLE(of, bgx_port_match);

static struct platform_driver bgx_port_driver = {
	.probe		= bgx_port_probe,
	.remove		= bgx_port_remove,
	.shutdown       = bgx_port_shutdown,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= KBUILD_MODNAME,
		.of_match_table = bgx_port_match,
	},
};

static int __init bgx_port_driver_init(void)
{
	int r;
	int i;
	int j;
	int k;

	for (i = 0; i < MAX_NODES; i++) {
		for (j = 0; j < MAX_BGX_PER_NODE; j++) {
			for (k = 0; k < MAX_LMAC_PER_BGX; k++)
				lmac_pknd[i][j][k] = -1;
		}
	}

	bgx_nexus_load();
	r =  platform_driver_register(&bgx_port_driver);
	return r;
}
module_init(bgx_port_driver_init);

static void __exit bgx_port_driver_exit(void)
{
	platform_driver_unregister(&bgx_port_driver);
	if (check_state_wq)
		destroy_workqueue(check_state_wq);
}
module_exit(bgx_port_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cavium, Inc. <support@caviumnetworks.com>");
MODULE_DESCRIPTION("Cavium, Inc. BGX Ethernet MAC driver.");
