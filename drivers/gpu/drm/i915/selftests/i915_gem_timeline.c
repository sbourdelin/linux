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

#include "../i915_selftest.h"
#include "mock_gem_device.h"

static int igt_seqmap(void *arg)
{
	struct drm_i915_private *i915 = arg;
	const struct {
		const char *name;
		u32 seqno;
		bool expected;
		bool set;
	} pass[] = {
		{ "unset", 0, false, false },
		{ "new", 0, false, true },
		{ "0a", 0, true, true },
		{ "1a", 1, false, true },
		{ "1b", 1, true, true },
		{ "0b", 0, true, false },
		{ "2a", 2, false, true },
		{ "4", 4, false, true },
		{ "INT_MAX", INT_MAX, false, true },
		{ "INT_MAX-1", INT_MAX-1, true, false },
		{ "INT_MAX+1", (u32)INT_MAX+1, false, true },
		{ "INT_MAX", INT_MAX, true, false },
		{ "UINT_MAX", UINT_MAX, false, true },
		{ "wrap", 0, false, true },
		{ "unwrap", UINT_MAX, true, false },
		{},
	}, *p;
	struct intel_timeline *tl;
	int order, offset;
	int ret;

	tl = &i915->gt.global_timeline.engine[RCS];
	for (p = pass; p->name; p++) {
		for (order = 1; order < 64; order++) {
			for (offset = -1; offset <= (order > 1); offset++) {
				u64 ctx = BIT_ULL(order) + offset;

				if (intel_timeline_sync_get(tl,
							    ctx,
							    p->seqno) != p->expected) {
					pr_err("1: %s(ctx=%llu, seqno=%u) expected passed %s but failed\n",
					       p->name, ctx, p->seqno, yesno(p->expected));
					return -EINVAL;
				}

				if (p->set) {
					ret = intel_timeline_sync_reserve(tl);
					if (ret)
						return ret;

					intel_timeline_sync_set(tl, ctx, p->seqno);
				}
			}
		}
	}

	tl = &i915->gt.global_timeline.engine[BCS];
	for (order = 1; order < 64; order++) {
		for (offset = -1; offset <= (order > 1); offset++) {
			u64 ctx = BIT_ULL(order) + offset;

			for (p = pass; p->name; p++) {
				if (intel_timeline_sync_get(tl,
							    ctx,
							    p->seqno) != p->expected) {
					pr_err("2: %s(ctx=%llu, seqno=%u) expected passed %s but failed\n",
					       p->name, ctx, p->seqno, yesno(p->expected));
					return -EINVAL;
				}

				if (p->set) {
					ret = intel_timeline_sync_reserve(tl);
					if (ret)
						return ret;

					intel_timeline_sync_set(tl, ctx, p->seqno);
				}
			}
		}
	}

	return 0;
}

int i915_gem_timeline_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_seqmap),
	};
	struct drm_i915_private *i915;
	int err;

	i915 = mock_gem_device();
	if (!i915)
		return -ENOMEM;

	err = i915_subtests(tests, i915);
	drm_dev_unref(&i915->drm);

	return err;
}
