// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Linus Walleij <linus.walleij@linaro.org>
 * Parts of this file were based on the MCDE driver by Marcus Lorentzon
 * (C) ST-Ericsson SA 2013
 */
#include <linux/clk.h>
#include <linux/version.h>
#include <linux/dma-buf.h>
#include <linux/of_graph.h>

#include <drm/drmP.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <video/mipi_display.h>

#include "mcde_drm.h"

/* PP (pixel processor) interrupts */
#define MCDE_IMSCPP 0x00000104
#define MCDE_RISPP 0x00000114
#define MCDE_MISPP 0x00000124
#define MCDE_SISPP 0x00000134

#define MCDE_PP_VCMPA BIT(0)
#define MCDE_PP_VCMPB BIT(1)
#define MCDE_PP_VSCC0 BIT(2)
#define MCDE_PP_VSCC1 BIT(3)
#define MCDE_PP_VCMPC0 BIT(4)
#define MCDE_PP_VCMPC1 BIT(5)
#define MCDE_PP_ROTFD_A BIT(6)
#define MCDE_PP_ROTFD_B BIT(7)

/* Overlay interrupts */
#define MCDE_IMSCOVL 0x00000108
#define MCDE_RISOVL 0x00000118
#define MCDE_MISOVL 0x00000128
#define MCDE_SISOVL 0x00000138

/* Channel interrupts */
#define MCDE_IMSCCHNL 0x0000010C
#define MCDE_RISCHNL 0x0000011C
#define MCDE_MISCHNL 0x0000012C
#define MCDE_SISCHNL 0x0000013C

/* X = 0..9 */
#define MCDE_EXTSRCXA0 0x00000200
#define MCDE_EXTSRCXA0_GROUPOFFSET 0x20
#define MCDE_EXTSRCXA0_BASEADDRESS0_SHIFT 3
#define MCDE_EXTSRCXA0_BASEADDRESS0_MASK 0xFFFFFFF8

#define MCDE_EXTSRCXA1 0x00000204
#define MCDE_EXTSRCXA1_GROUPOFFSET 0x20
#define MCDE_EXTSRCXA1_BASEADDRESS1_SHIFT 3
#define MCDE_EXTSRCXA1_BASEADDRESS1_MASK 0xFFFFFFF8

/* External sources 0..9 */
#define MCDE_EXTSRC0CONF 0x0000020C
#define MCDE_EXTSRC1CONF 0x0000022C
#define MCDE_EXTSRC2CONF 0x0000024C
#define MCDE_EXTSRC3CONF 0x0000026C
#define MCDE_EXTSRC4CONF 0x0000028C
#define MCDE_EXTSRC5CONF 0x000002AC
#define MCDE_EXTSRC6CONF 0x000002CC
#define MCDE_EXTSRC7CONF 0x000002EC
#define MCDE_EXTSRC8CONF 0x0000030C
#define MCDE_EXTSRC9CONF 0x0000032C
#define MCDE_EXTSRCXCONF_GROUPOFFSET 0x20
#define MCDE_EXTSRCXCONF_BUF_ID_SHIFT 0
#define MCDE_EXTSRCXCONF_BUF_ID_MASK 0x00000003
#define MCDE_EXTSRCXCONF_BUF_NB_SHIFT 2
#define MCDE_EXTSRCXCONF_BUF_NB_MASK 0x0000000C
#define MCDE_EXTSRCXCONF_PRI_OVLID_SHIFT 4
#define MCDE_EXTSRCXCONF_PRI_OVLID_MASK 0x000000F0
#define MCDE_EXTSRCXCONF_BPP_SHIFT 8
#define MCDE_EXTSRCXCONF_BPP_MASK 0x00000F00
#define MCDE_EXTSRCXCONF_BPP_1BPP_PAL 0
#define MCDE_EXTSRCXCONF_BPP_2BPP_PAL 1
#define MCDE_EXTSRCXCONF_BPP_4BPP_PAL 2
#define MCDE_EXTSRCXCONF_BPP_8BPP_PAL 3
#define MCDE_EXTSRCXCONF_BPP_RGB444 4
#define MCDE_EXTSRCXCONF_BPP_ARGB4444 5
#define MCDE_EXTSRCXCONF_BPP_IRGB1555 6
#define MCDE_EXTSRCXCONF_BPP_RGB565 7
#define MCDE_EXTSRCXCONF_BPP_RGB888 8
#define MCDE_EXTSRCXCONF_BPP_XRGB8888 9
#define MCDE_EXTSRCXCONF_BPP_ARGB8888 10
#define MCDE_EXTSRCXCONF_BPP_YCBCR422 11
#define MCDE_EXTSRCXCONF_BGR BIT(12)
#define MCDE_EXTSRCXCONF_BEBO BIT(13)
#define MCDE_EXTSRCXCONF_BEPO BIT(14)
#define MCDE_EXTSRCXCONF_TUNNELING_BUFFER_HEIGHT_SHIFT 16
#define MCDE_EXTSRCXCONF_TUNNELING_BUFFER_HEIGHT_MASK 0x0FFF0000

/* External sources 0..9 */
#define MCDE_EXTSRC0CR 0x00000210
#define MCDE_EXTSRC1CR 0x00000230
#define MCDE_EXTSRC2CR 0x00000250
#define MCDE_EXTSRC3CR 0x00000270
#define MCDE_EXTSRC4CR 0x00000290
#define MCDE_EXTSRC5CR 0x000002B0
#define MCDE_EXTSRC6CR 0x000002D0
#define MCDE_EXTSRC7CR 0x000002F0
#define MCDE_EXTSRC8CR 0x00000310
#define MCDE_EXTSRC9CR 0x00000330
#define MCDE_EXTSRC0CR_SEL_MOD_SHIFT 0
#define MCDE_EXTSRC0CR_SEL_MOD_MASK 0x00000003
#define MCDE_EXTSRC0CR_SEL_MOD_EXTERNAL_SEL 0
#define MCDE_EXTSRC0CR_SEL_MOD_AUTO_TOGGLE 1
#define MCDE_EXTSRC0CR_SEL_MOD_SOFTWARE_SEL 2
#define MCDE_EXTSRC0CR_MULTIOVL_CTRL_PRIMARY BIT(2) /* 0 = all */
#define MCDE_EXTSRC0CR_FS_DIV_DISABLE BIT(3)
#define MCDE_EXTSRC0CR_FORCE_FS_DIV BIT(4)

/* Only external source 6 has a second address register */
#define MCDE_EXTSRC6A2 0x000002C8

/* 6 overlays */
#define MCDE_OVL0CR 0x00000400
#define MCDE_OVL1CR 0x00000420
#define MCDE_OVL2CR 0x00000440
#define MCDE_OVL3CR 0x00000460
#define MCDE_OVL4CR 0x00000480
#define MCDE_OVL5CR 0x000004A0
#define MCDE_OVLXCR_OVLEN BIT(0)
#define MCDE_OVLXCR_COLCCTRL_DISABLED 0
#define MCDE_OVLXCR_COLCCTRL_ENABLED_NO_SAT (1 << 1)
#define MCDE_OVLXCR_COLCCTRL_ENABLED_SAT (2 << 1)
#define MCDE_OVLXCR_CKEYGEN BIT(3)
#define MCDE_OVLXCR_ALPHAPMEN BIT(4)
#define MCDE_OVLXCR_OVLF BIT(5)
#define MCDE_OVLXCR_OVLR BIT(6)
#define MCDE_OVLXCR_OVLB BIT(7)
#define MCDE_OVLXCR_FETCH_ROPC_SHIFT 8
#define MCDE_OVLXCR_FETCH_ROPC_MASK 0x0000FF00
#define MCDE_OVLXCR_STBPRIO_SHIFT 16
#define MCDE_OVLXCR_STBPRIO_MASK 0x000F0000
#define MCDE_OVLXCR_BURSTSIZE_SHIFT 20
#define MCDE_OVLXCR_BURSTSIZE_MASK 0x00F00000
#define MCDE_OVLXCR_BURSTSIZE_1W 0
#define MCDE_OVLXCR_BURSTSIZE_2W 1
#define MCDE_OVLXCR_BURSTSIZE_4W 2
#define MCDE_OVLXCR_BURSTSIZE_8W 3
#define MCDE_OVLXCR_BURSTSIZE_16W 4
#define MCDE_OVLXCR_BURSTSIZE_HW_1W 8
#define MCDE_OVLXCR_BURSTSIZE_HW_2W 9
#define MCDE_OVLXCR_BURSTSIZE_HW_4W 10
#define MCDE_OVLXCR_BURSTSIZE_HW_8W 11
#define MCDE_OVLXCR_BURSTSIZE_HW_16W 12
#define MCDE_OVLXCR_MAXOUTSTANDING_SHIFT 24
#define MCDE_OVLXCR_MAXOUTSTANDING_MASK 0x0F000000
#define MCDE_OVLXCR_MAXOUTSTANDING_1_REQ 0
#define MCDE_OVLXCR_MAXOUTSTANDING_2_REQ 1
#define MCDE_OVLXCR_MAXOUTSTANDING_4_REQ 2
#define MCDE_OVLXCR_MAXOUTSTANDING_8_REQ 3
#define MCDE_OVLXCR_MAXOUTSTANDING_16_REQ 4
#define MCDE_OVLXCR_ROTBURSTSIZE_SHIFT 28
#define MCDE_OVLXCR_ROTBURSTSIZE_MASK 0xF0000000
#define MCDE_OVLXCR_ROTBURSTSIZE_1W 0
#define MCDE_OVLXCR_ROTBURSTSIZE_2W 1
#define MCDE_OVLXCR_ROTBURSTSIZE_4W 2
#define MCDE_OVLXCR_ROTBURSTSIZE_8W 3
#define MCDE_OVLXCR_ROTBURSTSIZE_16W 4
#define MCDE_OVLXCR_ROTBURSTSIZE_HW_1W 8
#define MCDE_OVLXCR_ROTBURSTSIZE_HW_2W 9
#define MCDE_OVLXCR_ROTBURSTSIZE_HW_4W 10
#define MCDE_OVLXCR_ROTBURSTSIZE_HW_8W 11
#define MCDE_OVLXCR_ROTBURSTSIZE_HW_16W 12

#define MCDE_OVL0CONF 0x00000404
#define MCDE_OVL1CONF 0x00000424
#define MCDE_OVL2CONF 0x00000444
#define MCDE_OVL3CONF 0x00000464
#define MCDE_OVL4CONF 0x00000484
#define MCDE_OVL5CONF 0x000004A4
#define MCDE_OVLXCONF_PPL_SHIFT 0
#define MCDE_OVLXCONF_PPL_MASK 0x000007FF
#define MCDE_OVLXCONF_EXTSRC_ID_SHIFT 11
#define MCDE_OVLXCONF_EXTSRC_ID_MASK 0x00007800
#define MCDE_OVLXCONF_LPF_SHIFT 16
#define MCDE_OVLXCONF_LPF_MASK 0x07FF0000

#define MCDE_OVL0CONF2 0x00000408
#define MCDE_OVL1CONF2 0x00000428
#define MCDE_OVL2CONF2 0x00000448
#define MCDE_OVL3CONF2 0x00000468
#define MCDE_OVL4CONF2 0x00000488
#define MCDE_OVL5CONF2 0x000004A8
#define MCDE_OVLXCONF2_BP_PER_PIXEL_ALPHA 0
#define MCDE_OVLXCONF2_BP_CONSTANT_ALPHA BIT(0)
#define MCDE_OVLXCONF2_ALPHAVALUE_SHIFT 1
#define MCDE_OVLXCONF2_ALPHAVALUE_MASK 0x000001FE
#define MCDE_OVLXCONF2_OPQ BIT(9)
#define MCDE_OVLXCONF2_PIXOFF_SHIFT 10
#define MCDE_OVLXCONF2_PIXOFF_MASK 0x0000FC00
#define MCDE_OVLXCONF2_PIXELFETCHERWATERMARKLEVEL_SHIFT 16
#define MCDE_OVLXCONF2_PIXELFETCHERWATERMARKLEVEL_MASK 0x1FFF0000

#define MCDE_OVL0LJINC 0x0000040C
#define MCDE_OVL1LJINC 0x0000042C
#define MCDE_OVL2LJINC 0x0000044C
#define MCDE_OVL3LJINC 0x0000046C
#define MCDE_OVL4LJINC 0x0000048C
#define MCDE_OVL5LJINC 0x000004AC

#define MCDE_OVL0CROP 0x00000410
#define MCDE_OVL1CROP 0x00000430
#define MCDE_OVL2CROP 0x00000450
#define MCDE_OVL3CROP 0x00000470
#define MCDE_OVL4CROP 0x00000490
#define MCDE_OVL5CROP 0x000004B0
#define MCDE_OVLXCROP_TMRGN_SHIFT 0
#define MCDE_OVLXCROP_TMRGN_MASK 0x003FFFFF
#define MCDE_OVLXCROP_LMRGN_SHIFT 22
#define MCDE_OVLXCROP_LMRGN_MASK 0xFFC00000

#define MCDE_OVL0COMP 0x00000414
#define MCDE_OVL1COMP 0x00000434
#define MCDE_OVL2COMP 0x00000454
#define MCDE_OVL3COMP 0x00000474
#define MCDE_OVL4COMP 0x00000494
#define MCDE_OVL5COMP 0x000004B4
#define MCDE_OVLXCOMP_XPOS_SHIFT 0
#define MCDE_OVLXCOMP_XPOS_MASK 0x000007FF
#define MCDE_OVLXCOMP_CH_ID_SHIFT 11
#define MCDE_OVLXCOMP_CH_ID_MASK 0x00007800
#define MCDE_OVLXCOMP_YPOS_SHIFT 16
#define MCDE_OVLXCOMP_YPOS_MASK 0x07FF0000
#define MCDE_OVLXCOMP_Z_SHIFT 27
#define MCDE_OVLXCOMP_Z_MASK 0x78000000

#define MCDE_CRC 0x00000C00
#define MCDE_CRC_C1EN BIT(2)
#define MCDE_CRC_C2EN BIT(3)
#define MCDE_CRC_SYCEN0 BIT(7)
#define MCDE_CRC_SYCEN1 BIT(8)
#define MCDE_CRC_SIZE1 BIT(9)
#define MCDE_CRC_SIZE2 BIT(10)
#define MCDE_CRC_YUVCONVC1EN BIT(15)
#define MCDE_CRC_CS1EN BIT(16)
#define MCDE_CRC_CS2EN BIT(17)
#define MCDE_CRC_CS1POL BIT(19)
#define MCDE_CRC_CS2POL BIT(20)
#define MCDE_CRC_CD1POL BIT(21)
#define MCDE_CRC_CD2POL BIT(22)
#define MCDE_CRC_WR1POL BIT(23)
#define MCDE_CRC_WR2POL BIT(24)
#define MCDE_CRC_RD1POL BIT(25)
#define MCDE_CRC_RD2POL BIT(26)
#define MCDE_CRC_SYNCCTRL_SHIFT 29
#define MCDE_CRC_SYNCCTRL_MASK 0x60000000
#define MCDE_CRC_SYNCCTRL_NO_SYNC 0
#define MCDE_CRC_SYNCCTRL_DBI0 1
#define MCDE_CRC_SYNCCTRL_DBI1 2
#define MCDE_CRC_SYNCCTRL_PING_PONG 3
#define MCDE_CRC_CLAMPC1EN BIT(31)

#define MCDE_VSCRC0 0x00000C5C
#define MCDE_VSCRC1 0x00000C60
#define MCDE_VSCRC_VSPMIN_MASK 0x00000FFF
#define MCDE_VSCRC_VSPMAX_SHIFT 12
#define MCDE_VSCRC_VSPMAX_MASK 0x00FFF000
#define MCDE_VSCRC_VSPDIV_SHIFT 24
#define MCDE_VSCRC_VSPDIV_MASK 0x07000000
#define MCDE_VSCRC_VSPDIV_MCDECLK_DIV_1 0
#define MCDE_VSCRC_VSPDIV_MCDECLK_DIV_2 1
#define MCDE_VSCRC_VSPDIV_MCDECLK_DIV_4 2
#define MCDE_VSCRC_VSPDIV_MCDECLK_DIV_8 3
#define MCDE_VSCRC_VSPDIV_MCDECLK_DIV_16 4
#define MCDE_VSCRC_VSPDIV_MCDECLK_DIV_32 5
#define MCDE_VSCRC_VSPDIV_MCDECLK_DIV_64 6
#define MCDE_VSCRC_VSPDIV_MCDECLK_DIV_128 7
#define MCDE_VSCRC_VSPOL BIT(27) /* 0 active high, 1 active low */
#define MCDE_VSCRC_VSSEL BIT(28) /* 0 VSYNC0, 1 VSYNC1 */
#define MCDE_VSCRC_VSDBL BIT(29)

/* Channel config 0..3 */
#define MCDE_CHNL0CONF 0x00000600
#define MCDE_CHNL1CONF 0x00000620
#define MCDE_CHNL2CONF 0x00000640
#define MCDE_CHNL3CONF 0x00000660
#define MCDE_CHNLXCONF_PPL_SHIFT 0
#define MCDE_CHNLXCONF_PPL_MASK 0x000007FF
#define MCDE_CHNLXCONF_LPF_SHIFT 16
#define MCDE_CHNLXCONF_LPF_MASK 0x07FF0000
#define MCDE_MAX_WIDTH 2048

/* Channel status 0..3 */
#define MCDE_CHNL0STAT 0x00000604
#define MCDE_CHNL1STAT 0x00000624
#define MCDE_CHNL2STAT 0x00000644
#define MCDE_CHNL3STAT 0x00000664
#define MCDE_CHNLXSTAT_CHNLRD BIT(0)
#define MCDE_CHNLXSTAT_CHNLA BIT(1)
#define MCDE_CHNLXSTAT_CHNLBLBCKGND_EN BIT(16)
#define MCDE_CHNLXSTAT_PPLX2_V422 BIT(17)
#define MCDE_CHNLXSTAT_LPFX2_V422 BIT(18)

/* Sync settings for channel 0..3 */
#define MCDE_CHNL0SYNCHMOD 0x00000608
#define MCDE_CHNL1SYNCHMOD 0x00000628
#define MCDE_CHNL2SYNCHMOD 0x00000648
#define MCDE_CHNL3SYNCHMOD 0x00000668

#define MCDE_CHNLXSYNCHMOD_SRC_SYNCH_SHIFT 0
#define MCDE_CHNLXSYNCHMOD_SRC_SYNCH_MASK 0x00000003
#define MCDE_CHNLXSYNCHMOD_SRC_SYNCH_HARDWARE 0
#define MCDE_CHNLXSYNCHMOD_SRC_SYNCH_NO_SYNCH 1
#define MCDE_CHNLXSYNCHMOD_SRC_SYNCH_SOFTWARE 2
#define MCDE_CHNLXSYNCHMOD_OUT_SYNCH_SRC_SHIFT 2
#define MCDE_CHNLXSYNCHMOD_OUT_SYNCH_SRC_MASK 0x0000001C
#define MCDE_CHNLXSYNCHMOD_OUT_SYNCH_SRC_FORMATTER 0
#define MCDE_CHNLXSYNCHMOD_OUT_SYNCH_SRC_TE0 1
#define MCDE_CHNLXSYNCHMOD_OUT_SYNCH_SRC_TE1 2

/* Software sync triggers for channel 0..3 */
#define MCDE_CHNL0SYNCHSW 0x0000060C
#define MCDE_CHNL1SYNCHSW 0x0000062C
#define MCDE_CHNL2SYNCHSW 0x0000064C
#define MCDE_CHNL3SYNCHSW 0x0000066C
#define MCDE_CHNLXSYNCHSW_SW_TRIG BIT(0)

#define MCDE_CHNL0BCKGNDCOL 0x00000610
#define MCDE_CHNL1BCKGNDCOL 0x00000630
#define MCDE_CHNL2BCKGNDCOL 0x00000650
#define MCDE_CHNL3BCKGNDCOL 0x00000670
#define MCDE_CHNLXBCKGNDCOL_B_SHIFT 0
#define MCDE_CHNLXBCKGNDCOL_B_MASK 0x000000FF
#define MCDE_CHNLXBCKGNDCOL_G_SHIFT 8
#define MCDE_CHNLXBCKGNDCOL_G_MASK 0x0000FF00
#define MCDE_CHNLXBCKGNDCOL_R_SHIFT 16
#define MCDE_CHNLXBCKGNDCOL_R_MASK 0x00FF0000

#define MCDE_CHNL0MUXING 0x00000614
#define MCDE_CHNL1MUXING 0x00000634
#define MCDE_CHNL2MUXING 0x00000654
#define MCDE_CHNL3MUXING 0x00000674
#define MCDE_CHNLXMUXING_FIFO_ID_FIFO_A 0
#define MCDE_CHNLXMUXING_FIFO_ID_FIFO_B 1
#define MCDE_CHNLXMUXING_FIFO_ID_FIFO_C0 2
#define MCDE_CHNLXMUXING_FIFO_ID_FIFO_C1 3

/* Pixel processing control registers for channel A B,  */
#define MCDE_CRA0 0x00000800
#define MCDE_CRB0 0x00000A00
#define MCDE_CRX0_FLOEN BIT(0)
#define MCDE_CRX0_POWEREN BIT(1)
#define MCDE_CRX0_BLENDEN BIT(2)
#define MCDE_CRX0_AFLICKEN BIT(3)
#define MCDE_CRX0_PALEN BIT(4)
#define MCDE_CRX0_DITHEN BIT(5)
#define MCDE_CRX0_GAMEN BIT(6)
#define MCDE_CRX0_KEYCTRL_SHIFT 7
#define MCDE_CRX0_KEYCTRL_MASK 0x00000380
#define MCDE_CRX0_KEYCTRL_OFF 0
#define MCDE_CRX0_KEYCTRL_ALPHA_RGB 1
#define MCDE_CRX0_KEYCTRL_RGB 2
#define MCDE_CRX0_KEYCTRL_FALPHA_FRGB 4
#define MCDE_CRX0_KEYCTRL_FRGB 5
#define MCDE_CRX0_BLENDCTRL BIT(10)
#define MCDE_CRX0_FLICKMODE_SHIFT 11
#define MCDE_CRX0_FLICKMODE_MASK 0x00001800
#define MCDE_CRX0_FLICKMODE_FORCE_FILTER_0 0
#define MCDE_CRX0_FLICKMODE_ADAPTIVE 1
#define MCDE_CRX0_FLICKMODE_TEST_MODE 2
#define MCDE_CRX0_FLOCKFORMAT_RGB BIT(13) /* 0 = YCVCR */
#define MCDE_CRX0_PALMODE_GAMMA BIT(14) /* 0 = palette */
#define MCDE_CRX0_OLEDEN BIT(15)
#define MCDE_CRX0_ALPHABLEND_SHIFT 16
#define MCDE_CRX0_ALPHABLEND_MASK 0x00FF0000
#define MCDE_CRX0_ROTEN BIT(24)

#define MCDE_CRA1 0x00000804
#define MCDE_CRB1 0x00000A04
#define MCDE_CRX1_PCD_SHIFT 0
#define MCDE_CRX1_PCD_MASK 0x000003FF
#define MCDE_CRX1_CLKSEL_SHIFT 10
#define MCDE_CRX1_CLKSEL_MASK 0x00001C00
#define MCDE_CRX1_CLKSEL_CLKPLL72 0
#define MCDE_CRX1_CLKSEL_CLKPLL27 2
#define MCDE_CRX1_CLKSEL_TV1CLK 3
#define MCDE_CRX1_CLKSEL_TV2CLK 4
#define MCDE_CRX1_CLKSEL_MCDECLK 5
#define MCDE_CRX1_CDWIN_SHIFT 13
#define MCDE_CRX1_CDWIN_MASK 0x0001E000
#define MCDE_CRX1_CDWIN_8BPP_C1 0
#define MCDE_CRX1_CDWIN_12BPP_C1 1
#define MCDE_CRX1_CDWIN_12BPP_C2 2
#define MCDE_CRX1_CDWIN_16BPP_C1 3
#define MCDE_CRX1_CDWIN_16BPP_C2 4
#define MCDE_CRX1_CDWIN_16BPP_C3 5
#define MCDE_CRX1_CDWIN_18BPP_C1 6
#define MCDE_CRX1_CDWIN_18BPP_C2 7
#define MCDE_CRX1_CDWIN_24BPP 8
#define MCDE_CRX1_OUTBPP_SHIFT 25
#define MCDE_CRX1_OUTBPP_MASK 0x1E000000
#define MCDE_CRX1_OUTBPP_MONO1 0
#define MCDE_CRX1_OUTBPP_MONO2 1
#define MCDE_CRX1_OUTBPP_MONO4 2
#define MCDE_CRX1_OUTBPP_MONO8 3
#define MCDE_CRX1_OUTBPP_8BPP 4
#define MCDE_CRX1_OUTBPP_12BPP 5
#define MCDE_CRX1_OUTBPP_15BPP 6
#define MCDE_CRX1_OUTBPP_16BPP 7
#define MCDE_CRX1_OUTBPP_18BPP 8
#define MCDE_CRX1_OUTBPP_24BPP 9
#define MCDE_CRX1_BCD BIT(29)
#define MCDE_CRA1_CLKTYPE_TVXCLKSEL1 BIT(30) /* 0 = TVXCLKSEL1 */

#define MCDE_COLKEYA 0x00000808
#define MCDE_COLKEYB 0x00000A08

#define MCDE_FCOLKEYA 0x0000080C
#define MCDE_FCOLKEYB 0x00000A0C

#define MCDE_RGBCONV1A 0x00000810
#define MCDE_RGBCONV1B 0x00000A10

#define MCDE_RGBCONV2A 0x00000814
#define MCDE_RGBCONV2B 0x00000A14

#define MCDE_RGBCONV3A 0x00000818
#define MCDE_RGBCONV3B 0x00000A18

#define MCDE_RGBCONV4A 0x0000081C
#define MCDE_RGBCONV4B 0x00000A1C

#define MCDE_RGBCONV5A 0x00000820
#define MCDE_RGBCONV5B 0x00000A20

#define MCDE_RGBCONV6A 0x00000824
#define MCDE_RGBCONV6B 0x00000A24

/* Rotation */
#define MCDE_ROTACONF 0x0000087C
#define MCDE_ROTBCONF 0x00000A7C

#define MCDE_SYNCHCONFA 0x00000880
#define MCDE_SYNCHCONFB 0x00000A80

/* Channel A+B control registers */
#define MCDE_CTRLA 0x00000884
#define MCDE_CTRLB 0x00000A84
#define MCDE_CTRLX_FIFOWTRMRK_SHIFT 0
#define MCDE_CTRLX_FIFOWTRMRK_MASK 0x000003FF
#define MCDE_CTRLX_FIFOEMPTY BIT(12)
#define MCDE_CTRLX_FIFOFULL BIT(13)
#define MCDE_CTRLX_FORMID_SHIFT 16
#define MCDE_CTRLX_FORMID_MASK 0x00070000
#define MCDE_CTRLX_FORMID_DSI0VID 0
#define MCDE_CTRLX_FORMID_DSI0CMD 1
#define MCDE_CTRLX_FORMID_DSI1VID 2
#define MCDE_CTRLX_FORMID_DSI1CMD 3
#define MCDE_CTRLX_FORMID_DSI2VID 4
#define MCDE_CTRLX_FORMID_DSI2CMD 5
#define MCDE_CTRLX_FORMID_DPIA 0
#define MCDE_CTRLX_FORMID_DPIB 1
#define MCDE_CTRLX_FORMTYPE_SHIFT 20
#define MCDE_CTRLX_FORMTYPE_MASK 0x00700000
#define MCDE_CTRLX_FORMTYPE_DPITV 0
#define MCDE_CTRLX_FORMTYPE_DBI 1
#define MCDE_CTRLX_FORMTYPE_DSI 2

#define MCDE_DSIVID0CONF0 0x00000E00
#define MCDE_DSICMD0CONF0 0x00000E20
#define MCDE_DSIVID1CONF0 0x00000E40
#define MCDE_DSICMD1CONF0 0x00000E60
#define MCDE_DSIVID2CONF0 0x00000E80
#define MCDE_DSICMD2CONF0 0x00000EA0
#define MCDE_DSICONF0_BLANKING_SHIFT 0
#define MCDE_DSICONF0_BLANKING_MASK 0x000000FF
#define MCDE_DSICONF0_VID_MODE_CMD 0
#define MCDE_DSICONF0_VID_MODE_VID BIT(12)
#define MCDE_DSICONF0_CMD8 BIT(13)
#define MCDE_DSICONF0_BIT_SWAP BIT(16)
#define MCDE_DSICONF0_BYTE_SWAP BIT(17)
#define MCDE_DSICONF0_DCSVID_NOTGEN BIT(18)
#define MCDE_DSICONF0_PACKING_SHIFT 20
#define MCDE_DSICONF0_PACKING_MASK 0x00700000
#define MCDE_DSICONF0_PACKING_RGB565 0
#define MCDE_DSICONF0_PACKING_RGB666 1
#define MCDE_DSICONF0_PACKING_RGB666_PACKED 2
#define MCDE_DSICONF0_PACKING_RGB888 3
#define MCDE_DSICONF0_PACKING_HDTV 4

#define MCDE_DSIVID0FRAME 0x00000E04
#define MCDE_DSICMD0FRAME 0x00000E24
#define MCDE_DSIVID1FRAME 0x00000E44
#define MCDE_DSICMD1FRAME 0x00000E64
#define MCDE_DSIVID2FRAME 0x00000E84
#define MCDE_DSICMD2FRAME 0x00000EA4

#define MCDE_DSIVID0PKT 0x00000E08
#define MCDE_DSICMD0PKT 0x00000E28
#define MCDE_DSIVID1PKT 0x00000E48
#define MCDE_DSICMD1PKT 0x00000E68
#define MCDE_DSIVID2PKT 0x00000E88
#define MCDE_DSICMD2PKT 0x00000EA8

#define MCDE_DSIVID0SYNC 0x00000E0C
#define MCDE_DSICMD0SYNC 0x00000E2C
#define MCDE_DSIVID1SYNC 0x00000E4C
#define MCDE_DSICMD1SYNC 0x00000E6C
#define MCDE_DSIVID2SYNC 0x00000E8C
#define MCDE_DSICMD2SYNC 0x00000EAC

#define MCDE_DSIVID0CMDW 0x00000E10
#define MCDE_DSICMD0CMDW 0x00000E30
#define MCDE_DSIVID1CMDW 0x00000E50
#define MCDE_DSICMD1CMDW 0x00000E70
#define MCDE_DSIVID2CMDW 0x00000E90
#define MCDE_DSICMD2CMDW 0x00000EB0
#define MCDE_DSIVIDXCMDW_CMDW_CONTINUE_SHIFT 0
#define MCDE_DSIVIDXCMDW_CMDW_CONTINUE_MASK 0x0000FFFF
#define MCDE_DSIVIDXCMDW_CMDW_START_SHIFT 16
#define MCDE_DSIVIDXCMDW_CMDW_START_MASK 0xFFFF0000

#define MCDE_DSIVID0DELAY0 0x00000E14
#define MCDE_DSICMD0DELAY0 0x00000E34
#define MCDE_DSIVID1DELAY0 0x00000E54
#define MCDE_DSICMD1DELAY0 0x00000E74
#define MCDE_DSIVID2DELAY0 0x00000E94
#define MCDE_DSICMD2DELAY0 0x00000EB4

#define MCDE_DSIVID0DELAY1 0x00000E18
#define MCDE_DSICMD0DELAY1 0x00000E38
#define MCDE_DSIVID1DELAY1 0x00000E58
#define MCDE_DSICMD1DELAY1 0x00000E78
#define MCDE_DSIVID2DELAY1 0x00000E98
#define MCDE_DSICMD2DELAY1 0x00000EB8

void mcde_display_irq(struct mcde *mcde)
{
	u32 mispp, misovl, mischnl;
	bool vblank;

	/* Handle display IRQs */
	mispp = readl(mcde->regs + MCDE_MISPP);
	misovl = readl(mcde->regs + MCDE_MISOVL);
	mischnl = readl(mcde->regs + MCDE_MISCHNL);

	/*
	 * Handle IRQs from the DSI link. All IRQs from the DSI links
	 * are just latched onto the MCDE IRQ line, so we need to traverse
	 * any active DSI masters and check if an IRQ is originating from
	 * them.
	 *
	 * FIXME: Currently only one DSI link is supported.
	 */
	if (mcde_dsi_irq(mcde->mdsi)) {
		u32 val;

		/*
		 * In oneshot mode we do not send continuous updates
		 * to the display, instead we only push out updates when
		 * the update function is called, then we disable the
		 * flow on the channel once we get the TE IRQ.
		 */
		if (mcde->oneshot_mode) {
			spin_lock(&mcde->flow_lock);
			if (--mcde->flow_active == 0) {
				dev_dbg(mcde->dev, "TE0 IRQ\n");
				/* Disable FIFO A flow */
				val = readl(mcde->regs + MCDE_CRA0);
				val &= ~MCDE_CRX0_FLOEN;
				writel(val, mcde->regs + MCDE_CRA0);
			}
			spin_unlock(&mcde->flow_lock);
		}
	}

	/* Vblank from one of the channels */
	if (mispp & MCDE_PP_VCMPA) {
		dev_dbg(mcde->dev, "chnl A vblank IRQ\n");
		vblank = true;
	}
	if (mispp & MCDE_PP_VCMPB) {
		dev_dbg(mcde->dev, "chnl B vblank IRQ\n");
		vblank = true;
	}
	if (mispp & MCDE_PP_VCMPC0)
		dev_dbg(mcde->dev, "chnl C0 vblank IRQ\n");
	if (mispp & MCDE_PP_VCMPC1)
		dev_dbg(mcde->dev, "chnl C1 vblank IRQ\n");
	if (mispp & MCDE_PP_VSCC0)
		dev_dbg(mcde->dev, "chnl C0 TE IRQ\n");
	if (mispp & MCDE_PP_VSCC1)
		dev_dbg(mcde->dev, "chnl C1 TE IRQ\n");
	writel(mispp, mcde->regs + MCDE_RISPP);

	if (vblank)
		drm_crtc_handle_vblank(&mcde->pipe.crtc);

	if (misovl)
		dev_info(mcde->dev, "some stray overlay IRQ %08x\n", misovl);
	writel(misovl, mcde->regs + MCDE_RISOVL);

	if (mischnl)
		dev_info(mcde->dev, "some stray channel error IRQ %08x\n",
			 mischnl);
	writel(mischnl, mcde->regs + MCDE_RISCHNL);
}

void mcde_display_disable_irqs(struct mcde *mcde)
{
	/* Disable all IRQs */
	writel(0, mcde->regs + MCDE_IMSCPP);
	writel(0, mcde->regs + MCDE_IMSCOVL);
	writel(0, mcde->regs + MCDE_IMSCCHNL);

	/* Clear any pending IRQs */
	writel(0xFFFFFFFF, mcde->regs + MCDE_RISPP);
	writel(0xFFFFFFFF, mcde->regs + MCDE_RISOVL);
	writel(0xFFFFFFFF, mcde->regs + MCDE_RISCHNL);
}

static int mcde_display_check(struct drm_simple_display_pipe *pipe,
			       struct drm_plane_state *pstate,
			       struct drm_crtc_state *cstate)
{
	const struct drm_display_mode *mode = &cstate->mode;
	struct drm_framebuffer *old_fb = pipe->plane.state->fb;
	struct drm_framebuffer *fb = pstate->fb;

	if (fb) {
		u32 offset = drm_fb_cma_get_gem_addr(fb, pstate, 0);

		/* FB base address must be dword aligned. */
		if (offset & 3) {
			DRM_DEBUG_KMS("FB not 32-bit aligned\n");
			return -EINVAL;
		}

		/*
		 * There's no pitch register, the mode's hdisplay
		 * controls this.
		 */
		if (fb->pitches[0] != mode->hdisplay * fb->format->cpp[0]) {
			DRM_DEBUG_KMS("can't handle pitches\n");
			return -EINVAL;
		}

		/*
		 * We can't change the FB format in a flicker-free
		 * manner (and only update it during CRTC enable).
		 */
		if (old_fb && old_fb->format != fb->format)
			cstate->mode_changed = true;
	}

	return 0;
}

static int mcde_dsi_get_pkt_div(int ppl, int fifo_size)
{
	/*
	 * DSI command mode line packets should be split into an even number of
	 * packets smaller than or equal to the fifo size.
	 */
	int div;
	const int max_div = DIV_ROUND_UP(MCDE_MAX_WIDTH, fifo_size);

	for (div = 1; div < max_div; div++)
		if (ppl % div == 0 && ppl / div <= fifo_size)
			return div;
	return 1;
}

static void mcde_display_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *cstate,
				struct drm_plane_state *plane_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_plane *plane = &pipe->plane;
	struct drm_device *drm = crtc->dev;
	struct mcde *mcde = drm->dev_private;
	const struct drm_display_mode *mode = &cstate->mode;
	struct drm_framebuffer *fb = plane->state->fb;
	u32 format = fb->format->format;
	u32 formatter_ppl = mode->hdisplay; /* pixels per line */
	u32 formatter_lpf = mode->vdisplay; /* lines per frame */
	int pkt_size, fifo_wtrmrk;
	int cpp = drm_format_plane_cpp(format, 0);
	int formatter_cpp;
	struct drm_format_name_buf tmp;
	u32 formatter_frame;
	u32 pkt_div;
	u32 val;

	dev_info(drm->dev, "enable MCDE, %d x %d format %s\n",
		 mode->hdisplay, mode->vdisplay,
		 drm_get_format_name(format, &tmp));
	if (!mcde->mdsi) {
		/* TODO: deal with this for non-DSI output */
		dev_err(drm->dev, "no DSI master attached!\n");
		return;
	}

	dev_info(drm->dev, "output in %s mode, format %dbpp\n",
		 (mcde->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO) ?
		 "VIDEO" : "CMD",
		 mipi_dsi_pixel_format_to_bpp(mcde->mdsi->format));
	formatter_cpp =
		mipi_dsi_pixel_format_to_bpp(mcde->mdsi->format) / 8;
	dev_info(drm->dev, "overlay CPP %d bytes, DSI CPP %d bytes\n",
		 cpp,
		 formatter_cpp);

	/* Calculations from mcde_fmtr_dsi.c, fmtr_dsi_enable_video() */

	/*
	 * Set up FIFO A watermark level:
	 * 128 for LCD 32bpp video mode
	 * 48  for LCD 32bpp command mode
	 * 128 for LCD 16bpp video mode
	 * 64  for LCD 16bpp command mode
	 * 128 for HDMI 32bpp
	 * 192 for HDMI 16bpp
	 */
	fifo_wtrmrk = mode->hdisplay;
	if (mcde->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO) {
		fifo_wtrmrk = min(fifo_wtrmrk, 128);
		pkt_div = 1;
	} else {
		fifo_wtrmrk = min(fifo_wtrmrk, 48);
		/* The FIFO is 640 entries deep on this v3 hardware */
		pkt_div = mcde_dsi_get_pkt_div(mode->hdisplay, 640);
	}
	dev_dbg(drm->dev, "FIFO watermark after flooring: %d bytes\n",
		 fifo_wtrmrk);
	dev_dbg(drm->dev, "Packet divisor: %d bytes\n", pkt_div);


	/* NOTE: pkt_div is 1 for video mode */
	pkt_size = (formatter_ppl * formatter_cpp) / pkt_div;
	/* Commands CMD8 need one extra byte */
	if (!(mcde->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO))
		pkt_size++;

	dev_dbg(drm->dev, "DSI packet size: %d * %d bytes per line\n",
		pkt_size, pkt_div);
	dev_dbg(drm->dev, "Overlay frame size: %u bytes\n",
		mode->hdisplay * mode->vdisplay * cpp);
	mcde->stride = mode->hdisplay * cpp;
	dev_dbg(drm->dev, "Overlay line stride: %u bytes\n",
		mcde->stride);
	/* NOTE: pkt_div is 1 for video mode */
	formatter_frame = pkt_size * pkt_div * formatter_lpf;
	dev_dbg(drm->dev, "Formatter frame size: %u bytes\n", formatter_frame);

	/* Check that the hardware on channel A is in a sane state */
	val = readl(mcde->regs + MCDE_CTRLA);
	if (!(val & MCDE_CTRLX_FIFOEMPTY)) {
		int timeout = 100;

		dev_err(drm->dev, "Channel A FIFO not empty (handover)\n");
		/* Attempt to clear the FIFO */
		/* Enable FIFO A flow */
		val = readl(mcde->regs + MCDE_CRA0);
		val |= MCDE_CRX0_FLOEN;
		writel(val, mcde->regs + MCDE_CRA0);
		/* Trigger a software sync out on channel 0 */
		writel(MCDE_CHNLXSYNCHSW_SW_TRIG,
		       mcde->regs + MCDE_CHNL0SYNCHSW);
		/* Disable FIFO A flow again */
		val = readl(mcde->regs + MCDE_CRA0);
		val &= ~MCDE_CRX0_FLOEN;
		writel(val, mcde->regs + MCDE_CRA0);
		while (readl(mcde->regs + MCDE_CRA0) & MCDE_CRX0_FLOEN) {
			usleep_range(1000, 1500);
			if (!--timeout) {
				dev_err(drm->dev,
					"FIFO A timeout while clearing\n");
				break;
			}
		}
	}

	/* Set up FIFO A and channel 0 (based on chnl_update_registers()) */

	if (mcde->te_sync) {
		/*
		 * Turn on hardware TE0 synchronization
		 */
		val = MCDE_CHNLXSYNCHMOD_SRC_SYNCH_HARDWARE
			<< MCDE_CHNLXSYNCHMOD_SRC_SYNCH_SHIFT;
		val |= MCDE_CHNLXSYNCHMOD_OUT_SYNCH_SRC_TE0
			<< MCDE_CHNLXSYNCHMOD_OUT_SYNCH_SRC_SHIFT;
	} else {
		/*
		 * Set up sync source to software, out sync formatter
		 * Code mostly from mcde_hw.c chnl_update_registers()
		 */
		val = MCDE_CHNLXSYNCHMOD_SRC_SYNCH_SOFTWARE
			<< MCDE_CHNLXSYNCHMOD_SRC_SYNCH_SHIFT;
		val |= MCDE_CHNLXSYNCHMOD_OUT_SYNCH_SRC_FORMATTER
			<< MCDE_CHNLXSYNCHMOD_OUT_SYNCH_SRC_SHIFT;
	}
	writel(val, mcde->regs + MCDE_CHNL0SYNCHMOD);

	/* Set up FIFO for channel A */
	val = fifo_wtrmrk << MCDE_CTRLX_FIFOWTRMRK_SHIFT;
	/* We only support DSI formatting for now */
	val |= MCDE_CTRLX_FORMTYPE_DSI <<
		MCDE_CTRLX_FORMTYPE_SHIFT;
	/* Use formatter 0 for FIFO A */
	val |= 0 << MCDE_CTRLX_FORMID_SHIFT;
	writel(val, mcde->regs + MCDE_CTRLA);

	/* Set up muxing: connect channel 0 to FIFO A */
	writel(MCDE_CHNLXMUXING_FIFO_ID_FIFO_A,
	       mcde->regs + MCDE_CHNL0MUXING);

	/* Pixel-per-line and line-per-frame set-up for the channel */
	val = (mode->hdisplay - 1) << MCDE_CHNLXCONF_PPL_SHIFT;
	val |= (mode->vdisplay - 1) << MCDE_CHNLXCONF_LPF_SHIFT;
	writel(val, mcde->regs + MCDE_CHNL0CONF);

	/*
	 * Normalize color conversion:
	 * black background, OLED conversion disable on channel 0
	 * FIFO A, no rotation.
	 */
	val = MCDE_CHNLXSTAT_CHNLBLBCKGND_EN |
		MCDE_CHNLXSTAT_CHNLRD;
	writel(val, mcde->regs + MCDE_CHNL0STAT);
	writel(0, mcde->regs + MCDE_CHNL0BCKGNDCOL);
	/* Blend source with Alpha 0xff on FIFO A */
	val = MCDE_CRX0_BLENDEN |
		0xff << MCDE_CRX0_ALPHABLEND_SHIFT;
	writel(val, mcde->regs + MCDE_CRA0);

	/*
	 * Configure external source 0 one buffer (buffer 0)
	 * primary overlay ID 0.
	 * From mcde_hw.c ovly_update_registers() in the vendor tree
	 */
	val = 0 << MCDE_EXTSRCXCONF_BUF_ID_SHIFT;
	val |= 1 << MCDE_EXTSRCXCONF_BUF_NB_SHIFT;
	val |= 0 << MCDE_EXTSRCXCONF_PRI_OVLID_SHIFT;
	/*
	 * MCDE has inverse semantics from DRM on RBG/BGR which is why
	 * all the modes are inversed here.
	 */
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		val |= MCDE_EXTSRCXCONF_BPP_ARGB8888 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		val |= MCDE_EXTSRCXCONF_BGR;
		break;
	case DRM_FORMAT_ABGR8888:
		val |= MCDE_EXTSRCXCONF_BPP_ARGB8888 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		break;
	case DRM_FORMAT_XRGB8888:
		val |= MCDE_EXTSRCXCONF_BPP_XRGB8888 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		val |= MCDE_EXTSRCXCONF_BGR;
		break;
	case DRM_FORMAT_XBGR8888:
		val |= MCDE_EXTSRCXCONF_BPP_XRGB8888 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		break;
	case DRM_FORMAT_RGB888:
		val |= MCDE_EXTSRCXCONF_BPP_RGB888 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		val |= MCDE_EXTSRCXCONF_BGR;
		break;
	case DRM_FORMAT_BGR888:
		val |= MCDE_EXTSRCXCONF_BPP_RGB888 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		break;
	case DRM_FORMAT_ARGB4444:
		val |= MCDE_EXTSRCXCONF_BPP_ARGB4444 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		val |= MCDE_EXTSRCXCONF_BGR;
		break;
	case DRM_FORMAT_ABGR4444:
		val |= MCDE_EXTSRCXCONF_BPP_ARGB4444 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		break;
	case DRM_FORMAT_XRGB4444:
		val |= MCDE_EXTSRCXCONF_BPP_RGB444 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		val |= MCDE_EXTSRCXCONF_BGR;
		break;
	case DRM_FORMAT_XBGR4444:
		val |= MCDE_EXTSRCXCONF_BPP_RGB444 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		break;
	case DRM_FORMAT_XRGB1555:
		val |= MCDE_EXTSRCXCONF_BPP_IRGB1555 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		val |= MCDE_EXTSRCXCONF_BGR;
		break;
	case DRM_FORMAT_XBGR1555:
		val |= MCDE_EXTSRCXCONF_BPP_IRGB1555 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		break;
	case DRM_FORMAT_RGB565:
		val |= MCDE_EXTSRCXCONF_BPP_RGB565 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		val |= MCDE_EXTSRCXCONF_BGR;
		break;
	case DRM_FORMAT_BGR565:
		val |= MCDE_EXTSRCXCONF_BPP_RGB565 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		break;
	case DRM_FORMAT_YUV422:
		val |= MCDE_EXTSRCXCONF_BPP_YCBCR422 <<
			MCDE_EXTSRCXCONF_BPP_SHIFT;
		break;
	default:
		dev_err(drm->dev, "Unknown pixel format 0x%08x\n",
			fb->format->format);
		break;
	}
	writel(val, mcde->regs + MCDE_EXTSRC0CONF);
	/* Software select, primary */
	val = MCDE_EXTSRC0CR_SEL_MOD_SOFTWARE_SEL;
	val |= MCDE_EXTSRC0CR_MULTIOVL_CTRL_PRIMARY;
	writel(val, mcde->regs + MCDE_EXTSRC0CR);

	/* Configure overlay 0 */
	val = mode->hdisplay << MCDE_OVLXCONF_PPL_SHIFT;
	val |= mode->vdisplay << MCDE_OVLXCONF_LPF_SHIFT;
	/* Use external source 0 that we just configured */
	val |= 0 << MCDE_OVLXCONF_EXTSRC_ID_SHIFT;
	writel(val, mcde->regs + MCDE_OVL0CONF);

	val = MCDE_OVLXCONF2_BP_PER_PIXEL_ALPHA;
	val |= 0xff << MCDE_OVLXCONF2_ALPHAVALUE_SHIFT;
	/* OPQ: overlay is opaque */
	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_XBGR1555:
		/* No OPQ */
		break;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_BGR565:
	case DRM_FORMAT_YUV422:
		val |= MCDE_OVLXCONF2_OPQ;
		break;
	default:
		dev_err(drm->dev, "Unknown pixel format 0x%08x\n",
			fb->format->format);
		break;
	}
	/* The default watermark level for overlay 0 is 48 */
	val |= 48 << MCDE_OVLXCONF2_PIXELFETCHERWATERMARKLEVEL_SHIFT;
	writel(val, mcde->regs + MCDE_OVL0CONF2);

	/* Number of bytes to fetch per line */
	writel(mcde->stride, mcde->regs + MCDE_OVL0LJINC);
	/* No cropping */
	writel(0, mcde->regs + MCDE_OVL0CROP);

	/* Set up overlay control register */
	val = MCDE_OVLXCR_OVLEN;
	val |= MCDE_OVLXCR_COLCCTRL_DISABLED;
	val |= MCDE_OVLXCR_BURSTSIZE_8W <<
		MCDE_OVLXCR_BURSTSIZE_SHIFT;
	val |= MCDE_OVLXCR_MAXOUTSTANDING_8_REQ <<
		MCDE_OVLXCR_MAXOUTSTANDING_SHIFT;
	/* Not using rotation but set it up anyways */
	val |= MCDE_OVLXCR_ROTBURSTSIZE_8W <<
		MCDE_OVLXCR_ROTBURSTSIZE_SHIFT;
	writel(val, mcde->regs + MCDE_OVL0CR);

	/* Set-up from mcde_fmtr_dsi.c, fmtr_dsi_enable_video() */

	/* Channel formatter set-up for channel A */
	val = MCDE_CRX1_CLKSEL_MCDECLK << MCDE_CRX1_CLKSEL_SHIFT;
	/* FIXME: when adding DPI support add OUTBPP etc here */
	writel(val, mcde->regs + MCDE_CRA1);

	/*
	 * Enable formatter
	 * 8 bit commands and DCS commands (notgen = not generic)
	 */
	val = MCDE_DSICONF0_CMD8 | MCDE_DSICONF0_DCSVID_NOTGEN;
	if (mcde->mdsi->mode_flags & MIPI_DSI_MODE_VIDEO)
		val |= MCDE_DSICONF0_VID_MODE_VID;
	switch (mcde->mdsi->format) {
	case MIPI_DSI_FMT_RGB888:
		val |= MCDE_DSICONF0_PACKING_RGB888 <<
			MCDE_DSICONF0_PACKING_SHIFT;
		break;
	case MIPI_DSI_FMT_RGB666:
		val |= MCDE_DSICONF0_PACKING_RGB666 <<
			MCDE_DSICONF0_PACKING_SHIFT;
		break;
	case MIPI_DSI_FMT_RGB666_PACKED:
		val |= MCDE_DSICONF0_PACKING_RGB666_PACKED <<
			MCDE_DSICONF0_PACKING_SHIFT;
		break;
	case MIPI_DSI_FMT_RGB565:
		val |= MCDE_DSICONF0_PACKING_RGB565 <<
			MCDE_DSICONF0_PACKING_SHIFT;
		break;
	default:
		dev_err(drm->dev, "unknown DSI format\n");
		return;
	}
	writel(val, mcde->regs + MCDE_DSIVID0CONF0);

	writel(formatter_frame, mcde->regs + MCDE_DSIVID0FRAME);
	writel(pkt_size, mcde->regs + MCDE_DSIVID0PKT);
	writel(0, mcde->regs + MCDE_DSIVID0SYNC);
	/* Define the MIPI command: we want to write into display memory */
	val = MIPI_DCS_WRITE_MEMORY_CONTINUE <<
		MCDE_DSIVIDXCMDW_CMDW_CONTINUE_SHIFT;
	val |= MIPI_DCS_WRITE_MEMORY_START <<
		MCDE_DSIVIDXCMDW_CMDW_START_SHIFT;
	writel(val, mcde->regs + MCDE_DSIVID0CMDW);
	/*
	 * FIXME: the vendor driver has some hack around this value in
	 * CMD mode with autotrig.
	 */
	writel(0, mcde->regs + MCDE_DSIVID0DELAY0);
	writel(0, mcde->regs + MCDE_DSIVID0DELAY1);

	if (mcde->te_sync) {
		if (mode->flags & DRM_MODE_FLAG_NVSYNC)
			val = MCDE_VSCRC_VSPOL;
		else
			val = 0;
		writel(val, mcde->regs + MCDE_VSCRC0);
		/* Enable VSYNC capture on TE0 */
		val = readl(mcde->regs + MCDE_CRC);
		val |= MCDE_CRC_SYCEN0;
		writel(val, mcde->regs + MCDE_CRC);
		drm_crtc_vblank_on(crtc);
	}

	dev_info(drm->dev, "MCDE display is enabled\n");
	mcde->enabled = true;
}

static void mcde_display_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct mcde *mcde = drm->dev_private;
	int timeout = 100;
	u32 val;

	/* Stops framebuffer updates */
	mcde->enabled = false;

	if (mcde->te_sync)
		drm_crtc_vblank_off(crtc);

	/* Disable FIFO A flow */
	val = readl(mcde->regs + MCDE_CRA0);
	val &= ~MCDE_CRX0_FLOEN;
	writel(val, mcde->regs + MCDE_CRA0);
	while (readl(mcde->regs + MCDE_CRA0) & MCDE_CRX0_FLOEN) {
		usleep_range(1000, 1500);
		if (!--timeout) {
			dev_err(drm->dev,
				"FIFO A timeout while stopping\n");
			break;
		}
	}
	spin_lock(&mcde->flow_lock);
	mcde->flow_active = 0;
	spin_unlock(&mcde->flow_lock);

	dev_info(drm->dev, "MCDE display is disabled\n");
}

static void mcde_display_send_one_frame(struct mcde *mcde)
{
	int timeout = 100;
	u32 val;

	/* Request a TE ACK */
	if (mcde->te_sync)
		mcde_dsi_te_request(mcde->mdsi);

	/* Enable FIFO A flow */
	spin_lock(&mcde->flow_lock);
	val = readl(mcde->regs + MCDE_CRA0);
	val |= MCDE_CRX0_FLOEN;
	writel(val, mcde->regs + MCDE_CRA0);
	mcde->flow_active++;
	spin_unlock(&mcde->flow_lock);

	if (mcde->te_sync) {
		/*
		 * If oneshot mode is enabled, the flow will be disabled
		 * when the TE0 IRQ arrives in the interrupt handler. Otherwise
		 * updates are continuously streamed to the display after this
		 * point.
		 */
		dev_dbg(mcde->dev, "sent TE0 framebuffer update\n");
		return;
	}

	/* Trigger a software sync out on channel 0 */
	writel(MCDE_CHNLXSYNCHSW_SW_TRIG,
	       mcde->regs + MCDE_CHNL0SYNCHSW);
	/* Disable FIFO A flow again */
	spin_lock(&mcde->flow_lock);
	val = readl(mcde->regs + MCDE_CRA0);
	val &= ~MCDE_CRX0_FLOEN;
	writel(val, mcde->regs + MCDE_CRA0);
	mcde->flow_active = 0;
	spin_unlock(&mcde->flow_lock);

	/*
	 * At this point the DSI link should be running a
	 * frame update.
	 */
	while (readl(mcde->regs + MCDE_CRA0) & MCDE_CRX0_FLOEN) {
		usleep_range(1000, 1500);
		if (!--timeout) {
			dev_err(mcde->dev, "FIFO A timeout\n");
			break;
		}
	}
	dev_dbg(mcde->dev, "sent SW framebuffer update\n");
}

static void mcde_display_update(struct drm_simple_display_pipe *pipe,
				 struct drm_plane_state *old_pstate)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct mcde *mcde = drm->dev_private;
	struct drm_pending_vblank_event *event = crtc->state->event;
	struct drm_plane *plane = &pipe->plane;
	struct drm_plane_state *pstate = plane->state;
	struct drm_framebuffer *fb = pstate->fb;

	/*
	 * We do not start sending framebuffer updates before the
	 * display is enabled. Update events will however be dispatched
	 * from the DRM core before the display is enabled.
	 */
	if (fb && mcde->enabled) {
		/* Write bitmap base address to register */
		/* Set to 0x100000 to display random junk */
		writel(drm_fb_cma_get_gem_addr(fb, pstate, 0),
		       mcde->regs + MCDE_EXTSRCXA0);
		/*
		 * Base address for next line this is probably only used
		 * in interlace modes.
		 */
		writel(drm_fb_cma_get_gem_addr(fb, pstate, 0) + mcde->stride,
		       mcde->regs + MCDE_EXTSRCXA1);

		/* Set up overlay 0 compositor route to channel A */
		writel(0, mcde->regs + MCDE_OVL0COMP);

		/* Send a single frame using software sync */
		mcde_display_send_one_frame(mcde);
	}

	/* Handle any pending event */
	if (event) {
		crtc->state->event = NULL;

		spin_lock_irq(&crtc->dev->event_lock);
		/*
		 * Hardware must be on before we can arm any vblank event,
		 * this is not a scanout controller where there is always
		 * some periodic update going on, it is completely frozen
		 * until we get an update. If MCDE output isn't yet enabled,
		 * we just send a vblank dummy event back.
		 */
		if (mcde->enabled && crtc->state->active &&
		    drm_crtc_vblank_get(crtc) == 0 && !mcde->vblank_irq_on) {
			dev_dbg(mcde->dev, "arm vblank event\n");
			drm_crtc_arm_vblank_event(crtc, event);
		} else {
			dev_dbg(mcde->dev, "insert fake vblank event\n");
			drm_crtc_send_vblank_event(crtc, event);
		}

		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static int mcde_display_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct mcde *mcde = drm->dev_private;
	u32 val;

	/* Enable all VBLANK IRQs */
	val = MCDE_PP_VCMPA |
		MCDE_PP_VCMPB |
		MCDE_PP_VSCC0 |
		MCDE_PP_VSCC1 |
		MCDE_PP_VCMPC0 |
		MCDE_PP_VCMPC1;
	writel(val, mcde->regs + MCDE_IMSCPP);
	mcde->vblank_irq_on = true;

	return 0;
}

static void mcde_display_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_device *drm = crtc->dev;
	struct mcde *mcde = drm->dev_private;

	/* Disable all VBLANK IRQs */
	writel(0, mcde->regs + MCDE_IMSCPP);
	/* Clear any pending IRQs */
	writel(0xFFFFFFFF, mcde->regs + MCDE_RISPP);
	mcde->vblank_irq_on = false;
}

static int mcde_display_prepare_fb(struct drm_simple_display_pipe *pipe,
				    struct drm_plane_state *plane_state)
{
	return drm_gem_fb_prepare_fb(&pipe->plane, plane_state);
}

static struct drm_simple_display_pipe_funcs mcde_display_funcs = {
	.check = mcde_display_check,
	.enable = mcde_display_enable,
	.disable = mcde_display_disable,
	.update = mcde_display_update,
	.prepare_fb = mcde_display_prepare_fb,
};

int mcde_display_init(struct drm_device *drm)
{
	struct mcde *mcde = drm->dev_private;
	int ret;
	static const u32 formats[] = {
		DRM_FORMAT_ARGB8888,
		DRM_FORMAT_ABGR8888,
		DRM_FORMAT_XRGB8888,
		DRM_FORMAT_XBGR8888,
		DRM_FORMAT_RGB888,
		DRM_FORMAT_BGR888,
		DRM_FORMAT_ARGB4444,
		DRM_FORMAT_ABGR4444,
		DRM_FORMAT_XRGB4444,
		DRM_FORMAT_XBGR4444,
		/* These are actually IRGB1555 so intensity bit is lost */
		DRM_FORMAT_XRGB1555,
		DRM_FORMAT_XBGR1555,
		DRM_FORMAT_RGB565,
		DRM_FORMAT_BGR565,
		DRM_FORMAT_YUV422,
	};

	/* Provide vblank only when we have TE enabled */
	if (mcde->te_sync) {
		mcde_display_funcs.enable_vblank = mcde_display_enable_vblank;
		mcde_display_funcs.disable_vblank = mcde_display_disable_vblank;
	}

	ret = drm_simple_display_pipe_init(drm, &mcde->pipe,
					   &mcde_display_funcs,
					   formats, ARRAY_SIZE(formats),
					   NULL,
					   mcde->connector);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(mcde_display_init);

