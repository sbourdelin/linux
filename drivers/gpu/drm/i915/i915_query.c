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

static int query_slices_info(struct drm_i915_private *dev_priv,
			     struct drm_i915_query_item *query_item)
{
	const struct sseu_dev_info *sseu = &INTEL_INFO(dev_priv)->sseu;
	struct drm_i915_query_slices_info slices_info;
	u32 data_length, length;

	if (sseu->max_slices == 0)
		return -ENODEV;

	data_length = sizeof(u8);
	length = sizeof(slices_info) + data_length;

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

	memset(&slices_info, 0, sizeof(slices_info));
	slices_info.max_slices = sseu->max_slices;

	if (copy_to_user(u64_to_user_ptr(query_item->data_ptr), &slices_info,
			 sizeof(slices_info)))
		return -EFAULT;

	if (copy_to_user(u64_to_user_ptr(query_item->data_ptr +
					 offsetof(struct drm_i915_query_slices_info, data)),
			 &sseu->slice_mask, data_length))
		return -EFAULT;

	return 0;
}

static int query_subslices_info(struct drm_i915_private *dev_priv,
				struct drm_i915_query_item *query_item)
{
	const struct sseu_dev_info *sseu = &INTEL_INFO(dev_priv)->sseu;
	struct drm_i915_query_subslices_info subslices_info;
	u32 data_length, length;

	if (sseu->max_slices == 0)
		return -ENODEV;

	memset(&subslices_info, 0, sizeof(subslices_info));
	subslices_info.max_slices = sseu->max_slices;
	subslices_info.max_subslices = sseu->max_subslices;

	data_length = subslices_info.max_slices *
		DIV_ROUND_UP(subslices_info.max_subslices, BITS_PER_BYTE);
	length = sizeof(subslices_info) + data_length;

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
					 offsetof(struct drm_i915_query_subslices_info, data)),
			 sseu->subslice_mask, data_length))
		return -EFAULT;

	return 0;
}

static int query_eus_info(struct drm_i915_private *dev_priv,
			  struct drm_i915_query_item *query_item)
{
	const struct sseu_dev_info *sseu = &INTEL_INFO(dev_priv)->sseu;
	struct drm_i915_query_eus_info eus_info;
	u32 data_length, length;

	if (sseu->max_slices == 0)
		return -ENODEV;

	memset(&eus_info, 0, sizeof(eus_info));
	eus_info.max_slices = sseu->max_slices;
	eus_info.max_subslices = sseu->max_subslices;
	eus_info.max_eus_per_subslice = sseu->max_eus_per_subslice;

	data_length = eus_info.max_slices * eus_info.max_subslices *
		DIV_ROUND_UP(eus_info.max_eus_per_subslice, BITS_PER_BYTE);
	length = sizeof(eus_info) + data_length;

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
					 offsetof(struct drm_i915_query_eus_info, data)),
			 sseu->eu_mask, data_length))
		return -EFAULT;

	return 0;
}

int i915_query_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_query *args = data;
	struct drm_i915_query_item __user *user_item_ptr =
		u64_to_user_ptr(args->items_ptr);
	u32 i;

	for (i = 0; i < args->num_items; i++, user_item_ptr++) {
		struct drm_i915_query_item item;
		int ret;

		if (copy_from_user(&item, user_item_ptr, sizeof(item)))
			return -EFAULT;

		switch (item.query_id) {
		case DRM_I915_QUERY_ID_SLICES_INFO:
			ret = query_slices_info(dev_priv, &item);
			break;
		case DRM_I915_QUERY_ID_SUBSLICES_INFO:
			ret = query_subslices_info(dev_priv, &item);
			break;
		case DRM_I915_QUERY_ID_EUS_INFO:
			ret = query_eus_info(dev_priv, &item);
			break;
		default:
			return -EINVAL;
		}

		if (ret)
			return ret;

		if (copy_to_user(user_item_ptr, &item, sizeof(item)))
			return -EFAULT;
	}

	return 0;
}
