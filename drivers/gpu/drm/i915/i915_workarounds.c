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
#include "i915_workarounds.h"

static int ctx_wa_add(struct drm_i915_private *dev_priv,
		      i915_reg_t addr,
		      const u32 mask, const u32 val)
{
	const u32 idx = dev_priv->workarounds.ctx_wa_count;

	if (WARN_ON(idx >= I915_MAX_CTX_WA_REGS))
		return -ENOSPC;

	dev_priv->workarounds.ctx_wa_reg[idx].addr = addr;
	dev_priv->workarounds.ctx_wa_reg[idx].value = val;
	dev_priv->workarounds.ctx_wa_reg[idx].mask = mask;

	dev_priv->workarounds.ctx_wa_count++;

	return 0;
}

#define CTXWA_REG(addr, mask, val) do { \
		const int r = ctx_wa_add(dev_priv, (addr), (mask), (val)); \
		if (r) \
			return r; \
	} while (0)

#define CTXWA_SET_BIT_MSK(addr, mask) \
	CTXWA_REG(addr, (mask), _MASKED_BIT_ENABLE(mask))

#define CTXWA_CLR_BIT_MSK(addr, mask) \
	CTXWA_REG(addr, (mask), _MASKED_BIT_DISABLE(mask))

#define CTXWA_SET_FIELD_MSK(addr, mask, value) \
	CTXWA_REG(addr, mask, _MASKED_FIELD(mask, value))

static int gen8_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	CTXWA_SET_BIT_MSK(INSTPM, INSTPM_FORCE_ORDERING);

	/* WaDisableAsyncFlipPerfMode:bdw,chv */
	CTXWA_SET_BIT_MSK(MI_MODE, ASYNC_FLIP_PERF_DISABLE);

	/* WaDisablePartialInstShootdown:bdw,chv */
	CTXWA_SET_BIT_MSK(GEN8_ROW_CHICKEN,
			  PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE);

	/* Use Force Non-Coherent whenever executing a 3D context. This is a
	 * workaround for for a possible hang in the unlikely event a TLB
	 * invalidation occurs during a PSD flush.
	 */
	/* WaForceEnableNonCoherent:bdw,chv */
	/* WaHdcDisableFetchWhenMasked:bdw,chv */
	CTXWA_SET_BIT_MSK(HDC_CHICKEN0,
			  HDC_DONOT_FETCH_MEM_WHEN_MASKED |
			  HDC_FORCE_NON_COHERENT);

	/* From the Haswell PRM, Command Reference: Registers, CACHE_MODE_0:
	 * "The Hierarchical Z RAW Stall Optimization allows non-overlapping
	 *  polygons in the same 8x4 pixel/sample area to be processed without
	 *  stalling waiting for the earlier ones to write to Hierarchical Z
	 *  buffer."
	 *
	 * This optimization is off by default for BDW and CHV; turn it on.
	 */
	CTXWA_CLR_BIT_MSK(CACHE_MODE_0_GEN7, HIZ_RAW_STALL_OPT_DISABLE);

	/* Wa4x4STCOptimizationDisable:bdw,chv */
	CTXWA_SET_BIT_MSK(CACHE_MODE_1, GEN8_4x4_STC_OPTIMIZATION_DISABLE);

	/*
	 * BSpec recommends 8x4 when MSAA is used,
	 * however in practice 16x4 seems fastest.
	 *
	 * Note that PS/WM thread counts depend on the WIZ hashing
	 * disable bit, which we don't touch here, but it's good
	 * to keep in mind (see 3DSTATE_PS and 3DSTATE_WM).
	 */
	CTXWA_SET_FIELD_MSK(GEN7_GT_MODE,
			    GEN6_WIZ_HASHING_MASK,
			    GEN6_WIZ_HASHING_16x4);

	return 0;
}

static int bdw_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen8_ctx_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/* WaDisableThreadStallDopClockGating:bdw (pre-production) */
	CTXWA_SET_BIT_MSK(GEN8_ROW_CHICKEN, STALL_DOP_GATING_DISABLE);

	/* WaDisableDopClockGating:bdw
	 *
	 * Also see the related UCGTCL1 write in broadwell_init_clock_gating()
	 * to disable EUTC clock gating.
	 */
	CTXWA_SET_BIT_MSK(GEN7_ROW_CHICKEN2,
			  DOP_CLOCK_GATING_DISABLE);

	CTXWA_SET_BIT_MSK(HALF_SLICE_CHICKEN3,
			  GEN8_SAMPLER_POWER_BYPASS_DIS);

	CTXWA_SET_BIT_MSK(HDC_CHICKEN0,
			  /* WaForceContextSaveRestoreNonCoherent:bdw */
			  HDC_FORCE_CONTEXT_SAVE_RESTORE_NON_COHERENT |
			  /* WaDisableFenceDestinationToSLM:bdw (pre-prod) */
			  (IS_BDW_GT3(dev_priv) ? HDC_FENCE_DEST_SLM_DISABLE : 0));

	return 0;
}

static int chv_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen8_ctx_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/* WaDisableThreadStallDopClockGating:chv */
	CTXWA_SET_BIT_MSK(GEN8_ROW_CHICKEN, STALL_DOP_GATING_DISABLE);

	/* Improve HiZ throughput on CHV. */
	CTXWA_SET_BIT_MSK(HIZ_CHICKEN, CHV_HZ_8X8_MODE_IN_1X);

	return 0;
}

static int gen9_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	if (HAS_LLC(dev_priv)) {
		/* WaCompressedResourceSamplerPbeMediaNewHashMode:skl,kbl
		 *
		 * Must match Display Engine. See
		 * WaCompressedResourceDisplayNewHashMode.
		 */
		CTXWA_SET_BIT_MSK(COMMON_SLICE_CHICKEN2,
				  GEN9_PBE_COMPRESSED_HASH_SELECTION);
		CTXWA_SET_BIT_MSK(GEN9_HALF_SLICE_CHICKEN7,
				  GEN9_SAMPLER_HASH_COMPRESSED_READ_ADDR);
	}

	/* WaClearFlowControlGpgpuContextSave:skl,bxt,kbl,glk,cfl */
	/* WaDisablePartialInstShootdown:skl,bxt,kbl,glk,cfl */
	CTXWA_SET_BIT_MSK(GEN8_ROW_CHICKEN,
			  FLOW_CONTROL_ENABLE |
			  PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE);

	/* Syncing dependencies between camera and graphics:skl,bxt,kbl */
	if (!IS_COFFEELAKE(dev_priv))
		CTXWA_SET_BIT_MSK(HALF_SLICE_CHICKEN3,
				  GEN9_DISABLE_OCL_OOB_SUPPRESS_LOGIC);

	/* WaDisableDgMirrorFixInHalfSliceChicken5:bxt */
	if (IS_BXT_REVID(dev_priv, 0, BXT_REVID_A1))
		CTXWA_CLR_BIT_MSK(GEN9_HALF_SLICE_CHICKEN5,
				  GEN9_DG_MIRROR_FIX_ENABLE);

	/* WaSetDisablePixMaskCammingAndRhwoInCommonSliceChicken:bxt */
	if (IS_BXT_REVID(dev_priv, 0, BXT_REVID_A1)) {
		CTXWA_SET_BIT_MSK(GEN7_COMMON_SLICE_CHICKEN1,
				  GEN9_RHWO_OPTIMIZATION_DISABLE);
		/*
		 * WA also requires GEN9_SLICE_COMMON_ECO_CHICKEN0[14:14] to be set
		 * but we do that in per ctx batchbuffer as there is an issue
		 * with this register not getting restored on ctx restore
		 */
	}

	/* WaEnableYV12BugFixInHalfSliceChicken7:skl,bxt,kbl,glk,cfl */
	/* WaEnableSamplerGPGPUPreemptionSupport:skl,bxt,kbl,cfl */
	CTXWA_SET_BIT_MSK(GEN9_HALF_SLICE_CHICKEN7,
			  GEN9_ENABLE_YV12_BUGFIX |
			  GEN9_ENABLE_GPGPU_PREEMPTION);

	/* Wa4x4STCOptimizationDisable:skl,bxt,kbl,glk,cfl */
	/* WaDisablePartialResolveInVc:skl,bxt,kbl,cfl */
	CTXWA_SET_BIT_MSK(CACHE_MODE_1, (GEN8_4x4_STC_OPTIMIZATION_DISABLE |
					 GEN9_PARTIAL_RESOLVE_IN_VC_DISABLE));

	/* WaCcsTlbPrefetchDisable:skl,bxt,kbl,glk,cfl */
	CTXWA_CLR_BIT_MSK(GEN9_HALF_SLICE_CHICKEN5,
			  GEN9_CCS_TLB_PREFETCH_ENABLE);

	/* WaDisableMaskBasedCammingInRCC:bxt */
	if (IS_BXT_REVID(dev_priv, 0, BXT_REVID_A1))
		CTXWA_SET_BIT_MSK(SLICE_ECO_CHICKEN0,
				  PIXEL_MASK_CAMMING_DISABLE);

	/* WaForceContextSaveRestoreNonCoherent:skl,bxt,kbl,cfl */
	CTXWA_SET_BIT_MSK(HDC_CHICKEN0,
			  HDC_FORCE_CONTEXT_SAVE_RESTORE_NON_COHERENT |
			  HDC_FORCE_CSR_NON_COHERENT_OVR_DISABLE);

	/* WaForceEnableNonCoherent and WaDisableHDCInvalidation are
	 * both tied to WaForceContextSaveRestoreNonCoherent
	 * in some hsds for skl. We keep the tie for all gen9. The
	 * documentation is a bit hazy and so we want to get common behaviour,
	 * even though there is no clear evidence we would need both on kbl/bxt.
	 * This area has been source of system hangs so we play it safe
	 * and mimic the skl regardless of what bspec says.
	 *
	 * Use Force Non-Coherent whenever executing a 3D context. This
	 * is a workaround for a possible hang in the unlikely event
	 * a TLB invalidation occurs during a PSD flush.
	 */

	/* WaForceEnableNonCoherent:skl,bxt,kbl,cfl */
	CTXWA_SET_BIT_MSK(HDC_CHICKEN0,
			  HDC_FORCE_NON_COHERENT);

	/* WaDisableSamplerPowerBypassForSOPingPong:skl,bxt,kbl,cfl */
	if (IS_SKYLAKE(dev_priv) ||
	    IS_KABYLAKE(dev_priv) ||
	    IS_COFFEELAKE(dev_priv) ||
	    IS_BXT_REVID(dev_priv, 0, BXT_REVID_B0))
		CTXWA_SET_BIT_MSK(HALF_SLICE_CHICKEN3,
				  GEN8_SAMPLER_POWER_BYPASS_DIS);

	/* WaDisableSTUnitPowerOptimization:skl,bxt,kbl,glk,cfl */
	CTXWA_SET_BIT_MSK(HALF_SLICE_CHICKEN2, GEN8_ST_PO_DISABLE);

	/*
	 * Supporting preemption with fine-granularity requires changes in the
	 * batch buffer programming. Since we can't break old userspace, we
	 * need to set our default preemption level to safe value. Userspace is
	 * still able to use more fine-grained preemption levels, since in
	 * WaEnablePreemptionGranularityControlByUMD we're whitelisting the
	 * per-ctx register. As such, WaDisable{3D,GPGPU}MidCmdPreemption are
	 * not real HW workarounds, but merely a way to start using preemption
	 * while maintaining old contract with userspace.
	 */

	/* WaDisable3DMidCmdPreemption:skl,bxt,glk,cfl,[cnl] */
	CTXWA_CLR_BIT_MSK(GEN8_CS_CHICKEN1, GEN9_PREEMPT_3D_OBJECT_LEVEL);

	/* WaDisableGPGPUMidCmdPreemption:skl,bxt,blk,cfl,[cnl] */
	CTXWA_SET_FIELD_MSK(GEN8_CS_CHICKEN1, GEN9_PREEMPT_GPGPU_LEVEL_MASK,
			    GEN9_PREEMPT_GPGPU_COMMAND_LEVEL);

	return 0;
}

static int skl_tune_iz_hashing(struct drm_i915_private *dev_priv)
{
	u8 vals[3] = { 0, 0, 0 };
	unsigned int i;

	for (i = 0; i < 3; i++) {
		u8 ss;

		/*
		 * Only consider slices where one, and only one, subslice has 7
		 * EUs
		 */
		if (!is_power_of_2(INTEL_INFO(dev_priv)->sseu.subslice_7eu[i]))
			continue;

		/*
		 * subslice_7eu[i] != 0 (because of the check above) and
		 * ss_max == 4 (maximum number of subslices possible per slice)
		 *
		 * ->    0 <= ss <= 3;
		 */
		ss = ffs(INTEL_INFO(dev_priv)->sseu.subslice_7eu[i]) - 1;
		vals[i] = 3 - ss;
	}

	if (vals[0] == 0 && vals[1] == 0 && vals[2] == 0)
		return 0;

	/* Tune IZ hashing. See intel_device_info_runtime_init() */
	CTXWA_SET_FIELD_MSK(GEN7_GT_MODE,
			    GEN9_IZ_HASHING_MASK(2) |
			    GEN9_IZ_HASHING_MASK(1) |
			    GEN9_IZ_HASHING_MASK(0),
			    GEN9_IZ_HASHING(2, vals[2]) |
			    GEN9_IZ_HASHING(1, vals[1]) |
			    GEN9_IZ_HASHING(0, vals[0]));

	return 0;
}

static int skl_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen9_ctx_workarounds_init(dev_priv);
	if (ret)
		return ret;

	return skl_tune_iz_hashing(dev_priv);
}

static int bxt_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen9_ctx_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/* WaDisableThreadStallDopClockGating:bxt */
	CTXWA_SET_BIT_MSK(GEN8_ROW_CHICKEN,
			  STALL_DOP_GATING_DISABLE);

	/* WaDisableSbeCacheDispatchPortSharing:bxt */
	if (IS_BXT_REVID(dev_priv, 0, BXT_REVID_B0)) {
		CTXWA_SET_BIT_MSK(
			GEN7_HALF_SLICE_CHICKEN1,
			GEN7_SBE_SS_CACHE_DISPATCH_PORT_SHARING_DISABLE);
	}

	/* WaToEnableHwFixForPushConstHWBug:bxt */
	if (IS_BXT_REVID(dev_priv, BXT_REVID_C0, REVID_FOREVER))
		CTXWA_SET_BIT_MSK(COMMON_SLICE_CHICKEN2,
				  GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION);

	return 0;
}

static int kbl_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen9_ctx_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/* WaDisableFenceDestinationToSLM:kbl (pre-prod) */
	if (IS_KBL_REVID(dev_priv, KBL_REVID_A0, KBL_REVID_A0))
		CTXWA_SET_BIT_MSK(HDC_CHICKEN0,
				  HDC_FENCE_DEST_SLM_DISABLE);

	/* WaToEnableHwFixForPushConstHWBug:kbl */
	if (IS_KBL_REVID(dev_priv, KBL_REVID_C0, REVID_FOREVER))
		CTXWA_SET_BIT_MSK(COMMON_SLICE_CHICKEN2,
				  GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION);

	/* WaDisableSbeCacheDispatchPortSharing:kbl */
	CTXWA_SET_BIT_MSK(
		GEN7_HALF_SLICE_CHICKEN1,
		GEN7_SBE_SS_CACHE_DISPATCH_PORT_SHARING_DISABLE);

	return 0;
}

static int glk_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen9_ctx_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/* WaToEnableHwFixForPushConstHWBug:glk */
	CTXWA_SET_BIT_MSK(COMMON_SLICE_CHICKEN2,
			  GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION);

	return 0;
}

static int cfl_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen9_ctx_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/* WaToEnableHwFixForPushConstHWBug:cfl */
	CTXWA_SET_BIT_MSK(COMMON_SLICE_CHICKEN2,
			  GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION);

	/* WaDisableSbeCacheDispatchPortSharing:cfl */
	CTXWA_SET_BIT_MSK(
		GEN7_HALF_SLICE_CHICKEN1,
		GEN7_SBE_SS_CACHE_DISPATCH_PORT_SHARING_DISABLE);

	return 0;
}

static int cnl_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	/* WaForceContextSaveRestoreNonCoherent:cnl */
	CTXWA_SET_BIT_MSK(CNL_HDC_CHICKEN0,
			  HDC_FORCE_CONTEXT_SAVE_RESTORE_NON_COHERENT);

	/* WaThrottleEUPerfToAvoidTDBackPressure:cnl(pre-prod) */
	if (IS_CNL_REVID(dev_priv, CNL_REVID_B0, CNL_REVID_B0))
		CTXWA_SET_BIT_MSK(GEN8_ROW_CHICKEN, THROTTLE_12_5);

	/* WaDisableReplayBufferBankArbitrationOptimization:cnl */
	CTXWA_SET_BIT_MSK(COMMON_SLICE_CHICKEN2,
			  GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION);

	/* WaDisableEnhancedSBEVertexCaching:cnl (pre-prod) */
	if (IS_CNL_REVID(dev_priv, 0, CNL_REVID_B0))
		CTXWA_SET_BIT_MSK(COMMON_SLICE_CHICKEN2,
				  GEN8_CSC2_SBE_VUE_CACHE_CONSERVATIVE);

	/* WaPushConstantDereferenceHoldDisable:cnl */
	CTXWA_SET_BIT_MSK(GEN7_ROW_CHICKEN2, PUSH_CONSTANT_DEREF_DISABLE);

	/* FtrEnableFastAnisoL1BankingFix:cnl */
	CTXWA_SET_BIT_MSK(HALF_SLICE_CHICKEN3, CNL_FAST_ANISO_L1_BANKING_FIX);

	/* WaDisable3DMidCmdPreemption:cnl */
	CTXWA_CLR_BIT_MSK(GEN8_CS_CHICKEN1, GEN9_PREEMPT_3D_OBJECT_LEVEL);

	/* WaDisableGPGPUMidCmdPreemption:cnl */
	CTXWA_SET_FIELD_MSK(GEN8_CS_CHICKEN1, GEN9_PREEMPT_GPGPU_LEVEL_MASK,
			    GEN9_PREEMPT_GPGPU_COMMAND_LEVEL);

	return 0;
}

int i915_ctx_workarounds_init(struct drm_i915_private *dev_priv)
{
	int err;

	dev_priv->workarounds.ctx_wa_count = 0;

	if (IS_BROADWELL(dev_priv))
		err = bdw_ctx_workarounds_init(dev_priv);
	else if (IS_CHERRYVIEW(dev_priv))
		err = chv_ctx_workarounds_init(dev_priv);
	else if (IS_SKYLAKE(dev_priv))
		err = skl_ctx_workarounds_init(dev_priv);
	else if (IS_BROXTON(dev_priv))
		err = bxt_ctx_workarounds_init(dev_priv);
	else if (IS_KABYLAKE(dev_priv))
		err = kbl_ctx_workarounds_init(dev_priv);
	else if (IS_GEMINILAKE(dev_priv))
		err = glk_ctx_workarounds_init(dev_priv);
	else if (IS_COFFEELAKE(dev_priv))
		err = cfl_ctx_workarounds_init(dev_priv);
	else if (IS_CANNONLAKE(dev_priv))
		err = cnl_ctx_workarounds_init(dev_priv);
	else
		err = 0;
	if (err)
		return err;

	DRM_DEBUG_DRIVER("Number of context specific w/a: %d\n",
			 dev_priv->workarounds.ctx_wa_count);
	return 0;
}

int i915_ctx_workarounds_emit(struct drm_i915_gem_request *req)
{
	struct i915_workarounds *w = &req->i915->workarounds;
	u32 *cs;
	int ret, i;

	if (w->ctx_wa_count == 0)
		return 0;

	ret = req->engine->emit_flush(req, EMIT_BARRIER);
	if (ret)
		return ret;

	cs = intel_ring_begin(req, (w->ctx_wa_count * 2 + 2));
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	*cs++ = MI_LOAD_REGISTER_IMM(w->ctx_wa_count);
	for (i = 0; i < w->ctx_wa_count; i++) {
		*cs++ = i915_mmio_reg_offset(w->ctx_wa_reg[i].addr);
		*cs++ = w->ctx_wa_reg[i].value;
	}
	*cs++ = MI_NOOP;

	intel_ring_advance(req, cs);

	ret = req->engine->emit_flush(req, EMIT_BARRIER);
	if (ret)
		return ret;

	return 0;
}

static int mmio_wa_add(struct drm_i915_private *dev_priv,
		       i915_reg_t addr,
		       const u32 mask, const u32 val)
{
	const u32 idx = dev_priv->workarounds.mmio_wa_count;

	if (WARN_ON(idx >= I915_MAX_MMIO_WA_REGS))
		return -ENOSPC;

	dev_priv->workarounds.mmio_wa_reg[idx].addr = addr;
	dev_priv->workarounds.mmio_wa_reg[idx].value = val;
	dev_priv->workarounds.mmio_wa_reg[idx].mask = mask;

	dev_priv->workarounds.mmio_wa_count++;

	return 0;
}

#define MMIOWA_REG(addr, mask, val) do { \
		const int r = mmio_wa_add(dev_priv, (addr), (mask), (val)); \
		if (r) \
			return r; \
	} while (0)

#define MMIOWA_SET_BIT(addr, mask) \
	MMIOWA_REG(addr, (mask), (mask))

#define MMIOWA_SET_BIT_MSK(addr, mask) \
	CTXWA_REG(addr, (mask), _MASKED_BIT_ENABLE(mask))

#define MMIOWA_CLR_BIT(addr, mask) \
	MMIOWA_REG(addr, (mask), 0)

#define MMIOWA_SET_FIELD(addr, mask, value) \
	MMIOWA_REG(addr, (mask), (value))

static int bdw_mmio_workarounds_init(struct drm_i915_private *dev_priv)
{
	return 0;
}

static int chv_mmio_workarounds_init(struct drm_i915_private *dev_priv)
{
	return 0;
}

static int gen9_mmio_workarounds_init(struct drm_i915_private *dev_priv)
{
	if (HAS_LLC(dev_priv)) {
		/* WaCompressedResourceSamplerPbeMediaNewHashMode:skl,kbl
		 *
		 * Must match Display Engine. See
		 * WaCompressedResourceDisplayNewHashMode.
		 */
		MMIOWA_SET_BIT(MMCD_MISC_CTRL, MMCD_PCLA | MMCD_HOTSPOT_EN);

		/*
		 * WaCompressedResourceDisplayNewHashMode:skl,kbl
		 * Display WA#0390: skl,kbl
		 *
		 * Must match Sampler, Pixel Back End, and Media. See
		 * WaCompressedResourceSamplerPbeMediaNewHashMode.
		 */
		MMIOWA_SET_BIT(CHICKEN_PAR1_1, SKL_DE_COMPRESSED_HASH_MODE);
	}

	/* See Bspec note for PSR2_CTL bit 31, Wa#828:skl,bxt,kbl,cfl */
	MMIOWA_SET_BIT(CHICKEN_PAR1_1, SKL_EDP_PSR_FIX_RDWRAP);

	MMIOWA_SET_BIT(GEN8_CONFIG0, GEN9_DEFAULT_FIXES);

	/* WaEnableChickenDCPR:skl,bxt,kbl,glk,cfl */
	MMIOWA_SET_BIT(GEN8_CHICKEN_DCPR_1, MASK_WAKEMEM);

	/* WaFbcTurnOffFbcWatermark:skl,bxt,kbl,cfl */
	/* WaFbcWakeMemOn:skl,bxt,kbl,glk,cfl */
	MMIOWA_SET_BIT(DISP_ARB_CTL, DISP_FBC_WM_DIS | DISP_FBC_MEMORY_WAKE);

	/* WaFbcHighMemBwCorruptionAvoidance:skl,bxt,kbl,cfl */
	MMIOWA_SET_BIT(ILK_DPFC_CHICKEN, ILK_DPFC_DISABLE_DUMMY0);

	/* WaContextSwitchWithConcurrentTLBInvalidate:skl,bxt,kbl,glk,cfl */
	MMIOWA_SET_BIT_MSK(GEN9_CSFE_CHICKEN1_RCS,
			   GEN9_PREEMPT_GPGPU_SYNC_SWITCH_DISABLE);

	/* WaEnableLbsSlaRetryTimerDecrement:skl,bxt,kbl,glk,cfl */
	MMIOWA_SET_BIT(BDW_SCRATCH1, GEN9_LBS_SLA_RETRY_TIMER_DECREMENT_ENABLE);

	/* WaDisableKillLogic:bxt,skl,kbl */
	if (!IS_COFFEELAKE(dev_priv))
		MMIOWA_SET_BIT(GAM_ECOCHK, ECOCHK_DIS_TLB);

	/* WaDisableHDCInvalidation:skl,bxt,kbl,cfl */
	MMIOWA_SET_BIT(GAM_ECOCHK, BDW_DISABLE_HDC_INVALIDATION);

	/* WaOCLCoherentLineFlush:skl,bxt,kbl,cfl */
	MMIOWA_SET_BIT(GEN8_L3SQCREG4, GEN8_LQSC_FLUSH_COHERENT_LINES);

	/* WaEnablePreemptionGranularityControlByUMD:skl,bxt,kbl,cfl,[cnl] */
	MMIOWA_SET_BIT_MSK(GEN7_FF_SLICE_CS_CHICKEN1,
			   GEN9_FFSC_PERCTX_PREEMPT_CTRL);
	
	return 0;
}

static int skl_mmio_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen9_mmio_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/* WaDisableDopClockGating */
	MMIOWA_CLR_BIT(GEN7_MISCCPCTL, GEN7_DOP_CLOCK_GATE_ENABLE);

	/* WAC6entrylatency:skl */
	MMIOWA_SET_BIT(FBC_LLC_READ_CTRL, FBC_LLC_FULLY_OPEN);

	/* WaFbcNukeOnHostModify:skl */
	MMIOWA_SET_BIT(ILK_DPFC_CHICKEN, ILK_DPFC_NUKE_ON_ANY_MODIFICATION);

	/* WaEnableGapsTsvCreditFix:skl */
	MMIOWA_SET_BIT(GEN8_GARBCNTL, GEN9_GAPS_TSV_CREDIT_DISABLE);

	/* WaDisableGafsUnitClkGating:skl */
	MMIOWA_SET_BIT(GEN7_UCGCTL4, GEN8_EU_GAUNIT_CLOCK_GATE_DISABLE);

	/* WaInPlaceDecompressionHang:skl */
	if (IS_SKL_REVID(dev_priv, SKL_REVID_H0, REVID_FOREVER))
		MMIOWA_SET_BIT(GEN9_GAMT_ECO_REG_RW_IA,
			       GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS);

	return 0;
}

static int bxt_mmio_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen9_mmio_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/* WaDisableSDEUnitClockGating:bxt */
	MMIOWA_SET_BIT(GEN8_UCGCTL6, GEN8_SDEUNIT_CLOCK_GATE_DISABLE);

	/*
	 * FIXME:
	 * GEN8_HDCUNIT_CLOCK_GATE_DISABLE_HDCREQ applies on 3x6 GT SKUs only.
	 */
	MMIOWA_SET_BIT(GEN8_UCGCTL6, GEN8_HDCUNIT_CLOCK_GATE_DISABLE_HDCREQ);

	/*
	 * Wa: Backlight PWM may stop in the asserted state, causing backlight
	 * to stay fully on.
	 */
	MMIOWA_SET_BIT(GEN9_CLKGATE_DIS_0, PWM1_GATING_DIS | PWM2_GATING_DIS);

	/* WaStoreMultiplePTEenable:bxt */
	/* This is a requirement according to Hardware specification */
	if (IS_BXT_REVID(dev_priv, 0, BXT_REVID_A1))
		MMIOWA_SET_BIT(TILECTL, TILECTL_TLBPF);

	/* WaSetClckGatingDisableMedia:bxt */
	if (IS_BXT_REVID(dev_priv, 0, BXT_REVID_A1)) {
		MMIOWA_CLR_BIT(GEN7_MISCCPCTL, GEN8_DOP_CLOCK_GATE_MEDIA_ENABLE);
	}

	/* WaDisablePooledEuLoadBalancingFix:bxt */
	if (IS_BXT_REVID(dev_priv, BXT_REVID_B0, REVID_FOREVER)) {
		MMIOWA_SET_BIT_MSK(FF_SLICE_CS_CHICKEN2,
				   GEN9_POOLED_EU_LOAD_BALANCING_FIX_DISABLE);
	}

	/* WaProgramL3SqcReg1DefaultForPerf:bxt */
	if (IS_BXT_REVID(dev_priv, BXT_REVID_B0, REVID_FOREVER))
		MMIOWA_SET_FIELD(GEN8_L3SQCREG1, L3_PRIO_CREDITS_MASK,
				 L3_GENERAL_PRIO_CREDITS(62) |
				 L3_HIGH_PRIO_CREDITS(2));

	/* WaInPlaceDecompressionHang:bxt */
	if (IS_BXT_REVID(dev_priv, BXT_REVID_C0, REVID_FOREVER))
		MMIOWA_SET_BIT(GEN9_GAMT_ECO_REG_RW_IA,
			GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS);

	return 0;
}

static int kbl_mmio_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen9_mmio_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/* WaDisableSDEUnitClockGating:kbl */
	if (IS_KBL_REVID(dev_priv, 0, KBL_REVID_B0))
		MMIOWA_SET_BIT(GEN8_UCGCTL6, GEN8_SDEUNIT_CLOCK_GATE_DISABLE);

	/* WaDisableGamClockGating:kbl */
	if (IS_KBL_REVID(dev_priv, 0, KBL_REVID_B0))
		MMIOWA_SET_BIT(GEN6_UCGCTL1, GEN6_GAMUNIT_CLOCK_GATE_DISABLE);

	/* WaFbcNukeOnHostModify:kbl */
	MMIOWA_SET_BIT(ILK_DPFC_CHICKEN, ILK_DPFC_NUKE_ON_ANY_MODIFICATION);

	/* WaEnableGapsTsvCreditFix:kbl */
	MMIOWA_SET_BIT(GEN8_GARBCNTL, GEN9_GAPS_TSV_CREDIT_DISABLE);

	/* WaDisableDynamicCreditSharing:kbl */
	if (IS_KBL_REVID(dev_priv, 0, KBL_REVID_B0))
		MMIOWA_SET_BIT(GAMT_CHKN_BIT_REG,
			       GAMT_CHKN_DISABLE_DYNAMIC_CREDIT_SHARING);

	/* WaDisableGafsUnitClkGating:kbl */
	MMIOWA_SET_BIT(GEN7_UCGCTL4, GEN8_EU_GAUNIT_CLOCK_GATE_DISABLE);

	/* WaInPlaceDecompressionHang:kbl */
	MMIOWA_SET_BIT(GEN9_GAMT_ECO_REG_RW_IA,
		       GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS);

	return 0;
}

static int glk_mmio_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen9_mmio_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/*
	 * WaDisablePWMClockGating:glk
	 * Backlight PWM may stop in the asserted state, causing backlight
	 * to stay fully on.
	 */
	MMIOWA_SET_BIT(GEN9_CLKGATE_DIS_0, PWM1_GATING_DIS | PWM2_GATING_DIS);

	/* WaDDIIOTimeout:glk */
	if (IS_GLK_REVID(dev_priv, 0, GLK_REVID_A1))
		MMIOWA_CLR_BIT(CHICKEN_MISC_2,
			       GLK_CL0_PWR_DOWN |
			       GLK_CL1_PWR_DOWN |
			       GLK_CL2_PWR_DOWN);

	/* Display WA #1133: WaFbcSkipSegments:glk */
	MMIOWA_SET_FIELD(ILK_DPFC_CHICKEN, GLK_SKIP_SEG_COUNT_MASK,
			 GLK_SKIP_SEG_EN | GLK_SKIP_SEG_COUNT(1));

	return 0;
}

static int cfl_mmio_workarounds_init(struct drm_i915_private *dev_priv)
{
	int ret;

	ret = gen9_mmio_workarounds_init(dev_priv);
	if (ret)
		return ret;

	/* WaFbcNukeOnHostModify:cfl */
	MMIOWA_SET_BIT(ILK_DPFC_CHICKEN, ILK_DPFC_NUKE_ON_ANY_MODIFICATION);

	/* WaEnableGapsTsvCreditFix:cfl */
	MMIOWA_SET_BIT(GEN8_GARBCNTL, GEN9_GAPS_TSV_CREDIT_DISABLE);

	/* WaDisableGafsUnitClkGating:cfl */
	MMIOWA_SET_BIT(GEN7_UCGCTL4, GEN8_EU_GAUNIT_CLOCK_GATE_DISABLE);

	/* WaInPlaceDecompressionHang:cfl */
	MMIOWA_SET_BIT(GEN9_GAMT_ECO_REG_RW_IA,
		       GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS);

	return 0;
}

static int cnl_mmio_workarounds_init(struct drm_i915_private *dev_priv)
{
	/* This is not an Wa. Enable for better image quality */
	MMIOWA_SET_BIT_MSK(_3D_CHICKEN3, _3D_CHICKEN3_AA_LINE_QUALITY_FIX_ENABLE);

	/* WaEnableChickenDCPR:cnl */
	MMIOWA_SET_BIT(GEN8_CHICKEN_DCPR_1, MASK_WAKEMEM);

	/* WaFbcWakeMemOn:cnl */
	MMIOWA_SET_BIT(DISP_ARB_CTL, DISP_FBC_MEMORY_WAKE);

	/* WaSarbUnitClockGatingDisable:cnl (pre-prod) */
	if (IS_CNL_REVID(dev_priv, CNL_REVID_A0, CNL_REVID_B0))
		MMIOWA_SET_BIT(SLICE_UNIT_LEVEL_CLKGATE, SARBUNIT_CLKGATE_DIS);

	/* Display WA #1133: WaFbcSkipSegments:cnl */
	MMIOWA_SET_FIELD(ILK_DPFC_CHICKEN, GLK_SKIP_SEG_COUNT_MASK,
			 GLK_SKIP_SEG_EN | GLK_SKIP_SEG_COUNT(1));

	/* WaDisableI2mCycleOnWRPort:cnl (pre-prod) */
	if (IS_CNL_REVID(dev_priv, CNL_REVID_B0, CNL_REVID_B0))
		MMIOWA_SET_BIT(GAMT_CHKN_BIT_REG,
			       GAMT_CHKN_DISABLE_I2M_CYCLE_ON_WR_PORT);

	/* WaInPlaceDecompressionHang:cnl */
	MMIOWA_SET_BIT(GEN9_GAMT_ECO_REG_RW_IA,
		       GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS);

	/* WaEnablePreemptionGranularityControlByUMD:cnl */
	MMIOWA_SET_BIT_MSK(GEN7_FF_SLICE_CS_CHICKEN1,
			   GEN9_FFSC_PERCTX_PREEMPT_CTRL);

	return 0;
}

int i915_mmio_workarounds_init(struct drm_i915_private *dev_priv)
{
	int err;

	dev_priv->workarounds.mmio_wa_count = 0;

	if (IS_BROADWELL(dev_priv))
		err = bdw_mmio_workarounds_init(dev_priv);
	else if (IS_CHERRYVIEW(dev_priv))
		err = chv_mmio_workarounds_init(dev_priv);
	else if (IS_SKYLAKE(dev_priv))
		err = skl_mmio_workarounds_init(dev_priv);
	else if (IS_BROXTON(dev_priv))
		err = bxt_mmio_workarounds_init(dev_priv);
	else if (IS_KABYLAKE(dev_priv))
		err = kbl_mmio_workarounds_init(dev_priv);
	else if (IS_GEMINILAKE(dev_priv))
		err = glk_mmio_workarounds_init(dev_priv);
	else if (IS_COFFEELAKE(dev_priv))
		err = cfl_mmio_workarounds_init(dev_priv);
	else if (IS_CANNONLAKE(dev_priv))
		err = cnl_mmio_workarounds_init(dev_priv);
	else
		err = 0;
	if (err)
		return err;

	DRM_DEBUG_DRIVER("Number of MMIO w/a: %d\n",
			 dev_priv->workarounds.mmio_wa_count);
	return 0;
}

void i915_mmio_workarounds_apply(struct drm_i915_private *dev_priv)
{
	struct i915_workarounds *w = &dev_priv->workarounds;
	int i;

	for (i = 0; i < w->mmio_wa_count; i++) {
		i915_reg_t addr = w->mmio_wa_reg[i].addr;
		u32 value = w->mmio_wa_reg[i].value;
		u32 mask = w->mmio_wa_reg[i].mask;

		I915_WRITE(addr, (I915_READ(addr) & ~mask) | value);
	}
}

static int wa_ring_whitelist_reg(struct intel_engine_cs *engine,
				 i915_reg_t reg)
{
	struct drm_i915_private *dev_priv = engine->i915;
	struct i915_workarounds *wa = &dev_priv->workarounds;
	const uint32_t index = wa->hw_whitelist_count[engine->id];

	if (WARN_ON(index >= RING_MAX_NONPRIV_SLOTS))
		return -EINVAL;

	I915_WRITE(RING_FORCE_TO_NONPRIV(engine->mmio_base, index),
		   i915_mmio_reg_offset(reg));
	wa->hw_whitelist_count[engine->id]++;

	return 0;
}

static int gen9_whitelist_workarounds_apply(struct intel_engine_cs *engine)
{
	int ret;

	/* WaVFEStateAfterPipeControlwithMediaStateClear:skl,bxt,glk,cfl */
	ret = wa_ring_whitelist_reg(engine, GEN9_CTX_PREEMPT_REG);
	if (ret)
		return ret;

	/* WaEnablePreemptionGranularityControlByUMD:skl,bxt,kbl,cfl,[cnl] */
	ret = wa_ring_whitelist_reg(engine, GEN8_CS_CHICKEN1);
	if (ret)
		return ret;

	/* WaAllowUMDToModifyHDCChicken1:skl,bxt,kbl,glk,cfl */
	ret = wa_ring_whitelist_reg(engine, GEN8_HDC_CHICKEN1);
	if (ret)
		return ret;

	return 0;
}

static int skl_whitelist_workarounds_apply(struct intel_engine_cs *engine)
{
	int ret = gen9_whitelist_workarounds_apply(engine);
	if (ret)
		return ret;

	/* WaDisableLSQCROPERFforOCL:skl */
	ret = wa_ring_whitelist_reg(engine, GEN8_L3SQCREG4);
	if (ret)
		return ret;

	return 0;
}

static int bxt_whitelist_workarounds_apply(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;

	int ret = gen9_whitelist_workarounds_apply(engine);
	if (ret)
		return ret;

	/* WaDisableObjectLevelPreemptionForTrifanOrPolygon:bxt */
	/* WaDisableObjectLevelPreemptionForInstancedDraw:bxt */
	/* WaDisableObjectLevelPreemtionForInstanceId:bxt */
	/* WaDisableLSQCROPERFforOCL:bxt */
	if (IS_BXT_REVID(dev_priv, 0, BXT_REVID_A1)) {
		ret = wa_ring_whitelist_reg(engine, GEN9_CS_DEBUG_MODE1);
		if (ret)
			return ret;

		ret = wa_ring_whitelist_reg(engine, GEN8_L3SQCREG4);
		if (ret)
			return ret;
	}

	return 0;
}

static int kbl_whitelist_workarounds_apply(struct intel_engine_cs *engine)
{
	int ret = gen9_whitelist_workarounds_apply(engine);
	if (ret)
		return ret;

	/* WaDisableLSQCROPERFforOCL:kbl */
	ret = wa_ring_whitelist_reg(engine, GEN8_L3SQCREG4);
	if (ret)
		return ret;

	return 0;
}

static int glk_whitelist_workarounds_apply(struct intel_engine_cs *engine)
{
	int ret = gen9_whitelist_workarounds_apply(engine);
	if (ret)
		return ret;

	return 0;
}

static int cfl_whitelist_workarounds_apply(struct intel_engine_cs *engine)
{
	int ret = gen9_whitelist_workarounds_apply(engine);
	if (ret)
		return ret;

	return 0;
}

static int cnl_whitelist_workarounds_apply(struct intel_engine_cs *engine)
{
	int ret;

	/* WaEnablePreemptionGranularityControlByUMD:cnl */
	ret = wa_ring_whitelist_reg(engine, GEN8_CS_CHICKEN1);
	if (ret)
		return ret;

	return 0;
}

int i915_whitelist_workarounds_apply(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	int err;

	WARN_ON(engine->id != RCS);

	dev_priv->workarounds.hw_whitelist_count[engine->id] = 0;

	if (IS_SKYLAKE(dev_priv))
		err = skl_whitelist_workarounds_apply(engine);
	else if (IS_BROXTON(dev_priv))
		err = bxt_whitelist_workarounds_apply(engine);
	else if (IS_KABYLAKE(dev_priv))
		err = kbl_whitelist_workarounds_apply(engine);
	else if (IS_GEMINILAKE(dev_priv))
		err = glk_whitelist_workarounds_apply(engine);
	else if (IS_COFFEELAKE(dev_priv))
		err = cfl_whitelist_workarounds_apply(engine);
	else if (IS_CANNONLAKE(dev_priv))
		err = cnl_whitelist_workarounds_apply(engine);
	else
		err = 0;
	if (err)
		return err;

	DRM_DEBUG_DRIVER("%s: Number of whitelist w/a: %d\n", engine->name,
			 dev_priv->workarounds.hw_whitelist_count[engine->id]);
	return 0;
}
