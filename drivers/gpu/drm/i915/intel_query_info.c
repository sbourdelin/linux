/*
 * Copyright © 2017 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "i915_drv.h"
#include <uapi/drm/i915_drm.h>

static int query_info_rcs_topology(struct drm_i915_private *dev_priv,
				   struct drm_i915_query_info *args)
{
	const struct sseu_dev_info *sseu = &INTEL_INFO(dev_priv)->sseu;
	struct drm_i915_rcs_topology_info __user *user_topology =
		u64_to_user_ptr(args->info_ptr);
	struct drm_i915_rcs_topology_info topology;
	u32 data_size, total_size;
	const u8 *data = NULL;
	int ret;

	/* Not supported on gen < 8. */
	if (sseu->max_slices == 0)
		return -ENODEV;

	switch (args->query_params[0]) {
	case I915_RCS_TOPOLOGY_SLICE:
		topology.params[0] = sseu->max_slices;
		data_size = sizeof(sseu->slice_mask);
		data = &sseu->slice_mask;
		break;

	case I915_RCS_TOPOLOGY_SUBSLICE:
		topology.params[0] = sseu->max_slices;
		topology.params[1] = ALIGN(sseu->max_subslices, 8) / 8;
		data_size = sseu->max_slices * topology.params[1];
		data = sseu->subslices_mask;
		break;

	case I915_RCS_TOPOLOGY_EU:
		topology.params[2] = ALIGN(sseu->max_eus_per_subslice, 8) / 8;
		topology.params[1] = sseu->max_subslices * topology.params[2];
		topology.params[0] = sseu->max_slices;
		data_size = sseu->max_slices * topology.params[1];
		data = sseu->eu_mask;
		break;

	default:
		return -EINVAL;
	}

	total_size = sizeof(topology) + data_size;

	if (args->info_ptr_len == 0) {
		args->info_ptr_len = total_size;
		return 0;
	}

	if (args->info_ptr_len < total_size)
		return -EINVAL;

	ret = copy_to_user(user_topology, &topology, sizeof(topology));
	if (ret)
		return -EFAULT;

	ret = copy_to_user(user_topology + 1, data, data_size);
	if (ret)
		return -EFAULT;

	return 0;
}
