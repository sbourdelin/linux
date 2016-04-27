/*
 * Copyright (C) 2016 Synopsys, Inc.
 *
 * Author: Manjunath M B <manjumb@synopsys.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __SDHCI_DWC_MSHC_PCI_H__
#define __SDHCI_DWC_MSHC_PCI_H__

#include "sdhci-pci.h"

#define SDHCI_UHS2_VENDOR	0xE8

#define DRIVER_NAME "sdhci-pci-dwc"
#define SDHC_DEF_TX_CLK_PH_VAL  4
#define SDHC_DEF_RX_CLK_PH_VAL  4

/* Synopsys Vendor Specific Registers */
#define SDHC_DBOUNCE                           0x08
#define SDHC_TUNING_RX_CLK_SEL_MASK            0x000000FF
#define SDHC_GPIO_OUT                          0x34
/* HAPS 51 Based implementation */
#define SDHC_BCLK_DCM_RST                      0x00000001
#define SDHC_CARD_TX_CLK_DCM_RST               0x00000002
#define SDHC_TUNING_RX_CLK_DCM_RST             0x00000004
#define SDHC_TUNING_TX_CLK_DCM_RST             0x00000008
#define SDHC_TUNING_TX_CLK_SEL_MASK            0x00000070
#define SDHC_TUNING_TX_CLK_SEL_SHIFT           4
#define SDHC_TX_CLK_SEL_TUNED                  0x00000080

/* Offset of BCLK DCM DRP Attributes */
/* Every attribute is of 16 bit wide */
#define BCLK_DCM_DRP_BASE_51                   0x1000

#define BCLK_DCM_MUL_DIV_DRP                   0x1050
#define MUL_MASK_DRP                           0xFF00
#define DIV_MASK_DRP                           0x00FF

/* Offset of TX and RX CLK DCM DRP */
#define TXRX_CLK_DCM_DRP_BASE_51               0x2000
#define TXRX_CLK_DCM_MUL_DIV_DRP               0x2050

int sdhci_pci_probe_slot_snps(struct sdhci_pci_slot *slot);

#endif
