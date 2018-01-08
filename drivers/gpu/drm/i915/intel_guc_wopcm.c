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

#include "intel_guc_wopcm.h"
#include "i915_drv.h"

static inline u32 guc_reserved_wopcm_size(struct intel_guc *guc)
{
	struct drm_i915_private *i915 = guc_to_i915(guc);

	/* On BXT, the top of WOPCM is reserved for RC6 context */
	if (IS_GEN9_LP(i915))
		return BXT_WOPCM_RC6_RESERVED;

	if (IS_GEN10(i915))
		return CNL_WOPCM_RESERVED;

	return 0;
}

static inline int gen9_wocpm_size_check(struct drm_i915_private *i915)
{
	struct intel_guc_wopcm *wopcm = &i915->guc.wopcm;
	u32 wopcm_base;
	u32 delta;

	/*
	 * Check hardware restriction on Gen9
	 * GuC WOPCM size is at least 4 bytes larger than GuC WOPCM base due
	 * to hardware limitation on Gen9.
	 */
	wopcm_base = wopcm->offset + GEN9_GUC_WOPCM_OFFSET;
	if (unlikely(wopcm_base > wopcm->size))
		return -E2BIG;

	delta = wopcm->size - wopcm_base;
	if (unlikely(delta < GEN9_GUC_WOPCM_DELTA))
		return -E2BIG;

	return 0;
}

static inline int cnl_a0_wopcm_size_check(struct drm_i915_private *i915)
{
	struct intel_guc_wopcm *wopcm = &i915->guc.wopcm;
	u32 huc_size = intel_uc_fw_get_size(&i915->huc.fw);

	/*
	 * On CNL A0, hardware requires guc size to be larger than or equal to
	 * HuC kernel size.
	 */
	if (unlikely(wopcm->size - GEN10_GUC_WOPCM_OFFSET < huc_size))
		return -E2BIG;

	return 0;
}

static inline int guc_wopcm_size_check(struct intel_guc *guc)
{
	struct drm_i915_private *i915 = guc_to_i915(guc);

	if (IS_GEN9(i915))
		return gen9_wocpm_size_check(i915);

	if (IS_CNL_REVID(i915, CNL_REVID_A0, CNL_REVID_A0))
		return cnl_a0_wopcm_size_check(i915);

	return 0;
}

static inline bool __reg_locked(struct drm_i915_private *dev_priv,
				 i915_reg_t reg)
{
	return !!(I915_READ(reg) & GUC_WOPCM_REG_LOCKED);
}

static inline bool guc_wopcm_locked(struct intel_guc *guc)
{
	struct drm_i915_private *i915 = guc_to_i915(guc);
	bool size_reg_locked = __reg_locked(i915, GUC_WOPCM_SIZE);
	bool offset_reg_locked = __reg_locked(i915, DMA_GUC_WOPCM_OFFSET);

	return size_reg_locked && offset_reg_locked;
}

static inline void guc_wopcm_hw_update(struct intel_guc *guc)
{
	struct drm_i915_private *dev_priv = guc_to_i915(guc);

	/* GuC WOPCM registers should be unlocked at this point. */
	GEM_BUG_ON(__reg_locked(dev_priv, GUC_WOPCM_SIZE));
	GEM_BUG_ON(__reg_locked(dev_priv, DMA_GUC_WOPCM_OFFSET));

	I915_WRITE(GUC_WOPCM_SIZE, guc->wopcm.size);
	I915_WRITE(DMA_GUC_WOPCM_OFFSET,
		   guc->wopcm.offset | HUC_LOADING_AGENT_GUC);

	GEM_BUG_ON(!__reg_locked(dev_priv, GUC_WOPCM_SIZE));
	GEM_BUG_ON(!__reg_locked(dev_priv, DMA_GUC_WOPCM_OFFSET));
}

/*
 * intel_guc_wopcm_init() - Initialize the GuC WOPCM partition.
 * @guc: intel guc.
 * @guc_fw_size: size of GuC firmware.
 * @huc_fw_size: size of HuC firmware.
 *
 * This function tries to initialize the WOPCM partition based on HuC firmware
 * size and the reserved WOPCM memory size.
 *
 * Return: 0 on success, non-zero error code on failure.
 */
int intel_guc_wopcm_init(struct intel_guc *guc, u32 guc_fw_size,
			 u32 huc_fw_size)
{
	u32 reserved = guc_reserved_wopcm_size(guc);
	u32 offset, size, top;
	int err;

	GEM_BUG_ON(guc->wopcm.flags & INTEL_GUC_WOPCM_VALID);

	if (!guc_fw_size)
		return -EINVAL;

	if (reserved >= WOPCM_DEFAULT_SIZE)
		return -E2BIG;

	offset = huc_fw_size + WOPCM_RESERVED_SIZE;
	if (offset >= WOPCM_DEFAULT_SIZE)
		return -E2BIG;

	/* Hardware requires GuC WOPCM offset needs to be 16K aligned. */
	offset = ALIGN(offset, WOPCM_OFFSET_ALIGNMENT);
	if ((offset + reserved) >= WOPCM_DEFAULT_SIZE)
		return -E2BIG;

	top = WOPCM_DEFAULT_SIZE - offset;
	size = top - reserved;

	/*
	 * GuC size needs to be less than or equal to GuC WOPCM size.
	 * Need extra 8K stack for GuC.
	 */
	if ((guc_fw_size + GUC_WOPCM_STACK_RESERVED) > size)
		return -E2BIG;

	guc->wopcm.offset = offset;
	guc->wopcm.size = size;
	guc->wopcm.top = top;

	/* Check platform specific restrictions */
	err = guc_wopcm_size_check(guc);
	if (err)
		return err;

	guc->wopcm.flags |= INTEL_GUC_WOPCM_VALID;

	DRM_DEBUG_DRIVER("GuC WOPCM offset %dKB, size %dKB, top %dKB\n",
			 offset >> 10, size >> 10, top >> 10);

	return 0;
}

/*
 * intel_guc_wopcm_init_hw() - Setup GuC WOPCM registers.
 * @guc: intel guc.
 *
 * Setup the GuC WOPCM size and offset registers with the stored values. It will
 * also check the registers locking status to determine whether these registers
 * are unlocked and can be updated.
 */
void intel_guc_wopcm_init_hw(struct intel_guc *guc)
{
	u32 locked = guc_wopcm_locked(guc);

	GEM_BUG_ON(!(guc->wopcm.flags & INTEL_GUC_WOPCM_VALID));

	/*
	 * Bug if driver hasn't updated the HW Registers and GuC WOPCM has been
	 * locked. Return directly if WOPCM was locked and we have updated
	 * the registers.
	 */
	if (locked) {
		GEM_BUG_ON(!(guc->wopcm.flags & INTEL_GUC_WOPCM_HW_UPDATED));
		return;
	}

	/* Always update registers when GuC WOPCM is not locked. */
	guc_wopcm_hw_update(guc);

	guc->wopcm.flags |= INTEL_GUC_WOPCM_HW_UPDATED;
}
