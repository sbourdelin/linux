/* SPDX-License-Identifier: LGPL-2.1 */
/*
 * Copyright 2018 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 */

#ifndef CODEC_V4L2_FWHT_H
#define CODEC_V4L2_FWHT_H

#include "codec-fwht.h"

enum pixfmt {
	pixfmt_rgb,
	pixfmt_yuv,
	pixfmt_hsv
};

struct v4l2_fwht_pixfmt_info {
	u32 id;
	unsigned int bytesperline_mult;
	unsigned int sizeimage_mult;
	unsigned int sizeimage_div;
	unsigned int luma_alpha_step;
	unsigned int chroma_step;
	/* Chroma plane subsampling */
	unsigned int width_div;
	unsigned int height_div;
	unsigned int components_num;
	unsigned int planes_num;
	enum pixfmt pixfmt_family;
};

struct v4l2_fwht_state {
	const struct v4l2_fwht_pixfmt_info *info;
	unsigned int visible_width;
	unsigned int visible_height;
	unsigned int coded_width;
	unsigned int coded_height;
	unsigned int stride;
	unsigned int gop_size;
	unsigned int gop_cnt;
	u16 i_frame_qp;
	u16 p_frame_qp;

	enum v4l2_colorspace colorspace;
	enum v4l2_ycbcr_encoding ycbcr_enc;
	enum v4l2_xfer_func xfer_func;
	enum v4l2_quantization quantization;

	struct fwht_raw_frame ref_frame;
	u8 *compressed_frame;
};

int pixfmt_mask_to_family(u32 msk);
int pixfmt_family_to_mask(u32 msk);
const struct v4l2_fwht_pixfmt_info *v4l2_fwht_find_pixfmt(u32 pixelformat);
const struct v4l2_fwht_pixfmt_info *v4l2_fwht_get_pixfmt(u32 idx);
const struct v4l2_fwht_pixfmt_info *v4l2_fwht_default_fmt(u32 width_div,
							  u32 height_div,
							  u32 components_num,
							  int pixfmt_family,
							  unsigned int start_idx);

int v4l2_fwht_encode(struct v4l2_fwht_state *state, u8 *p_in, u8 *p_out);
int v4l2_fwht_decode(struct v4l2_fwht_state *state, u8 *p_in, u8 *p_out);

#endif
