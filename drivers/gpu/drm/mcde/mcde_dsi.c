// SPDX-License-Identifier: GPL-2.0+
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/component.h>
#include <linux/of.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <video/mipi_display.h>

#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_panel.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_print.h>
#include <drm/drm_bridge.h>
#include <drm/drm_of.h>

#include "mcde_drm.h"

#define DSI_DEFAULT_LP_FREQ_HZ	19200000
#define DSI_DEFAULT_HS_FREQ_HZ	420160000

#define DSI_MCTL_INTEGRATION_MODE 0x00000000

#define DSI_MCTL_MAIN_DATA_CTL 0x00000004
#define DSI_MCTL_MAIN_DATA_CTL_LINK_EN BIT(0)
#define DSI_MCTL_MAIN_DATA_CTL_IF1_MODE BIT(1)
#define DSI_MCTL_MAIN_DATA_CTL_VID_EN BIT(2)
#define DSI_MCTL_MAIN_DATA_CTL_TVG_SEL BIT(3)
#define DSI_MCTL_MAIN_DATA_CTL_TBG_SEL BIT(4)
#define DSI_MCTL_MAIN_DATA_CTL_IF1_TE_EN BIT(5)
#define DSI_MCTL_MAIN_DATA_CTL_IF2_TE_EN BIT(6)
#define DSI_MCTL_MAIN_DATA_CTL_REG_TE_EN BIT(7)
#define DSI_MCTL_MAIN_DATA_CTL_READ_EN BIT(8)
#define DSI_MCTL_MAIN_DATA_CTL_BTA_EN BIT(9)
#define DSI_MCTL_MAIN_DATA_CTL_DISP_GEN_ECC BIT(10)
#define DSI_MCTL_MAIN_DATA_CTL_DISP_GEN_CHECKSUM BIT(11)
#define DSI_MCTL_MAIN_DATA_CTL_HOST_EOT_GEN BIT(12)
#define DSI_MCTL_MAIN_DATA_CTL_DISP_EOT_GEN BIT(13)
#define DSI_MCTL_MAIN_DATA_CTL_DLX_REMAP_EN BIT(14)
#define DSI_MCTL_MAIN_DATA_CTL_TE_POLLING_EN BIT(15)

#define DSI_MCTL_MAIN_PHY_CTL 0x00000008
#define DSI_MCTL_MAIN_PHY_CTL_LANE2_EN BIT(0)
#define DSI_MCTL_MAIN_PHY_CTL_FORCE_STOP_MODE BIT(1)
#define DSI_MCTL_MAIN_PHY_CTL_CLK_CONTINUOUS BIT(2)
#define DSI_MCTL_MAIN_PHY_CTL_CLK_ULPM_EN BIT(3)
#define DSI_MCTL_MAIN_PHY_CTL_DAT1_ULPM_EN BIT(4)
#define DSI_MCTL_MAIN_PHY_CTL_DAT2_ULPM_EN BIT(5)
#define DSI_MCTL_MAIN_PHY_CTL_WAIT_BURST_TIME_SHIFT 6
#define DSI_MCTL_MAIN_PHY_CTL_WAIT_BURST_TIME_MASK 0x000003C0
#define DSI_MCTL_MAIN_PHY_CTL_CLOCK_FORCE_STOP_MODE BIT(10)

#define DSI_MCTL_PLL_CTL 0x0000000C
#define DSI_MCTL_LANE_STS 0x00000010

#define DSI_MCTL_DPHY_TIMEOUT 0x00000014
#define DSI_MCTL_DPHY_TIMEOUT_CLK_DIV_SHIFT 0
#define DSI_MCTL_DPHY_TIMEOUT_CLK_DIV_MASK 0x0000000F
#define DSI_MCTL_DPHY_TIMEOUT_HSTX_TO_VAL_SHIFT 4
#define DSI_MCTL_DPHY_TIMEOUT_HSTX_TO_VAL_MASK 0x0003FFF0
#define DSI_MCTL_DPHY_TIMEOUT_LPRX_TO_VAL_SHIFT 18
#define DSI_MCTL_DPHY_TIMEOUT_LPRX_TO_VAL_MASK 0xFFFC0000

#define DSI_MCTL_ULPOUT_TIME 0x00000018
#define DSI_MCTL_ULPOUT_TIME_CKLANE_ULPOUT_TIME_SHIFT 0
#define DSI_MCTL_ULPOUT_TIME_CKLANE_ULPOUT_TIME_MASK 0x000001FF
#define DSI_MCTL_ULPOUT_TIME_DATA_ULPOUT_TIME_SHIFT 9
#define DSI_MCTL_ULPOUT_TIME_DATA_ULPOUT_TIME_MASK 0x0003FE00

#define DSI_MCTL_DPHY_STATIC 0x0000001C
#define DSI_MCTL_DPHY_STATIC_SWAP_PINS_CLK BIT(0)
#define DSI_MCTL_DPHY_STATIC_HS_INVERT_CLK BIT(1)
#define DSI_MCTL_DPHY_STATIC_SWAP_PINS_DAT1 BIT(2)
#define DSI_MCTL_DPHY_STATIC_HS_INVERT_DAT1 BIT(3)
#define DSI_MCTL_DPHY_STATIC_SWAP_PINS_DAT2 BIT(4)
#define DSI_MCTL_DPHY_STATIC_HS_INVERT_DAT2 BIT(5)
#define DSI_MCTL_DPHY_STATIC_UI_X4_SHIFT 6
#define DSI_MCTL_DPHY_STATIC_UI_X4_MASK 0x00000FC0

#define DSI_MCTL_MAIN_EN 0x00000020
#define DSI_MCTL_MAIN_EN_PLL_START BIT(0)
#define DSI_MCTL_MAIN_EN_CKLANE_EN BIT(3)
#define DSI_MCTL_MAIN_EN_DAT1_EN BIT(4)
#define DSI_MCTL_MAIN_EN_DAT2_EN BIT(5)
#define DSI_MCTL_MAIN_EN_CLKLANE_ULPM_REQ BIT(6)
#define DSI_MCTL_MAIN_EN_DAT1_ULPM_REQ BIT(7)
#define DSI_MCTL_MAIN_EN_DAT2_ULPM_REQ BIT(8)
#define DSI_MCTL_MAIN_EN_IF1_EN BIT(9)
#define DSI_MCTL_MAIN_EN_IF2_EN BIT(10)

#define DSI_MCTL_MAIN_STS 0x00000024
#define DSI_MCTL_MAIN_STS_PLL_LOCK BIT(0)
#define DSI_MCTL_MAIN_STS_CLKLANE_READY BIT(1)
#define DSI_MCTL_MAIN_STS_DAT1_READY BIT(2)
#define DSI_MCTL_MAIN_STS_DAT2_READY BIT(3)
#define DSI_MCTL_MAIN_STS_HSTX_TO_ERR BIT(4)
#define DSI_MCTL_MAIN_STS_LPRX_TO_ERR BIT(5)
#define DSI_MCTL_MAIN_STS_CRS_UNTERM_PCK BIT(6)
#define DSI_MCTL_MAIN_STS_VRS_UNTERM_PCK BIT(7)

#define DSI_MCTL_DPHY_ERR 0x00000028
#define DSI_INT_VID_RDDATA 0x00000030
#define DSI_INT_VID_GNT 0x00000034
#define DSI_INT_CMD_RDDATA 0x00000038
#define DSI_INT_CMD_GNT 0x0000003C
#define DSI_INT_INTERRUPT_CTL 0x00000040

#define DSI_CMD_MODE_CTL 0x00000050
#define DSI_CMD_MODE_CTL_IF1_ID_SHIFT 0
#define DSI_CMD_MODE_CTL_IF1_ID_MASK 0x00000003
#define DSI_CMD_MODE_CTL_IF2_ID_SHIFT 2
#define DSI_CMD_MODE_CTL_IF2_ID_MASK 0x0000000C
#define DSI_CMD_MODE_CTL_IF1_LP_EN BIT(4)
#define DSI_CMD_MODE_CTL_IF2_LP_EN BIT(5)
#define DSI_CMD_MODE_CTL_ARB_MODE BIT(6)
#define DSI_CMD_MODE_CTL_ARB_PRI BIT(7)
#define DSI_CMD_MODE_CTL_FIL_VALUE_SHIFT 8
#define DSI_CMD_MODE_CTL_FIL_VALUE_MASK 0x0000FF00
#define DSI_CMD_MODE_CTL_TE_TIMEOUT_SHIFT 16
#define DSI_CMD_MODE_CTL_TE_TIMEOUT_MASK 0x03FF0000

#define DSI_CMD_MODE_STS 0x00000054
#define DSI_CMD_MODE_STS_ERR_NO_TE BIT(0)
#define DSI_CMD_MODE_STS_ERR_TE_MISS BIT(1)
#define DSI_CMD_MODE_STS_ERR_SDI1_UNDERRUN BIT(2)
#define DSI_CMD_MODE_STS_ERR_SDI2_UNDERRUN BIT(3)
#define DSI_CMD_MODE_STS_ERR_UNWANTED_RD BIT(4)
#define DSI_CMD_MODE_STS_CSM_RUNNING BIT(5)

#define DSI_DIRECT_CMD_SEND 0x00000060

#define DSI_DIRECT_CMD_MAIN_SETTINGS 0x00000064
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_NAT_SHIFT 0
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_NAT_MASK 0x00000007
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_NAT_WRITE 0
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_NAT_READ 1
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_NAT_TE_REQ 4
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_NAT_TRIG_REQ 5
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_NAT_BTA_REQ 6
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_LONGNOTSHORT BIT(3)
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_SHIFT 8
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_MASK 0x00003F00
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_TURN_ON_PERIPHERAL 50
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_SHUT_DOWN_PERIPHERAL 34
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_GENERIC_SHORT_WRITE_0 3
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_GENERIC_SHORT_WRITE_1 19
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_GENERIC_SHORT_WRITE_2 35
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_GENERIC_LONG_WRITE 41
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_DCS_SHORT_WRITE_0 5
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_DCS_SHORT_WRITE_1 21
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_DCS_LONG_WRITE 57
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_DCS_READ 6
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_SET_MAX_PKT_SIZE 55
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_ID_SHIFT 14
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_SIZE_SHIFT 16
#define DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_LP_EN BIT(21)
#define DSI_DIRECT_CMD_MAIN_SETTINGS_TRIGGER_VAL_SHIFT 24
#define DSI_DIRECT_CMD_MAIN_SETTINGS_TRIGGER_VAL_MASK 0x0F000000

#define DSI_DIRECT_CMD_STS 0x00000068
#define DSI_DIRECT_CMD_STS_CMD_TRANSMISSION BIT(0)
#define DSI_DIRECT_CMD_STS_WRITE_COMPLETED BIT(1)
#define DSI_DIRECT_CMD_STS_TRIGGER_COMPLETED BIT(2)
#define DSI_DIRECT_CMD_STS_READ_COMPLETED BIT(3)
#define DSI_DIRECT_CMD_STS_ACKNOWLEDGE_RECEIVED_SHIFT BIT(4)
#define DSI_DIRECT_CMD_STS_ACKNOWLEDGE_WITH_ERR_RECEIVED BIT(5)
#define DSI_DIRECT_CMD_STS_TRIGGER_RECEIVED BIT(6)
#define DSI_DIRECT_CMD_STS_TE_RECEIVED BIT(7)
#define DSI_DIRECT_CMD_STS_BTA_COMPLETED BIT(8)
#define DSI_DIRECT_CMD_STS_BTA_FINISHED BIT(9)
#define DSI_DIRECT_CMD_STS_READ_COMPLETED_WITH_ERR BIT(10)
#define DSI_DIRECT_CMD_STS_TRIGGER_VAL_MASK 0x00007800
#define DSI_DIRECT_CMD_STS_TRIGGER_VAL_SHIFT 11
#define DSI_DIRECT_CMD_STS_ACK_VAL_SHIFT 16
#define DSI_DIRECT_CMD_STS_ACK_VAL_MASK 0xFFFF0000

#define DSI_DIRECT_CMD_RD_INIT 0x0000006C
#define DSI_DIRECT_CMD_RD_INIT_RESET_SHIFT 0
#define DSI_DIRECT_CMD_RD_INIT_RESET_MASK 0xFFFFFFFF

#define DSI_DIRECT_CMD_WRDAT0 0x00000070
#define DSI_DIRECT_CMD_WRDAT1 0x00000074
#define DSI_DIRECT_CMD_WRDAT2 0x00000078
#define DSI_DIRECT_CMD_WRDAT3 0x0000007C

#define DSI_DIRECT_CMD_RDDAT 0x00000080

#define DSI_DIRECT_CMD_RD_PROPERTY 0x00000084
#define DSI_DIRECT_CMD_RD_PROPERTY_RD_SIZE_SHIFT 0
#define DSI_DIRECT_CMD_RD_PROPERTY_RD_SIZE_MASK 0x0000FFFF
#define DSI_DIRECT_CMD_RD_PROPERTY_RD_ID_SHIFT 16
#define DSI_DIRECT_CMD_RD_PROPERTY_RD_ID_MASK 0x00030000
#define DSI_DIRECT_CMD_RD_PROPERTY_RD_DCSNOTGENERIC_SHIFT 18
#define DSI_DIRECT_CMD_RD_PROPERTY_RD_DCSNOTGENERIC_MASK 0x00040000

#define DSI_DIRECT_CMD_RD_STS 0x00000088

#define DSI_VID_MAIN_CTL 0x00000090
#define DSI_VID_MAIN_CTL_START_MODE_SHIFT 0
#define DSI_VID_MAIN_CTL_START_MODE_MASK 0x00000003
#define DSI_VID_MAIN_CTL_STOP_MODE_SHIFT 2
#define DSI_VID_MAIN_CTL_STOP_MODE_MASK 0x0000000C
#define DSI_VID_MAIN_CTL_VID_ID_SHIFT 4
#define DSI_VID_MAIN_CTL_VID_ID_MASK 0x00000030
#define DSI_VID_MAIN_CTL_HEADER_SHIFT 6
#define DSI_VID_MAIN_CTL_HEADER_MASK 0x00000FC0
#define DSI_VID_MAIN_CTL_VID_PIXEL_MODE_16BITS 0
#define DSI_VID_MAIN_CTL_VID_PIXEL_MODE_18BITS BIT(12)
#define DSI_VID_MAIN_CTL_VID_PIXEL_MODE_18BITS_LOOSE BIT(13)
#define DSI_VID_MAIN_CTL_VID_PIXEL_MODE_24BITS (BIT(12) | BIT(13))
#define DSI_VID_MAIN_CTL_BURST_MODE BIT(14)
#define DSI_VID_MAIN_CTL_SYNC_PULSE_ACTIVE BIT(15)
#define DSI_VID_MAIN_CTL_SYNC_PULSE_HORIZONTAL BIT(16)
#define DSI_VID_MAIN_CTL_REG_BLKLINE_MODE_NULL 0
#define DSI_VID_MAIN_CTL_REG_BLKLINE_MODE_BLANKING BIT(17)
#define DSI_VID_MAIN_CTL_REG_BLKLINE_MODE_LP_0 BIT(18)
#define DSI_VID_MAIN_CTL_REG_BLKLINE_MODE_LP_1 (BIT(17) | BIT(18))
#define DSI_VID_MAIN_CTL_REG_BLKEOL_MODE_NULL 0
#define DSI_VID_MAIN_CTL_REG_BLKEOL_MODE_BLANKING BIT(19)
#define DSI_VID_MAIN_CTL_REG_BLKEOL_MODE_LP_0 BIT(20)
#define DSI_VID_MAIN_CTL_REG_BLKEOL_MODE_LP_1 (BIT(19) | BIT(20))
#define DSI_VID_MAIN_CTL_RECOVERY_MODE_SHIFT 21
#define DSI_VID_MAIN_CTL_RECOVERY_MODE_MASK 0x00600000

#define DSI_VID_VSIZE 0x00000094
#define DSI_VID_VSIZE_VSA_LENGTH_SHIFT 0
#define DSI_VID_VSIZE_VSA_LENGTH_MASK 0x0000003F
#define DSI_VID_VSIZE_VBP_LENGTH_SHIFT 6
#define DSI_VID_VSIZE_VBP_LENGTH_MASK 0x00000FC0
#define DSI_VID_VSIZE_VFP_LENGTH_SHIFT 12
#define DSI_VID_VSIZE_VFP_LENGTH_MASK 0x000FF000
#define DSI_VID_VSIZE_VACT_LENGTH_SHIFT 20
#define DSI_VID_VSIZE_VACT_LENGTH_MASK 0x7FF00000

#define DSI_VID_HSIZE1 0x00000098
#define DSI_VID_HSIZE1_HSA_LENGTH_SHIFT 0
#define DSI_VID_HSIZE1_HSA_LENGTH_MASK 0x000003FF
#define DSI_VID_HSIZE1_HBP_LENGTH_SHIFT 10
#define DSI_VID_HSIZE1_HBP_LENGTH_MASK 0x000FFC00
#define DSI_VID_HSIZE1_HFP_LENGTH_SHIFT 20
#define DSI_VID_HSIZE1_HFP_LENGTH_MASK 0x7FF00000

#define DSI_VID_HSIZE2 0x0000009C
#define DSI_VID_HSIZE2_RGB_SIZE_SHIFT 0
#define DSI_VID_HSIZE2_RGB_SIZE_MASK 0x00001FFF

#define DSI_VID_BLKSIZE1 0x000000A0
#define DSI_VID_BLKSIZE1_BLKLINE_EVENT_PCK_SHIFT 0
#define DSI_VID_BLKSIZE1_BLKLINE_EVENT_PCK_MASK 0x00001FFF
#define DSI_VID_BLKSIZE1_BLKEOL_PCK_SHIFT 13
#define DSI_VID_BLKSIZE1_BLKEOL_PCK_MASK 0x03FFE000

#define DSI_VID_BLKSIZE2 0x000000A4
#define DSI_VID_BLKSIZE2_BLKLINE_PULSE_PCK_SHIFT 0
#define DSI_VID_BLKSIZE2_BLKLINE_PULSE_PCK_MASK 0x00001FFF

#define DSI_VID_PCK_TIME 0x000000A8
#define DSI_VID_PCK_TIME_BLKEOL_DURATION_SHIFT 0

#define DSI_VID_DPHY_TIME 0x000000AC
#define DSI_VID_DPHY_TIME_REG_LINE_DURATION_SHIFT 0
#define DSI_VID_DPHY_TIME_REG_LINE_DURATION_MASK 0x00001FFF
#define DSI_VID_DPHY_TIME_REG_WAKEUP_TIME_SHIFT 13
#define DSI_VID_DPHY_TIME_REG_WAKEUP_TIME_MASK 0x00FFE000

#define DSI_VID_MODE_STS 0x000000BC
#define DSI_VID_MODE_STS_VSG_RUNNING BIT(0)

#define DSI_VID_VCA_SETTING1 0x000000C0
#define DSI_VID_VCA_SETTING1_MAX_BURST_LIMIT_SHIFT 0
#define DSI_VID_VCA_SETTING1_MAX_BURST_LIMIT_MASK 0x0000FFFF
#define DSI_VID_VCA_SETTING1_BURST_LP BIT(16)

#define DSI_VID_VCA_SETTING2 0x000000C4
#define DSI_VID_VCA_SETTING2_EXACT_BURST_LIMIT_SHIFT 0
#define DSI_VID_VCA_SETTING2_EXACT_BURST_LIMIT_MASK 0x0000FFFF
#define DSI_VID_VCA_SETTING2_MAX_LINE_LIMIT_SHIFT 16
#define DSI_VID_VCA_SETTING2_MAX_LINE_LIMIT_MASK 0xFFFF0000

#define DSI_CMD_MODE_STS_CTL 0x000000F4
#define DSI_CMD_MODE_STS_CTL_ERR_NO_TE_EN BIT(0)
#define DSI_CMD_MODE_STS_CTL_ERR_TE_MISS_EN BIT(1)
#define DSI_CMD_MODE_STS_CTL_ERR_SDI1_UNDERRUN_EN BIT(2)
#define DSI_CMD_MODE_STS_CTL_ERR_SDI2_UNDERRUN_EN BIT(3)
#define DSI_CMD_MODE_STS_CTL_ERR_UNWANTED_RD_EN BIT(4)
#define DSI_CMD_MODE_STS_CTL_CSM_RUNNING_EN BIT(5)
#define DSI_CMD_MODE_STS_CTL_ERR_NO_TE_EDGE BIT(16)
#define DSI_CMD_MODE_STS_CTL_ERR_TE_MISS_EDGE BIT(17)
#define DSI_CMD_MODE_STS_CTL_ERR_SDI1_UNDERRUN_EDGE BIT(18)
#define DSI_CMD_MODE_STS_CTL_ERR_SDI2_UNDERRUN_EDGE BIT(19)
#define DSI_CMD_MODE_STS_CTL_ERR_UNWANTED_RD_EDGE BIT(20)
#define DSI_CMD_MODE_STS_CTL_CSM_RUNNING_EDGE BIT(21)

#define DSI_DIRECT_CMD_STS_CTL 0x000000F8
#define DSI_DIRECT_CMD_STS_CTL_CMD_TRANSMISSION_EN BIT(0)
#define DSI_DIRECT_CMD_STS_CTL_WRITE_COMPLETED_EN BIT(1)
#define DSI_DIRECT_CMD_STS_CTL_TRIGGER_COMPLETED_EN BIT(2)
#define DSI_DIRECT_CMD_STS_CTL_READ_COMPLETED_EN BIT(3)
#define DSI_DIRECT_CMD_STS_CTL_ACKNOWLEDGE_RECEIVED_EN BIT(4)
#define DSI_DIRECT_CMD_STS_CTL_ACKNOWLEDGE_WITH_ERR_EN BIT(5)
#define DSI_DIRECT_CMD_STS_CTL_TRIGGER_RECEIVED_EN BIT(6)
#define DSI_DIRECT_CMD_STS_CTL_TE_RECEIVED_EN BIT(7)
#define DSI_DIRECT_CMD_STS_CTL_BTA_COMPLETED_EN BIT(8)
#define DSI_DIRECT_CMD_STS_CTL_BTA_FINISHED_EN BIT(9)
#define DSI_DIRECT_CMD_STS_CTL_READ_COMPLETED_WITH_ERR_EN BIT(10)
#define DSI_DIRECT_CMD_STS_CTL_CMD_TRANSMISSION_EDGE BIT(16)
#define DSI_DIRECT_CMD_STS_CTL_WRITE_COMPLETED_EDGE BIT(17)
#define DSI_DIRECT_CMD_STS_CTL_TRIGGER_COMPLETED_EDGE BIT(18)
#define DSI_DIRECT_CMD_STS_CTL_READ_COMPLETED_EDGE BIT(19)
#define DSI_DIRECT_CMD_STS_CTL_ACKNOWLEDGE_RECEIVED_EDGE BIT(20)
#define DSI_DIRECT_CMD_STS_CTL_ACKNOWLEDGE_WITH_ERR_EDGE BIT(21)
#define DSI_DIRECT_CMD_STS_CTL_TRIGGER_RECEIVED_EDGE BIT(22)
#define DSI_DIRECT_CMD_STS_CTL_TE_RECEIVED_EDGE BIT(23)
#define DSI_DIRECT_CMD_STS_CTL_BTA_COMPLETED_EDGE BIT(24)
#define DSI_DIRECT_CMD_STS_CTL_BTA_FINISHED_EDGE BIT(25)
#define DSI_DIRECT_CMD_STS_CTL_READ_COMPLETED_WITH_ERR_EDGE BIT(26)

#define DSI_VID_MODE_STS_CTL 0x00000100
#define DSI_VID_MODE_STS_CTL_VSG_RUNNING BIT(0)
#define DSI_VID_MODE_STS_CTL_ERR_MISSING_DATA BIT(1)
#define DSI_VID_MODE_STS_CTL_ERR_MISSING_HSYNC BIT(2)
#define DSI_VID_MODE_STS_CTL_ERR_MISSING_VSYNC BIT(3)
#define DSI_VID_MODE_STS_CTL_REG_ERR_SMALL_LENGTH BIT(4)
#define DSI_VID_MODE_STS_CTL_REG_ERR_SMALL_HEIGHT BIT(5)
#define DSI_VID_MODE_STS_CTL_ERR_BURSTWRITE BIT(6)
#define DSI_VID_MODE_STS_CTL_ERR_LONGWRITE BIT(7)
#define DSI_VID_MODE_STS_CTL_ERR_LONGREAD BIT(8)
#define DSI_VID_MODE_STS_CTL_ERR_VRS_WRONG_LENGTH BIT(9)
#define DSI_VID_MODE_STS_CTL_VSG_RUNNING_EDGE BIT(16)
#define DSI_VID_MODE_STS_CTL_ERR_MISSING_DATA_EDGE BIT(17)
#define DSI_VID_MODE_STS_CTL_ERR_MISSING_HSYNC_EDGE BIT(18)
#define DSI_VID_MODE_STS_CTL_ERR_MISSING_VSYNC_EDGE BIT(19)
#define DSI_VID_MODE_STS_CTL_REG_ERR_SMALL_LENGTH_EDGE BIT(20)
#define DSI_VID_MODE_STS_CTL_REG_ERR_SMALL_HEIGHT_EDGE BIT(21)
#define DSI_VID_MODE_STS_CTL_ERR_BURSTWRITE_EDGE BIT(22)
#define DSI_VID_MODE_STS_CTL_ERR_LONGWRITE_EDGE BIT(23)
#define DSI_VID_MODE_STS_CTL_ERR_LONGREAD_EDGE BIT(24)
#define DSI_VID_MODE_STS_CTL_ERR_VRS_WRONG_LENGTH_EDGE BIT(25)
#define DSI_VID_MODE_STS_CTL_VSG_RECOVERY_EDGE BIT(26)

#define DSI_TG_STS_CTL 0x00000104
#define DSI_MCTL_DHPY_ERR_CTL 0x00000108
#define DSI_MCTL_MAIN_STS_CLR 0x00000110

#define DSI_CMD_MODE_STS_CLR 0x00000114
#define DSI_CMD_MODE_STS_CLR_ERR_NO_TE_CLR BIT(0)
#define DSI_CMD_MODE_STS_CLR_ERR_TE_MISS_CLR BIT(1)
#define DSI_CMD_MODE_STS_CLR_ERR_SDI1_UNDERRUN_CLR BIT(2)
#define DSI_CMD_MODE_STS_CLR_ERR_SDI2_UNDERRUN_CLR BIT(3)
#define DSI_CMD_MODE_STS_CLR_ERR_UNWANTED_RD_CLR BIT(4)
#define DSI_CMD_MODE_STS_CLR_CSM_RUNNING_CLR BIT(5)

#define DSI_DIRECT_CMD_STS_CLR 0x00000118
#define DSI_DIRECT_CMD_STS_CLR_CMD_TRANSMISSION_CLR BIT(0)
#define DSI_DIRECT_CMD_STS_CLR_WRITE_COMPLETED_CLR BIT(1)
#define DSI_DIRECT_CMD_STS_CLR_TRIGGER_COMPLETED_CLR BIT(2)
#define DSI_DIRECT_CMD_STS_CLR_READ_COMPLETED_CLR BIT(3)
#define DSI_DIRECT_CMD_STS_CLR_ACKNOWLEDGE_RECEIVED_CLR BIT(4)
#define DSI_DIRECT_CMD_STS_CLR_ACKNOWLEDGE_WITH_ERR_RECEIVED_CLR BIT(5)
#define DSI_DIRECT_CMD_STS_CLR_TRIGGER_RECEIVED_CLR BIT(6)
#define DSI_DIRECT_CMD_STS_CLR_TE_RECEIVED_CLR BIT(7)
#define DSI_DIRECT_CMD_STS_CLR_BTA_COMPLETED_CLR BIT(8)
#define DSI_DIRECT_CMD_STS_CLR_BTA_FINISHED_CLR BIT(9)
#define DSI_DIRECT_CMD_STS_CLR_READ_COMPLETED_WITH_ERR_CLR BIT(10)

#define DSI_DIRECT_CMD_RD_STS_CLR 0x0000011C
#define DSI_VID_MODE_STS_CLR 0x00000120
#define DSI_TG_STS_CLR 0x00000124
#define DSI_MCTL_DPHY_ERR_CLR 0x00000128
#define DSI_MCTL_MAIN_STS_FLAG 0x00000130
#define DSI_CMD_MODE_STS_FLAG 0x00000134
#define DSI_DIRECT_CMD_STS_FLAG 0x00000138
#define DSI_DIRECT_CMD_RD_STS_FLAG 0x0000013C
#define DSI_VID_MODE_STS_FLAG 0x00000140
#define DSI_TG_STS_FLAG 0x00000144

#define DSI_DPHY_LANES_TRIM 0x00000150
#define DSI_DPHY_LANES_TRIM_DPHY_SKEW_DAT1_SHIFT 0
#define DSI_DPHY_LANES_TRIM_DPHY_SKEW_DAT1_MASK 0x00000003
#define DSI_DPHY_LANES_TRIM_DPHY_CD_OFF_DAT1 BIT(2)
#define DSI_DPHY_LANES_TRIM_DPHY_HSTX_SLEWRATE_UP_DAT1 BIT(3)
#define DSI_DPHY_LANES_TRIM_DPHY_HSTX_SLEWRATE_DOWN_DAT1 BIT(4)
#define DSI_DPHY_LANES_TRIM_DPHY_TEST_RESERVED_1_DAT1 BIT(5)
#define DSI_DPHY_LANES_TRIM_DPHY_SKEW_CLK_SHIFT 6
#define DSI_DPHY_LANES_TRIM_DPHY_SKEW_CLK_MASK 0x000000C0
#define DSI_DPHY_LANES_TRIM_DPHY_LP_RX_VIL_CLK_SHIFT 8
#define DSI_DPHY_LANES_TRIM_DPHY_LP_RX_VIL_CLK_MASK 0x00000300
#define DSI_DPHY_LANES_TRIM_DPHY_LP_TX_SLEWRATE_CLK_SHIFT 10
#define DSI_DPHY_LANES_TRIM_DPHY_LP_TX_SLEWRATE_CLK_MASK 0x00000C00
#define DSI_DPHY_LANES_TRIM_DPHY_SPECS_90_81B_0_81 0
#define DSI_DPHY_LANES_TRIM_DPHY_SPECS_90_81B_0_90 BIT(12)
#define DSI_DPHY_LANES_TRIM_DPHY_HSTX_SLEWRATE_UP_CLK BIT(13)
#define DSI_DPHY_LANES_TRIM_DPHY_HSTX_SLEWRATE_DOWN_CLK BIT(14)
#define DSI_DPHY_LANES_TRIM_DPHY_TEST_RESERVED_1_CLK BIT(15)
#define DSI_DPHY_LANES_TRIM_DPHY_SKEW_DAT2 BIT(16)
#define DSI_DPHY_LANES_TRIM_DPHY_HSTX_SLEWRATE_UP_DAT2 BIT(18)
#define DSI_DPHY_LANES_TRIM_DPHY_HSTX_SLEWRATE_DOWN_DAT2 BIT(19)
#define DSI_DPHY_LANES_TRIM_DPHY_TEST_RESERVED_1_DAT2 BIT(20)

#define DSI_ID_REG	0x00000FF0

/* PRCMU DSI reset registers */
#define PRCM_DSI_SW_RESET 0x324
#define PRCM_DSI_SW_RESET_DSI0_SW_RESETN BIT(0)
#define PRCM_DSI_SW_RESET_DSI1_SW_RESETN BIT(1)
#define PRCM_DSI_SW_RESET_DSI2_SW_RESETN BIT(2)

struct mcde_dsi {
	struct device *dev;
	struct mcde *mcde;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_panel *panel;
	struct drm_bridge *bridge;
	struct mipi_dsi_host dsi_host;
	struct mipi_dsi_device *mdsi;
	unsigned long hs_freq;
	unsigned long lp_freq;
	bool unused;

	void __iomem *regs;
	struct regmap *prcmu;
};

static inline struct mcde_dsi *encoder_to_mcde_dsi(struct drm_encoder *e)
{
	return container_of(e, struct mcde_dsi, encoder);
}

static inline struct mcde_dsi *host_to_mcde_dsi(struct mipi_dsi_host *h)
{
	return container_of(h, struct mcde_dsi, dsi_host);
}

static inline struct mcde_dsi *connector_to_mcde_dsi(struct drm_connector *c)
{
	return container_of(c, struct mcde_dsi, connector);
}

bool mcde_dsi_irq(struct mipi_dsi_device *mdsi)
{
	struct mcde_dsi *d;
	u32 val;
	bool te_received = false;

	d = host_to_mcde_dsi(mdsi->host);

	dev_dbg(d->dev, "%s called\n", __func__);

	val = readl(d->regs + DSI_DIRECT_CMD_STS_FLAG);
	if (val)
		dev_dbg(d->dev, "DSI_DIRECT_CMD_STS_FLAG = %08x\n", val);
	if (val & DSI_DIRECT_CMD_STS_WRITE_COMPLETED)
		dev_dbg(d->dev, "direct command write completed\n");
	if (val & DSI_DIRECT_CMD_STS_TE_RECEIVED) {
		te_received = true;
		dev_dbg(d->dev, "direct command TE received\n");
	}
	if (val & DSI_DIRECT_CMD_STS_ACKNOWLEDGE_WITH_ERR_RECEIVED)
		dev_err(d->dev, "direct command ACK ERR received\n");
	if (val & DSI_DIRECT_CMD_STS_READ_COMPLETED_WITH_ERR)
		dev_err(d->dev, "direct command read ERR received\n");
	/* Mask off the ACK value and clear status */
	writel(val, d->regs + DSI_DIRECT_CMD_STS_CLR);

	val = readl(d->regs + DSI_CMD_MODE_STS_FLAG);
	if (val)
		dev_dbg(d->dev, "DSI_CMD_MODE_STS_FLAG = %08x\n", val);
	if (val & DSI_CMD_MODE_STS_ERR_NO_TE)
		/* This happens all the time (safe to ignore) */
		dev_dbg(d->dev, "CMD mode no TE\n");
	if (val & DSI_CMD_MODE_STS_ERR_TE_MISS)
		/* This happens all the time (safe to ignore) */
		dev_dbg(d->dev, "CMD mode TE miss\n");
	if (val & DSI_CMD_MODE_STS_ERR_SDI1_UNDERRUN)
		dev_err(d->dev, "CMD mode SD1 underrun\n");
	if (val & DSI_CMD_MODE_STS_ERR_SDI2_UNDERRUN)
		dev_err(d->dev, "CMD mode SD2 underrun\n");
	if (val & DSI_CMD_MODE_STS_ERR_UNWANTED_RD)
		dev_err(d->dev, "CMD mode unwanted RD\n");
	writel(val, d->regs + DSI_CMD_MODE_STS_CLR);

	val = readl(d->regs + DSI_DIRECT_CMD_RD_STS_FLAG);
	if (val)
		dev_dbg(d->dev, "DSI_DIRECT_CMD_RD_STS_FLAG = %08x\n", val);
	writel(val, d->regs + DSI_DIRECT_CMD_RD_STS_CLR);

	val = readl(d->regs + DSI_TG_STS_FLAG);
	if (val)
		dev_dbg(d->dev, "DSI_TG_STS_FLAG = %08x\n", val);
	writel(val, d->regs + DSI_TG_STS_CLR);

	val = readl(d->regs + DSI_VID_MODE_STS_FLAG);
	if (val)
		dev_err(d->dev, "some video mode error status\n");
	writel(val, d->regs + DSI_VID_MODE_STS_CLR);

	return te_received;
}

static int mcde_dsi_host_attach(struct mipi_dsi_host *host,
				struct mipi_dsi_device *mdsi)
{
	struct mcde_dsi *d = host_to_mcde_dsi(host);

	if (mdsi->lanes < 1 || mdsi->lanes > 2) {
		DRM_ERROR("dsi device params invalid, 1 or 2 lanes supported\n");
		return -EINVAL;
	}

	dev_info(d->dev, "attached DSI device with %d lanes\n", mdsi->lanes);
	/* MIPI_DSI_FMT_RGB88 etc */
	dev_info(d->dev, "format %08x, %dbpp\n", mdsi->format,
		 mipi_dsi_pixel_format_to_bpp(mdsi->format));
	dev_info(d->dev, "mode flags: %08lx\n", mdsi->mode_flags);

	d->mdsi = mdsi;
	if (d->mcde)
		d->mcde->mdsi = mdsi;

	return 0;
}

static int mcde_dsi_host_detach(struct mipi_dsi_host *host,
				struct mipi_dsi_device *mdsi)
{
	struct mcde_dsi *d = host_to_mcde_dsi(host);

	d->mdsi = NULL;
	if (d->mcde)
		d->mcde->mdsi = NULL;

	return 0;
}

#define MCDE_DSI_HOST_IS_READ(type)			    \
	((type == MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM) || \
	 (type == MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM) || \
	 (type == MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM) || \
	 (type == MIPI_DSI_DCS_READ))

static ssize_t mcde_dsi_host_transfer(struct mipi_dsi_host *host,
				      const struct mipi_dsi_msg *msg)
{
	struct mcde_dsi *d = host_to_mcde_dsi(host);
	const u32 loop_delay_us = 10; /* us */
	const u8 *tx = msg->tx_buf;
	u32 loop_counter;
	size_t txlen;
	u32 val;
	int ret;
	int i;

	txlen = msg->tx_len;
	if (txlen > 12) {
		dev_err(d->dev,
			"dunno how to write more than 12 bytes yet\n");
		return -EIO;
	}

	dev_dbg(d->dev,
		"message to channel %d, %u bytes",
		msg->channel,
		txlen);

	/* Command "nature" */
	if (MCDE_DSI_HOST_IS_READ(msg->type))
		/* MCTL_MAIN_DATA_CTL already set up */
		val = DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_NAT_READ;
	else
		val = DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_NAT_WRITE;
	/*
	 * More than 2 bytes will not fit in a single packet, so it's
	 * time to set the "long not short" bit. One byte is used by
	 * the MIPI DCS command leaving just one byte for the payload
	 * in a short package.
	 */
	if (mipi_dsi_packet_format_is_long(msg->type))
		val |= DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_LONGNOTSHORT;
	val |= 0 << DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_ID_SHIFT;
	/* Add one to the length for the MIPI DCS command */
	val |= txlen
		<< DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_SIZE_SHIFT;
	val |= DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_LP_EN;
	val |= msg->type << DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_SHIFT;
	writel(val, d->regs + DSI_DIRECT_CMD_MAIN_SETTINGS);

	/* MIPI DCS command is part of the data */
	if (txlen > 0) {
		val = 0;
		for (i = 0; i < 4 && i < txlen; i++)
			val |= tx[i] << (i & 3) * 8;
	}
	writel(val, d->regs + DSI_DIRECT_CMD_WRDAT0);
	if (txlen > 4) {
		val = 0;
		for (i = 0; i < 4 && (i + 4) < txlen; i++)
			val |= tx[i + 4] << (i & 3) * 8;
		writel(val, d->regs + DSI_DIRECT_CMD_WRDAT1);
	}
	if (txlen > 8) {
		val = 0;
		for (i = 0; i < 4 && (i + 8) < txlen; i++)
			val |= tx[i + 8] << (i & 3) * 8;
		writel(val, d->regs + DSI_DIRECT_CMD_WRDAT2);
	}
	if (txlen > 12) {
		val = 0;
		for (i = 0; i < 4 && (i + 12) < txlen; i++)
			val |= tx[i + 12] << (i & 3) * 8;
		writel(val, d->regs + DSI_DIRECT_CMD_WRDAT3);
	}

	writel(~0, d->regs + DSI_DIRECT_CMD_STS_CLR);
	writel(~0, d->regs + DSI_CMD_MODE_STS_CLR);
	/* Send command */
	writel(1, d->regs + DSI_DIRECT_CMD_SEND);

	loop_counter = 1000 * 1000 / loop_delay_us;
	while (!(readl(d->regs + DSI_DIRECT_CMD_STS) &
		 DSI_DIRECT_CMD_STS_WRITE_COMPLETED)
	       && --loop_counter)
		usleep_range(loop_delay_us, (loop_delay_us * 3) / 2);

	if (!loop_counter) {
		dev_err(d->dev, "DSI write timeout!\n");
		return -ETIME;
	}

	val = readl(d->regs + DSI_DIRECT_CMD_STS);
	if (val & DSI_DIRECT_CMD_STS_ACKNOWLEDGE_WITH_ERR_RECEIVED) {
		val >>= DSI_DIRECT_CMD_STS_ACK_VAL_SHIFT;
		dev_err(d->dev, "error during transmission: %04x\n",
			val);
		return -EIO;
	}

	if (!MCDE_DSI_HOST_IS_READ(msg->type)) {
		/* Return number of bytes written */
		if (mipi_dsi_packet_format_is_long(msg->type))
			ret = 4 + txlen;
		else
			ret = 4;
	} else {
		/* OK this is a read command, get the response */
		u32 rdsz;
		u32 rddat;
		u8 *rx = msg->rx_buf;

		rdsz = readl(d->regs + DSI_DIRECT_CMD_RD_PROPERTY);
		rdsz &= DSI_DIRECT_CMD_RD_PROPERTY_RD_SIZE_MASK;
		rddat = readl(d->regs + DSI_DIRECT_CMD_RDDAT);
		for (i = 0; i < 4 && i < rdsz; i++)
			rx[i] = (rddat >> (i * 8)) & 0xff;
		ret = rdsz;
	}

	writel(~0, d->regs + DSI_DIRECT_CMD_STS_CLR);
	writel(~0, d->regs + DSI_CMD_MODE_STS_CLR);

	return ret;
}

static const struct mipi_dsi_host_ops mcde_dsi_host_ops = {
	.attach = mcde_dsi_host_attach,
	.detach = mcde_dsi_host_detach,
	.transfer = mcde_dsi_host_transfer,
};

/* This sends a direct (short) command to request TE */
void mcde_dsi_te_request(struct mipi_dsi_device *mdsi)
{
	struct mcde_dsi *d;
	u32 val;

	d = host_to_mcde_dsi(mdsi->host);

	/* Command "nature" TE request */
	val = DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_NAT_TE_REQ;
	val |= 0 << DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_ID_SHIFT;
	val |= 2 << DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_SIZE_SHIFT;
	val |= DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_LP_EN;
	val |= DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_DCS_SHORT_WRITE_1 <<
		DSI_DIRECT_CMD_MAIN_SETTINGS_CMD_HEAD_SHIFT;
	writel(val, d->regs + DSI_DIRECT_CMD_MAIN_SETTINGS);

	/* Clear TE reveived and error status bits and enables them */
	writel(DSI_DIRECT_CMD_STS_CLR_TE_RECEIVED_CLR |
	       DSI_DIRECT_CMD_STS_CLR_ACKNOWLEDGE_WITH_ERR_RECEIVED_CLR,
	       d->regs + DSI_DIRECT_CMD_STS_CLR);
	val = readl(d->regs + DSI_DIRECT_CMD_STS_CTL);
	val |= DSI_DIRECT_CMD_STS_CTL_TE_RECEIVED_EN;
	val |= DSI_DIRECT_CMD_STS_CTL_ACKNOWLEDGE_WITH_ERR_EN;
	writel(val, d->regs + DSI_DIRECT_CMD_STS_CTL);

	/* Clear and enable no TE or TE missing status */
	writel(DSI_CMD_MODE_STS_CLR_ERR_NO_TE_CLR |
	       DSI_CMD_MODE_STS_CLR_ERR_TE_MISS_CLR,
	       d->regs + DSI_CMD_MODE_STS_CLR);
	val = readl(d->regs + DSI_CMD_MODE_STS_CTL);
	val |= DSI_CMD_MODE_STS_CTL_ERR_NO_TE_EN;
	val |= DSI_CMD_MODE_STS_CTL_ERR_TE_MISS_EN;
	writel(val, d->regs + DSI_CMD_MODE_STS_CTL);

	/* Send this TE request command */
	writel(1, d->regs + DSI_DIRECT_CMD_SEND);
}

static void mcde_dsi_setup_video_mode(struct mcde_dsi *d,
				      struct drm_display_mode *mode)
{
	u8 bpp = mipi_dsi_pixel_format_to_bpp(d->mdsi->format);
	u64 bpl;
	u32 hfp;
	u32 hbp;
	u32 hsa;
	u32 blkline_pck, line_duration;
	u32 blkeol_pck, blkeol_duration;
	u32 val;

	val = 0;
	if (d->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
		val |= DSI_VID_MAIN_CTL_BURST_MODE;
	if (d->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE) {
		val |= DSI_VID_MAIN_CTL_SYNC_PULSE_ACTIVE;
		val |= DSI_VID_MAIN_CTL_SYNC_PULSE_HORIZONTAL;
	}
	/* RGB header and pixel mode */
	switch (d->mdsi->format) {
	case MIPI_DSI_FMT_RGB565:
		val |= MIPI_DSI_PACKED_PIXEL_STREAM_16 <<
			DSI_VID_MAIN_CTL_HEADER_SHIFT;
		val |= DSI_VID_MAIN_CTL_VID_PIXEL_MODE_16BITS;
		break;
	case MIPI_DSI_FMT_RGB666_PACKED:
		val |= MIPI_DSI_PACKED_PIXEL_STREAM_18 <<
			DSI_VID_MAIN_CTL_HEADER_SHIFT;
		val |= DSI_VID_MAIN_CTL_VID_PIXEL_MODE_18BITS;
		break;
	case MIPI_DSI_FMT_RGB666:
		val |= MIPI_DSI_PIXEL_STREAM_3BYTE_18
			<< DSI_VID_MAIN_CTL_HEADER_SHIFT;
		val |= DSI_VID_MAIN_CTL_VID_PIXEL_MODE_18BITS_LOOSE;
		break;
	case MIPI_DSI_FMT_RGB888:
		val |= MIPI_DSI_PACKED_PIXEL_STREAM_24 <<
			DSI_VID_MAIN_CTL_HEADER_SHIFT;
		val |= DSI_VID_MAIN_CTL_VID_PIXEL_MODE_24BITS;
		break;
	default:
		dev_err(d->dev, "unknown pixel mode\n");
		return;
	}

	/* FIXME: TVG could be enabled here */

	/* Send blanking packet */
	val |= DSI_VID_MAIN_CTL_REG_BLKLINE_MODE_LP_0;
	/* Send EOL packet */
	val |= DSI_VID_MAIN_CTL_REG_BLKEOL_MODE_LP_0;
	/* Recovery mode 1 */
	val |= 1 << DSI_VID_MAIN_CTL_RECOVERY_MODE_SHIFT;
	/* All other fields zero */
	writel(val, d->regs + DSI_VID_MAIN_CTL);

	/* Vertical frame parameters are pretty straight-forward */
	val = mode->vdisplay << DSI_VID_VSIZE_VSA_LENGTH_SHIFT;
	/* vertical front porch */
	val |= (mode->vsync_start - mode->vdisplay)
		<< DSI_VID_VSIZE_VFP_LENGTH_SHIFT;
	/* vertical sync active */
	val |= (mode->vsync_end - mode->vsync_start)
		<< DSI_VID_VSIZE_VACT_LENGTH_SHIFT;
	/* vertical back porch */
	val |= (mode->vtotal - mode->vsync_end)
		<< DSI_VID_VSIZE_VBP_LENGTH_SHIFT;
	writel(val, d->regs + DSI_VID_VSIZE);

	/*
	 * Horizontal frame parameters:
	 * horizontal resolution is given in pixels and must be re-calculated
	 * into bytes since this is what the hardware expects.
	 *
	 * 6 + 2 is HFP header + checksum
	 */
	hfp = (mode->hsync_start - mode->hdisplay) * bpp - 6 - 2;
	if (d->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE) {
		/*
		 * 6 is HBP header + checksum
		 * 4 is RGB header + checksum
		 */
		hbp = (mode->htotal - mode->hsync_end) * bpp - 4 - 6;
		/*
		 * 6 is HBP header + checksum
		 * 4 is HSW packet bytes
		 * 4 is RGB header + checksum
		 */
		hsa = (mode->hsync_end - mode->hsync_start) * bpp - 4 - 4 - 6;
	} else {
		/*
		 * HBP includes both back porch and sync
		 * 6 is HBP header + checksum
		 * 4 is HSW packet bytes
		 * 4 is RGB header + checksum
		 */
		hbp = (mode->htotal - mode->hsync_start) * bpp - 4 - 4 - 6;
		/* HSA is not considered in this mode and set to 0 */
		hsa = 0;
	}
	dev_dbg(d->dev, "hfp: %u, hbp: %u, hsa: %u\n",
		hfp, hbp, hsa);

	/* Frame parameters: horizontal sync active */
	val = hsa << DSI_VID_HSIZE1_HSA_LENGTH_SHIFT;
	/* horizontal back porch */
	val |= hbp << DSI_VID_HSIZE1_HBP_LENGTH_SHIFT;
	/* horizontal front porch */
	val |= hfp << DSI_VID_HSIZE1_HFP_LENGTH_SHIFT;
	writel(val, d->regs + DSI_VID_HSIZE1);

	/* RGB data length (bytes on one scanline) */
	val = mode->hdisplay * (bpp / 8);
	writel(val, d->regs + DSI_VID_HSIZE2);

	/* TODO: further adjustments for TVG mode here */

	/*
	 * EOL packet length from bits per line calculations: pixel clock
	 * is given in kHz, calculate the time between two pixels in
	 * picoseconds.
	 */
	bpl = mode->clock * mode->htotal;
	bpl *= (d->hs_freq / 8);
	do_div(bpl, 1000000); /* microseconds */
	do_div(bpl, 1000000); /* seconds */
	bpl *= d->mdsi->lanes;
	dev_dbg(d->dev, "calculated bytes per line: %llu\n", bpl);
	/*
	 * 6 is header + checksum, header = 4 bytes, checksum = 2 bytes
	 * 4 is short packet for vsync/hsync
	 */
	if (d->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE) {
		/* Fixme: isn't the hsync width in pixels? */
		blkline_pck = bpl - (mode->hsync_end - mode->hsync_start) - 6;
		val = blkline_pck << DSI_VID_BLKSIZE2_BLKLINE_PULSE_PCK_SHIFT;
		writel(val, d->regs + DSI_VID_BLKSIZE2);
	} else {
		blkline_pck = bpl - 4 - 6;
		val = blkline_pck << DSI_VID_BLKSIZE1_BLKLINE_EVENT_PCK_SHIFT;
		writel(val, d->regs + DSI_VID_BLKSIZE1);
	}

	line_duration = (blkline_pck + 6) / d->mdsi->lanes;
	dev_dbg(d->dev, "line duration %u\n", line_duration);
	val = line_duration << DSI_VID_DPHY_TIME_REG_LINE_DURATION_SHIFT;
	/*
	 * This is the time to perform LP->HS on D-PHY
	 * FIXME: nowhere to get this from: DT property on the DSI?
	 */
	val |= 0 << DSI_VID_DPHY_TIME_REG_WAKEUP_TIME_SHIFT;
	writel(val, d->regs + DSI_VID_DPHY_TIME);

	/* Calculate block end of line */
	blkeol_pck = bpl - mode->hdisplay * bpp - 6;
	blkeol_duration = (blkeol_pck + 6) / d->mdsi->lanes;
	dev_dbg(d->dev, "blkeol pck: %u, duration: %u\n",
		 blkeol_pck, blkeol_duration);

	if (d->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO_BURST) {
		/* Set up EOL clock for burst mode */
		val = readl(d->regs + DSI_VID_BLKSIZE1);
		val |= blkeol_pck << DSI_VID_BLKSIZE1_BLKEOL_PCK_SHIFT;
		writel(val, d->regs + DSI_VID_BLKSIZE1);
		writel(blkeol_pck, d->regs + DSI_VID_VCA_SETTING2);

		writel(blkeol_duration, d->regs + DSI_VID_PCK_TIME);
		writel(blkeol_duration - 6, d->regs + DSI_VID_VCA_SETTING1);
	}

	/* Maximum line limit */
	val = readl(d->regs + DSI_VID_VCA_SETTING2);
	val |= blkline_pck <<
		DSI_VID_VCA_SETTING2_EXACT_BURST_LIMIT_SHIFT;
	writel(val, d->regs + DSI_VID_VCA_SETTING2);

	/* Put IF1 into video mode */
	val = readl(d->regs + DSI_MCTL_MAIN_DATA_CTL);
	val |= DSI_MCTL_MAIN_DATA_CTL_IF1_MODE;
	writel(val, d->regs + DSI_MCTL_MAIN_DATA_CTL);

	/* Disable command mode on IF1 */
	val = readl(d->regs + DSI_CMD_MODE_CTL);
	val &= ~DSI_CMD_MODE_CTL_IF1_LP_EN;
	writel(val, d->regs + DSI_CMD_MODE_CTL);

	/* Enable some error interrupts */
	val = readl(d->regs + DSI_VID_MODE_STS_CTL);
	val |= DSI_VID_MODE_STS_CTL_ERR_MISSING_VSYNC;
	val |= DSI_VID_MODE_STS_CTL_ERR_MISSING_DATA;
	writel(val, d->regs + DSI_VID_MODE_STS_CTL);

	/* Enable video mode */
	val = readl(d->regs + DSI_MCTL_MAIN_DATA_CTL);
	val |= DSI_MCTL_MAIN_DATA_CTL_VID_EN;
	writel(val, d->regs + DSI_MCTL_MAIN_DATA_CTL);
}

static void mcde_dsi_start(struct mcde_dsi *d)
{
	unsigned long hs_freq;
	u32 val;
	int i;

	/* No integration mode */
	writel(0, d->regs + DSI_MCTL_INTEGRATION_MODE);

	/* Enable the DSI port, from drivers/video/mcde/dsilink_v2.c */
	val = DSI_MCTL_MAIN_DATA_CTL_LINK_EN |
		DSI_MCTL_MAIN_DATA_CTL_BTA_EN |
		DSI_MCTL_MAIN_DATA_CTL_READ_EN |
		DSI_MCTL_MAIN_DATA_CTL_REG_TE_EN;
	if (d->mdsi->mode_flags & MIPI_DSI_MODE_EOT_PACKET)
		val |= DSI_MCTL_MAIN_DATA_CTL_HOST_EOT_GEN;
	writel(val, d->regs + DSI_MCTL_MAIN_DATA_CTL);

	/* Set a high command timeout, clear other fields */
	val = 0x3ff << DSI_CMD_MODE_CTL_TE_TIMEOUT_SHIFT;
	writel(val, d->regs + DSI_CMD_MODE_CTL);

	/*
	 * UI_X4 is described as "unit interval times four"
	 * I guess since DSI packets are 4 bytes wide, one unit
	 * is one byte.
	 */
	hs_freq = clk_get_rate(d->mcde->dsi0_clk);
	hs_freq /= 1000000; /* MHz */
	val = 4000 / hs_freq;
	dev_dbg(d->dev, "UI value: %d\n", val);
	val <<= DSI_MCTL_DPHY_STATIC_UI_X4_SHIFT;
	val &= DSI_MCTL_DPHY_STATIC_UI_X4_MASK;
	writel(val, d->regs + DSI_MCTL_DPHY_STATIC);

	/*
	 * Enable clocking: 0x0f (something?) between each burst,
	 * enable the second lane if needed, enable continuous clock if
	 * needed, enable switch into ULPM (ultra-low power mode) on
	 * all the lines.
	 */
	val = 0x0f << DSI_MCTL_MAIN_PHY_CTL_WAIT_BURST_TIME_SHIFT;
	if (d->mdsi->lanes == 2)
		val |= DSI_MCTL_MAIN_PHY_CTL_LANE2_EN;
	if (!(d->mdsi->mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS))
		val |= DSI_MCTL_MAIN_PHY_CTL_CLK_CONTINUOUS;
	val |= DSI_MCTL_MAIN_PHY_CTL_CLK_ULPM_EN |
		DSI_MCTL_MAIN_PHY_CTL_DAT1_ULPM_EN |
		DSI_MCTL_MAIN_PHY_CTL_DAT2_ULPM_EN;
	writel(val, d->regs + DSI_MCTL_MAIN_PHY_CTL);

	val = (1 << DSI_MCTL_ULPOUT_TIME_CKLANE_ULPOUT_TIME_SHIFT) |
		(1 << DSI_MCTL_ULPOUT_TIME_DATA_ULPOUT_TIME_SHIFT);
	writel(val, d->regs + DSI_MCTL_ULPOUT_TIME);

	writel(DSI_DPHY_LANES_TRIM_DPHY_SPECS_90_81B_0_90,
	       d->regs + DSI_DPHY_LANES_TRIM);

	/* High PHY timeout */
	val = (0x0f << DSI_MCTL_DPHY_TIMEOUT_CLK_DIV_SHIFT) |
		(0x3fff << DSI_MCTL_DPHY_TIMEOUT_HSTX_TO_VAL_SHIFT) |
		(0x3fff << DSI_MCTL_DPHY_TIMEOUT_LPRX_TO_VAL_SHIFT);
	writel(val, d->regs + DSI_MCTL_DPHY_TIMEOUT);

	val = DSI_MCTL_MAIN_EN_PLL_START |
		DSI_MCTL_MAIN_EN_CKLANE_EN |
		DSI_MCTL_MAIN_EN_DAT1_EN |
		DSI_MCTL_MAIN_EN_IF1_EN;
	if (d->mdsi->lanes == 2)
		val |= DSI_MCTL_MAIN_EN_DAT2_EN;
	writel(val, d->regs + DSI_MCTL_MAIN_EN);

	/* Wait for the PLL to lock and the clock and data lines to come up */
	i = 0;
	val = DSI_MCTL_MAIN_STS_PLL_LOCK |
		DSI_MCTL_MAIN_STS_CLKLANE_READY |
		DSI_MCTL_MAIN_STS_DAT1_READY;
	if (d->mdsi->lanes == 2)
		val |= DSI_MCTL_MAIN_STS_DAT2_READY;
	while ((readl(d->regs + DSI_MCTL_MAIN_STS) & val) != val) {
		/* Sleep for a millisecond */
		usleep_range(1000, 1500);
		if (i++ == 100) {
			dev_warn(d->dev, "DSI lanes did not start up\n");
			return;
		}
	}

	/* TODO needed? */

	/* Command mode, clear IF1 ID */
	val = readl(d->regs + DSI_CMD_MODE_CTL);
	/* FIXME: enable low-power mode? */
	// val |= DSI_CMD_MODE_CTL_IF1_LP_EN;
	val &= ~DSI_CMD_MODE_CTL_IF1_ID_MASK;
	writel(val, d->regs + DSI_CMD_MODE_CTL);

	/* Wait for DSI PHY to initialize */
	usleep_range(100, 200);
	dev_info(d->dev, "DSI link enabled\n");
}

static void mcde_dsi_enable(struct drm_encoder *encoder)
{
	struct drm_display_mode *mode = &encoder->crtc->state->adjusted_mode;
	struct mcde_dsi *d = encoder_to_mcde_dsi(encoder);
	unsigned long pixel_clock_hz = mode->clock * 1000;
	unsigned long hs_freq, lp_freq;
	u32 val;
	int ret;

	if (!d->mdsi) {
		dev_err(d->dev, "no DSI device attached to encoder!\n");
		return;
	}

	dev_info(d->dev, "enable DSI master for %dx%d %lu Hz %s mode\n",
		 mode->hdisplay, mode->vdisplay, pixel_clock_hz,
		 (d->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO) ? "VIDEO" : "CMD"
		);

	/* Copy maximum clock frequencies */
	if (d->mdsi->lp_rate)
		lp_freq = d->mdsi->lp_rate;
	else
		lp_freq = DSI_DEFAULT_LP_FREQ_HZ;
	if (d->mdsi->hs_rate)
		hs_freq = d->mdsi->hs_rate;
	else
		hs_freq = DSI_DEFAULT_HS_FREQ_HZ;

	/* Enable LP (Low Power, Energy Save, ES) and HS (High Speed) clocks */
	d->lp_freq = clk_round_rate(d->mcde->dsi0es_clk, lp_freq);
	ret = clk_set_rate(d->mcde->dsi0es_clk, d->lp_freq);
	if (ret)
		dev_err(d->dev, "failed to set LP clock rate %lu Hz\n",
			d->lp_freq);

	d->hs_freq = clk_round_rate(d->mcde->dsi0_clk, hs_freq);
	ret = clk_set_rate(d->mcde->dsi0_clk, d->hs_freq);
	if (ret)
		dev_err(d->dev, "failed to set HS clock rate %lu Hz\n",
			d->hs_freq);

	/* Start clocks */
	ret = clk_prepare_enable(d->mcde->dsi0es_clk);
	if (ret)
		dev_err(d->dev, "failed to enable LP clock\n");
	else
		dev_info(d->dev, "DSI LP clock rate %lu Hz\n",
			 d->lp_freq);
	ret = clk_prepare_enable(d->mcde->dsi0_clk);
	if (ret)
		dev_err(d->dev, "failed to enable HS clock\n");
	else
		dev_info(d->dev, "DSI HS clock rate %lu Hz\n",
			 d->hs_freq);

	if (d->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO) {
		mcde_dsi_setup_video_mode(d, mode);
	} else {
		/* Command mode, clear IF1 ID */
		val = readl(d->regs + DSI_CMD_MODE_CTL);
		/* FIXME: enable low-power mode? */
		// val |= DSI_CMD_MODE_CTL_IF1_LP_EN;
		val &= ~DSI_CMD_MODE_CTL_IF1_ID_MASK;
		writel(val, d->regs + DSI_CMD_MODE_CTL);
	}
}

static void mcde_dsi_wait_for_command_mode_stop(struct mcde_dsi *d)
{
	u32 val;
	int i;

	/*
	 * Wait until we get out of command mode
	 * CSM = Command State Machine
	 */
	i = 0;
	val = DSI_CMD_MODE_STS_CSM_RUNNING;
	while ((readl(d->regs + DSI_CMD_MODE_STS) & val) == val) {
		/* Sleep for a millisecond */
		usleep_range(1000, 2000);
		if (i++ == 100) {
			dev_warn(d->dev,
				 "could not get out of command mode\n");
			return;
		}
	}
}

static void mcde_dsi_wait_for_video_mode_stop(struct mcde_dsi *d)
{
	u32 val;
	int i;

	/* Wait until we get out og video mode */
	i = 0;
	val = DSI_VID_MODE_STS_VSG_RUNNING;
	while ((readl(d->regs + DSI_VID_MODE_STS) & val) == val) {
		/* Sleep for a millisecond */
		usleep_range(1000, 2000);
		if (i++ == 100) {
			dev_warn(d->dev,
				 "could not get out of video mode\n");
			return;
		}
	}
}

static void mcde_dsi_disable(struct drm_encoder *encoder)
{
	struct mcde_dsi *d = encoder_to_mcde_dsi(encoder);
	u32 val;

	/* Disable all error interrupts */
	writel(0, d->regs + DSI_VID_MODE_STS_CTL);

	if (d->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO) {
		/* Stop video mode */
		val = readl(d->regs + DSI_MCTL_MAIN_DATA_CTL);
		val &= ~DSI_MCTL_MAIN_DATA_CTL_VID_EN;
		writel(val, d->regs + DSI_MCTL_MAIN_DATA_CTL);
		mcde_dsi_wait_for_video_mode_stop(d);
	} else {
		/* Stop command mode */
		mcde_dsi_wait_for_command_mode_stop(d);
	}

	/* Stop clocks */
	clk_disable_unprepare(d->mcde->dsi0_clk);
	clk_disable_unprepare(d->mcde->dsi0es_clk);
}

static const struct drm_encoder_helper_funcs mcde_dsi_encoder_helper_funcs = {
	.enable = mcde_dsi_enable,
	.disable = mcde_dsi_disable,
};

static const struct drm_encoder_funcs mcde_dsi_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_connector_funcs mcde_dsi_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int mcde_dsi_get_modes(struct drm_connector *connector)
{
	struct mcde_dsi *d = connector_to_mcde_dsi(connector);

	if (d->panel)
		return drm_panel_get_modes(d->panel);

	/* TODO: deal with bridges */

	return 0;
}

static const struct drm_connector_helper_funcs
mcde_dsi_connector_helper_funcs = {
	.get_modes = mcde_dsi_get_modes,
};

static int mcde_dsi_bind(struct device *dev, struct device *master,
			 void *data)
{
	struct drm_device *drm = data;
	struct mcde *mcde = drm->dev_private;
	struct mcde_dsi *d = dev_get_drvdata(dev);
	struct drm_encoder *encoder = &d->encoder;
	struct drm_connector *connector = &d->connector;
	struct drm_panel *panel;
	struct drm_bridge *bridge;
	int ret;

	if (!of_get_available_child_count(dev->of_node)) {
		dev_info(dev, "unused DSI interface\n");
		d->unused = true;
		return 0;
	}
	d->mcde = mcde;
	/* If the display attached before binding, set this up */
	if (d->mdsi)
		d->mcde->mdsi = d->mdsi;

	/* Assert RESET through the PRCMU, active low */
	/* FIXME: which DSI block? */
	regmap_update_bits(d->prcmu, PRCM_DSI_SW_RESET,
			   PRCM_DSI_SW_RESET_DSI0_SW_RESETN, 0);

	usleep_range(100, 200);

	/* De-assert RESET again */
	regmap_update_bits(d->prcmu, PRCM_DSI_SW_RESET,
			   PRCM_DSI_SW_RESET_DSI0_SW_RESETN,
			   PRCM_DSI_SW_RESET_DSI0_SW_RESETN);

	/* Start up the hardware */
	mcde_dsi_start(d);

	/* Create an encoder and attach the display bridge to it */
	drm_encoder_init(drm, encoder, &mcde_dsi_encoder_funcs,
			 DRM_MODE_ENCODER_DSI, NULL);
	drm_encoder_helper_add(encoder, &mcde_dsi_encoder_helper_funcs);

	/* Create a connector and attach the encoder to it */
	connector->polled = DRM_CONNECTOR_POLL_HPD;
	ret = drm_connector_init(encoder->dev, connector,
				 &mcde_dsi_connector_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret) {
		dev_err(dev, "failed to initialize connector\n");
		drm_encoder_cleanup(encoder);
		return ret;
	}
	connector->status = connector_status_disconnected;
	drm_connector_helper_add(connector, &mcde_dsi_connector_helper_funcs);
	drm_connector_attach_encoder(connector, encoder);
	drm_connector_register(connector);

	dev_info(dev, "initialized encoder and connector\n");

	/* The DSI encoder connects to a panel or other bridge */
	ret = drm_of_find_panel_or_bridge(dev->of_node,
					  0, 0, &panel, &bridge);
	if (ret && ret != -ENODEV) {
		dev_err(dev, "no panel or bridge %d\n", ret);
		return ret;
	}
	if (panel) {
		bridge = drm_panel_bridge_add(panel,
					      DRM_MODE_CONNECTOR_DSI);
		if (IS_ERR(bridge)) {
			dev_err(dev, "error adding panel bridge\n");
			return PTR_ERR(bridge);
		}
		dev_info(dev, "connected to panel\n");
		d->panel = panel;
	} else if (bridge) {
		/* FIXME: AV8100 HDMI encoder goes here for example */
		dev_info(dev, "connected to non-panel bridge (unsupported)\n");
		return -ENODEV;
	} else {
		dev_err(dev, "no panel or bridge\n");
		return -ENODEV;
	}

	d->bridge = bridge;
	d->connector.status = connector_status_connected;

	ret = drm_bridge_attach(encoder, bridge, NULL);
	if (ret) {
		dev_err(dev, "bridge attach failed: %d\n", ret);
		return ret;
	}

	/* FIXME: first come first serve, use a list */
	mcde->connector = connector;
	mcde->bridge = bridge;
	dev_info(dev, "set up DSI connector and panel bridge\n");

	return 0;
}

static void mcde_dsi_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct mcde_dsi *d = dev_get_drvdata(dev);

	regmap_update_bits(d->prcmu, PRCM_DSI_SW_RESET,
			   PRCM_DSI_SW_RESET_DSI0_SW_RESETN, 0);
	clk_disable_unprepare(d->mcde->dsi0_clk);
	clk_disable_unprepare(d->mcde->dsi0es_clk);
}

static const struct component_ops mcde_dsi_component_ops = {
	.bind   = mcde_dsi_bind,
	.unbind = mcde_dsi_unbind,
};

static int mcde_dsi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mcde_dsi *d;
	struct mipi_dsi_host *host;
	struct resource *res;
	u32 dsi_id;
	int ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = dev;
	platform_set_drvdata(pdev, d);

	/* Get a handle on the PRCMU so we can do reset */
	d->prcmu =
		syscon_regmap_lookup_by_compatible("stericsson,db8500-prcmu");
	if (IS_ERR(d->prcmu)) {
		dev_err(dev, "no PRCMU regmap\n");
		return PTR_ERR(d->prcmu);
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	d->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(d->regs)) {
		dev_err(dev, "no DSI regs\n");
		return PTR_ERR(d->regs);
	}

	dsi_id = readl(d->regs + DSI_ID_REG);
	dev_info(dev, "HW revision 0x%08x\n", dsi_id);

	host = &d->dsi_host;
	host->dev = dev;
	host->ops = &mcde_dsi_host_ops;
	ret = mipi_dsi_host_register(host);
	if (ret < 0) {
		dev_err(dev, "failed to register DSI host: %d\n", ret);
		return ret;
	}
	dev_info(dev, "registered DSI host\n");

	platform_set_drvdata(pdev, d);
	return component_add(dev, &mcde_dsi_component_ops);
}

static int mcde_dsi_remove(struct platform_device *pdev)
{
	struct mcde_dsi *d = platform_get_drvdata(pdev);

	component_del(&pdev->dev, &mcde_dsi_component_ops);
	mcde_dsi_disable(&d->encoder);
	mipi_dsi_host_unregister(&d->dsi_host);

	return 0;
}

static const struct of_device_id mcde_dsi_of_match[] = {
	{
		.compatible = "ste,mcde-dsi",
	},
	{},
};

struct platform_driver mcde_dsi_driver = {
	.driver = {
		.name           = "mcde-dsi",
		.of_match_table = of_match_ptr(mcde_dsi_of_match),
	},
	.probe = mcde_dsi_probe,
	.remove = mcde_dsi_remove,
};
