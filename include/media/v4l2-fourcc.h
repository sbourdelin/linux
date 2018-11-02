/*
 * Copyright (c) 2018 Collabora, Ltd.
 *
 * Based on drm-fourcc:
 * Copyright (c) 2016 Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */
#ifndef __V4L2_FOURCC_H__
#define __V4L2_FOURCC_H__

#include <linux/types.h>

/**
 * struct v4l2_format_info - information about a V4L2 format
 * @format: 4CC format identifier (V4L2_PIX_FMT_*)
 * @header_size: Size of header, optional and used by compressed formats
 * @num_planes: Number of planes (1 to 3)
 * @cpp: Number of bytes per pixel (per plane)
 * @hsub: Horizontal chroma subsampling factor
 * @vsub: Vertical chroma subsampling factor
 * @is_compressed: Is it a compressed format?
 */
struct v4l2_format_info {
	u32 format;
	u32 header_size;
	u8 num_planes;
	u8 cpp[3];
	u8 hsub;
	u8 vsub;
	bool is_compressed;
	bool has_contiguous_planes;
};

const struct v4l2_format_info *v4l2_format_info(u32 format);

#endif
