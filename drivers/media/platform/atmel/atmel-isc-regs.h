#ifndef __ATMEL_ISC_REGS_H
#define __ATMEL_ISC_REGS_H

#include <linux/bitops.h>

/* ISC Control Enable Register 0 */
#define ISC_CTRLEN      0x00000000

#define ISC_CTRLEN_CAPTURE              BIT(0)
#define ISC_CTRLEN_CAPTURE_MASK         BIT(0)

#define ISC_CTRLEN_UPPRO                BIT(1)
#define ISC_CTRLEN_UPPRO_MASK           BIT(1)

#define ISC_CTRLEN_HISREQ               BIT(2)
#define ISC_CTRLEN_HISREQ_MASK          BIT(2)

#define ISC_CTRLEN_HISCLR               BIT(3)
#define ISC_CTRLEN_HISCLR_MASK          BIT(3)

/* ISC Control Disable Register 0 */
#define ISC_CTRLDIS     0x00000004

#define ISC_CTRLDIS_DISABLE             BIT(0)
#define ISC_CTRLDIS_DISABLE_MASK        BIT(0)

#define ISC_CTRLDIS_SWRST               BIT(8)
#define ISC_CTRLDIS_SWRST_MASK          BIT(8)

/* ISC Control Status Register 0 */
#define ISC_CTRLSR      0x00000008

#define ISC_CTRLSR_CAPTURE      BIT(0)
#define ISC_CTRLSR_UPPRO        BIT(1)
#define ISC_CTRLSR_HISREQ       BIT(2)
#define ISC_CTRLSR_FIELD        BIT(4)
#define ISC_CTRLSR_SIP          BIT(31)

/* ISC Parallel Front End Configuration 0 Register */
#define ISC_PFE_CFG0    0x0000000c

#define ISC_PFE_CFG0_HPOL_LOW   BIT(0)
#define ISC_PFE_CFG0_HPOL_HIGH  0x0
#define ISC_PFE_CFG0_HPOL_MASK  BIT(0)

#define ISC_PFE_CFG0_VPOL_LOW   BIT(1)
#define ISC_PFE_CFG0_VPOL_HIGH  0x0
#define ISC_PFE_CFG0_VPOL_MASK  BIT(1)

#define ISC_PFE_CFG0_PPOL_LOW   BIT(2)
#define ISC_PFE_CFG0_PPOL_HIGH  0x0
#define ISC_PFE_CFG0_PPOL_MASK  BIT(2)

#define ISC_PFE_CFG0_MODE_PROGRESSIVE   0x0
#define ISC_PFE_CFG0_MODE_MASK          GENMASK(6, 4)

#define ISC_PFE_CFG0_BPS_EIGHT  (0x4 << 28)
#define ISC_PFG_CFG0_BPS_NINE   (0x3 << 28)
#define ISC_PFG_CFG0_BPS_TEN    (0x2 << 28)
#define ISC_PFG_CFG0_BPS_ELEVEN (0x1 << 28)
#define ISC_PFG_CFG0_BPS_TWELVE 0x0
#define ISC_PFE_CFG0_BPS_MASK   GENMASK(30, 28)

/* ISC Clock Enable Register */
#define ISC_CLKEN               0x00000018
#define ISC_CLKEN_EN            0x1
#define ISC_CLKEN_EN_SHIFT(n)   (n)
#define ISC_CLKEN_EN_MASK(n)    BIT(n)

/* ISC Clock Disable Register */
#define ISC_CLKDIS              0x0000001c
#define ISC_CLKDIS_DIS          0x1
#define ISC_CLKDIS_DIS_SHIFT(n) (n)
#define ISC_CLKDIS_DIS_MASK(n)  BIT(n)

/* ISC Clock Status Register */
#define ISC_CLKSR               0x00000020
#define ISC_CLKSR_CLK_MASK(n)   BIT(n)
#define ISC_CLKSR_SIP_PROGRESS  BIT(31)

/* ISC Clock Configuration Register */
#define ISC_CLKCFG              0x00000024
#define ISC_CLKCFG_DIV_SHIFT(n) ((n)*16)
#define ISC_CLKCFG_DIV_MASK(n)  GENMASK(((n)*16 + 7), (n)*16)
#define ISC_CLKCFG_SEL_SHIFT(n) ((n)*16 + 8)
#define ISC_CLKCFG_SEL_MASK(n)  GENMASK(((n)*17 + 8), ((n)*16 + 8))

/* ISC Interrupt Enable Register */
#define ISC_INTEN       0x00000028

#define ISC_INTEN_DDONE         BIT(8)
#define ISC_INTEN_DDONE_MASK    BIT(8)

/* ISC Interrupt Disable Register */
#define ISC_INTDIS      0x0000002c

#define ISC_INTDIS_DDONE        BIT(8)
#define ISC_INTDIS_DDONE_MASK   BIT(8)

/* ISC Interrupt Mask Register */
#define ISC_INTMASK     0x00000030

/* ISC Interrupt Status Register */
#define ISC_INTSR       0x00000034

#define ISC_INTSR_DDONE         BIT(8)

/* ISC White Balance Control Register */
#define ISC_WB_CTRL     0x00000058

#define ISC_WB_CTRL_EN          BIT(0)
#define ISC_WB_CTRL_DIS         0x0
#define ISC_WB_CTRL_MASK        BIT(0)

/* ISC White Balance Configuration Register */
#define ISC_WB_CFG      0x0000005c

#define ISC_WB_CFG_BAYCFG_GRGR  0x0
#define ISC_WB_CFG_BAYCFG_RGRG  0x1
#define ISC_WB_CFG_BAYCFG_GBGB  0x2
#define ISC_WB_CFG_BAYCFG_BGBG  0x3
#define ISC_WB_CFG_BAYCFG_MASK  GENMASK(1, 0)

/* ISC Color Filter Array Control Register */
#define ISC_CFA_CTRL    0x00000070

#define ISC_CFA_CTRL_EN         BIT(0)
#define ISC_CFA_CTRL_DIS        0x0
#define ISC_CFA_CTRL_MASK       BIT(0)

/* ISC Color Filter Array Configuration Register */
#define ISC_CFA_CFG     0x00000074

#define ISC_CFA_CFG_BAY_GRGR    0x0
#define ISC_CFA_CFG_BAY_RGRG    0x1
#define ISC_CFA_CFG_BAY_GBGB    0x2
#define ISC_CFA_CFG_BAY_BGBG    0x3
#define ISC_CFA_CFG_BAY_MASK    GENMASK(1, 0)

/* ISC Color Correction Control Register */
#define ISC_CC_CTRL     0x00000078

#define ISC_CC_CTRL_EN          BIT(0)
#define ISC_CC_CTRL_DIS         0x0
#define ISC_CC_CTRL_MASK        BIT(0)

/* ISC Gamma Correction Control Register */
#define ISC_GAM_CTRL    0x00000094

#define ISC_GAM_CTRL_EN         BIT(0)
#define ISC_GAM_CTRL_DIS        0x0
#define ISC_GAM_CTRL_MASK       BIT(0)

#define ISC_GAM_CTRL_B_EN       BIT(1)
#define ISC_GAM_CTRL_B_DIS      0x0
#define ISC_GAM_CTRL_B_MASK     BIT(1)

#define ISC_GAM_CTRL_G_EN       BIT(2)
#define ISC_GAM_CTRL_G_DIS      0x0
#define ISC_GAM_CTRL_G_MASK     BIT(2)

#define ISC_GAM_CTRL_R_EN       BIT(3)
#define ISC_GAM_CTRL_R_DIS      0x0
#define ISC_GAM_CTRL_R_MASK     BIT(3)

#define ISC_GAM_CTRL_ALL_CHAN_MASK (ISC_GAM_CTRL_B_MASK | \
				    ISC_GAM_CTRL_G_MASK | \
				    ISC_GAM_CTRL_R_MASK)

/* Color Space Conversion Control Register */
#define ISC_CSC_CTRL    0x00000398

#define ISC_CSC_CTRL_EN         BIT(0)
#define ISC_CSC_CTRL_DIS        0x0
#define ISC_CSC_CTRL_MASK       BIT(0)

/* Contrast And Brightness Control Register */
#define ISC_CBC_CTRL    0x000003b4

#define ISC_CBC_CTRL_EN         BIT(0)
#define ISC_CBC_CTRL_DIS        0x0
#define ISC_CBC_CTRL_MASK       BIT(0)

/* Subsampling 4:4:4 to 4:2:2 Control Register */
#define ISC_SUB422_CTRL 0x000003c4

#define ISC_SUB422_CTRL_EN      BIT(0)
#define ISC_SUB422_CTRL_DIS     0x0
#define ISC_SUB422_CTRL_MASK    BIT(0)

/* Subsampling 4:2:2 to 4:2:0 Control Register */
#define ISC_SUB420_CTRL 0x000003cc

#define ISC_SUB420_CTRL_EN      BIT(0)
#define ISC_SUB420_CTRL_DIS     0x0
#define ISC_SUB420_CTRL_MASK    BIT(0)

#define ISC_SUB420_CTRL_FILTER_PROG     0x0
#define ISC_SUB420_CTRL_FILTER_INTER    BIT(4)
#define ISC_SUB420_CTRL_FILTER_MASK     BIT(4)

/* Rounding, Limiting and Packing Configuration Register */
#define ISC_RLP_CFG     0x000003d0

#define ISC_RLP_CFG_MODE_DAT8           0x0
#define ISC_RLP_CFG_MODE_DAT9           0x1
#define ISC_RLP_CFG_MODE_DAT10          0x2
#define ISC_RLP_CFG_MODE_DAT11          0x3
#define ISC_RLP_CFG_MODE_DAT12          0x4
#define ISC_RLP_CFG_MODE_DATY8          0x5
#define ISC_RLP_CFG_MODE_DATY10         0x6
#define ISC_RLP_CFG_MODE_ARGB444        0x7
#define ISC_RLP_CFG_MODE_ARGB555        0x8
#define ISC_RLP_CFG_MODE_RGB565         0x9
#define ISC_RLP_CFG_MODE_ARGB32         0xa
#define ISC_RLP_CFG_MODE_YYCC           0xb
#define ISC_RLP_CFG_MODE_YYCC_LIMITED   0xc
#define ISC_RLP_CFG_MODE_MASK           GENMASK(3, 0)

/* DMA Configuration Register */
#define ISC_DCFG        0x000003e0
#define ISC_DCFG_IMODE_PACKED8          0x0
#define ISC_DCFG_IMODE_PACKED16         0x1
#define ISC_DCFG_IMODE_PACKED32         0x2
#define ISC_DCFG_IMODE_YC422SP          0x3
#define ISC_DCFG_IMODE_YC422P           0x4
#define ISC_DCFG_IMODE_YC420SP          0x5
#define ISC_DCFG_IMODE_YC420P           0x6
#define ISC_DCFG_IMODE_MASK             GENMASK(2, 0)

#define ISC_DCFG_YMBSIZE_SINGLE         0x0
#define ISC_DCFG_YMBSIZE_BEATS4         (0x1 << 4)
#define ISC_DCFG_YMBSIZE_BEATS8         (0x2 << 4)
#define ISC_DCFG_YMBSIZE_BEATS16        (0x3 << 4)
#define ISC_DCFG_YMBSIZE_MASK           GENMASK(5, 4)

#define ISC_DCFG_CMBSIZE_SINGLE         0x0
#define ISC_DCFG_CMBSIZE_BEATS4         (0x1 << 8)
#define ISC_DCFG_CMBSIZE_BEATS8         (0x2 << 8)
#define ISC_DCFG_CMBSIZE_BEATS16        (0x3 << 8)
#define ISC_DCFG_CMBSIZE_MASK           GENMASK(9, 8)

/* DMA Control Register */
#define ISC_DCTRL       0x000003e4

#define ISC_DCTRL_DE_EN         BIT(0)
#define ISC_DCTRL_DE_DIS        0x0
#define ISC_DCTRL_DE_MASK       BIT(0)

#define ISC_DCTRL_DVIEW_PACKED          0x0
#define ISC_DCTRL_DVIEW_SEMIPLANAR      (0x1 << 1)
#define ISC_DCTRL_DVIEW_PLANAR          (0x2 << 1)
#define ISC_DCTRL_DVIEW_MASK            GENMASK(2, 1)

#define ISC_DCTRL_IE_IS         0x0
#define ISC_DCTRL_IE_NOT        BIT(4)
#define ISC_DCTRL_IE_MASK       BIT(4)

#define ISC_DCTRL_WB_EN         BIT(5)
#define ISC_DCTRL_WB_DIS        0x0
#define ISC_DCTRL_WB_MASK       BIT(5)

#define ISC_DCTRL_DESC_IS_DONE  BIT(7)
#define ISC_DCTRL_DESC_NOT_DONE 0x0
#define ISC_DCTRL_DESC_MASK     BIT(7)

/* DMA Descriptor Address Register */
#define ISC_DNDA        0x000003e8

/* DMA Address 0 Register */
#define ISC_DAD0        0x000003ec

/* DMA Stride 0 Register */
#define ISC_DST0        0x000003f0

#endif
