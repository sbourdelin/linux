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

#include <linux/ctype.h>
#include <linux/videodev2.h>
#include <media/v4l2-fourcc.h>

static char printable_char(int c)
{
	return isascii(c) && isprint(c) ? c : '?';
}

const char *v4l2_get_format_name(uint32_t format)
{
	static char buf[4];

	snprintf(buf, 4,
		 "%c%c%c%c",
		 printable_char(format & 0xff),
		 printable_char((format >> 8) & 0xff),
		 printable_char((format >> 16) & 0xff),
		 printable_char((format >> 24) & 0x7f));

	return buf;
}
EXPORT_SYMBOL(v4l2_get_format_name);

const struct v4l2_format_info *v4l2_format_info(u32 format)
{
	static const struct v4l2_format_info formats[] = {
		/* RGB formats */
		{ .format = V4L2_PIX_FMT_BGR24,		.num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_RGB24,		.num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_HSV24,		.num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_BGR32,		.num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_XBGR32,	.num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_RGB32,		.num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_XRGB32,	.num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_HSV32,		.num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_ARGB32,	.num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_ABGR32,	.num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_GREY,		.num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },

		/* YUV formats */
		{ .format = V4L2_PIX_FMT_YUYV,		.num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_YVYU,		.num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_UYVY,		.num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_VYUY,		.num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1 },

		{ .format = V4L2_PIX_FMT_NV12,		.num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 2 },
		{ .format = V4L2_PIX_FMT_NV21,		.num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 2 },
		{ .format = V4L2_PIX_FMT_NV16,		.num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_NV61,		.num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_NV24,		.num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_NV42,		.num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 1, .vsub = 1 },

		{ .format = V4L2_PIX_FMT_YUV410,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 4 },
		{ .format = V4L2_PIX_FMT_YVU410,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 4 },
		{ .format = V4L2_PIX_FMT_YUV411P,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 1 },
		{ .format = V4L2_PIX_FMT_YUV420,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 2 },
		{ .format = V4L2_PIX_FMT_YVU420,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 2 },
		{ .format = V4L2_PIX_FMT_YUV422P,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 1 },

		{ .format = V4L2_PIX_FMT_YUV420M,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 2, .multiplanar = 1 },
		{ .format = V4L2_PIX_FMT_YVU420M,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 2, .multiplanar = 1 },
		{ .format = V4L2_PIX_FMT_YUV422M,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 1, .multiplanar = 1 },
		{ .format = V4L2_PIX_FMT_YVU422M,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 1, .multiplanar = 1 },
		{ .format = V4L2_PIX_FMT_YUV444M,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 1, .vsub = 1, .multiplanar = 1 },
		{ .format = V4L2_PIX_FMT_YVU444M,	.num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 1, .vsub = 1, .multiplanar = 1 },

		{ .format = V4L2_PIX_FMT_NV12M,		.num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 2, .multiplanar = 1 },
		{ .format = V4L2_PIX_FMT_NV21M,		.num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 2, .multiplanar = 1 },
		{ .format = V4L2_PIX_FMT_NV16M,		.num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 1, .multiplanar = 1 },
		{ .format = V4L2_PIX_FMT_NV61M,		.num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 1, .multiplanar = 1 },

	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); ++i) {
		if (formats[i].format == format)
			return &formats[i];
	}

	pr_warn("Unsupported V4L 4CC format %s (%08x)\n", v4l2_get_format_name(format), format);
	return NULL;
}
EXPORT_SYMBOL(v4l2_format_info);
