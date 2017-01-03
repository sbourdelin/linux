/*
 * V4L2 Media Controller Driver for Freescale i.MX5/6 SOC
 *
 * Copyright (c) 2016 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _IMX_MEDIA_H
#define _IMX_MEDIA_H
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-of.h>
#include <media/videobuf2-dma-contig.h>
#include <video/imx-ipu-v3.h>

/*
 * This is somewhat arbitrary, but we need at least:
 * - 2 camera interface subdevs
 * - 3 IC subdevs
 * - 2 CSI subdevs
 * - 1 mipi-csi2 receiver subdev
 * - 2 video-mux subdevs
 * - 3 camera sensor subdevs (2 parallel, 1 mipi-csi2)
 *
 * And double the above numbers for quad i.mx!
 */
#define IMX_MEDIA_MAX_SUBDEVS       48
/* max pads per subdev */
#define IMX_MEDIA_MAX_PADS          16
/* max links per pad */
#define IMX_MEDIA_MAX_LINKS          8

/* How long to wait for EOF interrupts in the buffer-capture subdevs */
#define IMX_MEDIA_EOF_TIMEOUT       1000

/* A sensor's inputs parsed from a sensor node */
#define IMX_MEDIA_MAX_SENSOR_INPUTS 16
struct imx_media_sensor_input {
	/* number of inputs */
	int num;
	/* input values passed to s_routing */
	u32 value[IMX_MEDIA_MAX_SENSOR_INPUTS];
	/* input names */
	char name[IMX_MEDIA_MAX_SENSOR_INPUTS][32];
};

struct imx_media_pixfmt {
	u32     fourcc;
	u32     codes[4];
	int     bpp;     /* total bpp */
	enum ipu_color_space cs;
	bool    planar;  /* is a planar format */
};

struct imx_media_buffer {
	struct vb2_v4l2_buffer vbuf; /* v4l buffer must be first */
	struct list_head  list;
};

static inline struct imx_media_buffer *to_imx_media_vb(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	return container_of(vbuf, struct imx_media_buffer, vbuf);
}

struct imx_media_link {
	struct device_node *remote_sd_node;
	char               remote_devname[32];
	int                local_pad;
	int                remote_pad;
};

struct imx_media_pad {
	struct media_pad  pad;
	struct imx_media_link link[IMX_MEDIA_MAX_LINKS];
	bool devnode; /* does this pad link to a device node */
	int num_links;
};

struct imx_media_internal_sd_platformdata {
	char sd_name[V4L2_SUBDEV_NAME_SIZE];
	u32 grp_id;
	int ipu_id;
};

struct imx_media_subdev {
	struct v4l2_async_subdev asd;
	struct v4l2_subdev       *sd; /* set when bound */

	struct imx_media_pad     pad[IMX_MEDIA_MAX_PADS];
	int num_sink_pads;
	int num_src_pads;

	/* the devname is needed for async devname match */
	char devname[32];

	/* if this is a sensor */
	struct imx_media_sensor_input input;
	struct v4l2_of_endpoint sensor_ep;
};

struct imx_media_dev {
	struct media_device md;
	struct v4l2_device  v4l2_dev;
	struct device *dev;

	/* master subdev list */
	struct imx_media_subdev subdev[IMX_MEDIA_MAX_SUBDEVS];
	int num_subdevs;

	/* IPUs this media driver control, valid after subdevs bound */
	struct ipu_soc *ipu[2];

	/* used during link_notify */
	struct media_entity_graph link_notify_graph;

	/* for async subdev registration */
	struct v4l2_async_subdev *async_ptrs[IMX_MEDIA_MAX_SUBDEVS];
	struct v4l2_async_notifier subdev_notifier;
};

const struct imx_media_pixfmt *imx_media_find_format(u32 fourcc, u32 code,
						     bool allow_rgb,
						     bool allow_planar);
int imx_media_enum_format(u32 *code, u32 index,
			  bool allow_rgb, bool allow_planar);

int imx_media_init_mbus_fmt(struct v4l2_mbus_framefmt *mbus,
			    u32 width, u32 height, u32 code, u32 field,
			    const struct imx_media_pixfmt **cc);

int imx_media_mbus_fmt_to_pix_fmt(struct v4l2_pix_format *fmt,
				  struct v4l2_mbus_framefmt *mbus);
int imx_media_mbus_fmt_to_ipu_image(struct ipu_image *image,
				    struct v4l2_mbus_framefmt *mbus);
int imx_media_ipu_image_to_mbus_fmt(struct v4l2_mbus_framefmt *mbus,
				    struct ipu_image *image);

struct imx_media_subdev *
imx_media_find_async_subdev(struct imx_media_dev *imxmd,
			    struct device_node *np,
			    const char *devname);
struct imx_media_subdev *
imx_media_add_async_subdev(struct imx_media_dev *imxmd,
			   struct device_node *np,
			   const char *devname);
int imx_media_add_pad_link(struct imx_media_dev *imxmd,
			   struct imx_media_pad *pad,
			   struct device_node *remote_node,
			   const char *remote_devname,
			   int local_pad, int remote_pad);

void imx_media_grp_id_to_sd_name(char *sd_name, int sz,
				 u32 grp_id, int ipu_id);

int imx_media_add_internal_subdevs(struct imx_media_dev *imxmd,
				   struct imx_media_subdev *csi[4]);

struct imx_media_subdev *
imx_media_find_subdev_by_sd(struct imx_media_dev *imxmd,
			    struct v4l2_subdev *sd);
struct imx_media_subdev *
imx_media_find_subdev_by_id(struct imx_media_dev *imxmd,
			    u32 grp_id);
int imx_media_find_mipi_csi2_channel(struct imx_media_dev *imxmd,
				     struct media_entity *start_entity);
struct imx_media_subdev *
imx_media_find_pipeline_subdev(struct imx_media_dev *imxmd,
			       struct media_entity *start_entity,
			       u32 grp_id);
struct imx_media_subdev *
__imx_media_find_sensor(struct imx_media_dev *imxmd,
			struct media_entity *start_entity);
struct imx_media_subdev *
imx_media_find_sensor(struct imx_media_dev *imxmd,
		      struct media_entity *start_entity);

enum imx_media_dma_buf_status {
	IMX_MEDIA_BUF_STATUS_PREPARED = 0,
	IMX_MEDIA_BUF_STATUS_QUEUED,
	IMX_MEDIA_BUF_STATUS_ACTIVE,
	IMX_MEDIA_BUF_STATUS_DONE,
	IMX_MEDIA_BUF_STATUS_ERROR,
};

struct imx_media_dma_buf_ring;

struct imx_media_dma_buf {
	/* owning ring if any */
	struct imx_media_dma_buf_ring *ring;
	/* if !NULL this is a vb2_buffer */
	struct vb2_buffer *vb;
	void          *virt;
	dma_addr_t     phys;
	unsigned long  len;
	int            index;
	unsigned long  seq;
	/* buffer state */
	enum imx_media_dma_buf_status state;
	/* completion status */
	enum imx_media_dma_buf_status status;
};

enum imx_media_priv_ioctl {
	IMX_MEDIA_REQ_DMA_BUF_SINK_RING = 1, /* src requests ring from sink */
	IMX_MEDIA_REQ_DMA_BUF_SRC_RING,  /* sink requests ring from src */
	IMX_MEDIA_NEW_DMA_BUF,           /* src hands new buffer to sink */
	IMX_MEDIA_REL_DMA_BUF_SINK_RING, /* src informs sink that its ring
					    can be released */
	IMX_MEDIA_REL_DMA_BUF_SRC_RING,  /* sink informs src that its ring
					    can be released */
};

#define IMX_MEDIA_MIN_RING_BUFS 2
/* prpvf needs at least 3 buffers */
#define IMX_MEDIA_MIN_RING_BUFS_PRPVF 3
#define IMX_MEDIA_MAX_RING_BUFS 8
void imx_media_free_dma_buf(struct imx_media_dev *imxmd,
			    struct imx_media_dma_buf *buf);
int imx_media_alloc_dma_buf(struct imx_media_dev *imxmd,
			    struct imx_media_dma_buf *buf,
			    int size);
void imx_media_free_dma_buf_ring(struct imx_media_dma_buf_ring *ring);
struct imx_media_dma_buf_ring *
imx_media_alloc_dma_buf_ring(struct imx_media_dev *imxmd,
			     struct media_entity *src,
			     struct media_entity *sink,
			     int size, int num_bufs,
			     bool alloc_bufs);
int imx_media_dma_buf_queue(struct imx_media_dma_buf_ring *ring, int index);
int imx_media_dma_buf_queue_from_vb(struct imx_media_dma_buf_ring *ring,
				    struct vb2_buffer *vb);
void imx_media_dma_buf_done(struct imx_media_dma_buf *buf,
			    enum imx_media_dma_buf_status status);
struct imx_media_dma_buf *
imx_media_dma_buf_dequeue(struct imx_media_dma_buf_ring *ring);
struct imx_media_dma_buf *
imx_media_dma_buf_get_active(struct imx_media_dma_buf_ring *ring);
int imx_media_dma_buf_set_active(struct imx_media_dma_buf *buf);
struct imx_media_dma_buf *
imx_media_dma_buf_get_next_queued(struct imx_media_dma_buf_ring *ring);
struct imx_media_dma_buf *
imx_media_dma_buf_get(struct imx_media_dma_buf_ring *ring, int index);

int imx_media_pipeline_set_power(struct imx_media_dev *imxmd,
				 struct media_entity_graph *graph,
				 struct media_entity *entity, bool on);
int imx_media_pipeline_set_stream(struct imx_media_dev *imxmd,
				  struct media_entity *entity,
				  struct media_pipeline *pipe,
				  bool on);
int imx_media_inherit_controls(struct imx_media_dev *imxmd,
			       struct video_device *vfd,
			       struct media_entity *start_entity);

/* imx-media-fim.c */
struct imx_media_fim;
void imx_media_fim_eof_monitor(struct imx_media_fim *fim, struct timespec *ts);
int imx_media_fim_set_power(struct imx_media_fim *fim, bool on);
int imx_media_fim_set_stream(struct imx_media_fim *fim,
			     struct imx_media_subdev *sensor,
			     bool on);
struct imx_media_fim *imx_media_fim_init(struct v4l2_subdev *sd);
void imx_media_fim_free(struct imx_media_fim *fim);

#define IMX_MEDIA_GRP_ID_SENSOR    (1 << 8)
#define IMX_MEDIA_GRP_ID_VIDMUX    (1 << 9)
#define IMX_MEDIA_GRP_ID_CSI2      (1 << 10)
#define IMX_MEDIA_GRP_ID_CSI_BIT   11
#define IMX_MEDIA_GRP_ID_CSI       (0x3 << 11)
#define IMX_MEDIA_GRP_ID_CSI0      (1 << 11)
#define IMX_MEDIA_GRP_ID_CSI1      (2 << 11)
#define IMX_MEDIA_GRP_ID_SMFC_BIT  13
#define IMX_MEDIA_GRP_ID_SMFC      (0x7 << 13)
#define IMX_MEDIA_GRP_ID_SMFC0     (1 << 13)
#define IMX_MEDIA_GRP_ID_SMFC1     (2 << 13)
#define IMX_MEDIA_GRP_ID_SMFC2     (3 << 13)
#define IMX_MEDIA_GRP_ID_SMFC3     (4 << 13)
#define IMX_MEDIA_GRP_ID_IC_PRPENC (1 << 16)
#define IMX_MEDIA_GRP_ID_IC_PRPVF  (1 << 17)
#define IMX_MEDIA_GRP_ID_IC_PP_BIT 18
#define IMX_MEDIA_GRP_ID_IC_PP     (0x7 << 18)
#define IMX_MEDIA_GRP_ID_IC_PP0    (1 << 18)
#define IMX_MEDIA_GRP_ID_IC_PP1    (2 << 18)
#define IMX_MEDIA_GRP_ID_IC_PP2    (3 << 18)
#define IMX_MEDIA_GRP_ID_IC_PP3    (4 << 18)
#define IMX_MEDIA_GRP_ID_CAMIF_BIT 21
#define IMX_MEDIA_GRP_ID_CAMIF     (0x7 << 21)
#define IMX_MEDIA_GRP_ID_CAMIF0    (1 << 21)
#define IMX_MEDIA_GRP_ID_CAMIF1    (2 << 21)
#define IMX_MEDIA_GRP_ID_CAMIF2    (3 << 21)
#define IMX_MEDIA_GRP_ID_CAMIF3    (4 << 21)

#endif
