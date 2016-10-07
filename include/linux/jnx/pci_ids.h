/*
 * Juniper PCI ID(s) - for devices on Juniper Boards
 *
 * Rajat Jain <rajatjain@juniper.net>
 * Copyright 2014 Juniper Networks
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef __JNX_PCI_IDS_H__
#define __JNX_PCI_IDS_H__

#define PCI_VENDOR_ID_JUNIPER		0x1304

/*
 * PTX SAM FPGA, device ID as present on various Juniper boards, such as
 * - Sangria FPC
 * - Hendricks FPC
 * - Sangria 24x10GE PIC
 * - Gladiator FPC
 */
#define PCI_DEVICE_ID_JNX_SAM		0x0004

/* Juniper Broadway ASIC family */
#define PCI_DEVICE_ID_JNX_TF		0x003c
#define PCI_DEVICE_ID_JNX_TL		0x003d
#define PCI_DEVICE_ID_JNX_TQ		0x003e
#define PCI_DEVICE_ID_JNX_OTN_FRAMER	0x0055
#define PCI_DEVICE_ID_JNX_PE		0x005e
#define PCI_DEVICE_ID_JNX_PF		0x005f	/* Juniper Paradise ASIC */

/* Juniper SAM FPGA - Omega SIB, Sochu SHAM, Gladiator SIB */
#define PCI_DEVICE_ID_JNX_SAM_OMEGA	0x006a

/* Juniper SAM FPGA - present on GLD FPC board */
#define PCI_DEVICE_ID_JNX_SAM_X		0x006b

/* Juniper PAM FPGA - present on PTX MLC board */
#define PCI_DEVICE_ID_JNX_PAM		0x006c
/* Juniper CBC FPGA - present on PTX1K RCB */
#define PCI_DEVICE_ID_JNX_CBC		0x006e
#define PCI_DEVICE_ID_JNX_CBC_P2	0x0079
#define PCI_DEVICE_ID_JNX_OMG_CBC	0x0083

/* Other Vendors' devices */
#define PCI_DEVICE_ID_IDT_PES12NT3_TRANS_AB	0x8058
#define PCI_DEVICE_ID_IDT_PES12NT3_TRANS_C	0x8059
#define PCI_DEVICE_ID_IDT_PES12NT3_INT_NTB_C	0x805a
#define PCI_DEVICE_ID_IDT_48H12G2		0x807a
#define PCI_DEVICE_ID_IDT_PES24NT24G2		0x808e
#define PCI_DEVICE_ID_IDT_PES16NT16G2		0x8090

#define PCI_DEVICE_ID_PLX_8614		0x8614
#define PCI_DEVICE_ID_PLX_8618		0x8618
#define PCI_DEVICE_ID_PLX_8713		0x8713

/*
 *  Juniper CBD FPGA Device ID(s)
 */
#define JNX_CBD_FPGA_DID_09B3           0x004D
#define JNX_CBD_FPGA_DID_0BA8           0x005A

#endif /* __JNX_PCI_IDS_H__ */
