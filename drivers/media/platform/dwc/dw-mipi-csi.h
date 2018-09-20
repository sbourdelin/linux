/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * DWC MIPI CSI-2 Host device driver
 *
 * Copyright (C) 2018 Synopsys, Inc. All rights reserved.
 * Author: Luis Oliveira <Luis.Oliveira@synopsys.com>
 *
 */

#ifndef _DW_MIPI_CSI_H__
#define _DW_MIPI_CSI_H__

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/phy/phy.h>
#include <media/v4l2-dv-timings.h>
#include <media/dwc/dw-mipi-csi-pltfrm.h>

/* DW MIPI CSI-2 register addresses*/

struct R_CSI2 {
	u16 VERSION;
	u16 N_LANES;
	u16 CTRL_RESETN;
	u16 INTERRUPT;
	u16 DATA_IDS_1;
	u16 DATA_IDS_2;
	u16 DATA_IDS_VC_1;
	u16 DATA_IDS_VC_2;
	u16 IPI_MODE;
	u16 IPI_VCID;
	u16 IPI_DATA_TYPE;
	u16 IPI_MEM_FLUSH;
	u16 IPI_HSA_TIME;
	u16 IPI_HBP_TIME;
	u16 IPI_HSD_TIME;
	u16 IPI_HLINE_TIME;
	u16 IPI_SOFTRSTN;
	u16 IPI_ADV_FEATURES;
	u16 IPI_VSA_LINES;
	u16 IPI_VBP_LINES;
	u16 IPI_VFP_LINES;
	u16 IPI_VACTIVE_LINES;
	u16 VC_EXTENSION;
	u16 INT_PHY_FATAL;
	u16 MASK_INT_PHY_FATAL;
	u16 FORCE_INT_PHY_FATAL;
	u16 INT_PKT_FATAL;
	u16 MASK_INT_PKT_FATAL;
	u16 FORCE_INT_PKT_FATAL;
	u16 INT_FRAME_FATAL;
	u16 MASK_INT_FRAME_FATAL;
	u16 FORCE_INT_FRAME_FATAL;
	u16 INT_PHY;
	u16 MASK_INT_PHY;
	u16 FORCE_INT_PHY;
	u16 INT_PKT;
	u16 MASK_INT_PKT;
	u16 FORCE_INT_PKT;
	u16 INT_LINE;
	u16 MASK_INT_LINE;
	u16 FORCE_INT_LINE;
	u16 INT_IPI;
	u16 MASK_INT_IPI;
	u16 FORCE_INT_IPI;
	u16 ST_BNDRY_FRAME_FATAL;
	u16 MSK_BNDRY_FRAME_FATAL;
	u16 FORCE_BNDRY_FRAME_FATAL;
	u16 ST_SEQ_FRAME_FATAL;
	u16 MSK_SEQ_FRAME_FATAL;
	u16 FORCE_SEQ_FRAME_FATAL;
	u16 ST_CRC_FRAME_FATAL;
	u16 MSK_CRC_FRAME_FATAL;
	u16 FORCE_CRC_FRAME_FATAL;
	u16 ST_PLD_CRC_FATAL;
	u16 MSK_PLD_CRC_FATAL;
	u16 FORCE_PLD_CRC_FATAL;
	u16 ST_DATA_ID;
	u16 MSK_DATA_ID;
	u16 FORCE_DATA_ID;
	u16 ST_ECC_CORRECT;
	u16 MSK_ECC_CORRECT;
	u16 FORCE_ECC_CORRECT;
};
/* Interrupt Masks */
struct interrupt_type {
	u32 PHY_FATAL;
	u32 PKT_FATAL;
	u32 FRAME_FATAL;
	u32 PHY;
	u32 PKT;
	u32 LINE;
	u32 IPI;
	u32 BNDRY_FRAME_FATAL;
	u32 SEQ_FRAME_FATAL;
	u32 CRC_FRAME_FATAL;
	u32 PLD_CRC_FATAL;
	u32 DATA_ID;
	u32 ECC_CORRECTED;
};

/* IPI Data Types */
enum data_type {
	CSI_2_YUV420_8 = 0x18,
	CSI_2_YUV420_10 = 0x19,
	CSI_2_YUV420_8_LEG = 0x1A,
	CSI_2_YUV420_8_SHIFT = 0x1C,
	CSI_2_YUV420_10_SHIFT = 0x1D,
	CSI_2_YUV422_8 = 0x1E,
	CSI_2_YUV422_10 = 0x1F,
	CSI_2_RGB444 = 0x20,
	CSI_2_RGB555 = 0x21,
	CSI_2_RGB565 = 0x22,
	CSI_2_RGB666 = 0x23,
	CSI_2_RGB888 = 0x24,
	CSI_2_RAW6 = 0x28,
	CSI_2_RAW7 = 0x29,
	CSI_2_RAW8 = 0x2A,
	CSI_2_RAW10 = 0x2B,
	CSI_2_RAW12 = 0x2C,
	CSI_2_RAW14 = 0x2D,
};

/* DWC MIPI CSI-2 output types */
enum output {
	IPI_OUT = 0,
	IDI_OUT = 1,
	BOTH_OUT = 2
};

/* IPI output types */
enum ipi_output {
	CAMERA_TIMING = 0,
	AUTO_TIMING = 1
};

/* Format template */
struct mipi_fmt {
	u32 code;
	u8 depth;
};

/* CSI specific configuration */
struct csi_hw {

	uint32_t num_lanes;
	uint32_t output;
	uint32_t ipi_mode;
	uint32_t ipi_color_mode;
	uint32_t ipi_auto_flush;
	uint32_t virtual_ch;
	uint32_t hsa;
	uint32_t hbp;
	uint32_t hsd;
	uint32_t htotal;
	uint32_t vsa;
	uint32_t vbp;
	uint32_t vfp;
	uint32_t vactive;
};

/* Structure to embed device driver information */
struct mipi_csi_dev {
	struct v4l2_subdev sd;
	struct video_device vdev;
	struct device *dev;

	struct mutex lock;
	spinlock_t slock;
	struct media_pad pads[CSI_PADS_NUM];
	u8 index;

	/* Store current format */
	const struct mipi_fmt *fmt;
	struct v4l2_mbus_framefmt format;

	/* Device Tree Information */
	void __iomem *base_address;
	uint32_t ctrl_irq_number;

	struct csi_hw hw;
	struct phy *phy;
	struct reset_control *rst;

	u8 ipi_dt;
	u8 hw_version_major;
	u16 hw_version_minor;
};

void dw_mipi_csi_reset(struct mipi_csi_dev *csi_dev);
int dw_mipi_csi_mask_irq_power_off(struct mipi_csi_dev *csi_dev);
int dw_mipi_csi_hw_stdby(struct mipi_csi_dev *csi_dev);
void dw_mipi_csi_set_ipi_fmt(struct mipi_csi_dev *csi_dev);
void dw_mipi_csi_start(struct mipi_csi_dev *csi_dev);
int dw_mipi_csi_irq_handler(struct mipi_csi_dev *csi_dev);
void dw_mipi_csi_get_version(struct mipi_csi_dev *csi_dev);
int dw_mipi_csi_specific_mappings(struct mipi_csi_dev *csi_dev);
void dw_mipi_csi_fill_timings(struct mipi_csi_dev *dev,
		const struct v4l2_bt_timings *bt);

#endif /*_DW_MIPI_CSI_H__ */
