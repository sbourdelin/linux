/*
 * Driver for Renesas R-Car VIN IP
 *
 * Copyright (C) 2016 Renesas Solutions Corp.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <media/v4l2-device.h>
#include <media/v4l2-of.h>
#include <media/videobuf2-v4l2.h>

#include "rcar-vin.h"

#define notifier_to_vin(n) container_of(n, struct rvin_dev, notifier)

/* -----------------------------------------------------------------------------
 * HW Functions
 */

/* Register offsets for R-Car VIN */
#define VNMC_REG	0x00	/* Video n Main Control Register */
#define VNMS_REG	0x04	/* Video n Module Status Register */
#define VNFC_REG	0x08	/* Video n Frame Capture Register */
#define VNSLPRC_REG	0x0C	/* Video n Start Line Pre-Clip Register */
#define VNELPRC_REG	0x10	/* Video n End Line Pre-Clip Register */
#define VNSPPRC_REG	0x14	/* Video n Start Pixel Pre-Clip Register */
#define VNEPPRC_REG	0x18	/* Video n End Pixel Pre-Clip Register */
#define VNSLPOC_REG	0x1C	/* Video n Start Line Post-Clip Register */
#define VNELPOC_REG	0x20	/* Video n End Line Post-Clip Register */
#define VNSPPOC_REG	0x24	/* Video n Start Pixel Post-Clip Register */
#define VNEPPOC_REG	0x28	/* Video n End Pixel Post-Clip Register */
#define VNIS_REG	0x2C	/* Video n Image Stride Register */
#define VNMB_REG(m)	(0x30 + ((m) << 2)) /* Video n Memory Base m Register */
#define VNIE_REG	0x40	/* Video n Interrupt Enable Register */
#define VNINTS_REG	0x44	/* Video n Interrupt Status Register */
#define VNSI_REG	0x48	/* Video n Scanline Interrupt Register */
#define VNMTC_REG	0x4C	/* Video n Memory Transfer Control Register */
#define VNYS_REG	0x50	/* Video n Y Scale Register */
#define VNXS_REG	0x54	/* Video n X Scale Register */
#define VNDMR_REG	0x58	/* Video n Data Mode Register */
#define VNDMR2_REG	0x5C	/* Video n Data Mode Register 2 */
#define VNUVAOF_REG	0x60	/* Video n UV Address Offset Register */
#define VNC1A_REG	0x80	/* Video n Coefficient Set C1A Register */
#define VNC1B_REG	0x84	/* Video n Coefficient Set C1B Register */
#define VNC1C_REG	0x88	/* Video n Coefficient Set C1C Register */
#define VNC2A_REG	0x90	/* Video n Coefficient Set C2A Register */
#define VNC2B_REG	0x94	/* Video n Coefficient Set C2B Register */
#define VNC2C_REG	0x98	/* Video n Coefficient Set C2C Register */
#define VNC3A_REG	0xA0	/* Video n Coefficient Set C3A Register */
#define VNC3B_REG	0xA4	/* Video n Coefficient Set C3B Register */
#define VNC3C_REG	0xA8	/* Video n Coefficient Set C3C Register */
#define VNC4A_REG	0xB0	/* Video n Coefficient Set C4A Register */
#define VNC4B_REG	0xB4	/* Video n Coefficient Set C4B Register */
#define VNC4C_REG	0xB8	/* Video n Coefficient Set C4C Register */
#define VNC5A_REG	0xC0	/* Video n Coefficient Set C5A Register */
#define VNC5B_REG	0xC4	/* Video n Coefficient Set C5B Register */
#define VNC5C_REG	0xC8	/* Video n Coefficient Set C5C Register */
#define VNC6A_REG	0xD0	/* Video n Coefficient Set C6A Register */
#define VNC6B_REG	0xD4	/* Video n Coefficient Set C6B Register */
#define VNC6C_REG	0xD8	/* Video n Coefficient Set C6C Register */
#define VNC7A_REG	0xE0	/* Video n Coefficient Set C7A Register */
#define VNC7B_REG	0xE4	/* Video n Coefficient Set C7B Register */
#define VNC7C_REG	0xE8	/* Video n Coefficient Set C7C Register */
#define VNC8A_REG	0xF0	/* Video n Coefficient Set C8A Register */
#define VNC8B_REG	0xF4	/* Video n Coefficient Set C8B Register */
#define VNC8C_REG	0xF8	/* Video n Coefficient Set C8C Register */

/* Register bit fields for R-Car VIN */
/* Video n Main Control Register bits */
#define VNMC_FOC		(1 << 21)
#define VNMC_YCAL		(1 << 19)
#define VNMC_INF_YUV8_BT656	(0 << 16)
#define VNMC_INF_YUV8_BT601	(1 << 16)
#define VNMC_INF_YUV10_BT656	(2 << 16)
#define VNMC_INF_YUV10_BT601	(3 << 16)
#define VNMC_INF_YUV16		(5 << 16)
#define VNMC_INF_RGB888		(6 << 16)
#define VNMC_VUP		(1 << 10)
#define VNMC_IM_ODD		(0 << 3)
#define VNMC_IM_ODD_EVEN	(1 << 3)
#define VNMC_IM_EVEN		(2 << 3)
#define VNMC_IM_FULL		(3 << 3)
#define VNMC_BPS		(1 << 1)
#define VNMC_ME			(1 << 0)

/* Video n Module Status Register bits */
#define VNMS_FBS_MASK		(3 << 3)
#define VNMS_FBS_SHIFT		3
#define VNMS_AV			(1 << 1)
#define VNMS_CA			(1 << 0)

/* Video n Frame Capture Register bits */
#define VNFC_C_FRAME		(1 << 1)
#define VNFC_S_FRAME		(1 << 0)

/* Video n Interrupt Enable Register bits */
#define VNIE_FIE		(1 << 4)
#define VNIE_EFE		(1 << 1)

/* Video n Data Mode Register bits */
#define VNDMR_EXRGB		(1 << 8)
#define VNDMR_BPSM		(1 << 4)
#define VNDMR_DTMD_YCSEP	(1 << 1)
#define VNDMR_DTMD_ARGB1555	(1 << 0)

/* Video n Data Mode Register 2 bits */
#define VNDMR2_VPS		(1 << 30)
#define VNDMR2_HPS		(1 << 29)
#define VNDMR2_FTEV		(1 << 17)
#define VNDMR2_VLV(n)		((n & 0xf) << 12)

#define RVIN_HSYNC_ACTIVE_LOW       (1 << 0)
#define RVIN_VSYNC_ACTIVE_LOW       (1 << 1)
#define RVIN_BT601                  (1 << 2)
#define RVIN_BT656                  (1 << 3)

static void rvin_write(struct rvin_dev *vin, u32 value, u32 offset)
{
	iowrite32(value, vin->base + offset);
}

static u32 rvin_read(struct rvin_dev *vin, u32 offset)
{
	return ioread32(vin->base + offset);
}

int rvin_get_active_slot(struct rvin_dev *vin)
{
	if (is_continuous_transfer(vin))
		return (rvin_read(vin, VNMS_REG) & VNMS_FBS_MASK)
			>> VNMS_FBS_SHIFT;
	return  0;
}

void rvin_set_slot_addr(struct rvin_dev *vin, int slot, dma_addr_t addr)
{
	rvin_write(vin, addr, VNMB_REG(slot));
}

int rvin_setup(struct rvin_dev *vin)
{
	u32 vnmc, dmr, dmr2, interrupts;
	bool progressive = false, output_is_yuv = false, input_is_yuv = false;
	int ret;

	ret = rvin_scale_setup(vin);
	if (ret < 0)
		return ret;

	switch (vin->format.field) {
	case V4L2_FIELD_TOP:
		vnmc = VNMC_IM_ODD;
		break;
	case V4L2_FIELD_BOTTOM:
		vnmc = VNMC_IM_EVEN;
		break;
	case V4L2_FIELD_INTERLACED:
	case V4L2_FIELD_INTERLACED_TB:
		vnmc = VNMC_IM_FULL;
		break;
	case V4L2_FIELD_INTERLACED_BT:
		vnmc = VNMC_IM_FULL | VNMC_FOC;
		break;
	case V4L2_FIELD_NONE:
		if (is_continuous_transfer(vin)) {
			vnmc = VNMC_IM_ODD_EVEN;
			progressive = true;
		} else {
			vnmc = VNMC_IM_ODD;
		}
		break;
	default:
		vnmc = VNMC_IM_ODD;
		break;
	}

	/*
	 * Input interface
	 */
	switch (vin->fmtinfo->code) {
	case MEDIA_BUS_FMT_YUYV8_1X16:
		/* BT.601/BT.1358 16bit YCbCr422 */
		vnmc |= VNMC_INF_YUV16;
		input_is_yuv = true;
		break;
	case MEDIA_BUS_FMT_YUYV8_2X8:
		/* BT.656 8bit YCbCr422 or BT.601 8bit YCbCr422 */
		vnmc |= vin->pdata_flags & RVIN_BT656 ?
			VNMC_INF_YUV8_BT656 : VNMC_INF_YUV8_BT601;
		input_is_yuv = true;
		break;
	case MEDIA_BUS_FMT_RGB888_1X24:
		vnmc |= VNMC_INF_RGB888;
		break;
	case MEDIA_BUS_FMT_YUYV10_2X10:
		/* BT.656 10bit YCbCr422 or BT.601 10bit YCbCr422 */
		vnmc |= vin->pdata_flags & RVIN_BT656 ?
			VNMC_INF_YUV10_BT656 : VNMC_INF_YUV10_BT601;
		input_is_yuv = true;
		break;
	default:
		break;
	}

	/* Enable VSYNC Field Toogle mode after one VSYNC input */
	dmr2 = VNDMR2_FTEV | VNDMR2_VLV(1);

	/* Hsync Signal Polarity Select */
	if (!(vin->pdata_flags & RVIN_HSYNC_ACTIVE_LOW))
		dmr2 |= VNDMR2_HPS;

	/* Vsync Signal Polarity Select */
	if (!(vin->pdata_flags & RVIN_VSYNC_ACTIVE_LOW))
		dmr2 |= VNDMR2_VPS;

	rvin_write(vin, dmr2, VNDMR2_REG);

	/*
	 * Output format
	 */
	switch (vin->fmtinfo->fourcc) {
	case V4L2_PIX_FMT_NV16:
		rvin_write(vin,
			ALIGN(vin->format.width * vin->format.height, 0x80),
			VNUVAOF_REG);
		dmr = VNDMR_DTMD_YCSEP;
		output_is_yuv = true;
		break;
	case V4L2_PIX_FMT_YUYV:
		dmr = VNDMR_BPSM;
		output_is_yuv = true;
		break;
	case V4L2_PIX_FMT_UYVY:
		dmr = 0;
		output_is_yuv = true;
		break;
	case V4L2_PIX_FMT_RGB555X:
		dmr = VNDMR_DTMD_ARGB1555;
		break;
	case V4L2_PIX_FMT_RGB565:
		dmr = 0;
		break;
	case V4L2_PIX_FMT_RGB32:
		if (vin->chip == RCAR_GEN2 || vin->chip == RCAR_H1 ||
				vin->chip == RCAR_E1) {
			dmr = VNDMR_EXRGB;
			break;
		}
	default:
		vin_warn(vin, "Invalid fourcc format (0x%x)\n",
				vin->fmtinfo->fourcc);
		return -EINVAL;
	}

	/* Always update on field change */
	vnmc |= VNMC_VUP;

	/* If input and output use the same colorspace, use bypass mode */
	if (input_is_yuv == output_is_yuv)
		vnmc |= VNMC_BPS;

	/* Progressive or interlaced mode */
	interrupts = progressive ? VNIE_FIE : VNIE_EFE;

	/* Ack interrupts */
	rvin_write(vin, interrupts, VNINTS_REG);
	/* Enable interrupts */
	rvin_write(vin, interrupts, VNIE_REG);
	/* Start capturing */
	rvin_write(vin, dmr, VNDMR_REG);
	rvin_write(vin, vnmc | VNMC_ME, VNMC_REG);

	return 0;
}

void rvin_capture(struct rvin_dev *vin)
{
	if (is_continuous_transfer(vin))
		/* Continuous Frame Capture Mode */
		rvin_write(vin, VNFC_C_FRAME, VNFC_REG);
	else
		/* Single Frame Capture Mode */
		rvin_write(vin, VNFC_S_FRAME, VNFC_REG);
}

void rvin_request_capture_stop(struct rvin_dev *vin)
{
	vin->state = STOPPING;

	/* Set continuous & single transfer off */
	rvin_write(vin, 0, VNFC_REG);
	/* Disable capture (release DMA buffer), reset */
	rvin_write(vin, rvin_read(vin, VNMC_REG) & ~VNMC_ME, VNMC_REG);

	/* Update the status if stopped already */
	if (!(rvin_read(vin, VNMS_REG) & VNMS_CA))
		vin->state = STOPPED;
}

void rvin_disable_interrupts(struct rvin_dev *vin)
{
	rvin_write(vin, 0, VNIE_REG);
}

void rvin_disable_capture(struct rvin_dev *vin)
{
	rvin_write(vin, rvin_read(vin, VNMC_REG) & ~VNMC_ME,
			VNMC_REG);
}

u32 rvin_get_interrupt_status(struct rvin_dev *vin)
{
	return rvin_read(vin, VNINTS_REG);
}

void rvin_ack_interrupt(struct rvin_dev *vin)
{
	rvin_write(vin, rvin_read(vin, VNINTS_REG), VNINTS_REG);
}

bool rvin_capture_active(struct rvin_dev *vin)
{
	return rvin_read(vin, VNMS_REG) & VNMS_CA;
}

/* -----------------------------------------------------------------------------
 * Format Convertions
 */

static const struct rvin_video_format rvin_formats_conv[] = {

	{
		.code			= 0,
		.fourcc                 = V4L2_PIX_FMT_NV16,
		.name                   = "NV16",
		.bits_per_sample        = 8,
		.packing                = RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= 0,
		.fourcc                 = V4L2_PIX_FMT_YUYV,
		.name                   = "YUYV",
		.bits_per_sample        = 16,
		.packing                = RVIN_MBUS_PACKING_NONE,
	},
	{
		.code			= 0,
		.fourcc                 = V4L2_PIX_FMT_UYVY,
		.name                   = "UYVY",
		.bits_per_sample        = 16,
		.packing                = RVIN_MBUS_PACKING_NONE,
	},
	{
		.code			= 0,
		.fourcc                 = V4L2_PIX_FMT_RGB565,
		.name                   = "RGB565",
		.bits_per_sample        = 16,
		.packing                = RVIN_MBUS_PACKING_NONE,
	},
	{
		.code			= 0,
		.fourcc                 = V4L2_PIX_FMT_RGB555X,
		.name                   = "ARGB1555",
		.bits_per_sample        = 16,
		.packing                = RVIN_MBUS_PACKING_NONE,
	},
	{
		.code			= 0,
		.fourcc                 = V4L2_PIX_FMT_RGB32,
		.name                   = "RGB888",
		.bits_per_sample        = 32,
		.packing                = RVIN_MBUS_PACKING_NONE,
	},
};

static const struct rvin_video_format rvin_formats_pass[] = {
	{
		.code = MEDIA_BUS_FMT_YVYU8_2X8,
		.fourcc			= V4L2_PIX_FMT_YVYU,
		.name			= "YVYU",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= MEDIA_BUS_FMT_UYVY8_2X8,
		.fourcc			= V4L2_PIX_FMT_UYVY,
		.name			= "UYVY",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= MEDIA_BUS_FMT_VYUY8_2X8,
		.fourcc			= V4L2_PIX_FMT_VYUY,
		.name			= "VYUY",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= MEDIA_BUS_FMT_RGB555_2X8_PADHI_LE,
		.fourcc			= V4L2_PIX_FMT_RGB555,
		.name			= "RGB555",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= MEDIA_BUS_FMT_RGB555_2X8_PADHI_BE,
		.fourcc			= V4L2_PIX_FMT_RGB555X,
		.name			= "RGB555X",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= MEDIA_BUS_FMT_RGB565_2X8_LE,
		.fourcc			= V4L2_PIX_FMT_RGB565,
		.name			= "RGB565",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= MEDIA_BUS_FMT_RGB565_2X8_BE,
		.fourcc			= V4L2_PIX_FMT_RGB565X,
		.name			= "RGB565X",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= MEDIA_BUS_FMT_RGB666_1X18,
		.fourcc			= V4L2_PIX_FMT_RGB32,
		.name			= "RGB666/32bpp",
		.bits_per_sample	= 18,
		.packing		= RVIN_MBUS_PACKING_EXTEND32,
	},
	{
		.code			= MEDIA_BUS_FMT_RGB888_2X12_BE,
		.fourcc			= V4L2_PIX_FMT_RGB32,
		.name			= "RGB888/32bpp",
		.bits_per_sample	= 12,
		.packing		= RVIN_MBUS_PACKING_EXTEND32,
	},
	{
		.code			= MEDIA_BUS_FMT_RGB888_2X12_LE,
		.fourcc			= V4L2_PIX_FMT_RGB32,
		.name			= "RGB888/32bpp",
		.bits_per_sample	= 12,
		.packing		= RVIN_MBUS_PACKING_EXTEND32,
	},
	{
		.code			= MEDIA_BUS_FMT_SBGGR8_1X8,
		.fourcc			= V4L2_PIX_FMT_SBGGR8,
		.name			= "Bayer 8 BGGR",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_NONE,
	},
	{
		.code			= MEDIA_BUS_FMT_SBGGR10_1X10,
		.fourcc			= V4L2_PIX_FMT_SBGGR10,
		.name			= "Bayer 10 BGGR",
		.bits_per_sample	= 10,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_Y8_1X8,
		.fourcc			= V4L2_PIX_FMT_GREY,
		.name			= "Grey",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_NONE,
	},
	{
		.code			= MEDIA_BUS_FMT_Y10_1X10,
		.fourcc			= V4L2_PIX_FMT_Y10,
		.name			= "Grey 10bit",
		.bits_per_sample	= 10,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_LE,
		.fourcc			= V4L2_PIX_FMT_SBGGR10,
		.name			= "Bayer 10 BGGR",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= MEDIA_BUS_FMT_SBGGR10_2X8_PADLO_LE,
		.fourcc			= V4L2_PIX_FMT_SBGGR10,
		.name			= "Bayer 10 BGGR",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADLO,
	},
	{
		.code			= MEDIA_BUS_FMT_SBGGR10_2X8_PADHI_BE,
		.fourcc			= V4L2_PIX_FMT_SBGGR10,
		.name			= "Bayer 10 BGGR",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= MEDIA_BUS_FMT_SBGGR10_2X8_PADLO_BE,
		.fourcc			= V4L2_PIX_FMT_SBGGR10,
		.name			= "Bayer 10 BGGR",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADLO,
	},
	{
		.code			= MEDIA_BUS_FMT_JPEG_1X8,
		.fourcc                 = V4L2_PIX_FMT_JPEG,
		.name                   = "JPEG",
		.bits_per_sample        = 8,
		.packing                = RVIN_MBUS_PACKING_VARIABLE,
	},
	{
		.code			= MEDIA_BUS_FMT_RGB444_2X8_PADHI_BE,
		.fourcc			= V4L2_PIX_FMT_RGB444,
		.name			= "RGB444",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_2X8_PADHI,
	},
	{
		.code			= MEDIA_BUS_FMT_YUYV8_1_5X8,
		.fourcc			= V4L2_PIX_FMT_YUV420,
		.name			= "YUYV 4:2:0",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_1_5X8,
	},
	{
		.code			= MEDIA_BUS_FMT_YVYU8_1_5X8,
		.fourcc			= V4L2_PIX_FMT_YVU420,
		.name			= "YVYU 4:2:0",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_1_5X8,
	},
	{
		.code			= MEDIA_BUS_FMT_UYVY8_1X16,
		.fourcc			= V4L2_PIX_FMT_UYVY,
		.name			= "UYVY 16bit",
		.bits_per_sample	= 16,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_VYUY8_1X16,
		.fourcc			= V4L2_PIX_FMT_VYUY,
		.name			= "VYUY 16bit",
		.bits_per_sample	= 16,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_YVYU8_1X16,
		.fourcc			= V4L2_PIX_FMT_YVYU,
		.name			= "YVYU 16bit",
		.bits_per_sample	= 16,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_SGRBG8_1X8,
		.fourcc			= V4L2_PIX_FMT_SGRBG8,
		.name			= "Bayer 8 GRBG",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_NONE,
	},
	{
		.code			= MEDIA_BUS_FMT_SGRBG10_DPCM8_1X8,
		.fourcc			= V4L2_PIX_FMT_SGRBG10DPCM8,
		.name			= "Bayer 10 BGGR DPCM 8",
		.bits_per_sample	= 8,
		.packing		= RVIN_MBUS_PACKING_NONE,
	},
	{
		.code			= MEDIA_BUS_FMT_SGBRG10_1X10,
		.fourcc			= V4L2_PIX_FMT_SGBRG10,
		.name			= "Bayer 10 GBRG",
		.bits_per_sample	= 10,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_SGRBG10_1X10,
		.fourcc			= V4L2_PIX_FMT_SGRBG10,
		.name			= "Bayer 10 GRBG",
		.bits_per_sample	= 10,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_SRGGB10_1X10,
		.fourcc			= V4L2_PIX_FMT_SRGGB10,
		.name			= "Bayer 10 RGGB",
		.bits_per_sample	= 10,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_SBGGR12_1X12,
		.fourcc			= V4L2_PIX_FMT_SBGGR12,
		.name			= "Bayer 12 BGGR",
		.bits_per_sample	= 12,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_SGBRG12_1X12,
		.fourcc			= V4L2_PIX_FMT_SGBRG12,
		.name			= "Bayer 12 GBRG",
		.bits_per_sample	= 12,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_SGRBG12_1X12,
		.fourcc			= V4L2_PIX_FMT_SGRBG12,
		.name			= "Bayer 12 GRBG",
		.bits_per_sample	= 12,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
	{
		.code			= MEDIA_BUS_FMT_SRGGB12_1X12,
		.fourcc			= V4L2_PIX_FMT_SRGGB12,
		.name			= "Bayer 12 RGGB",
		.bits_per_sample	= 12,
		.packing		= RVIN_MBUS_PACKING_EXTEND16,
	},
};

static bool rvin_packing_supported(const struct rvin_video_format *fmt)
{
	return  fmt->packing == RVIN_MBUS_PACKING_NONE ||
		(fmt->bits_per_sample > 8 &&
		 fmt->packing == RVIN_MBUS_PACKING_EXTEND16);
}

static int rvin_add_formats(struct rvin_dev *vin, u32 code, bool *conv_done,
		struct rvin_video_format *fmts)
{
	const struct rvin_video_format *fmt;
	int i;

	switch (code) {
	default:
		break;
	case MEDIA_BUS_FMT_YUYV8_1X16:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_YUYV10_2X10:
	case MEDIA_BUS_FMT_RGB888_1X24:
		/* Add dynamic formats, once */
		if (*conv_done)
			return 0;
		*conv_done = true;

		if (fmts) {
			for (i = 0; i < ARRAY_SIZE(rvin_formats_conv); i++) {
				memcpy(fmts, &rvin_formats_conv[i],
						sizeof(*fmts));
				fmts->code = code;
				vin_dbg(vin,
						"Providing format %s using code %d\n",
						fmts->name, code);
				fmts++;
			}
		}
		return ARRAY_SIZE(rvin_formats_conv);
	}

	fmt = NULL;
	for (i = 0; i < ARRAY_SIZE(rvin_formats_pass); i++)
		if (rvin_formats_pass[i].code == code)
			fmt = &rvin_formats_pass[i];

	if (!fmt) {
		vin_warn(vin, "Unsupported format code: %d\n", code);
		return 0;
	}

	if (!rvin_packing_supported(fmt))
		return 0;

	if (fmts) {
		memcpy(fmts, fmt, sizeof(*fmts));
		vin_dbg(vin, "Providing format %s in pass-through mode\n",
				fmts->name);
		fmts++;
	}
	return 1;
}

static int rvin_init_formats(struct rvin_dev *vin)
{
	struct v4l2_subdev *sd;
	struct v4l2_subdev_mbus_code_enum code = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	bool conv_done;
	int numfmts;

	sd = vin_to_sd(vin);

	/* First pass - Count formats this sensor configuration can provide */
	code.index = 0;
	conv_done = false;
	numfmts = 0;
	while (!v4l2_subdev_call(sd, pad, enum_mbus_code, NULL, &code)) {
		code.index++;
		numfmts += rvin_add_formats(vin, code.code, &conv_done, NULL);
	}

	if (!numfmts)
		return -ENXIO;

	vin->sensor.formats = vmalloc(numfmts *
			sizeof(struct rvin_video_format));
	if (!vin->sensor.formats)
		return -ENOMEM;

	vin->sensor.num_formats = numfmts;
	vin_dbg(vin, "Found %d supported formats.\n", vin->sensor.num_formats);


	/* Second pass - Actually fill data formats */
	code.index = 0;
	conv_done = false;
	numfmts = 0;
	while (!v4l2_subdev_call(sd, pad, enum_mbus_code, NULL, &code)) {
		code.index++;
		numfmts += rvin_add_formats(vin, code.code, &conv_done,
				&vin->sensor.formats[numfmts]);
	}

	return 0;
}

static void rvin_free_formats(struct rvin_dev *vin)
{
	if (vin->sensor.formats) {
		vin->sensor.num_formats = 0;
		vfree(vin->sensor.formats);
	}
}

const struct rvin_video_format *rvin_get_format_by_fourcc(struct rvin_dev *vin,
		u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < vin->sensor.num_formats; i++) {
		if (vin->sensor.formats[i].fourcc == fourcc)
			return vin->sensor.formats + i;
	}
	return NULL;
}

s32 rvin_bytes_per_line(const struct rvin_video_format *info, u32 width)
{
	if (info->fourcc == V4L2_PIX_FMT_NV16)
		return width * info->bits_per_sample / 8;

	switch (info->packing) {
	case RVIN_MBUS_PACKING_NONE:
		return width * info->bits_per_sample / 8;
	case RVIN_MBUS_PACKING_2X8_PADHI:
	case RVIN_MBUS_PACKING_2X8_PADLO:
	case RVIN_MBUS_PACKING_EXTEND16:
		return width * 2;
	case RVIN_MBUS_PACKING_1_5X8:
		return width * 3 / 2;
	case RVIN_MBUS_PACKING_VARIABLE:
		return 0;
	case RVIN_MBUS_PACKING_EXTEND32:
		return width * 4;
	}
	return -EINVAL;
}

s32 rvin_image_size(const struct rvin_video_format *info, u32 bytes_per_line,
		u32 height)
{
	if (info->fourcc != V4L2_PIX_FMT_NV16)
		return bytes_per_line * height;

	switch (info->packing) {
	case RVIN_MBUS_PACKING_2X8_PADHI:
	case RVIN_MBUS_PACKING_2X8_PADLO:
		return bytes_per_line * height * 2;
	case RVIN_MBUS_PACKING_1_5X8:
		return bytes_per_line * height * 3 / 2;
	default:
		return -EINVAL;
	}
}

/* -----------------------------------------------------------------------------
 * Crop and Scaling Gen2
 */

struct vin_coeff {
	unsigned short xs_value;
	u32 coeff_set[24];
};

static const struct vin_coeff vin_coeff_set[] = {
	{ 0x0000, {
			  0x00000000, 0x00000000, 0x00000000,
			  0x00000000, 0x00000000, 0x00000000,
			  0x00000000, 0x00000000, 0x00000000,
			  0x00000000, 0x00000000, 0x00000000,
			  0x00000000, 0x00000000, 0x00000000,
			  0x00000000, 0x00000000, 0x00000000,
			  0x00000000, 0x00000000, 0x00000000,
			  0x00000000, 0x00000000, 0x00000000 },
	},
	{ 0x1000, {
			  0x000fa400, 0x000fa400, 0x09625902,
			  0x000003f8, 0x00000403, 0x3de0d9f0,
			  0x001fffed, 0x00000804, 0x3cc1f9c3,
			  0x001003de, 0x00000c01, 0x3cb34d7f,
			  0x002003d2, 0x00000c00, 0x3d24a92d,
			  0x00200bca, 0x00000bff, 0x3df600d2,
			  0x002013cc, 0x000007ff, 0x3ed70c7e,
			  0x00100fde, 0x00000000, 0x3f87c036 },
	},
	{ 0x1200, {
			  0x002ffff1, 0x002ffff1, 0x02a0a9c8,
			  0x002003e7, 0x001ffffa, 0x000185bc,
			  0x002007dc, 0x000003ff, 0x3e52859c,
			  0x00200bd4, 0x00000002, 0x3d53996b,
			  0x00100fd0, 0x00000403, 0x3d04ad2d,
			  0x00000bd5, 0x00000403, 0x3d35ace7,
			  0x3ff003e4, 0x00000801, 0x3dc674a1,
			  0x3fffe800, 0x00000800, 0x3e76f461 },
	},
	{ 0x1400, {
			  0x00100be3, 0x00100be3, 0x04d1359a,
			  0x00000fdb, 0x002003ed, 0x0211fd93,
			  0x00000fd6, 0x002003f4, 0x0002d97b,
			  0x000007d6, 0x002ffffb, 0x3e93b956,
			  0x3ff003da, 0x001003ff, 0x3db49926,
			  0x3fffefe9, 0x00100001, 0x3d655cee,
			  0x3fffd400, 0x00000003, 0x3d65f4b6,
			  0x000fb421, 0x00000402, 0x3dc6547e },
	},
	{ 0x1600, {
			  0x00000bdd, 0x00000bdd, 0x06519578,
			  0x3ff007da, 0x00000be3, 0x03c24973,
			  0x3ff003d9, 0x00000be9, 0x01b30d5f,
			  0x3ffff7df, 0x001003f1, 0x0003c542,
			  0x000fdfec, 0x001003f7, 0x3ec4711d,
			  0x000fc400, 0x002ffffd, 0x3df504f1,
			  0x001fa81a, 0x002ffc00, 0x3d957cc2,
			  0x002f8c3c, 0x00100000, 0x3db5c891 },
	},
	{ 0x1800, {
			  0x3ff003dc, 0x3ff003dc, 0x0791e558,
			  0x000ff7dd, 0x3ff007de, 0x05328554,
			  0x000fe7e3, 0x3ff00be2, 0x03232546,
			  0x000fd7ee, 0x000007e9, 0x0143bd30,
			  0x001fb800, 0x000007ee, 0x00044511,
			  0x002fa015, 0x000007f4, 0x3ef4bcee,
			  0x002f8832, 0x001003f9, 0x3e4514c7,
			  0x001f7853, 0x001003fd, 0x3de54c9f },
	},
	{ 0x1a00, {
			  0x000fefe0, 0x000fefe0, 0x08721d3c,
			  0x001fdbe7, 0x000ffbde, 0x0652a139,
			  0x001fcbf0, 0x000003df, 0x0463292e,
			  0x002fb3ff, 0x3ff007e3, 0x0293a91d,
			  0x002f9c12, 0x3ff00be7, 0x01241905,
			  0x001f8c29, 0x000007ed, 0x3fe470eb,
			  0x000f7c46, 0x000007f2, 0x3f04b8ca,
			  0x3fef7865, 0x000007f6, 0x3e74e4a8 },
	},
	{ 0x1c00, {
			  0x001fd3e9, 0x001fd3e9, 0x08f23d26,
			  0x002fbff3, 0x001fe3e4, 0x0712ad23,
			  0x002fa800, 0x000ff3e0, 0x05631d1b,
			  0x001f9810, 0x000ffbe1, 0x03b3890d,
			  0x000f8c23, 0x000003e3, 0x0233e8fa,
			  0x3fef843b, 0x000003e7, 0x00f430e4,
			  0x3fbf8456, 0x3ff00bea, 0x00046cc8,
			  0x3f8f8c72, 0x3ff00bef, 0x3f3490ac },
	},
	{ 0x1e00, {
			  0x001fbbf4, 0x001fbbf4, 0x09425112,
			  0x001fa800, 0x002fc7ed, 0x0792b110,
			  0x000f980e, 0x001fdbe6, 0x0613110a,
			  0x3fff8c20, 0x001fe7e3, 0x04a368fd,
			  0x3fcf8c33, 0x000ff7e2, 0x0343b8ed,
			  0x3f9f8c4a, 0x000fffe3, 0x0203f8da,
			  0x3f5f9c61, 0x000003e6, 0x00e428c5,
			  0x3f1fb07b, 0x000003eb, 0x3fe440af },
	},
	{ 0x2000, {
			  0x000fa400, 0x000fa400, 0x09625902,
			  0x3fff980c, 0x001fb7f5, 0x0812b0ff,
			  0x3fdf901c, 0x001fc7ed, 0x06b2fcfa,
			  0x3faf902d, 0x001fd3e8, 0x055348f1,
			  0x3f7f983f, 0x001fe3e5, 0x04038ce3,
			  0x3f3fa454, 0x001fefe3, 0x02e3c8d1,
			  0x3f0fb86a, 0x001ff7e4, 0x01c3e8c0,
			  0x3ecfd880, 0x000fffe6, 0x00c404ac },
	},
	{ 0x2200, {
			  0x3fdf9c0b, 0x3fdf9c0b, 0x09725cf4,
			  0x3fbf9818, 0x3fffa400, 0x0842a8f1,
			  0x3f8f9827, 0x000fb3f7, 0x0702f0ec,
			  0x3f5fa037, 0x000fc3ef, 0x05d330e4,
			  0x3f2fac49, 0x001fcfea, 0x04a364d9,
			  0x3effc05c, 0x001fdbe7, 0x038394ca,
			  0x3ecfdc6f, 0x001fe7e6, 0x0273b0bb,
			  0x3ea00083, 0x001fefe6, 0x0183c0a9 },
	},
	{ 0x2400, {
			  0x3f9fa014, 0x3f9fa014, 0x098260e6,
			  0x3f7f9c23, 0x3fcf9c0a, 0x08629ce5,
			  0x3f4fa431, 0x3fefa400, 0x0742d8e1,
			  0x3f1fb440, 0x3fffb3f8, 0x062310d9,
			  0x3eefc850, 0x000fbbf2, 0x050340d0,
			  0x3ecfe062, 0x000fcbec, 0x041364c2,
			  0x3ea00073, 0x001fd3ea, 0x03037cb5,
			  0x3e902086, 0x001fdfe8, 0x022388a5 },
	},
	{ 0x2600, {
			  0x3f5fa81e, 0x3f5fa81e, 0x096258da,
			  0x3f3fac2b, 0x3f8fa412, 0x088290d8,
			  0x3f0fbc38, 0x3fafa408, 0x0772c8d5,
			  0x3eefcc47, 0x3fcfa800, 0x0672f4ce,
			  0x3ecfe456, 0x3fefaffa, 0x05531cc6,
			  0x3eb00066, 0x3fffbbf3, 0x047334bb,
			  0x3ea01c77, 0x000fc7ee, 0x039348ae,
			  0x3ea04486, 0x000fd3eb, 0x02b350a1 },
	},
	{ 0x2800, {
			  0x3f2fb426, 0x3f2fb426, 0x094250ce,
			  0x3f0fc032, 0x3f4fac1b, 0x086284cd,
			  0x3eefd040, 0x3f7fa811, 0x0782acc9,
			  0x3ecfe84c, 0x3f9fa807, 0x06a2d8c4,
			  0x3eb0005b, 0x3fbfac00, 0x05b2f4bc,
			  0x3eb0186a, 0x3fdfb3fa, 0x04c308b4,
			  0x3eb04077, 0x3fefbbf4, 0x03f31ca8,
			  0x3ec06884, 0x000fbff2, 0x03031c9e },
	},
	{ 0x2a00, {
			  0x3f0fc42d, 0x3f0fc42d, 0x090240c4,
			  0x3eefd439, 0x3f2fb822, 0x08526cc2,
			  0x3edfe845, 0x3f4fb018, 0x078294bf,
			  0x3ec00051, 0x3f6fac0f, 0x06b2b4bb,
			  0x3ec0185f, 0x3f8fac07, 0x05e2ccb4,
			  0x3ec0386b, 0x3fafac00, 0x0502e8ac,
			  0x3ed05c77, 0x3fcfb3fb, 0x0432f0a3,
			  0x3ef08482, 0x3fdfbbf6, 0x0372f898 },
	},
	{ 0x2c00, {
			  0x3eefdc31, 0x3eefdc31, 0x08e238b8,
			  0x3edfec3d, 0x3f0fc828, 0x082258b9,
			  0x3ed00049, 0x3f1fc01e, 0x077278b6,
			  0x3ed01455, 0x3f3fb815, 0x06c294b2,
			  0x3ed03460, 0x3f5fb40d, 0x0602acac,
			  0x3ef0506c, 0x3f7fb006, 0x0542c0a4,
			  0x3f107476, 0x3f9fb400, 0x0472c89d,
			  0x3f309c80, 0x3fbfb7fc, 0x03b2cc94 },
	},
	{ 0x2e00, {
			  0x3eefec37, 0x3eefec37, 0x088220b0,
			  0x3ee00041, 0x3effdc2d, 0x07f244ae,
			  0x3ee0144c, 0x3f0fd023, 0x07625cad,
			  0x3ef02c57, 0x3f1fc81a, 0x06c274a9,
			  0x3f004861, 0x3f3fbc13, 0x060288a6,
			  0x3f20686b, 0x3f5fb80c, 0x05529c9e,
			  0x3f408c74, 0x3f6fb805, 0x04b2ac96,
			  0x3f80ac7e, 0x3f8fb800, 0x0402ac8e },
	},
	{ 0x3000, {
			  0x3ef0003a, 0x3ef0003a, 0x084210a6,
			  0x3ef01045, 0x3effec32, 0x07b228a7,
			  0x3f00284e, 0x3f0fdc29, 0x073244a4,
			  0x3f104058, 0x3f0fd420, 0x06a258a2,
			  0x3f305c62, 0x3f2fc818, 0x0612689d,
			  0x3f508069, 0x3f3fc011, 0x05728496,
			  0x3f80a072, 0x3f4fc00a, 0x04d28c90,
			  0x3fc0c07b, 0x3f6fbc04, 0x04429088 },
	},
	{ 0x3200, {
			  0x3f00103e, 0x3f00103e, 0x07f1fc9e,
			  0x3f102447, 0x3f000035, 0x0782149d,
			  0x3f203c4f, 0x3f0ff02c, 0x07122c9c,
			  0x3f405458, 0x3f0fe424, 0x06924099,
			  0x3f607061, 0x3f1fd41d, 0x06024c97,
			  0x3f909068, 0x3f2fcc16, 0x05726490,
			  0x3fc0b070, 0x3f3fc80f, 0x04f26c8a,
			  0x0000d077, 0x3f4fc409, 0x04627484 },
	},
	{ 0x3400, {
			  0x3f202040, 0x3f202040, 0x07a1e898,
			  0x3f303449, 0x3f100c38, 0x0741fc98,
			  0x3f504c50, 0x3f10002f, 0x06e21495,
			  0x3f706459, 0x3f1ff028, 0x06722492,
			  0x3fa08060, 0x3f1fe421, 0x05f2348f,
			  0x3fd09c67, 0x3f1fdc19, 0x05824c89,
			  0x0000bc6e, 0x3f2fd014, 0x04f25086,
			  0x0040dc74, 0x3f3fcc0d, 0x04825c7f },
	},
	{ 0x3600, {
			  0x3f403042, 0x3f403042, 0x0761d890,
			  0x3f504848, 0x3f301c3b, 0x0701f090,
			  0x3f805c50, 0x3f200c33, 0x06a2008f,
			  0x3fa07458, 0x3f10002b, 0x06520c8d,
			  0x3fd0905e, 0x3f1ff424, 0x05e22089,
			  0x0000ac65, 0x3f1fe81d, 0x05823483,
			  0x0030cc6a, 0x3f2fdc18, 0x04f23c81,
			  0x0080e871, 0x3f2fd412, 0x0482407c },
	},
	{ 0x3800, {
			  0x3f604043, 0x3f604043, 0x0721c88a,
			  0x3f80544a, 0x3f502c3c, 0x06d1d88a,
			  0x3fb06851, 0x3f301c35, 0x0681e889,
			  0x3fd08456, 0x3f30082f, 0x0611fc88,
			  0x00009c5d, 0x3f200027, 0x05d20884,
			  0x0030b863, 0x3f2ff421, 0x05621880,
			  0x0070d468, 0x3f2fe81b, 0x0502247c,
			  0x00c0ec6f, 0x3f2fe015, 0x04a22877 },
	},
	{ 0x3a00, {
			  0x3f904c44, 0x3f904c44, 0x06e1b884,
			  0x3fb0604a, 0x3f70383e, 0x0691c885,
			  0x3fe07451, 0x3f502c36, 0x0661d483,
			  0x00009055, 0x3f401831, 0x0601ec81,
			  0x0030a85b, 0x3f300c2a, 0x05b1f480,
			  0x0070c061, 0x3f300024, 0x0562047a,
			  0x00b0d867, 0x3f3ff41e, 0x05020c77,
			  0x00f0f46b, 0x3f2fec19, 0x04a21474 },
	},
	{ 0x3c00, {
			  0x3fb05c43, 0x3fb05c43, 0x06c1b07e,
			  0x3fe06c4b, 0x3f902c3f, 0x0681c081,
			  0x0000844f, 0x3f703838, 0x0631cc7d,
			  0x00309855, 0x3f602433, 0x05d1d47e,
			  0x0060b459, 0x3f50142e, 0x0581e47b,
			  0x00a0c85f, 0x3f400828, 0x0531f078,
			  0x00e0e064, 0x3f300021, 0x0501fc73,
			  0x00b0fc6a, 0x3f3ff41d, 0x04a20873 },
	},
	{ 0x3e00, {
			  0x3fe06444, 0x3fe06444, 0x0681a07a,
			  0x00007849, 0x3fc0503f, 0x0641b07a,
			  0x0020904d, 0x3fa0403a, 0x05f1c07a,
			  0x0060a453, 0x3f803034, 0x05c1c878,
			  0x0090b858, 0x3f70202f, 0x0571d477,
			  0x00d0d05d, 0x3f501829, 0x0531e073,
			  0x0110e462, 0x3f500825, 0x04e1e471,
			  0x01510065, 0x3f40001f, 0x04a1f06d },
	},
	{ 0x4000, {
			  0x00007044, 0x00007044, 0x06519476,
			  0x00208448, 0x3fe05c3f, 0x0621a476,
			  0x0050984d, 0x3fc04c3a, 0x05e1b075,
			  0x0080ac52, 0x3fa03c35, 0x05a1b875,
			  0x00c0c056, 0x3f803030, 0x0561c473,
			  0x0100d45b, 0x3f70202b, 0x0521d46f,
			  0x0140e860, 0x3f601427, 0x04d1d46e,
			  0x01810064, 0x3f500822, 0x0491dc6b },
	},
	{ 0x5000, {
			  0x0110a442, 0x0110a442, 0x0551545e,
			  0x0140b045, 0x00e0983f, 0x0531585f,
			  0x0160c047, 0x00c08c3c, 0x0511645e,
			  0x0190cc4a, 0x00908039, 0x04f1685f,
			  0x01c0dc4c, 0x00707436, 0x04d1705e,
			  0x0200e850, 0x00506833, 0x04b1785b,
			  0x0230f453, 0x00305c30, 0x0491805a,
			  0x02710056, 0x0010542d, 0x04718059 },
	},
	{ 0x6000, {
			  0x01c0bc40, 0x01c0bc40, 0x04c13052,
			  0x01e0c841, 0x01a0b43d, 0x04c13851,
			  0x0210cc44, 0x0180a83c, 0x04a13453,
			  0x0230d845, 0x0160a03a, 0x04913c52,
			  0x0260e047, 0x01409838, 0x04714052,
			  0x0280ec49, 0x01208c37, 0x04514c50,
			  0x02b0f44b, 0x01008435, 0x04414c50,
			  0x02d1004c, 0x00e07c33, 0x0431544f },
	},
	{ 0x7000, {
			  0x0230c83e, 0x0230c83e, 0x04711c4c,
			  0x0250d03f, 0x0210c43c, 0x0471204b,
			  0x0270d840, 0x0200b83c, 0x0451244b,
			  0x0290dc42, 0x01e0b43a, 0x0441244c,
			  0x02b0e443, 0x01c0b038, 0x0441284b,
			  0x02d0ec44, 0x01b0a438, 0x0421304a,
			  0x02f0f445, 0x0190a036, 0x04213449,
			  0x0310f847, 0x01709c34, 0x04213848 },
	},
	{ 0x8000, {
			  0x0280d03d, 0x0280d03d, 0x04310c48,
			  0x02a0d43e, 0x0270c83c, 0x04311047,
			  0x02b0dc3e, 0x0250c83a, 0x04311447,
			  0x02d0e040, 0x0240c03a, 0x04211446,
			  0x02e0e840, 0x0220bc39, 0x04111847,
			  0x0300e842, 0x0210b438, 0x04012445,
			  0x0310f043, 0x0200b037, 0x04012045,
			  0x0330f444, 0x01e0ac36, 0x03f12445 },
	},
	{ 0xefff, {
			  0x0340dc3a, 0x0340dc3a, 0x03b0ec40,
			  0x0340e03a, 0x0330e039, 0x03c0f03e,
			  0x0350e03b, 0x0330dc39, 0x03c0ec3e,
			  0x0350e43a, 0x0320dc38, 0x03c0f43e,
			  0x0360e43b, 0x0320d839, 0x03b0f03e,
			  0x0360e83b, 0x0310d838, 0x03c0fc3b,
			  0x0370e83b, 0x0310d439, 0x03a0f83d,
			  0x0370e83c, 0x0300d438, 0x03b0fc3c },
	}
};

static void rvin_set_coeff(struct rvin_dev *vin, unsigned short xs)
{
	int i;
	const struct vin_coeff *p_prev_set = NULL;
	const struct vin_coeff *p_set = NULL;

	/* Look for suitable coefficient values */
	for (i = 0; i < ARRAY_SIZE(vin_coeff_set); i++) {
		p_prev_set = p_set;
		p_set = &vin_coeff_set[i];

		if (xs < p_set->xs_value)
			break;
	}

	/* Use previous value if its XS value is closer */
	if (p_prev_set && p_set &&
			xs - p_prev_set->xs_value < p_set->xs_value - xs)
		p_set = p_prev_set;

	/* Set coefficient registers */
	rvin_write(vin, p_set->coeff_set[0], VNC1A_REG);
	rvin_write(vin, p_set->coeff_set[1], VNC1B_REG);
	rvin_write(vin, p_set->coeff_set[2], VNC1C_REG);

	rvin_write(vin, p_set->coeff_set[3], VNC2A_REG);
	rvin_write(vin, p_set->coeff_set[4], VNC2B_REG);
	rvin_write(vin, p_set->coeff_set[5], VNC2C_REG);

	rvin_write(vin, p_set->coeff_set[6], VNC3A_REG);
	rvin_write(vin, p_set->coeff_set[7], VNC3B_REG);
	rvin_write(vin, p_set->coeff_set[8], VNC3C_REG);

	rvin_write(vin, p_set->coeff_set[9], VNC4A_REG);
	rvin_write(vin, p_set->coeff_set[10], VNC4B_REG);
	rvin_write(vin, p_set->coeff_set[11], VNC4C_REG);

	rvin_write(vin, p_set->coeff_set[12], VNC5A_REG);
	rvin_write(vin, p_set->coeff_set[13], VNC5B_REG);
	rvin_write(vin, p_set->coeff_set[14], VNC5C_REG);

	rvin_write(vin, p_set->coeff_set[15], VNC6A_REG);
	rvin_write(vin, p_set->coeff_set[16], VNC6B_REG);
	rvin_write(vin, p_set->coeff_set[17], VNC6C_REG);

	rvin_write(vin, p_set->coeff_set[18], VNC7A_REG);
	rvin_write(vin, p_set->coeff_set[19], VNC7B_REG);
	rvin_write(vin, p_set->coeff_set[20], VNC7C_REG);

	rvin_write(vin, p_set->coeff_set[21], VNC8A_REG);
	rvin_write(vin, p_set->coeff_set[22], VNC8B_REG);
	rvin_write(vin, p_set->coeff_set[23], VNC8C_REG);
}

int rvin_scale_setup(struct rvin_dev *vin)
{
	unsigned char dsize = 0;
	u32 value;

	/* Crop and scale*/
	struct v4l2_rect crop;
	/* TODO: This should be set in vidioc_s_selection and not be static */
	crop.left = 0;
	crop.top = 0;
	crop.width = vin->sensor.width;
	crop.height = vin->sensor.height;

	if (vin->fmtinfo->fourcc == V4L2_PIX_FMT_RGB32 && vin->chip == RCAR_E1)
		dsize = 1;

	/* Set Start/End Pixel/Line Pre-Clip */
	vin_dbg(vin, "Pre-Clip: %ux%u@%u:%u\n", crop.width, crop.height,
			crop.left, crop.top);
	rvin_write(vin, crop.left << dsize, VNSPPRC_REG);
	rvin_write(vin, (crop.left + crop.width - 1) << dsize,
			VNEPPRC_REG);
	switch (vin->format.field) {
	case V4L2_FIELD_INTERLACED:
	case V4L2_FIELD_INTERLACED_TB:
	case V4L2_FIELD_INTERLACED_BT:
		rvin_write(vin, crop.top / 2, VNSLPRC_REG);
		rvin_write(vin, (crop.top + crop.height) / 2 - 1,
				VNELPRC_REG);
		break;
	default:
		rvin_write(vin, crop.top, VNSLPRC_REG);
		rvin_write(vin, crop.top + crop.height - 1,
				VNELPRC_REG);
		break;
	}
	/* Set scaling coefficient */
	value = 0;
	if (crop.height != vin->format.height)
		value = (4096 * crop.height) / vin->format.height;
	vin_dbg(vin, "YS Value: 0x%x\n", value);
	rvin_write(vin, value, VNYS_REG);

	value = 0;
	if (crop.width != vin->format.width)
		value = (4096 * crop.width) / vin->format.width;

	/* Horizontal upscaling is up to double size */
	if (value > 0 && value < 2048)
		value = 2048;

	vin_dbg(vin, "XS Value: 0x%x\n", value);
	rvin_write(vin, value, VNXS_REG);

	/* Horizontal upscaling is done out by scaling down from double size */
	if (value < 4096)
		value *= 2;

	rvin_set_coeff(vin, value);

	/* Set Start/End Pixel/Line Post-Clip */
	vin_dbg(vin, "Post-Clip: %ux%u@%u:%u\n", vin->format.width,
			vin->format.height, 0, 0);
	rvin_write(vin, 0, VNSPPOC_REG);
	rvin_write(vin, 0, VNSLPOC_REG);
	rvin_write(vin, (vin->format.width - 1) << dsize, VNEPPOC_REG);
	switch (vin->format.field) {
	case V4L2_FIELD_INTERLACED:
	case V4L2_FIELD_INTERLACED_TB:
	case V4L2_FIELD_INTERLACED_BT:
		rvin_write(vin, vin->format.height / 2 - 1, VNELPOC_REG);
		break;
	default:
		rvin_write(vin, vin->format.height - 1, VNELPOC_REG);
		break;
	}

	rvin_write(vin, ALIGN(vin->format.width, 0x10), VNIS_REG);

	return 0;
}

int rvin_scale_try(struct rvin_dev *vin, struct v4l2_pix_format *pix,
		u32 width, u32 height)
{
	/* All VIN channels on Gen2 have scalers */
	pix->width = width;
	pix->height = height;
	return 0;
}

/* -----------------------------------------------------------------------------
 * Async notifier
 */

static int rvin_graph_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct v4l2_subdev *sd;
	struct rvin_dev *vin = notifier_to_vin(notifier);
	int ret;

	sd = vin_to_sd(vin);

	ret = v4l2_device_register_subdev_nodes(&vin->v4l2_dev);
	if (ret < 0) {
		vin_err(vin, "failed to register subdev nodes\n");
		return ret;
	}

	/* Figure out what formats are supported */
	ret = rvin_init_formats(vin);
	if (ret < 0)
		return ret;

	ret = rvin_dma_on(vin);
	if (ret)
		rvin_free_formats(vin);

	return ret;
}

static int rvin_graph_notify_bound(struct v4l2_async_notifier *notifier,
		struct v4l2_subdev *subdev,
		struct v4l2_async_subdev *asd)
{
	struct rvin_dev *vin = notifier_to_vin(notifier);

	vin_dbg(vin, "subdev %s bound\n", subdev->name);

	vin->entity.entity = &subdev->entity;
	vin->entity.subdev = subdev;

	return 0;
}

static int rvin_graph_parse(struct rvin_dev *vin,
		struct device_node *node)
{
	struct device_node *remote;
	struct device_node *ep = NULL;
	struct device_node *next;
	int ret = 0;

	while (1) {
		next = of_graph_get_next_endpoint(node, ep);
		if (!next)
			break;

		of_node_put(ep);
		ep = next;

		remote = of_graph_get_remote_port_parent(ep);
		if (!remote) {
			ret = -EINVAL;
			break;
		}

		/* Skip entities that we have already processed. */
		if (remote == vin->dev->of_node) {
			of_node_put(remote);
			continue;
		}

		/* Remote node to connect */
		if (!vin->entity.node) {
			vin->entity.node = remote;
			vin->entity.asd.match_type = V4L2_ASYNC_MATCH_OF;
			vin->entity.asd.match.of.node = remote;
			ret++;
		}
	}

	of_node_put(ep);

	return ret;
}

static int rvin_graph_init(struct rvin_dev *vin)
{
	struct v4l2_async_subdev **subdevs = NULL;
	int ret;

	/* Parse the graph to extract a list of subdevice DT nodes. */
	ret = rvin_graph_parse(vin, vin->dev->of_node);
	if (ret < 0) {
		vin_err(vin, "Graph parsing failed\n");
		goto done;
	}

	if (!ret) {
		vin_err(vin, "No subdev found in graph\n");
		goto done;
	}

	if (ret != 1) {
		vin_err(vin, "More then one subdev found in graph\n");
		goto done;
	}

	/* Register the subdevices notifier. */
	subdevs = devm_kzalloc(vin->dev, sizeof(*subdevs), GFP_KERNEL);
	if (subdevs == NULL) {
		ret = -ENOMEM;
		goto done;
	}

	subdevs[0] = &vin->entity.asd;

	vin->notifier.subdevs = subdevs;
	vin->notifier.num_subdevs = 1;
	vin->notifier.bound = rvin_graph_notify_bound;
	vin->notifier.complete = rvin_graph_notify_complete;

	ret = v4l2_async_notifier_register(&vin->v4l2_dev, &vin->notifier);
	if (ret < 0) {
		vin_err(vin, "Notifier registration failed\n");
		goto done;
	}

	ret = 0;

done:
	if (ret < 0) {
		v4l2_async_notifier_unregister(&vin->notifier);
		of_node_put(vin->entity.node);
	}

	return ret;
}

/* -----------------------------------------------------------------------------
 * Platform Device Driver
 */

static const struct of_device_id rvin_of_id_table[] = {
	{ .compatible = "renesas,vin-r8a7794", .data = (void *)RCAR_GEN2 },
	{ .compatible = "renesas,vin-r8a7793", .data = (void *)RCAR_GEN2 },
	{ .compatible = "renesas,vin-r8a7791", .data = (void *)RCAR_GEN2 },
	{ .compatible = "renesas,vin-r8a7790", .data = (void *)RCAR_GEN2 },
	{ .compatible = "renesas,vin-r8a7779", .data = (void *)RCAR_H1 },
	{ .compatible = "renesas,vin-r8a7778", .data = (void *)RCAR_M1 },
	{ },
};
MODULE_DEVICE_TABLE(of, rvin_of_id_table);

static int rvin_get_pdata_flags(struct device *dev, unsigned int *pdata_flags)
{
	struct v4l2_of_endpoint ep;
	struct device_node *np;
	unsigned int flags;
	int ret;

	np = of_graph_get_next_endpoint(dev->of_node, NULL);
	if (!np) {
		dev_err(dev, "Could not find endpoint\n");
		return -EINVAL;
	}

	ret = v4l2_of_parse_endpoint(np, &ep);
	if (ret) {
		dev_err(dev, "Could not parse endpoint\n");
		return ret;
	}

	if (ep.bus_type == V4L2_MBUS_BT656)
		flags = RVIN_BT656;
	else {
		flags = 0;
		if (ep.bus.parallel.flags & V4L2_MBUS_HSYNC_ACTIVE_LOW)
			flags |= RVIN_HSYNC_ACTIVE_LOW;
		if (ep.bus.parallel.flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)
			flags |= RVIN_VSYNC_ACTIVE_LOW;
	}

	of_node_put(np);

	*pdata_flags = flags;

	return 0;
}

static int rvin_init(struct rvin_dev *vin, struct platform_device *pdev)
{
	const struct of_device_id *match = NULL;
	struct resource *mem;
	int ret;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (mem == NULL)
		return -EINVAL;

	vin->dev = &pdev->dev;

	match = of_match_device(of_match_ptr(rvin_of_id_table), vin->dev);
	if (!match)
		return -ENODEV;
	vin->chip = (enum chip_id)match->data;

	ret = rvin_get_pdata_flags(vin->dev, &vin->pdata_flags);
	if (ret)
		return ret;

	vin->base = devm_ioremap_resource(vin->dev, mem);
	if (IS_ERR(vin->base))
		return PTR_ERR(vin->base);

	/* Initialize the top-level structure */
	return v4l2_device_register(vin->dev, &vin->v4l2_dev);
}

static int rcar_vin_probe(struct platform_device *pdev)
{
	struct rvin_dev *vin;
	int irq, ret;

	vin = devm_kzalloc(&pdev->dev, sizeof(*vin), GFP_KERNEL);
	if (!vin)
		return -ENOMEM;


	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return -EINVAL;


	ret = rvin_init(vin, pdev);
	if (ret)
		goto error;

	ret = rvin_dma_init(vin, irq);
	if (ret)
		goto free_dev;

	ret = rvin_graph_init(vin);
	if (ret < 0)
		goto free_dma;

	pm_suspend_ignore_children(&pdev->dev, true);
	pm_runtime_enable(&pdev->dev);

	platform_set_drvdata(pdev, vin);

	return 0;

free_dma:
	rvin_dma_cleanup(vin);
free_dev:
	v4l2_device_unregister(&vin->v4l2_dev);
error:

	return ret;
}

static int rcar_vin_remove(struct platform_device *pdev)
{
	struct rvin_dev *vin = platform_get_drvdata(pdev);

	v4l2_async_notifier_unregister(&vin->notifier);

	rvin_dma_cleanup(vin);

	rvin_free_formats(vin);

	pm_runtime_disable(&pdev->dev);

	v4l2_device_unregister(&vin->v4l2_dev);

	return 0;
}

static struct platform_driver rcar_vin_driver = {
	.driver = {
		.name = "rcar-vin",
		.of_match_table = rvin_of_id_table,
	},
	.probe = rcar_vin_probe,
	.remove = rcar_vin_remove,
};

module_platform_driver(rcar_vin_driver);

MODULE_AUTHOR("Niklas Söderlund <niklas.soderlund@ragnatech.se>");
MODULE_DESCRIPTION("Renesas R-Car VIN camera host driver");
MODULE_LICENSE("GPL v2");
