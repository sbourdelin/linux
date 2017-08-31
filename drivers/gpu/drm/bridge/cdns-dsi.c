/*
 * Copyright: 2017 Cadence Design Systems, Inc.
 *
 * Author: Boris Brezillon <boris.brezillon@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <video/mipi_display.h>

#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#define IP_CONF				0x0
#define SP_HS_FIFO_DEPTH(x)		(((x) & GENMASK(30, 26)) >> 26)
#define SP_LP_FIFO_DEPTH(x)		(((x) & GENMASK(25, 21)) >> 21)
#define VRS_FIFO_DEPTH(x)			(((x) & GENMASK(20, 16)) >> 16)
#define DIRCMD_FIFO_DEPTH(x)		(((x) & GENMASK(15, 13)) >> 13)
#define SDI_IFACE_32			BIT(12)
#define INTERNAL_DATAPATH_32		(0 << 10)
#define INTERNAL_DATAPATH_16		(1 << 10)
#define INTERNAL_DATAPATH_8		(3 << 10)
#define INTERNAL_DATAPATH_SIZE		((x) & GENMASK(11, 10))
#define INTERNAL_DATAPATH_32		(0 << 10)
#define INTERNAL_DATAPATH_16		(1 << 10)
#define INTERNAL_DATAPATH_8		(3 << 10)
#define NUM_IFACE(x)			((((x) & GENMASK(9, 8)) >> 8) + 1)
#define MAX_LANE_NB(x)			(((x) & GENMASK(7, 6)) >> 6)
#define RX_FIFO_DEPTH(x)		((x) & GENMASK(5, 0))

#define MCTL_MAIN_DATA_CTL		0x4
#define TE_MIPI_POLLING_EN		BIT(25)
#define TE_HW_POLLING_EN		BIT(24)
#define DISP_EOT_GEN			BIT(18)
#define HOST_EOT_GEN			BIT(17)
#define DISP_GEN_CHECKSUM		BIT(16)
#define DISP_GEN_ECC			BIT(15)
#define BTA_EN				BIT(14)
#define READ_EN				BIT(13)
#define REG_TE_EN			BIT(12)
#define IF_TE_EN(x)			BIT(8 + (x))
#define TBG_SEL				BIT(7)
#define TVG_SEL				BIT(6)
#define VID_EN				BIT(5)
#define IF_VID_SELECT(x)		((x) << 2)
#define IF_VID_SELECT_MASK		GENMASK(4, 2)
#define IF_VID_MODE			BIT(1)
#define LINK_EN				BIT(0)

#define MCTL_MAIN_PHY_CTL		0x8
#define CLK_FORCE_STOP			BIT(14)
#define WAIT_BURST_TIME(x)		((x) << 10)
#define DATA_ULPM_EN(x)			BIT(6 + (x))
#define CLK_ULPM_EN			BIT(5)
#define CLK_CONTINUOUS			BIT(4)
#define DATA_FORCE_STOP			BIT(3)
#define DATA_LANE_EN(x)			BIT((x) - 1)

#define MCTL_LANE_STS			0x10
#define LANE_STATE_START		0
#define LANE_STATE_IDLE			1
#define LANE_STATE_WRITE		2
#define LANE_STATE_ULPM			3
#define LANE_STATE_READ			4
#define DATA_LANE_STATE(l, val)		\
	(((val) >> (2 + 2 * (l) + ((l) ? 1 : 0))) & GENMASK((l) ? 1 : 2, 0))
#define CLK_LANE_STATE_HS		2
#define CLK_LANE_STATE(val)		((val) & GENMASK(1, 0))

#define MCTL_DPHY_TIMEOUT1		0x14
#define CLK_DIV(x)			(x)
#define CLK_DIV_MAX			GENMASK(3, 0)
#define HSTX_TIMEOUT(x)			((x) << 4)
#define HSTX_TIMEOUT_MAX		GENMASK(21, 4)

#define MCTL_DPHY_TIMEOUT2		0x18
#define LPRX_TIMEOUT(x)			(x)

#define MCTL_ULPOUT_TIME		0x1c
#define DATA_LANE_ULPOUT_TIME(x)	((x) << 9)
#define CLK_LANE_ULPOUT_TIME(x)		(x)

#define MCTL_DPHY_STATIC		0x20
#define INVERT_DATA_HS(l)		BIT((l) * (2 + 1) + 1)
#define SWAP_DATA_PINS(l)		BIT((l) * (2 + 1))
#define INVERT_CLK_HS			BIT(1)
#define SWAP_CLK_PINS			BIT(0)

#define MCTL_MAIN_EN			0x24
#define IF_EN(x)			BIT(13 + (x))
#define DATA_LANE_ULPM_REQ(l)		BIT(9 + (l))
#define CLK_LANE_ULPM_REQ		BIT(8)
#define DATA_LANE_START(x)		BIT(4 + (x))
#define CLK_LANE_EN			BIT(3)
#define PLL_START			BIT(0)

#define MCTL_MAIN_STS			0x28
#define MCTL_MAIN_STS_CTL		0x130
#define MCTL_MAIN_STS_CLR		0x150
#define MCTL_MAIN_STS_FLAG		0x170
#define IF_UNTERM_PKT_ERR(x)		BIT(8 + (x))
#define LPRX_TIMEOUT_ERR		BIT(7)
#define HSTX_TIMEOUT_ERR		BIT(6)
#define DATA_LANE_RDY(l)		BIT(2 + (l))
#define CLK_LANE_RDY			BIT(1)
#define PLL_LOCKED			BIT(0)

#define MCTL_DPHY_ERR			0x2c
#define MCTL_DPHY_ERR_CTL1		0x148
#define MCTL_DPHY_ERR_CLR		0x168
#define MCTL_DPHY_ERR_FLAG		0x188
#define ERR_CONT_LP(x, l)		BIT(18 + ((x) * 4) + (l))
#define ERR_CONTROL(l)			BIT(14 + (l))
#define ERR_SYNESC(l)			BIT(10 + (l))
#define ERR_ESC(l)			BIT(6 + (l))

#define MCTL_DPHY_ERR_CTL2		0x14c
#define ERR_CONT_LP_EDGE(x, l)		BIT(12 + ((x) * 4) + (l))
#define ERR_CONTROL_EDGE(l)		BIT(8 + (l))
#define ERR_SYN_ESC_EDGE(l)		BIT(4 + (l))
#define ERR_ESC_EDGE(l)			BIT(0 + (l))

#define CMD_MODE_CTL			0x70
#define IF_VCHAN_ID(x, c)		((c) << ((x) * 2))
#define IF_LP_EN(x)			BIT(9 + (x))

#define CMD_MODE_CTL2			0x74
#define TE_TIMEOUT(x)			((x) << 11)
#define FILL_VALUE(x)			((x) << 3)
#define ARB_IF_WITH_HIGHEST_PRIORITY(x)	((x) << 1)
#define ARB_ROUND_ROBIN_MODE		BIT(0)

#define CMD_MODE_STS			0x78
#define CMD_MODE_STS_CTL		0x134
#define CMD_MODE_STS_CLR		0x154
#define CMD_MODE_STS_FLAG		0x174
#define ERR_IF_UNDERRUN(x)		BIT(4 + (x))
#define ERR_UNWANTED_READ		BIT(3)
#define ERR_TE_MISS			BIT(2)
#define ERR_NO_TE			BIT(1)
#define CSM_RUNNING			BIT(0)

#define DIRECT_CMD_SEND			0x80

#define DIRECT_CMD_MAIN_SETTINGS	0x84
#define TRIGGER_VAL(x)			((x) << 25)
#define CMD_LP_EN			BIT(24)
#define CMD_SIZE(x)			((x) << 16)
#define CMD_VCHAN_ID(x)			((x) << 14)
#define CMD_DATATYPE(x)			((x) << 8)
#define CMD_LONG			BIT(3)
#define WRITE_CMD			0
#define READ_CMD			1
#define TE_REQ				4
#define TRIGGER_REQ			5
#define BTA_REQ				6

#define DIRECT_CMD_STS			0x88
#define DIRECT_CMD_STS_CTL		0x138
#define DIRECT_CMD_STS_CLR		0x158
#define DIRECT_CMD_STS_FLAG		0x178
#define RCVD_ACK_VAL(val)		((val) >> 16)
#define RCVD_TRIGGER_VAL(val)		(((val) & GENMASK(14, 11)) >> 11)
#define READ_COMPLETED_WITH_ERR		BIT(10)
#define BTA_FINISHED			BIT(9)
#define BTA_COMPLETED			BIT(8)
#define TE_RCVD				BIT(7)
#define TRIGGER_RCVD			BIT(6)
#define ACK_WITH_ERR_RCVD		BIT(5)
#define ACK_RCVD			BIT(4)
#define READ_COMPLETED			BIT(3)
#define TRIGGER_COMPLETED		BIT(2)
#define WRITE_COMPLETED			BIT(1)
#define SENDING_CMD			BIT(0)

#define DIRECT_CMD_STOP_READ		0x8c

#define DIRECT_CMD_WRDATA		0x90
#define WRDAT(x, val)			((val) << ((x) * 8))

#define DIRECT_CMD_FIFO_RST		0x94

#define DIRECT_CMD_RDDATA		0xa0
#define RDDAT(x, val)			(((val) >> ((x) * 8)) & GENMASK(7, 0))

#define DIRECT_CMD_RD_PROPS		0xa4
#define RD_DCS				BIT(18)
#define RD_VCHAN_ID(val)		(((val) >> 16) & GENMASK(1, 0))
#define RD_SIZE(val)			((val) & GENMASK(15, 0))

#define DIRECT_CMD_RD_STS		0xa8
#define DIRECT_CMD_RD_STS_CTL		0x13c
#define DIRECT_CMD_RD_STS_CLR		0x15c
#define DIRECT_CMD_RD_STS_FLAG		0x17c
#define ERR_EOT_WITH_ERR		BIT(8)
#define ERR_MISSING_EOT			BIT(7)
#define ERR_WRONG_LENGTH		BIT(6)
#define ERR_OVERSIZE			BIT(5)
#define ERR_RECEIVE			BIT(4)
#define ERR_UNDECODABLE			BIT(3)
#define ERR_CHECKSUM			BIT(2)
#define ERR_UNCORRECTABLE		BIT(1)
#define ERR_FIXED			BIT(0)

#define VID_MAIN_CTL			0xb0
#define VID_FIELD_SW			BIT(28)
#define VID_INTERLACED_EN		BIT(27)
#define RECOVERY_MODE(x)		((x) << 25)
#define RECOVERY_MODE_NEXT_HSYNC	0
#define RECOVERY_MODE_NEXT_STOP_POINT	2
#define RECOVERY_MODE_NEXT_VSYNC	3
#define REG_BLKEOL_MODE(x)		((x) << 23)
#define REG_BLKLINE_MODE(x)		((x) << 21)
#define REG_BLK_MODE_NULL_PKT		0
#define REG_BLK_MODE_BLANKING_PKT	1
#define REG_BLK_MODE_LP			2
#define SYNC_PULSE_HORIZONTAL		BIT(20)
#define SYNC_PULSE_ACTIVE		BIT(19)
#define BURST_MODE			BIT(18)
#define VID_PIXEL_MODE_MASK		GENMASK(17, 14)
#define VID_PIXEL_MODE_RGB565		(0 << 14)
#define VID_PIXEL_MODE_RGB666_PACKED	(1 << 14)
#define VID_PIXEL_MODE_RGB666		(2 << 14)
#define VID_PIXEL_MODE_RGB888		(3 << 14)
#define VID_PIXEL_MODE_RGB101010	(4 << 14)
#define VID_PIXEL_MODE_RGB121212	(5 << 14)
#define VID_PIXEL_MODE_YUV420		(8 << 14)
#define VID_PIXEL_MODE_YUV422_PACKED	(9 << 14)
#define VID_PIXEL_MODE_YUV422		(10 << 14)
#define VID_PIXEL_MODE_YUV422_24B	(11 << 14)
#define VID_DATATYPE(x)			((x) << 8)
#define VID_VIRTCHAN_ID(iface, x)	((x) << (4 + (iface) * 2))
#define STOP_MODE(x)			((x) << 2)
#define START_MODE(x)			(x)

#define VID_VSIZE1			0xb4
#define VFP_LEN(x)			((x) << 12)
#define VBP_LEN(x)			((x) << 6)
#define VSA_LEN(x)			(x)

#define VID_VSIZE2			0xb8
#define VACT_LEN(x)			(x)

#define VID_HSIZE1			0xc0
#define HFP_LEN(x)			((x) << 20)
#define HBP_LEN(x)			((x) << 10)
#define HSA_LEN(x)			(x)

#define VID_HSIZE2			0xc4
#define HACT_LEN(x)			(x)

#define VID_BLKSIZE1			0xcc
#define BLK_EOL_PKT_LEN(x)		((x) << 15)
#define BLK_LINE_EVENT_PKT_LEN(x)	(x)

#define VID_BLKSIZE2			0xd0
#define BLK_LINE_PULSE_PKT_LEN(x)	(x)

#define VID_PKT_TIME			0xd8
#define BLK_EOL_DURATION(x)		(x)

#define VID_DPHY_TIME			0xdc
#define REG_WAKEUP_TIME(x)		((x) << 17)
#define REG_LINE_DURATION(x)		(x)

#define VID_ERR_COLOR1			0xe0
#define COL_GREEN(x)			((x) << 12)
#define COL_RED(x)			(x)

#define VID_ERR_COLOR2			0xe4
#define PAD_VAL(x)			((x) << 12)
#define COL_BLUE(x)			(x)

#define VID_VPOS			0xe8
#define LINE_VAL(val)			(((val) & GENMASK(14, 2)) >> 2)
#define LINE_POS(val)			((val) & GENMASK(1, 0))

#define VID_HPOS			0xec
#define HORIZ_VAL(val)			(((val) & GENMASK(17, 3)) >> 3)
#define HORIZ_POS(val)			((val) & GENMASK(2, 0))

#define VID_MODE_STS			0xf0
#define VID_MODE_STS_CTL		0x140
#define VID_MODE_STS_CLR		0x160
#define VID_MODE_STS_FLAG		0x180
#define VSG_RECOVERY			BIT(10)
#define ERR_VRS_WRONG_LEN		BIT(9)
#define ERR_LONG_READ			BIT(8)
#define ERR_LINE_WRITE			BIT(7)
#define ERR_BURST_WRITE			BIT(6)
#define ERR_SMALL_HEIGHT		BIT(5)
#define ERR_SMALL_LEN			BIT(4)
#define ERR_MISSING_VSYNC		BIT(3)
#define ERR_MISSING_HSYNC		BIT(2)
#define ERR_MISSING_DATA		BIT(1)
#define VSG_RUNNING			BIT(0)

#define VID_VCA_SETTING1		0xf4
#define BURST_LP			BIT(16)
#define MAX_BURST_LIMIT(x)		(x)

#define VID_VCA_SETTING2		0xf8
#define MAX_LINE_LIMIT(x)		((x) << 16)
#define EXACT_BURST_LIMIT(x)		(x)

#define TVG_CTL				0xfc
#define TVG_STRIPE_SIZE(x)		((x) << 5)
#define TVG_MODE_MASK			GENMASK(4, 3)
#define TVG_MODE_SINGLE_COLOR		(0 << 3)
#define TVG_MODE_VSTRIPES		(2 << 3)
#define TVG_MODE_HSTRIPES		(3 << 3)
#define TVG_STOPMODE_MASK		GENMASK(2, 1)
#define TVG_STOPMODE_EOF		(0 << 1)
#define TVG_STOPMODE_EOL		(1 << 1)
#define TVG_STOPMODE_NOW		(2 << 1)
#define TVG_RUN				BIT(0)

#define TVG_IMG_SIZE			0x100
#define TVG_NBLINES(x)			((x) << 16)
#define TVG_LINE_SIZE(x)		(x)

#define TVG_COLOR1			0x104
#define TVG_COL1_GREEN(x)		((x) << 12)
#define TVG_COL1_RED(x)			(x)

#define TVG_COLOR1_BIS			0x108
#define TVG_COL1_BLUE(x)		(x)

#define TVG_COLOR2			0x10c
#define TVG_COL2_GREEN(x)		((x) << 12)
#define TVG_COL2_RED(x)			(x)

#define TVG_COLOR2_BIS			0x110
#define TVG_COL2_BLUE(x)		(x)

#define TVG_STS				0x114
#define TVG_STS_RUNNING			BIT(0)

#define TBG_CTL				0x118
#define TBG_MODE_MASK			GENMASK(4, 3)
#define TBG_MODE_START_1B_STOP		(0 << 3)
#define TBG_MODE_START_2B_STOP		(1 << 3)
#define TBG_MODE_START_BURST_CNT_STOP	(2 << 3)
#define TBG_MODE_START_BURST_STOP	(3 << 3)
#define TBG_DATA_SEL			BIT(2)
#define TBG_HS_REQ			BIT(1)
#define TBG_START			BIT(0)

#define TBG_SETTING1			0x11c
#define TBG_SETTING2			0x120

#define TBG_STS				0x124
#define TBG_STS_RUNNING			BIT(0)

#define TVG_TBG_STS_CTL			0x144
#define TVG_TBG_STS_CLR			0x164
#define TVG_TBG_STS_FLAG		0x184
#define TVG_TBG_STS_TBG_RUNNING		BIT(1)
#define TVG_TBG_STS_TVG_RUNNING		BIT(0)

#define STS_CTL_EDGE(e)			((e) << 16)

#define DPHY_LANES_MAP			0x198
#define DAT_REMAP_CFG(b, l)		((l) << ((b) * 8))

#define DPI_IRQ_EN			0x1a0
#define DPI_IRQ_CLR			0x1a4
#define DPI_IRQ_STS			0x1a8
#define PIXEL_BUF_OVERFLOW		BIT(0)

#define DPI_CFG				0x1ac
#define DPI_CFG_FIFO_LEVEL(x)		((x) & GENMASK(15, 0))
#define DPI_CFG_FIFO_DEPTH(x)		((x) >> 16)

#define DPHY_CFG0			0x1b0
#define DPHY_C_RSTB			BIT(20)
#define DPHY_D_RSTB(x)			((x) << 16)
#define DPHY_TIF_FORCE_WRITE		BIT(12)
#define DPHY_PLL_PDN			BIT(10)
#define DPHY_CMN_PDN			BIT(9)
#define DPHY_C_PDN			BIT(8)
#define DPHY_D_PDN(x)			((x) << 4)
#define DPHY_PLL_PSO			BIT(1)
#define DPHY_CMN_PSO			BIT(0)

#define DPHY_CFG1			0x1b4
#define PDHY_PLL_OPDIV(x)		((x) << 20)
#define PDHY_PLL_IPDIV(x)		((x) << 12)
#define PDHY_PLL_FBDIV(x)		(x)

#define DPHY_PLL_TM_LO			0x1b8
#define DPHY_PLL_TM_MID			0x1bc
#define DPHY_PLL_TM_HI			0x1c0

#define DPHY_STATUS			0x1c4
#define PPI_D_RX_ULPS_ESC(x)		((x) >> 12)
#define PPI_C_TX_READY_HS		BIT(8)
#define PPI_PLL_LOCK			BIT(7)
#define PPI_PLL_COARSE			BIT(6)
#define PPI_PLL_COARSE_CODE(x)		((x) & GENMASK(5, 0))

#define DPHY_BIST			0x1c8
#define PSO_BYPASS_CTX_EN		BIT(12)
#define PSO_BYPASS_TX_EN(l)		BIT(8 + (l))
#define BIST_CTX_EN			BIT(4)
#define BIST_TX_EN(l)			BIT(l)

#define TEST_GENERIC			0x1cc
#define TEST_STATUS(x)			((x) >> 16)
#define TEST_CTRL(x)			(x)

#define ID_REG				0x1f0
#define REV_VENDOR_ID(x)		(((x) & GENMASK(31, 20)) >> 20)
#define REV_PRODUCT_ID(x)		(((x) & GENMASK(19, 12)) >> 12)
#define REV_HW(x)			(((x) & GENMASK(11, 8)) >> 8)
#define REV_MAJOR(x)			(((x) & GENMASK(7, 4)) >> 4)
#define REV_MINOR(x)			((x) & GENMASK(3, 0))

#define DSI_OUTPUT_PORT			0
#define DSI_INPUT_PORT(inputid)		(1 + (inputid))

struct cdns_dsi_output {
	struct mipi_dsi_device *dev;
	struct drm_panel *panel;
	struct drm_bridge *bridge;
};

enum cdns_dsi_input_id {
	CDNS_DPI_INPUT = 0,
};

struct cdns_dsi_input {
	enum cdns_dsi_input_id id;
	struct drm_bridge bridge;
};

struct cdns_dsi {
	struct mipi_dsi_host base;
	void __iomem *regs;
	struct cdns_dsi_input input;
	struct cdns_dsi_output output;
	unsigned int direct_cmd_fifo_depth;
	unsigned int rx_fifo_depth;
	struct completion direct_cmd_comp;
	struct clk *pclk;
	struct clk *sysclk;
};

static inline struct cdns_dsi *input_to_dsi(struct cdns_dsi_input *input)
{
	return container_of(input, struct cdns_dsi, input);
}

static inline struct cdns_dsi *to_cdns_dsi(struct mipi_dsi_host *host)
{
	return container_of(host, struct cdns_dsi, base);
}

static inline struct cdns_dsi_input *
bridge_to_cdns_dsi_input(struct drm_bridge *bridge)
{
	return container_of(bridge, struct cdns_dsi_input, bridge);
}

static int cdns_dsi_bridge_attach(struct drm_bridge *bridge)
{
	struct cdns_dsi_input *input = bridge_to_cdns_dsi_input(bridge);
	struct cdns_dsi *dsi = input_to_dsi(input);
	struct cdns_dsi_output *output = &dsi->output;

	if (!drm_core_check_feature(bridge->dev, DRIVER_ATOMIC)) {
		dev_err(dsi->base.dev,
			"cdns-dsi driver is only compatible with DRM devices supporting atomic updates");
		return -ENOTSUPP;
	}

	return drm_bridge_attach(bridge->encoder, output->bridge, bridge);
}

static enum drm_mode_status
cdns_dsi_bridge_mode_valid(struct drm_bridge *bridge,
			   const struct drm_display_mode *mode)
{
	struct cdns_dsi_input *input = bridge_to_cdns_dsi_input(bridge);
	struct cdns_dsi *dsi = input_to_dsi(input);
	struct cdns_dsi_output *output = &dsi->output;
	int bpp;

	/*
	 * VFP_DSI should be less than VFP_DPI and VFP_DSI should be at
	 * least 1.
	 */
	if (mode->vtotal - mode->vsync_end < 2)
		return MODE_V_ILLEGAL;

	/* VSA_DSI = VSA_DPI and must be at least 2. */
	if (mode->vsync_end - mode->vsync_start < 2)
		return MODE_V_ILLEGAL;

	/* HACT must be a 32-bits aligned. */
	bpp = mipi_dsi_pixel_format_to_bpp(output->dev->format);
	if ((mode->hdisplay * bpp) % 32)
		return MODE_H_ILLEGAL;

	return MODE_OK;
}

static void cdns_dsi_bridge_disable(struct drm_bridge *bridge)
{
	struct cdns_dsi_input *input = bridge_to_cdns_dsi_input(bridge);
	struct cdns_dsi *dsi = input_to_dsi(input);
	u32 val;

	val = readl(dsi->regs + MCTL_MAIN_DATA_CTL);
	val &= ~(IF_VID_SELECT_MASK | IF_VID_MODE | VID_EN | HOST_EOT_GEN |
		 DISP_EOT_GEN);
	writel(val, dsi->regs + MCTL_MAIN_DATA_CTL);

	val = readl(dsi->regs + MCTL_MAIN_EN) & ~IF_EN(input->id);
	writel(val, dsi->regs + MCTL_MAIN_EN);
}

#define DSI_HBP_FRAME_OVERHEAD		12
#define DSI_HSA_FRAME_OVERHEAD		14
#define DSI_HFP_FRAME_OVERHEAD		6
#define DSI_HSS_VSS_VSE_FRAME_OVERHEAD	4
#define DSI_BLANKING_FRAME_OVERHEAD	6
#define DSI_NULL_FRAME_OVERHEAD		6
#define DSI_EOT_PKT_SIZE		4

#define REG_WAKEUP_TIME_NS		800
#define DPHY_PLL_RATE_HZ		108000000

static void cdns_dsi_bridge_enable(struct drm_bridge *bridge)
{
	struct cdns_dsi_input *input = bridge_to_cdns_dsi_input(bridge);
	struct cdns_dsi *dsi = input_to_dsi(input);
	struct cdns_dsi_output *output = &dsi->output;
	u32 hsize0, hsa, hline, tmp, reg_wakeup, div;
	unsigned long dphy_pll_period, tx_byte_period;
	struct drm_display_mode *mode;
	int bpp, nlanes;

	mode = &bridge->encoder->crtc->state->adjusted_mode;
	bpp = mipi_dsi_pixel_format_to_bpp(output->dev->format);
	nlanes = output->dev->lanes;

	if (output->dev->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
		tmp = mode->crtc_htotal - mode->crtc_hsync_end;
	else
		tmp = mode->crtc_htotal - mode->crtc_hsync_start;
	tmp = (tmp * bpp) / 8;
	tmp = tmp < DSI_HBP_FRAME_OVERHEAD ? 0 : tmp - DSI_HBP_FRAME_OVERHEAD;
	hsize0 = HBP_LEN(tmp);

	tmp = mode->crtc_hsync_start - mode->crtc_hdisplay;
	tmp = (tmp * bpp) / 8;
	tmp = tmp < DSI_HFP_FRAME_OVERHEAD ? 0 : tmp - DSI_HFP_FRAME_OVERHEAD;
	hsize0 |= HFP_LEN(tmp);

	if (output->dev->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
		tmp = mode->crtc_hsync_end - mode->crtc_hsync_start;
	else
		tmp = 0;
	tmp = (tmp * 8) / bpp;
	tmp = tmp < DSI_HSA_FRAME_OVERHEAD ? 0 : tmp - DSI_HSA_FRAME_OVERHEAD;
	hsa = tmp;
	hsize0 |= HSA_LEN(tmp);

	writel(hsize0, dsi->regs + VID_HSIZE1);
	writel((mode->crtc_hdisplay * bpp) / 8, dsi->regs + VID_HSIZE2);

	writel(VBP_LEN(mode->crtc_vtotal - mode->crtc_vsync_end - 1) |
	       VFP_LEN(mode->crtc_vsync_start - mode->crtc_vdisplay) |
	       VSA_LEN(mode->crtc_vsync_end - mode->crtc_vsync_start),
	       dsi->regs + VID_VSIZE1);
	writel(mode->crtc_vdisplay, dsi->regs + VID_VSIZE2);

	hline = (mode->crtc_htotal * bpp) / 8;
	tmp = hline - DSI_HSA_FRAME_OVERHEAD;
	if (output->dev->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
		tmp -= hsa + DSI_HSA_FRAME_OVERHEAD;

	writel(BLK_LINE_PULSE_PKT_LEN(tmp), dsi->regs + VID_BLKSIZE2);
	if (output->dev->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
		writel(MAX_LINE_LIMIT(tmp - DSI_NULL_FRAME_OVERHEAD),
		       dsi->regs + VID_VCA_SETTING2);

	tmp = hline -
	      (DSI_HSS_VSS_VSE_FRAME_OVERHEAD + DSI_BLANKING_FRAME_OVERHEAD);
	writel(BLK_LINE_EVENT_PKT_LEN(tmp), dsi->regs + VID_BLKSIZE1);
	if (!(output->dev->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE))
		writel(MAX_LINE_LIMIT(tmp - DSI_NULL_FRAME_OVERHEAD),
		       dsi->regs + VID_VCA_SETTING2);

	tmp = DIV_ROUND_UP(hline, nlanes) - DIV_ROUND_UP(hsa, nlanes);

	if (!(output->dev->mode_flags & MIPI_DSI_MODE_EOT_PACKET))
		tmp -= DIV_ROUND_UP(DSI_EOT_PKT_SIZE, nlanes);

	dphy_pll_period = NSEC_PER_SEC / DPHY_PLL_RATE_HZ;
	tx_byte_period = (dphy_pll_period * 2) / 8;
	reg_wakeup = REG_WAKEUP_TIME_NS / dphy_pll_period;
	writel(REG_WAKEUP_TIME(reg_wakeup) | REG_LINE_DURATION(tmp),
	       dsi->regs + VID_DPHY_TIME);

	/*
	 * HSTX and LPRX timeouts are both expressed in TX byte clk cycles and
	 * both should be set to at least the time it takes to transmit a
	 * frame.
	 */
	tmp = NSEC_PER_SEC / mode->vrefresh;
	tmp /= tx_byte_period;

	for (div = 0; div <= CLK_DIV_MAX; div++) {
		if (tmp <= HSTX_TIMEOUT_MAX)
			break;

		tmp >>= 1;
	}

	if (tmp > HSTX_TIMEOUT_MAX)
		tmp = HSTX_TIMEOUT_MAX;

	writel(CLK_DIV(div) | HSTX_TIMEOUT(tmp),
	       dsi->regs + MCTL_DPHY_TIMEOUT1);

	writel(LPRX_TIMEOUT(tmp), dsi->regs + MCTL_DPHY_TIMEOUT2);

	if (output->dev->mode_flags & MIPI_DSI_MODE_VIDEO) {
		switch (output->dev->format) {
		case MIPI_DSI_FMT_RGB888:
			tmp = VID_PIXEL_MODE_RGB888 |
			      VID_DATATYPE(MIPI_DSI_PACKED_PIXEL_STREAM_24);
			break;

		case MIPI_DSI_FMT_RGB666:
			tmp = VID_PIXEL_MODE_RGB666 |
			      VID_DATATYPE(MIPI_DSI_PIXEL_STREAM_3BYTE_18);
			break;

		case MIPI_DSI_FMT_RGB666_PACKED:
			tmp = VID_PIXEL_MODE_RGB666_PACKED |
			      VID_DATATYPE(MIPI_DSI_PACKED_PIXEL_STREAM_18);
			break;

		case MIPI_DSI_FMT_RGB565:
			tmp = VID_PIXEL_MODE_RGB666_PACKED |
			      VID_DATATYPE(MIPI_DSI_PACKED_PIXEL_STREAM_16);
			break;

		default:
			dev_err(dsi->base.dev, "Unsupported DSI format\n");
			return;
		}

		if (output->dev->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
			tmp |= SYNC_PULSE_ACTIVE | SYNC_PULSE_HORIZONTAL;

		tmp |= REG_BLKLINE_MODE(REG_BLK_MODE_BLANKING_PKT) |
		       REG_BLKEOL_MODE(REG_BLK_MODE_BLANKING_PKT) |
		       RECOVERY_MODE(RECOVERY_MODE_NEXT_HSYNC);

		writel(tmp, dsi->regs + VID_MAIN_CTL);
	}

	tmp = readl(dsi->regs + MCTL_MAIN_DATA_CTL);
	tmp &= ~(IF_VID_SELECT_MASK | HOST_EOT_GEN | IF_VID_MODE);

	if (!(output->dev->mode_flags & MIPI_DSI_MODE_EOT_PACKET))
		tmp |= HOST_EOT_GEN;

	if (output->dev->mode_flags & MIPI_DSI_MODE_VIDEO)
		tmp |= IF_VID_MODE | IF_VID_SELECT(input->id) | VID_EN;

	writel(tmp, dsi->regs + MCTL_MAIN_DATA_CTL);

	tmp = readl(dsi->regs + MCTL_MAIN_EN) | IF_EN(input->id);
	writel(tmp, dsi->regs + MCTL_MAIN_EN);
}

static const struct drm_bridge_funcs cdns_dsi_bridge_funcs = {
	.attach = cdns_dsi_bridge_attach,
	.mode_valid = cdns_dsi_bridge_mode_valid,
	.disable = cdns_dsi_bridge_disable,
	.enable = cdns_dsi_bridge_enable,
};

static int cdns_dsi_init_link(struct cdns_dsi *dsi)
{
	struct cdns_dsi_output *output = &dsi->output;
	unsigned long sysclk_period, ulpout;
	u32 val;
	int i;

	writel(0, dsi->regs + MCTL_DPHY_STATIC);

	val = 0;
	for (i = 1; i < output->dev->lanes; i++)
		val |= DATA_LANE_EN(i);

	if (!(output->dev->mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS))
		val |= CLK_CONTINUOUS;

	writel(val, dsi->regs + MCTL_MAIN_PHY_CTL);

	/* ULPOUT should be set to 1ms and is expressed in sysclk cycles. */
	sysclk_period = NSEC_PER_SEC / clk_get_rate(dsi->sysclk);
	ulpout = DIV_ROUND_UP(NSEC_PER_MSEC, sysclk_period);
	writel(CLK_LANE_ULPOUT_TIME(ulpout) | DATA_LANE_ULPOUT_TIME(ulpout),
	       dsi->regs + MCTL_ULPOUT_TIME);

	writel(LINK_EN, dsi->regs + MCTL_MAIN_DATA_CTL);

	val = CLK_LANE_EN | PLL_START;
	for (i = 0; i < output->dev->lanes; i++)
		val |= DATA_LANE_START(i);

	writel(val, dsi->regs + MCTL_MAIN_EN);

	ndelay(100);

	return 0;
}

static int cdns_dsi_attach(struct mipi_dsi_host *host,
			   struct mipi_dsi_device *dev)
{
	struct cdns_dsi *dsi = to_cdns_dsi(host);
	struct cdns_dsi_output *output = &dsi->output;
	struct cdns_dsi_input *input = &dsi->input;
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	struct device_node *np;
	int ret;

	/*
	 * We currently do not support connecting several DSI devices to the
	 * same host. In order to support that we'd need the DRM bridge
	 * framework to allow dynamic reconfiguration of the bridge chain.
	 */
	if (output->dev)
		return -EBUSY;

	/* We do not support burst mode yet. */
	if (dev->mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
		return -ENOTSUPP;

	/*
	 * The host <-> device link might be described using an OF-graph
	 * representation, in this case we extract the device of_node from
	 * this representation, otherwise we use dsidev->dev.of_node which
	 * should have been filled by the core.
	 */
	np = of_graph_get_remote_node(dsi->base.dev->of_node, DSI_OUTPUT_PORT,
				      dev->channel);
	if (!np)
		np = of_node_get(dev->dev.of_node);

	panel = of_drm_find_panel(np);
	if (panel) {
		bridge = drm_panel_bridge_add(panel, DRM_MODE_CONNECTOR_DSI);
	} else {
		bridge = of_drm_find_bridge(dev->dev.of_node);
		if (!bridge)
			bridge = ERR_PTR(-EINVAL);
	}

	of_node_put(np);

	if (IS_ERR(bridge)) {
		ret = PTR_ERR(bridge);
		dev_err(host->dev, "failed to add DSI device %s (err = %d)",
			dev->name, ret);
		return ret;
	}

	output->dev = dev;
	output->bridge = bridge;
	output->panel = panel;

	ret = cdns_dsi_init_link(dsi);
	if (ret)
		goto err_rst_output;

	/*
	 * The DSI output has been properly configured, we can now safely
	 * register the input to the bridge framework so that it can take place
	 * in a display pipeline.
	 */
	drm_bridge_add(&input->bridge);

	return 0;

err_rst_output:
	if (panel)
		drm_panel_bridge_remove(bridge);

	output->bridge = NULL;
	output->panel = NULL;
	output->dev = NULL;

	return ret;
}

static int cdns_dsi_detach(struct mipi_dsi_host *host,
			   struct mipi_dsi_device *dev)
{
	struct cdns_dsi *dsi = to_cdns_dsi(host);
	struct cdns_dsi_output *output = &dsi->output;
	struct cdns_dsi_input *input = &dsi->input;

	drm_bridge_remove(&input->bridge);
	if (output->panel)
		drm_panel_bridge_remove(output->bridge);

	writel(0, dsi->regs + MCTL_MAIN_EN);
	writel(0, dsi->regs + MCTL_MAIN_DATA_CTL);
	writel(0, dsi->regs + MCTL_MAIN_PHY_CTL);

	return 0;
}

static irqreturn_t cdns_dsi_interrupt(int irq, void *data)
{
	struct cdns_dsi *dsi = data;
	irqreturn_t ret = IRQ_NONE;
	u32 flag, ctl;

	flag = readl(dsi->regs + DIRECT_CMD_STS_FLAG);
	if (flag) {
		ctl = readl(dsi->regs + DIRECT_CMD_STS_CTL);
		ctl &= ~flag;
		writel(ctl, dsi->regs + DIRECT_CMD_STS_CTL);
		complete(&dsi->direct_cmd_comp);
		ret = IRQ_HANDLED;
	}

	return ret;
}

static ssize_t cdns_dsi_transfer(struct mipi_dsi_host *host,
				 const struct mipi_dsi_msg *msg)
{
	struct cdns_dsi *dsi = to_cdns_dsi(host);
	u32 cmd, sts, val, wait = WRITE_COMPLETED, ctl = 0;
	struct mipi_dsi_packet packet;
	int ret, i, tx_len, rx_len;

	ret = mipi_dsi_create_packet(&packet, msg);
	if (ret)
		return ret;

	tx_len = msg->tx_buf ? msg->tx_len : 0;
	rx_len = msg->rx_buf ? msg->rx_len : 0;

	/* For read operations, the maximum TX len is 2. */
	if (rx_len && tx_len > 2)
		return -ENOTSUPP;

	/* TX len is limited by the CMD FIFO depth. */
	if (tx_len > dsi->direct_cmd_fifo_depth)
		return -ENOTSUPP;

	/* RX len is limited by the RX FIFO depth. */
	if (rx_len > dsi->rx_fifo_depth)
		return -ENOTSUPP;

	cmd = CMD_SIZE(tx_len) | CMD_VCHAN_ID(msg->channel) |
	      CMD_DATATYPE(msg->type);

	if (msg->flags & MIPI_DSI_MSG_USE_LPM)
		cmd |= CMD_LP_EN;

	if (mipi_dsi_packet_format_is_long(msg->type))
		cmd |= CMD_LONG;

	if (rx_len) {
		cmd |= READ_CMD;
		wait = READ_COMPLETED_WITH_ERR | READ_COMPLETED;
		ctl = READ_EN | BTA_EN;
	} else if (msg->flags & MIPI_DSI_MSG_REQ_ACK) {
		cmd |= BTA_REQ;
		wait = ACK_WITH_ERR_RCVD | ACK_RCVD;
		ctl = BTA_EN;
	}

	writel(readl(dsi->regs + MCTL_MAIN_DATA_CTL) | ctl,
	       dsi->regs + MCTL_MAIN_DATA_CTL);

	writel(cmd, dsi->regs + DIRECT_CMD_MAIN_SETTINGS);

	for (i = 0; i < tx_len; i += 4) {
		const u8 *buf = msg->tx_buf;
		int j;

		val = 0;
		for (j = 0; j < 4 && j + i < tx_len; j++)
			val |= (u32)buf[i + j] << (8 * j);

		writel(val, dsi->regs + DIRECT_CMD_WRDATA);
	}

	/* Clear status flags before sending the command. */
	writel(wait, dsi->regs + DIRECT_CMD_STS_CLR);
	writel(wait, dsi->regs + DIRECT_CMD_STS_CTL);
	reinit_completion(&dsi->direct_cmd_comp);
	writel(0, dsi->regs + DIRECT_CMD_SEND);

	wait_for_completion_timeout(&dsi->direct_cmd_comp,
				    msecs_to_jiffies(1000));

	sts = readl(dsi->regs + DIRECT_CMD_STS);
	writel(wait, dsi->regs + DIRECT_CMD_STS_CLR);
	writel(0, dsi->regs + DIRECT_CMD_STS_CTL);

	writel(readl(dsi->regs + MCTL_MAIN_DATA_CTL) & ~ctl,
	       dsi->regs + MCTL_MAIN_DATA_CTL);

	/* We did not receive the events we were waiting for. */
	if (!(sts & wait))
		return -ETIMEDOUT;

	/* READ of WRITE with ACK failed. */
	if (sts & (READ_COMPLETED_WITH_ERR | ACK_WITH_ERR_RCVD))
		return -EIO;

	for (i = 0; i < rx_len; i += 4) {
		u8 *buf = msg->rx_buf;
		int j;

		val = readl(dsi->regs + DIRECT_CMD_RDDATA);
		for (j = 0; j < 4 && j + i < rx_len; j++)
			buf[i + j] = val >> (8 * j);
	}

	return 0;
}

static const struct mipi_dsi_host_ops cdns_dsi_ops = {
	.attach = cdns_dsi_attach,
	.detach = cdns_dsi_detach,
	.transfer = cdns_dsi_transfer,
};

static int cdns_dsi_drm_probe(struct platform_device *pdev)
{
	struct cdns_dsi *dsi;
	struct cdns_dsi_input *input;
	struct resource *res;
	int ret, irq;
	u32 val;

	dsi = devm_kzalloc(&pdev->dev, sizeof(*dsi), GFP_KERNEL);
	if (!dsi)
		return -ENOMEM;

	input = &dsi->input;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dsi->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dsi->regs))
		return PTR_ERR(dsi->regs);

	dsi->pclk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(dsi->pclk))
		return PTR_ERR(dsi->pclk);

	dsi->sysclk = devm_clk_get(&pdev->dev, "sysclk");
	if (IS_ERR(dsi->sysclk))
		return PTR_ERR(dsi->sysclk);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = clk_prepare_enable(dsi->pclk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(dsi->sysclk);
	if (ret)
		goto err_dis_pclk;

	val = readl(dsi->regs + ID_REG);
	if (REV_VENDOR_ID(val) != 0xcad) {
		dev_err(&pdev->dev, "invalid vendor id\n");
		return -EINVAL;
	}

	val = readl(dsi->regs + IP_CONF);
	dsi->direct_cmd_fifo_depth = 1 << (DIRCMD_FIFO_DEPTH(val) + 2);
	dsi->rx_fifo_depth = RX_FIFO_DEPTH(val);
	init_completion(&dsi->direct_cmd_comp);

	writel(0, dsi->regs + MCTL_MAIN_DATA_CTL);
	writel(0, dsi->regs + MCTL_MAIN_EN);
	writel(0, dsi->regs + MCTL_MAIN_PHY_CTL);

	/*
	 * We only support the DPI input, so force input->id to
	 * CDNS_DPI_INPUT.
	 */
	input->id = CDNS_DPI_INPUT;
	input->bridge.funcs = &cdns_dsi_bridge_funcs;
	input->bridge.of_node = pdev->dev.of_node;

	/* Mask all interrupts before registering the IRQ handler. */
	writel(0, dsi->regs + MCTL_MAIN_STS_CTL);
	writel(0, dsi->regs + MCTL_DPHY_ERR_CTL1);
	writel(0, dsi->regs + CMD_MODE_STS_CTL);
	writel(0, dsi->regs + DIRECT_CMD_STS_CTL);
	writel(0, dsi->regs + DIRECT_CMD_RD_STS_CTL);
	writel(0, dsi->regs + VID_MODE_STS_CTL);
	writel(0, dsi->regs + TVG_TBG_STS_CTL);
	writel(0, dsi->regs + DPI_IRQ_EN);
	ret = devm_request_irq(&pdev->dev, irq, cdns_dsi_interrupt, 0,
			       dev_name(&pdev->dev), dsi);
	if (ret)
		goto err_dis_sysclk;

	dsi->base.dev = &pdev->dev;
	dsi->base.ops = &cdns_dsi_ops;

	ret = mipi_dsi_host_register(&dsi->base);
	if (ret)
		goto err_dis_sysclk;

	platform_set_drvdata(pdev, dsi);

	return 0;

err_dis_sysclk:
	clk_disable_unprepare(dsi->sysclk);

err_dis_pclk:
	clk_disable_unprepare(dsi->pclk);

	return ret;
}

static int cdns_dsi_drm_remove(struct platform_device *pdev)
{
	struct cdns_dsi *dsi = platform_get_drvdata(pdev);

	mipi_dsi_host_unregister(&dsi->base);
	clk_disable_unprepare(dsi->sysclk);
	clk_disable_unprepare(dsi->pclk);

	return 0;
}

static const struct of_device_id cdns_dsi_of_match[] = {
	{ .compatible = "cdns,dsi-1.3.1" },
	{ },
};

static struct platform_driver cdns_dsi_platform_driver = {
	.probe  = cdns_dsi_drm_probe,
	.remove = cdns_dsi_drm_remove,
	.driver = {
		.name   = "cdns-dsi",
		.of_match_table = cdns_dsi_of_match,
	},
};
module_platform_driver(cdns_dsi_platform_driver);

MODULE_AUTHOR("Boris Brezillon <boris.brezillon@free-electrons.com>");
MODULE_DESCRIPTION("Cadence DSI driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:cdns-dsi");

