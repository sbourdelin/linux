
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

struct intel_vgpu_plane_info {
	uint32_t plane_id;
	uint32_t drm_format;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t start;
	uint32_t x_pos;
	uint32_t y_pos;
	uint32_t size;
	uint64_t drm_format_mod;
};

#define INTEL_VGPU_QUERY_PLANE		0
#define INTEL_VGPU_GENERATE_DMABUF	1

struct intel_vgpu_dmabuf {
	uint32_t fd;
	struct intel_vgpu_plane_info plane_info;
};

int intel_vgpu_query_plane(struct intel_vgpu *vgpu, void *args);
int intel_vgpu_create_dmabuf(struct intel_vgpu *vgpu, void *args);

#endif
