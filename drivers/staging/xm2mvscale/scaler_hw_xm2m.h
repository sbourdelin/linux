/*
 * Xilinx Memory-to-Memory Video Scaler IP
 *
 * Copyright (C) 2018 Xilinx, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef __SCALER_HW_XM2M_H__
#define __SCALER_HW_XM2M_H__

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/types.h>

#include "ioctl_xm2mvsc.h"

#define XSCALER_MAX_WIDTH               (3840)
#define XSCALER_MAX_HEIGHT              (2160)
#define XSCALER_MAX_PHASES              (64)

#define XV_SCALER_MAX_TAPS		(12)

#define XV_SCALER_TAPS_6		(6)
#define XV_SCALER_TAPS_8		(8)
#define XV_SCALER_TAPS_10		(10)
#define XV_SCALER_TAPS_12		(12)

/* Filter bank ID  for various filter tap configurations */
enum xm2mvsc_filter_bank_id {
	FILTER_BANK_TAPS_6 = 0,
	FILTER_BANK_TAPS_8,
	FILTER_BANK_TAPS_10,
	FILTER_BANK_TAPS_12,
};

#define XSCALER_BATCH_SIZE_MAX		(8)
#define XSCALER_BATCH_SIZE_MIN		(1)

struct xm2m_vscale_dev;

/**
 * struct xm2m_scaler_hw - Scaler Hardware Info
 * @regs: IO mapped base address of the HW/IP
 * @dev: Pointer to struct device instance
 * @num_taps: Polyhphase filter taps Scaler IP
 * @max_chan: Maximum number of Scaling Channels
 * @max_pixels: Maximum number of pixel supported in a line
 * @max_lines: Maximum number of lines supported in a frame
 * @hscaler_coeff: Array of filter coefficients for the Horizontal Scaler
 * @vscaler_coeff: Array of filter coefficients for the Vertical Scaler
 */
struct xm2m_scaler_hw {
	void __iomem *regs;
	struct device *dev;
	u32 num_taps;
	u32 max_chan;
	u32 max_pixels;
	u32 max_lines;
	short hscaler_coeff[XSCALER_MAX_PHASES][XV_SCALER_MAX_TAPS];
	short vscaler_coeff[XSCALER_MAX_PHASES][XV_SCALER_MAX_TAPS];
};

/**
 * struct xm2m_vscale_desc - Video Scale Frame Descriptor
 * @data: Data enqueued by the application
 * @line_rate: Line rate needed by a scaling channel
 * @pixel_rate: Pixel rate needed by a scaling channel
 * @filter_bank: Filter Bank ID needed to source filter coefficients
 * @channel_offset: Channel offset of the descriptor mapping to HW register
 * @srcbuf_addr: physical address of source buffer
 * @dstbuf_addr: physical address of destination buffer
 * @xm2mvsc_dev: Pointer to parent xm2mvsc driver structure
 * @node: List node to control descriptors in lists
 * @src_kaddr: Kernel VA for source buffer allocated by the driver
 * @dst_kaddr: Kernel VA for destination buffer allocated by the driver
 */
struct xm2m_vscale_desc {
	struct xm2mvsc_qdata data;
	u32 line_rate;
	u32 pixel_rate;
	u8 filter_bank;
	u8 channel_offset;
	dma_addr_t srcbuf_addr;
	dma_addr_t dstbuf_addr;
	struct xm2m_vscale_dev *xm2mvsc_dev;
	struct list_head node;
	void *src_kaddr;
	void *dst_kaddr;
};

/**
 * struct xm2m_vscale_dev - Xilinx M2M Scaler Device
 * @dev: Pointer to struct device instance used by the driver
 * @hw: HW/IP specific structure describing the capabilities
 * @lock: Spinlock to protect driver data structures
 * @pending_list: List containing descriptors not yet processed
 * @ongoing_list: List containing descriptors that are in-flight
 * @done_list: List containing descriptors that are done processing
 * @free_list: List containing descriptors that need to be freed
 * @waitq: Wait queue used by the driver
 * @irq: IRQ number
 * @chdev: Char device handle
 * @id: Device instance ID
 * @rst_gpio: GPIO reset line to bring VPSS Scaler out of reset
 * @desc_count: Desc Count issued by the driver
 * @user_count: Count of users who have opened the device
 * @batch_size: Number of channel actively used in a scaling operation
 * @ongoing_count: Number of channels already used in the ongoing operation
 */
struct xm2m_vscale_dev {
	struct device *dev;
	struct xm2m_scaler_hw hw;
	/* Synchronize access to lists */
	spinlock_t lock;
	struct list_head pending_list;
	struct list_head ongoing_list;
	struct list_head done_list;
	struct list_head free_list;
	wait_queue_head_t waitq;
	int irq;
	struct cdev chdev;
	u32 id;
	struct gpio_desc *rst_gpio;
	atomic_t desc_count;
	atomic_t user_count;
	u16 batch_size;
	atomic_t ongoing_count;
};

static inline u32 xvip_read(const struct xm2m_scaler_hw *hw, const u32 addr)
{
	return ioread32(hw->regs + addr);
}

static inline void xvip_write(const struct xm2m_scaler_hw *hw,
			      const u32 addr, const u32 value)
{
	iowrite32(value, hw->regs + addr);
}

void xm2mvsc_write_desc(struct xm2m_vscale_desc *desc);
void xm2mvsc_start_scaling(const struct xm2m_scaler_hw *hw,
			   const u8 batch_size);
void xm2mvsc_stop_scaling(const struct xm2m_scaler_hw *hw);
u32 xm2mvsc_get_irq_status(const struct xm2m_scaler_hw *hw);
void xm2mvsc_log_register(const struct xm2m_scaler_hw *hw, const u8 chan_off);
void xm2mvsc_initialize_coeff_banks(struct xm2m_scaler_hw *hw);

#endif /* __XM2M_SCALER_SETUP_H__ */
