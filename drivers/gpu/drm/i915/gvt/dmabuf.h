/*
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef _GVT_DMABUF_H_
#define _GVT_DMABUF_H_

#define INTEL_VGPU_QUERY_DMABUF		0
#define INTEL_VGPU_GENERATE_DMABUF	1

struct intel_vgpu_dmabuf {
	__u32 plane_id;
	/* out */
	__u32 fd;
	__u32 drm_format;
	__u32 width;
	__u32 height;
	__u32 stride;
	__u32 start;
	__u32 x_pos;
	__u32 y_pos;
	__u32 size;
	__u32 tiled;
};

int intel_vgpu_query_dmabuf(struct intel_vgpu *vgpu, void *args);
int intel_vgpu_generate_dmabuf(struct intel_vgpu *vgpu,	void *args);

#endif

