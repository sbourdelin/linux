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

#ifndef __RCAR_VIN__
#define __RCAR_VIN__

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

enum chip_id {
	RCAR_GEN2,
	RCAR_H1,
	RCAR_M1,
	RCAR_E1,
};

/* Max number of HW buffers */
#define MAX_BUFFER_NUM 3

/**
 * enum rvin_mbus_packing - data packing types on the media-bus
 * @RVIN_MBUS_PACKING_NONE:      no packing, bit-for-bit transfer to RAM, one
 *				 sample represents one pixel
 * @RVIN_MBUS_PACKING_2X8_PADHI: 16 bits transferred in 2 8-bit samples, in the
 *				 possibly incomplete byte high bits are padding
 * @RVIN_MBUS_PACKING_2X8_PADLO: as above, but low bits are padding
 * @RVIN_MBUS_PACKING_EXTEND16:	 sample width (e.g., 10 bits) has to be extended
 *				 to 16 bits
 * @RVIN_MBUS_PACKING_VARIABLE:	 compressed formats with variable packing
 * @RVIN_MBUS_PACKING_1_5X8:	 used for packed YUV 4:2:0 formats, where 4
 *				 pixels occupy 6 bytes in RAM
 * @RVIN_MBUS_PACKING_EXTEND32:	 sample width (e.g., 24 bits) has to be extended
 *				 to 32 bits
 */
enum rvin_mbus_packing {
	RVIN_MBUS_PACKING_NONE,
	RVIN_MBUS_PACKING_2X8_PADHI,
	RVIN_MBUS_PACKING_2X8_PADLO,
	RVIN_MBUS_PACKING_EXTEND16,
	RVIN_MBUS_PACKING_VARIABLE,
	RVIN_MBUS_PACKING_1_5X8,
	RVIN_MBUS_PACKING_EXTEND32,
};

/**
 * struct rvin_video_format - Data format on the media bus
 * @code		Media bus format
 * @name:		Name of the format
 * @fourcc:		Fourcc code, that will be obtained if the data is
 *			stored in memory in the following way:
 * @packing:		Type of sample-packing, that has to be used
 * @bits_per_sample:	How many bits the bridge has to sample
 */
struct rvin_video_format {
	u32			code;
	const char		*name;
	u32			fourcc;
	enum rvin_mbus_packing	packing;
	u8			bits_per_sample;
};

/**
 * struct rvin_sensor - Sensor information
 * @width		Width of camera output
 * @height:		Height of camere output
 * @num_formats:	Number of formats camera support
 * @formats:		Supported format information
 */
struct rvin_sensor {
	u32 width;
	u32 height;

	int num_formats;
	struct rvin_video_format *formats;
};

enum rvin_dma_state {
	STOPPED = 0,
	RUNNING,
	STOPPING,
};

struct rvin_graph_entity {
	struct device_node *node;
	struct media_entity *entity;

	struct v4l2_async_subdev asd;
	struct v4l2_subdev *subdev;
};

/* TODO: split out a 'struct rvin_dma' */
struct rvin_dev {
	struct device *dev;
	void __iomem *base;
	enum chip_id chip;

	struct v4l2_device v4l2_dev;
	struct v4l2_ctrl_handler ctrl_handler;

	struct video_device vdev;
	struct mutex lock;

	struct vb2_queue queue;
	struct vb2_v4l2_buffer *queue_buf[MAX_BUFFER_NUM];
	struct vb2_alloc_ctx *alloc_ctx;

	spinlock_t qlock;
	struct list_head buf_list;
	unsigned sequence;

	/* TODO: needed ? */
	unsigned int pdata_flags;

	struct v4l2_async_notifier notifier;
	struct rvin_graph_entity entity;

	struct v4l2_pix_format format;
	const struct rvin_video_format *fmtinfo;

	struct rvin_sensor sensor;

	enum rvin_dma_state state;
	unsigned int vb_count;
	unsigned int nr_hw_slots;
	bool request_to_stop;
	struct completion capture_stop;
};

#define vin_to_sd(vin)			vin->entity.subdev
#define is_continuous_transfer(vin)	(vin->vb_count > MAX_BUFFER_NUM)

/* Debug */
#define vin_dbg(d, fmt, arg...)		dev_dbg(d->dev, fmt, ##arg)
#define vin_info(d, fmt, arg...)	dev_info(d->dev, fmt, ##arg)
#define vin_warn(d, fmt, arg...)	dev_warn(d->dev, fmt, ##arg)
#define vin_err(d, fmt, arg...)		dev_err(d->dev, fmt, ##arg)

/* Format */
const struct rvin_video_format *rvin_get_format_by_fourcc(
		struct rvin_dev *vin, u32 fourcc);
s32 rvin_bytes_per_line(const struct rvin_video_format *info, u32 width);
s32 rvin_image_size(const struct rvin_video_format *info, u32 bytes_per_line,
		u32 height);

/* Scaling */
int rvin_scale_try(struct rvin_dev *vin, struct v4l2_pix_format *pix, u32 width,
		u32 height);
int rvin_scale_setup(struct rvin_dev *vin);

/* HW */
int rvin_get_active_slot(struct rvin_dev *vin);
void rvin_set_slot_addr(struct rvin_dev *vin, int slot, dma_addr_t addr);
void rvin_disable_interrupts(struct rvin_dev *vin);
void rvin_disable_capture(struct rvin_dev *vin);
u32 rvin_get_interrupt_status(struct rvin_dev *vin);
void rvin_ack_interrupt(struct rvin_dev *vin);
bool rvin_capture_active(struct rvin_dev *vin);
int rvin_setup(struct rvin_dev *vin);
void rvin_capture(struct rvin_dev *vin);
void rvin_request_capture_stop(struct rvin_dev *vin);

/* DMA Core */
int rvin_dma_init(struct rvin_dev *vin, int irq);
void rvin_dma_cleanup(struct rvin_dev *vin);
int rvin_dma_on(struct rvin_dev *vin);

#endif
