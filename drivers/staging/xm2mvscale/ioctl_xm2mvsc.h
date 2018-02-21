/*
 * Xilinx Memory-to-Memory Video Scaler IP
 *
 * Copyright (C) 2018 Xilinx, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
 */
#ifndef __IOCTL_XM2MVSC_H__
#define __IOCTL_XM2MVSC_H__

/* Xilinx Video Specific Color/Pixel Formats */
enum xm2mvsc_pix_fmt {
	XILINX_FRMBUF_FMT_RGBX8 = 10,
	XILINX_FRMBUF_FMT_YUVX8,
	XILINX_FRMBUF_FMT_YUYV8,
	XILINX_FRMBUF_FMT_RGBA8,
	XILINX_FRMBUF_FMT_YUVA8,
	XILINX_FRMBUF_FMT_RGBX10,
	XILINX_FRMBUF_FMT_YUVX10,
	/* RGB565 takes the value of 17 */
	XILINX_FRMBUF_FMT_Y_UV8 = 18,
	XILINX_FRMBUF_FMT_Y_UV8_420,
	XILINX_FRMBUF_FMT_RGB8,
	XILINX_FRMBUF_FMT_YUV8,
	XILINX_FRMBUF_FMT_Y_UV10,
	XILINX_FRMBUF_FMT_Y_UV10_420,
	XILINX_FRMBUF_FMT_Y8,
	XILINX_FRMBUF_FMT_Y10,
	XILINX_FRMBUF_FMT_BGRA8,
	XILINX_FRMBUF_FMT_BGRX8,
	XILINX_FRMBUF_FMT_UYVY8,
	XILINX_FRMBUF_FMT_BGR8,
};

/* struct xm2mvsc_qdata - Struct to enqueue a descriptor
 * @srcbuf_ht: Height of source buffer
 * @srcbuf_wt: Width of source buffer
 * @srcbuf_bpp: Bytes per pixel of source buffer
 * @srcbuf_cft: Color/Pixel format of source buffer
 * @srcbuf_size: Size of the source buffer requested
 * @srcbuf_mmap: Identify if srcbuf is mmap'ed
 * @srcbuf_stride: Stride of the source buffer
 * @dstbuf_ht: Height of destination buffer
 * @dstbuf_wt: Width of destination buffer
 * @dstbuf_bpp: Bytes per pixel of destination buffer
 * @dstbuf_cft: Color/Pixel format of source buffer
 * @dstbuf_size: Size of the source buffer requested
 * @dstbuf_mmap: Identify if srcbuf is mmap'ed
 * @dstbuf_stride: Stride of the source buffer
 * @dstbuf_cft: Color Format of destination buffer
 * @desc_id: Keep a track of the descriptors
 */
struct xm2mvsc_qdata {
	/* Source information */
	u32 srcbuf_ht;
	u32 srcbuf_wt;
	u32 srcbuf_bpp;
	enum xm2mvsc_pix_fmt srcbuf_cft;
	size_t srcbuf_size;
	/* srcbuf_mmap : For use by the library, do not touch */
	bool srcbuf_mmap;
	u16 srcbuf_stride;
	/* Destination information */
	u32 dstbuf_ht;
	u32 dstbuf_wt;
	u32 dstbuf_bpp;
	enum xm2mvsc_pix_fmt dstbuf_cft;
	size_t dstbuf_size;
	/* dstbuf_mmap : For use by the library, do not touch */
	bool dstbuf_mmap;
	u16 dstbuf_stride;
	u32 desc_id;
};

/**
 * struct xm2mvsc_dqdata - Struct to dequeue a completed descriptor
 * @desc_id: Descriptor ID that needs to be dequeued
 */
struct xm2mvsc_dqdata {
	u32 desc_id;
};

/**
 * struct xm2mvsc_batch - Struct to specify the batch size
 * @batch_size: Number of channels the scaler should operate per scaling op
 */
struct xm2mvsc_batch {
	u16 batch_size;
};

/* XM2MVSCALE IOCTL LIST */
#define XM2MVSC_MAGIC		'X'

/*
 * DOC: XM2MVSC_ENQUEUE
 * Enqueue  a descriptor that describes the scaling operation for a channel.
 * Returns the descriptor ID
 */
#define XM2MVSC_ENQUEUE		_IOWR(XM2MVSC_MAGIC, 1, struct xm2mvsc_qdata *)

/*
 * DOC: XM2MVSC_START
 * Start the M2M Scaler IP. Driver will operate on descriptors in the
 * pending list.
 */
#define XM2MVSC_START		_IO(XM2MVSC_MAGIC, 2)

/*
 * DOC: XM2MVSC_DEQUEUE
 * Dequeue a descriptor by providing the driver with information about the
 * descriptor that needs to be dequeued.
 */
#define XM2MVSC_DEQUEUE		_IOW(XM2MVSC_MAGIC, 3, struct xm2mvsc_dqdata *)

/*
 * DOC: XM2MVSC_STOP
 * Stop the M2M Scaler IP. Clear driver state and reset the IP.
 */
#define XM2MVSC_STOP		_IO(XM2MVSC_MAGIC, 4)

/*
 * DOC: XM2MVSC_FREE
 * Free a descriptor after being dequeued via XM2MVSC_DEQUEUE ioctl.
 */
#define XM2MVSC_FREE		_IOW(XM2MVSC_MAGIC, 5, struct xm2mvsc_dqdata *)

/*
 * DOC: XM2MVSC_BATCH_SIZE
 * Set the batch size that the M2M Scaler IP should use when programming the
 * scaler. Driver may reject the incoming batch size.
 */
#define XM2MVSC_BATCH_SIZE	_IOW(XM2MVSC_MAGIC, 6, struct xm2mvsc_batch *)

#endif /* __IOCTL_XM2MVSC_H__ */
