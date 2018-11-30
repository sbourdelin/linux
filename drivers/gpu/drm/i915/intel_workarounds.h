/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2014-2018 Intel Corporation
 */

#ifndef _I915_WORKAROUNDS_H_
#define _I915_WORKAROUNDS_H_

#include <linux/slab.h>

struct i915_wa {
	i915_reg_t	  reg;
	u32		  mask;
	u32		  val;
};

struct i915_wa_list {
	const char	*name;
	unsigned int 	count;
	unsigned int	wa_count;
	struct i915_wa	*list;
	unsigned int	__size;
};

static inline void intel_wa_list_free(struct i915_wa_list *wal)
{
	kfree(wal->list);
	memset(wal, 0, sizeof(*wal));
}

void intel_ctx_workarounds_init(struct drm_i915_private *dev_priv);
int intel_ctx_workarounds_emit(struct i915_request *rq);

void intel_gt_workarounds_init(struct drm_i915_private *dev_priv);
void intel_gt_workarounds_apply(struct drm_i915_private *dev_priv);
bool intel_gt_workarounds_verify(struct drm_i915_private *dev_priv,
				 const char *from);

void intel_whitelist_workarounds_init(struct intel_engine_cs *engine);
void intel_whitelist_workarounds_apply(struct intel_engine_cs *engine);

void intel_engine_workarounds_init(struct intel_engine_cs *engine);
void intel_engine_workarounds_apply(struct intel_engine_cs *engine);

#endif
