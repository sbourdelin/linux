// SPDX-License-Identifier: GPL-2.0+
/*
 * dw-mipi-csi.c
 *
 * Copyright(c) 2018-present, Synopsys, Inc. and/or its affiliates.
 * Luis Oliveira <Luis.Oliveira@synopsys.com>
 *
 */

#include "dw-mipi-csi.h"

static struct R_CSI2 reg = {
	.VERSION = 0x00,
	.N_LANES = 0x04,
	.CTRL_RESETN = 0x08,
	.INTERRUPT = 0x0C,
	.DATA_IDS_1	= 0x10,
	.DATA_IDS_2	= 0x14,
	.IPI_MODE = 0x80,
	.IPI_VCID = 0x84,
	.IPI_DATA_TYPE = 0x88,
	.IPI_MEM_FLUSH = 0x8C,
	.IPI_HSA_TIME = 0x90,
	.IPI_HBP_TIME = 0x94,
	.IPI_HSD_TIME = 0x98,
	.IPI_HLINE_TIME = 0x9C,
	.IPI_SOFTRSTN = 0xA0,
	.IPI_ADV_FEATURES = 0xAC,
	.IPI_VSA_LINES = 0xB0,
	.IPI_VBP_LINES = 0xB4,
	.IPI_VFP_LINES = 0xB8,
	.IPI_VACTIVE_LINES = 0xBC,
	.INT_PHY_FATAL = 0xe0,
	.MASK_INT_PHY_FATAL = 0xe4,
	.FORCE_INT_PHY_FATAL = 0xe8,
	.INT_PKT_FATAL = 0xf0,
	.MASK_INT_PKT_FATAL = 0xf4,
	.FORCE_INT_PKT_FATAL = 0xf8,
	.INT_PHY = 0x110,
	.MASK_INT_PHY = 0x114,
	.FORCE_INT_PHY = 0x118,
	.INT_LINE = 0x130,
	.MASK_INT_LINE = 0x134,
	.FORCE_INT_LINE = 0x138,
	.INT_IPI = 0x140,
	.MASK_INT_IPI = 0x144,
	.FORCE_INT_IPI = 0x148,
};
struct interrupt_type csi_int = {
	.PHY_FATAL = BIT(0),
	.PKT_FATAL = BIT(1),
	.PHY = BIT(16),
};
static void dw_mipi_csi_write(struct mipi_csi_dev *dev,
		  unsigned int address, unsigned int data)
{
	iowrite32(data, dev->base_address + address);
}

static u32 dw_mipi_csi_read(struct mipi_csi_dev *dev, unsigned long address)
{
	return ioread32(dev->base_address + address);
}

void dw_mipi_csi_write_part(struct mipi_csi_dev *dev,
		       unsigned long address, unsigned long data,
		       unsigned char shift, unsigned char width)
{
	u32 mask = (1 << width) - 1;
	u32 temp = dw_mipi_csi_read(dev, address);

	temp &= ~(mask << shift);
	temp |= (data & mask) << shift;
	dw_mipi_csi_write(dev, address, temp);
}

void dw_mipi_csi_reset(struct mipi_csi_dev *csi_dev)
{
	dw_mipi_csi_write(csi_dev, reg.CTRL_RESETN, 0);
	usleep_range(100, 200);
	dw_mipi_csi_write(csi_dev, reg.CTRL_RESETN, 1);
}

int dw_mipi_csi_mask_irq_power_off(struct mipi_csi_dev *csi_dev)
{
	if ((csi_dev->hw_version_major) == 1) {

		/* set only one lane (lane 0) as active (ON) */
		dw_mipi_csi_write(csi_dev, reg.N_LANES, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PHY_FATAL, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PKT_FATAL, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PHY, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PKT, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_LINE, 0);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_IPI, 0);

		/* only for version 1.30 */
		if ((csi_dev->hw_version_minor) == 30)
			dw_mipi_csi_write(csi_dev, reg.MASK_INT_FRAME_FATAL, 0);

		dw_mipi_csi_write(csi_dev, reg.CTRL_RESETN, 0);

		/* only for version 1.40 */
		if ((csi_dev->hw_version_minor) == 40) {
			dw_mipi_csi_write(csi_dev,
					reg.MSK_BNDRY_FRAME_FATAL, 0);
			dw_mipi_csi_write(csi_dev, reg.MSK_SEQ_FRAME_FATAL, 0);
			dw_mipi_csi_write(csi_dev, reg.MSK_CRC_FRAME_FATAL, 0);
			dw_mipi_csi_write(csi_dev, reg.MSK_PLD_CRC_FATAL, 0);
			dw_mipi_csi_write(csi_dev, reg.MSK_DATA_ID, 0);
			dw_mipi_csi_write(csi_dev, reg.MSK_ECC_CORRECT, 0);
		}
	}

	return 0;
}

int dw_mipi_csi_hw_stdby(struct mipi_csi_dev *csi_dev)
{
	if ((csi_dev->hw_version_major) == 1) {

		/* set only one lane (lane 0) as active (ON) */
		dw_mipi_csi_reset(csi_dev);
		dw_mipi_csi_write(csi_dev, reg.N_LANES, 0);
		phy_init(csi_dev->phy);

		/* only for version 1.30 */
		if ((csi_dev->hw_version_minor) == 30)
			dw_mipi_csi_write(csi_dev,
					reg.MASK_INT_FRAME_FATAL, 0xFFFFFFFF);

		/* common */
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PHY_FATAL, 0xFFFFFFFF);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PKT_FATAL, 0xFFFFFFFF);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PHY, 0xFFFFFFFF);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_PKT, 0xFFFFFFFF);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_LINE, 0xFFFFFFFF);
		dw_mipi_csi_write(csi_dev, reg.MASK_INT_IPI, 0xFFFFFFFF);

		/* only for version 1.40 */
		if ((csi_dev->hw_version_minor) == 40) {
			dw_mipi_csi_write(csi_dev,
					reg.MSK_BNDRY_FRAME_FATAL, 0xFFFFFFFF);
			dw_mipi_csi_write(csi_dev,
					reg.MSK_SEQ_FRAME_FATAL, 0xFFFFFFFF);
			dw_mipi_csi_write(csi_dev,
					reg.MSK_CRC_FRAME_FATAL, 0xFFFFFFFF);
			dw_mipi_csi_write(csi_dev,
					reg.MSK_PLD_CRC_FATAL, 0xFFFFFFFF);
			dw_mipi_csi_write(csi_dev, reg.MSK_DATA_ID, 0xFFFFFFFF);
			dw_mipi_csi_write(csi_dev,
					reg.MSK_ECC_CORRECT, 0xFFFFFFFF);
		}
	}
	return 0;
}

void dw_mipi_csi_set_ipi_fmt(struct mipi_csi_dev *csi_dev)
{
	struct device *dev = csi_dev->dev;

	if (csi_dev->ipi_dt)
		dw_mipi_csi_write(csi_dev, reg.IPI_DATA_TYPE, csi_dev->ipi_dt);
	else {
		switch (csi_dev->fmt->code) {
		case MEDIA_BUS_FMT_RGB565_2X8_BE:
		case MEDIA_BUS_FMT_RGB565_2X8_LE:
			dw_mipi_csi_write(csi_dev,
					reg.IPI_DATA_TYPE, CSI_2_RGB565);
			dev_dbg(dev, "DT: RGB 565");
			break;

		case MEDIA_BUS_FMT_RGB888_2X12_LE:
		case MEDIA_BUS_FMT_RGB888_2X12_BE:
			dw_mipi_csi_write(csi_dev,
					reg.IPI_DATA_TYPE, CSI_2_RGB888);
			dev_dbg(dev, "DT: RGB 888");
			break;
		case MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_BE:
			dw_mipi_csi_write(csi_dev,
					reg.IPI_DATA_TYPE, CSI_2_RAW10);
			dev_dbg(dev, "DT: RAW 10");
			break;
		case MEDIA_BUS_FMT_SBGGR8_1X8:
			dw_mipi_csi_write(csi_dev,
					reg.IPI_DATA_TYPE, CSI_2_RAW8);
			dev_dbg(dev, "DT: RAW 8");
			break;
		default:
			dw_mipi_csi_write(csi_dev,
					reg.IPI_DATA_TYPE, CSI_2_RGB565);
			dev_dbg(dev, "Error");
			break;
		}
	}
}

void dw_mipi_csi_fill_timings(struct mipi_csi_dev *dev,
			   const struct v4l2_bt_timings *bt)
{
	if (bt == NULL)
		return;

	dev->hw.hsa = bt->hsync;
	dev->hw.hbp = bt->hbackporch;
	dev->hw.hsd = bt->hsync;
	dev->hw.htotal = bt->height + bt->vfrontporch +
	    bt->vsync + bt->vbackporch;
	dev->hw.vsa = bt->vsync;
	dev->hw.vbp = bt->vbackporch;
	dev->hw.vfp = bt->vfrontporch;
	dev->hw.vactive = bt->height;
}

void dw_mipi_csi_start(struct mipi_csi_dev *csi_dev)
{
	const struct v4l2_bt_timings *bt = &v4l2_dv_timings_presets[0].bt;
	struct device *dev = csi_dev->dev;

	dw_mipi_csi_fill_timings(csi_dev, bt);
	dw_mipi_csi_write(csi_dev, reg.N_LANES, (csi_dev->hw.num_lanes - 1));
	dev_dbg(dev, "N Lanes: %d\n", csi_dev->hw.num_lanes);

	/* IPI Related Configuration */
	if ((csi_dev->hw.output == IPI_OUT)
		|| (csi_dev->hw.output == BOTH_OUT)) {

		if (csi_dev->hw_version_major >= 1) {
			if (csi_dev->hw_version_minor >= 20)
				dw_mipi_csi_write(csi_dev,
					reg.IPI_ADV_FEATURES, 0x30000);

			if (csi_dev->hw_version_minor >= 30)
				dw_mipi_csi_write(csi_dev,
					reg.IPI_SOFTRSTN, 0x1);
		}
		/*  address | data, | shift | width */
		dw_mipi_csi_write_part(csi_dev, reg.IPI_MODE, 1, 24, 1);
		dw_mipi_csi_write_part(csi_dev,
					reg.IPI_MODE,
					csi_dev->hw.ipi_mode,
					0, 1);

		dw_mipi_csi_write_part(csi_dev,
					reg.IPI_MODE,
					csi_dev->hw.ipi_color_mode,
					8, 1);

		dw_mipi_csi_write_part(csi_dev,
					reg.IPI_VCID,
					csi_dev->hw.virtual_ch,
					0, 2);

		dw_mipi_csi_write_part(csi_dev,
					reg.IPI_MEM_FLUSH,
					csi_dev->hw.ipi_auto_flush,
					8, 1);

		dw_mipi_csi_write(csi_dev,
					reg.IPI_HSA_TIME, csi_dev->hw.hsa);

		dw_mipi_csi_write(csi_dev,
					reg.IPI_HBP_TIME, csi_dev->hw.hbp);

		dw_mipi_csi_write(csi_dev,
					reg.IPI_HSD_TIME, csi_dev->hw.hsd);

		dev_dbg(dev, "IPI enable\n");
		dev_dbg(dev, "IPI MODE: %d\n", csi_dev->hw.ipi_mode);
		dev_dbg(dev, "Color Mode: %d\n", csi_dev->hw.ipi_color_mode);
		dev_dbg(dev, "Virtual Channel: %d\n", csi_dev->hw.virtual_ch);
		dev_dbg(dev, "Auto-flush: %d\n", csi_dev->hw.ipi_auto_flush);
		dev_dbg(dev, "HSA: %d\n", csi_dev->hw.hsa);
		dev_dbg(dev, "HBP: %d\n", csi_dev->hw.hbp);
		dev_dbg(dev, "HSD: %d\n", csi_dev->hw.hsd);

		if (csi_dev->hw.ipi_mode == AUTO_TIMING) {
			dw_mipi_csi_write(csi_dev,
				reg.IPI_HLINE_TIME, csi_dev->hw.htotal);
			dw_mipi_csi_write(csi_dev,
				reg.IPI_VSA_LINES, csi_dev->hw.vsa);
			dw_mipi_csi_write(csi_dev,
				reg.IPI_VBP_LINES, csi_dev->hw.vbp);
			dw_mipi_csi_write(csi_dev,
				reg.IPI_VFP_LINES, csi_dev->hw.vfp);
			dw_mipi_csi_write(csi_dev,
				reg.IPI_VACTIVE_LINES, csi_dev->hw.vactive);
			dev_dbg(dev,
				"Horizontal Total: %d\n", csi_dev->hw.htotal);
			dev_dbg(dev,
				"Vertical Sync Active: %d\n", csi_dev->hw.vsa);
			dev_dbg(dev,
				"Vertical Back Porch: %d\n", csi_dev->hw.vbp);
			dev_dbg(dev,
				"Vertical Front Porch: %d\n", csi_dev->hw.vfp);
			dev_dbg(dev,
				"Vertical Active: %d\n", csi_dev->hw.vactive);
		}
	}
	phy_power_on(csi_dev->phy);
}

int dw_mipi_csi_irq_handler(struct mipi_csi_dev *csi_dev)
{
	struct device *dev = csi_dev->dev;
	u32 global_int_status, i_sts;
	unsigned long flags;

	global_int_status = dw_mipi_csi_read(csi_dev, reg.INTERRUPT);
	spin_lock_irqsave(&csi_dev->slock, flags);

	if (global_int_status & csi_int.PHY_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_PHY_FATAL);
		dev_err_ratelimited(dev,
			"interrupt %08X: PHY FATAL: %08X\n",
			reg.INT_PHY_FATAL, i_sts);
	}

	if (global_int_status & csi_int.PKT_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_PKT_FATAL);
		dev_err_ratelimited(dev,
			"interrupt %08X: PKT FATAL: %08X\n",
			reg.INT_PKT_FATAL, i_sts);
	}

	if ((global_int_status & csi_int.FRAME_FATAL)
	&& ((csi_dev->hw_version_major) == 1)
	&& ((csi_dev->hw_version_minor) == 30)) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_FRAME_FATAL);
			dev_err_ratelimited(dev,
			"interrupt %08X: FRAME FATAL: %08X\n",
			reg.INT_FRAME_FATAL, i_sts);
	}

	if (global_int_status & csi_int.PHY) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_PHY);
		dev_err_ratelimited(dev,
			"interrupt %08X: PHY: %08X\n",
			reg.INT_PHY, i_sts);
	}

	if (global_int_status & csi_int.PKT) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_PKT);
		dev_err_ratelimited(dev,
			"interrupt %08X: PKT: %08X\n",
			reg.INT_PKT, i_sts);
	}

	if (global_int_status & csi_int.LINE) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_LINE);
		dev_err_ratelimited(dev,
			"interrupt %08X: LINE: %08X\n",
			reg.INT_LINE, i_sts);
	}

	if (global_int_status & csi_int.IPI) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.INT_IPI);
		dev_err_ratelimited(dev,
			"interrupt %08X: IPI: %08X\n",
			reg.INT_IPI, i_sts);
	}

	if (global_int_status & csi_int.BNDRY_FRAME_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_BNDRY_FRAME_FATAL);
		dev_err_ratelimited(dev,
			"interrupt %08X: ST_BNDRY_FRAME_FATAL: %08X\n",
			reg.ST_BNDRY_FRAME_FATAL, i_sts);
	}

	if (global_int_status & csi_int.SEQ_FRAME_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_SEQ_FRAME_FATAL);
		dev_err_ratelimited(dev,
			"interrupt %08X: ST_SEQ_FRAME_FATAL: %08X\n",
			reg.ST_SEQ_FRAME_FATAL, i_sts);
	}

	if (global_int_status & csi_int.CRC_FRAME_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_CRC_FRAME_FATAL);
		dev_err_ratelimited(dev,
			"interrupt %08X: ST_CRC_FRAME_FATAL: %08X\n",
			reg.ST_CRC_FRAME_FATAL, i_sts);
	}

	if (global_int_status & csi_int.PLD_CRC_FATAL) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_PLD_CRC_FATAL);
		dev_err_ratelimited(dev,
			"interrupt %08X: ST_PLD_CRC_FATAL: %08X\n",
			reg.ST_PLD_CRC_FATAL, i_sts);
	}

	if (global_int_status & csi_int.DATA_ID) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_DATA_ID);
		dev_err_ratelimited(dev,
			"interrupt %08X: ST_DATA_ID: %08X\n",
			reg.ST_DATA_ID, i_sts);
	}

	if (global_int_status & csi_int.ECC_CORRECTED) {
		i_sts = dw_mipi_csi_read(csi_dev, reg.ST_ECC_CORRECT);
		dev_err_ratelimited(dev,
			"interrupt %08X: ST_ECC_CORRECT: %08X\n",
			reg.ST_ECC_CORRECT, i_sts);
	}

	spin_unlock_irqrestore(&csi_dev->slock, flags);

	return 1;
}

void dw_mipi_csi_get_version(struct mipi_csi_dev *csi_dev)
{
	uint32_t hw_version;

	hw_version = dw_mipi_csi_read(csi_dev, reg.VERSION);
	csi_dev->hw_version_major = (uint8_t) ((hw_version >> 24) - '0');
	csi_dev->hw_version_minor = (uint8_t) ((hw_version >> 16) - '0');
	csi_dev->hw_version_minor = csi_dev->hw_version_minor * 10;
	csi_dev->hw_version_minor += (uint8_t) ((hw_version >> 8) - '0');
}

int dw_mipi_csi_specific_mappings(struct mipi_csi_dev *csi_dev)
{
	struct device *dev = csi_dev->dev;

	if ((csi_dev->hw_version_major) == 1)
		if ((csi_dev->hw_version_minor) == 30) {

			dev_dbg(dev, "We are version 30");
			/*
			 * Hardware registers that were
			 * exclusive to version < 1.40
			 */
			reg.INT_FRAME_FATAL = 0x100;
			reg.MASK_INT_FRAME_FATAL = 0x104;
			reg.FORCE_INT_FRAME_FATAL = 0x108;
			reg.INT_PKT = 0x120;
			reg.MASK_INT_PKT = 0x124;
			reg.FORCE_INT_PKT = 0x128;

			/* interrupt source present until this release */
			csi_int.PKT = BIT(17);
			csi_int.LINE = BIT(18);
			csi_int.IPI = BIT(19);
			csi_int.FRAME_FATAL = BIT(2);

		} else if ((csi_dev->hw_version_minor) == 40) {
			dev_dbg(dev, "We are version 40");
			/*
			 * HW registers that were added
			 * to version 1.40
			 */
			reg.ST_BNDRY_FRAME_FATAL = 0x280;
			reg.MSK_BNDRY_FRAME_FATAL = 0x284;
			reg.FORCE_BNDRY_FRAME_FATAL	= 0x288;
			reg.ST_SEQ_FRAME_FATAL = 0x290;
			reg.MSK_SEQ_FRAME_FATAL	= 0x294;
			reg.FORCE_SEQ_FRAME_FATAL = 0x298;
			reg.ST_CRC_FRAME_FATAL = 0x2a0;
			reg.MSK_CRC_FRAME_FATAL	= 0x2a4;
			reg.FORCE_CRC_FRAME_FATAL = 0x2a8;
			reg.ST_PLD_CRC_FATAL = 0x2b0;
			reg.MSK_PLD_CRC_FATAL = 0x2b4;
			reg.FORCE_PLD_CRC_FATAL = 0x2b8;
			reg.ST_DATA_ID = 0x2c0;
			reg.MSK_DATA_ID = 0x2c4;
			reg.FORCE_DATA_ID = 0x2c8;
			reg.ST_ECC_CORRECT = 0x2d0;
			reg.MSK_ECC_CORRECT = 0x2d4;
			reg.FORCE_ECC_CORRECT = 0x2d8;
			reg.DATA_IDS_VC_1 = 0x0;
			reg.DATA_IDS_VC_2 = 0x0;
			reg.VC_EXTENSION = 0x0;

			/* interrupts map were changed */
			csi_int.LINE = BIT(17);
			csi_int.IPI = BIT(18);
			csi_int.BNDRY_FRAME_FATAL = BIT(2);
			csi_int.SEQ_FRAME_FATAL	= BIT(3);
			csi_int.CRC_FRAME_FATAL = BIT(4);
			csi_int.PLD_CRC_FATAL = BIT(5);
			csi_int.DATA_ID = BIT(6);
			csi_int.ECC_CORRECTED = BIT(7);

		} else
			dev_info(dev, "Version minor not supported.");
	else
		dev_info(dev, "Version major not supported.");

	return 0;
}
