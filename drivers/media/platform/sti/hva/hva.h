/*
 * Copyright (C) STMicroelectronics SA 2015
 * Authors: Yannick Fertre <yannick.fertre@st.com>
 *          Hugues Fruchet <hugues.fruchet@st.com>
 * License terms:  GNU General Public License (GPL), version 2
 */

#ifndef HVA_H
#define HVA_H

#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>

#define get_queue(c, t) (t == V4L2_BUF_TYPE_VIDEO_OUTPUT ? \
			 &c->q_frame : &c->q_stream)

#define to_type_str(type) (type == V4L2_BUF_TYPE_VIDEO_OUTPUT ? \
			   "frame" : "stream")

#define fh_to_ctx(f)    (container_of(f, struct hva_ctx, fh))

#define hva_to_dev(h)   (h->dev)

#define ctx_to_dev(c)   (c->dev)

#define ctx_to_hdev(c)  (c->hdev)

#define ctx_to_enc(c)   (c->encoder)

#define HVA_PREFIX "[---:----]"

#define MAX_CONTEXT 16

extern const struct hva_encoder nv12h264enc;
extern const struct hva_encoder nv21h264enc;
extern const struct hva_encoder uyvyh264enc;
extern const struct hva_encoder vyuyh264enc;
extern const struct hva_encoder xrgb32h264enc;
extern const struct hva_encoder xbgr32h264enc;
extern const struct hva_encoder rgbx32h264enc;
extern const struct hva_encoder bgrx32h264enc;
extern const struct hva_encoder rgb24h264enc;
extern const struct hva_encoder bgr24h264enc;

/**
 * struct hva_frame_fmt - driver's internal color format data
 * @pixelformat:fourcc code for this format
 * @nb_planes:  number of planes  (ex: [0]=RGB/Y - [1]=Cb/Cr, ...)
 * @bpp:        bits per pixel (general)
 * @bpp_plane0: byte per pixel for the 1st plane
 * @w_align:    width alignment in pixel (multiple of)
 * @h_align:    height alignment in pixel (multiple of)
 */
struct hva_frame_fmt {
	u32                     pixelformat;
	u8                      nb_planes;
	u8                      bpp;
	u8                      bpp_plane0;
	u8                      w_align;
	u8                      h_align;
};

/**
 * struct hva_frameinfo - information of frame
 *
 * @flags		flags of input frame
 * @fmt:		format of input frame
 * @width:		width of input frame
 * @height:		height of input frame
 * @crop:		cropping window due to encoder alignment constraints
 *			(1920x1080@0,0 inside 1920x1088 encoded frame for ex.)
 * @pixelaspect:	pixel aspect ratio of video (4/3, 5/4)
 * @frame_width:	width of output frame (encoder alignment constraint)
 * @frame_height:	height ""
*/
struct hva_frameinfo {
	u32 flags;
	struct hva_frame_fmt fmt;
	u32 width;
	u32 height;
	struct v4l2_rect crop;
	struct v4l2_fract pixelaspect;
	u32 frame_width;
	u32 frame_height;
};

/**
 * struct hva_streaminfo - information of stream
 *
 * @flags		flags of video stream
 * @width:		width of video stream
 * @height:		height ""
 * @streamformat:	fourcc compressed format of video (H264, JPEG, ...)
 * @dpb:		number of frames needed to encode a single frame
 *			(h264 dpb, up to 16 in standard)
 * @profile:		profile string
 * @level:		level string
 * @other:		other string information from codec
 */
struct hva_streaminfo {
	u32 flags;
	u32 streamformat;
	u32 width;
	u32 height;
	u32 dpb;
	u8 profile[32];
	u8 level[32];
	u8 other[32];
};

#define HVA_FRAMEINFO_FLAG_CROP		0x0001
#define HVA_FRAMEINFO_FLAG_PIXELASPECT	0x0002

#define HVA_STREAMINFO_FLAG_OTHER	0x0001
#define HVA_STREAMINFO_FLAG_JPEG	0x0002
#define HVA_STREAMINFO_FLAG_H264	0x0004
#define HVA_STREAMINFO_FLAG_VP8	0x0008

/**
 * struct hva_controls
 *
 * @level: level enumerate
 * @profile: video profile
 * @entropy_mode: entropy mode (CABAC or CVLC)
 * @bitrate_mode: bitrate mode (constant bitrate or variable bitrate)
 * @gop_size: groupe of picture size
 * @bitrate: bitrate
 * @cpb_size: coded picture buffer size
 * @intra_refresh: activate intra refresh
 * @dct8x8: enable transform mode 8x8
 * @qpmin: defines the minimum quantizer
 * @qpmax: defines the maximum quantizer
 * @jpeg_comp_quality: jpeg compression quality
 * @vui_sar: pixel aspect ratio enable
 * @vui_sar_idc: pixel aspect ratio identifier
 * @sei_fp: sei frame packing arrangement enable
 * @sei_fp_type: sei frame packing arrangement type
 */
struct hva_controls {
	enum v4l2_mpeg_video_h264_level level;
	enum v4l2_mpeg_video_h264_profile profile;
	enum v4l2_mpeg_video_h264_entropy_mode entropy_mode;
	enum v4l2_mpeg_video_bitrate_mode bitrate_mode;
	u32 gop_size;
	u32 bitrate;
	u32 cpb_size;
	bool intra_refresh;
	bool dct8x8;
	u32 qpmin;
	u32 qpmax;
	u32 jpeg_comp_quality;
	bool vui_sar;
	enum v4l2_mpeg_video_h264_vui_sar_idc vui_sar_idc;
	bool sei_fp;
	enum v4l2_mpeg_video_h264_sei_fp_arrangement_type sei_fp_type;
};

/**
 * struct hva_frame - structure.
 *
 * @v4l2:	video buffer information for v4l2.
 *		To be kept first and not to be wrote by driver.
 *		Allows to get the hva_frame fields by just casting a vb2_buffer
 *		with hva_frame struct. This is allowed through the use of
 *		vb2 custom buffer mechanism, cf @buf_struct_size of
 *		struct vb2_queue in include/media/videobuf2-core.h
 * @paddr:	physical address (for hardware)
 * @vaddr:	virtual address (kernel can read/write)
 * @prepared:	boolean, if set vaddr/paddr are resolved
 */
struct hva_frame {
	struct vb2_v4l2_buffer	v4l2; /* !keep first! */
	dma_addr_t		paddr;
	void			*vaddr;
	int			prepared;
};

/**
 * struct hva_stream - structure.
 *
 * @v4l2:	video buffer information for v4l2.
 *		To be kept first and not to be wrote by driver.
 *		Allows to get the hva_stream fields by just casting a vb2_buffer
 *		with hva_stream struct. This is allowed through the use of
 *		vb2 custom buffer mechanism, cf @buf_struct_size of
 *		struct vb2_queue in include/media/videobuf2-core.h
 * @list:	list element
 * @paddr:	physical address (for hardware)
 * @vaddr:	virtual address (kernel can read/write)
 * @prepared:	boolean, if set vaddr/paddr are resolved
 * @payload:	number of bytes occupied by data in the buffer
 */
struct hva_stream {
	struct vb2_v4l2_buffer	v4l2; /* !keep first! */
	struct list_head	list;
	dma_addr_t		paddr;
	void			*vaddr;
	int			prepared;
	unsigned int		payload;
};

/**
 * struct hva_buffer - structure.
 *
 * @name:	name of requester
 * @attrs:	dma attributes
 * @paddr:	physical address (for hardware)
 * @vaddr:	virtual address (kernel can read/write)
 * @size:	size of buffer
 */
struct hva_buffer {
	const char		*name;
	struct dma_attrs	attrs;
	dma_addr_t		paddr;
	void			*vaddr;
	u32			size;
};

struct hva_device;
struct hva_encoder;

#define HVA_MAX_ENCODERS 30

#define HVA_FLAG_STREAMINFO 0x0001
#define HVA_FLAG_FRAMEINFO 0x0002

/**
 * struct hva_ctx - context structure.
 *
 * @flags:		validity of fields (streaminfo,frameinfo)
 * @fh:			keep track of V4L2 file handle
 * @dev:		keep track of device context
 * @client_id:		Client Identifier
 * @q_frame:		V4L2 vb2 queue for access units, allocated by driver
 *			but managed by vb2 framework.
 * @q_stream:		V4L2 vb2 queue for frames, allocated by driver
 *			but managed by vb2 framework.
 * @name:		string naming this instance (debug purpose)
 * @list_stream:	list of stream queued for destination only
 * @frame_num		frame number
 * @frames:		set of src frames (input, reconstructed & reference)
 * @priv:		private codec context for this instance, allocated
 *			by encoder @open time.
 * @sys_errors:		number of system errors ( memory, resource, pm, ..)
 * @encode_errors:	number of encoding errors ( hw/driver errors)
 * @frames_errors:	number of frames errors ( format, size, header ..)
 * @hw_err:		hardware error detected
 */
struct hva_ctx {
	u32 flags;

	struct v4l2_fh fh;
	struct hva_device *hdev;
	struct device *dev;

	u8 client_id;

	/* vb2 queues */
	struct vb2_queue q_frame;
	struct vb2_queue q_stream;

	char name[100];

	struct list_head list_stream;

	u32 frame_num;

	struct hva_controls ctrls;
	struct v4l2_fract time_per_frame;
	u32 num_frames;

	/* stream */
	struct hva_streaminfo streaminfo;

	/* frame */
	struct hva_frameinfo frameinfo;

	/* current encoder */
	struct hva_encoder *encoder;

	/* stats */
	u32 encoded_frames;

	/* private data */
	void *priv;

	/* errors */
	u32 sys_errors;
	u32 encode_errors;
	u32 frame_skipped;
	u32 frame_errors;
	bool hw_err;

	/* hardware task descriptor*/
	struct hva_buffer *task;
};

/**
 * struct hva_device - device struct, 1 per probe (so single one for
 * all platform life)
 *
 * @v4l2_dev:		v4l2 device
 * @vdev:		v4l2 video device
 * @pdev:		platform device
 * @dev:		device
 * @lock:		device lock for critical section &
 *			V4L2 ops serialization
 * @instance_id:	instance identifier
 * @contexts_list:	contexts list
 * @regs:		register io memory access
 * @reg_size:		register size
 * @irq_its:		its interruption
 * @irq_err:		error interruption
 * @chip_id:		chipset identifier
 * @protect_mutex:	mutex use to lock access of hardware
 * @interrupt:		completion interrupt
 * @clk:		hva clock
 * @esram_addr:		esram address
 * @esram_size:		esram size
 * @sfl_reg:		Status fifo level register value
 * @sts_reg:		Status register value
 * @lmi_err_reg:	Local memory interface Error register value
 * @emi_err_reg:	External memory interface Error register value
 * @hec_mif_err_reg:HEC memory interface Error register value
 * @encoders:		list of all encoders registered
 * @nb_of_encoders:	number of encoders registered
 * @nb_of_instances:	number of instance
 */
struct hva_device {
	/* device */
	struct v4l2_device v4l2_dev;
	struct video_device *vdev;
	struct platform_device *pdev;
	struct device *dev;
	struct mutex lock; /* device lock for critical section & V4L2 ops */
	int instance_id;
	struct hva_ctx *contexts_list[MAX_CONTEXT];

	/* hardware */
	void __iomem *regs;
	int regs_size;
	int irq_its;
	int irq_err;
	unsigned long int chip_id;
	struct mutex protect_mutex; /* mutex use to lock access of hardware */
	struct completion interrupt;
	struct clk *clk;
	u32 esram_addr;
	u32 esram_size;

	/* registers */
	u32 sfl_reg;
	u32 sts_reg;
	u32 lmi_err_reg;
	u32 emi_err_reg;
	u32 hec_mif_err_reg;

	/* encoders */
	const struct hva_encoder *encoders[HVA_MAX_ENCODERS];
	u32 nb_of_encoders;
	u32 nb_of_instances;
};

struct hva_encoder {
	struct list_head list;
	const char *name;
	u32 streamformat;
	u32 pixelformat;
	u32 max_width;
	u32 max_height;

	/**
	 * Encoder ops
	 */
	int (*open)(struct hva_ctx *ctx);
	int (*close)(struct hva_ctx *ctx);

	/**
	 * encode() - encode a single access unit
	 * @ctx:	(in) instance
	 * @frame:		(in/out) access unit
	 *  @frame.size	(in) size of frame to encode
	 *  @frame.vaddr	(in) virtual address (kernel can read/write)
	 *  @frame.paddr	(in) physical address (for hardware)
	 *  @frame.flags	(out) frame type (V4L2_BUF_FLAG_KEYFRAME/
	 *			PFRAME/BFRAME)
	 * @stream:	(out) frame with encoded data:
	 *  @stream.index	(out) identifier of frame
	 *  @stream.vaddr	(out) virtual address (kernel can read/write)
	 *  @stream.paddr	(out) physical address (for hardware)
	 *  @stream.pix		(out) width/height/format/stride/...
	 *  @stream.flags	(out) stream type (V4L2_BUF_FLAG_KEYFRAME/
	 *			PFRAME/BFRAME)
	 *
	 * Encode the access unit given. Encode is synchronous;
	 * access unit memory is no more needed after this call.
	 * After this call, none, one or several frames could
	 * have been encoded, which can be retrieved using
	 * get_stream().
	*/
	int (*encode)(struct hva_ctx *ctx, struct hva_frame *frame,
		      struct hva_stream *stream);
};

static inline const char *profile_str(unsigned int p)
{
	switch (p) {
	case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
		return "baseline profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
		return "main profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED:
		return "extended profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
		return "high profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10:
		return "high 10 profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422:
		return "high 422 profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE:
		return "high 444 predictive profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10_INTRA:
		return "high 10 intra profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422_INTRA:
		return "high 422 intra profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_INTRA:
		return "high 444 intra profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_CAVLC_444_INTRA:
		return "calvc 444 intra profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_BASELINE:
		return "scalable baseline profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_HIGH:
		return "scalable high profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_SCALABLE_HIGH_INTRA:
		return "scalable high intra profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_STEREO_HIGH:
		return "stereo high profile";
	case V4L2_MPEG_VIDEO_H264_PROFILE_MULTIVIEW_HIGH:
		return "multiview high profile";
	default:
		return "unknown profile";
	}
}

static inline const char *level_str(enum v4l2_mpeg_video_h264_level l)
{
	switch (l) {
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_0:
		return "level 1.0";
	case V4L2_MPEG_VIDEO_H264_LEVEL_1B:
		return "level 1b";
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_1:
		return "level 1.1";
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_2:
		return "level 1.2";
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_3:
		return "level 1.3";
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_0:
		return "level 2.0";
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_1:
		return "level 2.1";
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_2:
		return "level 2.2";
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:
		return "level 3.0";
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
		return "level 3.1";
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:
		return "level 3.2";
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
		return "level 4.0";
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_1:
		return "level 4.1";
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:
		return "level 4.2";
	case V4L2_MPEG_VIDEO_H264_LEVEL_5_0:
		return "level 5.0";
	case V4L2_MPEG_VIDEO_H264_LEVEL_5_1:
		return "level 5.1";
	default:
		return "unknown level";
	}
}

static inline const char *bitrate_mode_str(enum v4l2_mpeg_video_bitrate_mode m)
{
	switch (m) {
	case V4L2_MPEG_VIDEO_BITRATE_MODE_VBR:
		return "variable bitrate";
	case V4L2_MPEG_VIDEO_BITRATE_MODE_CBR:
		return "constant bitrate";
	default:
		return "unknown bitrate mode";
	}
}

#endif /* HVA_H */
