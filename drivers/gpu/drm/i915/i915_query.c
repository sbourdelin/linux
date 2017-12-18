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

static int query_slices_mask(struct drm_i915_private *dev_priv,
			     struct drm_i915_query_item *query_item)
{
	const struct sseu_dev_info *sseu = &INTEL_INFO(dev_priv)->sseu;
	struct drm_i915_query_slices_mask slices_info;
	u32 data_length, length;

	if (sseu->max_slices == 0)
		return -ENODEV;

	memset(&slices_info, 0, sizeof(slices_info));

	slices_info.n_slices = sseu->max_slices;

	data_length = sizeof(u8);
	length = sizeof(struct drm_i915_query_slices_mask) + data_length;

	/*
	 * If we ever change the internal slice mask data type, we'll need to
	 * update this function.
	 */
	BUILD_BUG_ON(sizeof(u8) != sizeof(sseu->slice_mask));

	if (query_item->length == 0) {
		query_item->length = length;
		return 0;
	}

	if (query_item->length != length)
		return -EINVAL;

	if (copy_to_user(u64_to_user_ptr(query_item->data_ptr), &slices_info,
			 sizeof(slices_info)))
		return -EFAULT;

	if (copy_to_user(u64_to_user_ptr(query_item->data_ptr +
					 offsetof(struct drm_i915_query_slices_mask, data)),
			 &sseu->slice_mask, data_length))
		return -EFAULT;

	return 0;
}

static int query_subslices_mask(struct drm_i915_private *dev_priv,
				struct drm_i915_query_item *query_item)
{
	const struct sseu_dev_info *sseu = &INTEL_INFO(dev_priv)->sseu;
	struct drm_i915_query_subslices_mask subslices_info;
	u32 data_length, length;

	if (sseu->max_slices == 0)
		return -ENODEV;

	memset(&subslices_info, 0, sizeof(subslices_info));

	subslices_info.n_slices = sseu->max_slices;
	subslices_info.slice_stride = ALIGN(sseu->max_subslices, 8) / 8;

	data_length = subslices_info.n_slices * subslices_info.slice_stride;
	length = sizeof(struct drm_i915_query_subslices_mask) + data_length;

	if (query_item->length == 0) {
		query_item->length = length;
		return 0;
	}

	if (query_item->length != length)
		return -EINVAL;

	if (copy_to_user(u64_to_user_ptr(query_item->data_ptr), &subslices_info,
			 sizeof(subslices_info)))
		return -EFAULT;

	if (copy_to_user(u64_to_user_ptr(query_item->data_ptr +
					 offsetof(struct drm_i915_query_subslices_mask, data)),
			 sseu->subslices_mask, data_length))
		return -EFAULT;

	return 0;
}

static int query_eus_mask(struct drm_i915_private *dev_priv,
			  struct drm_i915_query_item *query_item)
{
	const struct sseu_dev_info *sseu = &INTEL_INFO(dev_priv)->sseu;
	struct drm_i915_query_eus_mask eus_info;
	u32 data_length, length;

	if (sseu->max_slices == 0)
		return -ENODEV;

	memset(&eus_info, 0, sizeof(eus_info));

	eus_info.subslice_stride = ALIGN(sseu->max_eus_per_subslice, 8) / 8;
	eus_info.slice_stride = sseu->max_subslices * eus_info.subslice_stride;
	eus_info.n_slices = sseu->max_slices;

	data_length = eus_info.n_slices * eus_info.slice_stride;
	length = sizeof(struct drm_i915_query_eus_mask) + data_length;

	if (query_item->length == 0) {
		query_item->length = length;
		return 0;
	}

	if (query_item->length != length)
		return -EINVAL;

	if (copy_to_user(u64_to_user_ptr(query_item->data_ptr), &eus_info,
			 sizeof(eus_info)))
		return -EFAULT;

	if (copy_to_user(u64_to_user_ptr(query_item->data_ptr +
					 offsetof(struct drm_i915_query_eus_mask, data)),
			 sseu->eu_mask, data_length))
		return -EFAULT;

	return 0;
}

int i915_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_query *args = data;
	u32 i;

	for (i = 0; i < args->num_items; i++) {
		struct drm_i915_query_item item;
		u64 item_user_ptr = args->items_ptr + sizeof(item) * i;
		int ret;

		if (copy_from_user(&item, u64_to_user_ptr(item_user_ptr),
				   sizeof(item)))
			return -EFAULT;

		switch (item.query_id) {
		case DRM_I915_QUERY_ID_SLICES_MASK:
			ret = query_slices_mask(dev_priv, &item);
			break;
		case DRM_I915_QUERY_ID_SUBSLICES_MASK:
			ret = query_subslices_mask(dev_priv, &item);
			break;
		case DRM_I915_QUERY_ID_EUS_MASK:
			ret = query_eus_mask(dev_priv, &item);
			break;
		default:
			return -EINVAL;
		}

		if (ret)
			return ret;

		if (copy_to_user(u64_to_user_ptr(item_user_ptr), &item,
				 sizeof(item)))
			return -EFAULT;
	}

	return 0;
}
