/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2014-2018 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_workarounds.h"

static void wa_add(struct drm_i915_private *dev_priv,
		   i915_reg_t addr,
		   const u32 mask, const u32 val)
{
	const u32 idx = dev_priv->workarounds.count;

	I915_WRITE(addr, val);

	if (WARN_ON(idx >= I915_MAX_WA_REGS))
		return;

	dev_priv->workarounds.reg[idx].addr = addr;
	dev_priv->workarounds.reg[idx].value = val;
	dev_priv->workarounds.reg[idx].mask = mask;

	dev_priv->workarounds.count++;
}

#define WA_SET_BIT_MASKED(addr, mask) \
	wa_add(dev_priv, (addr), (mask), _MASKED_BIT_ENABLE(mask))

#define WA_CLR_BIT_MASKED(addr, mask) \
	wa_add(dev_priv, (addr), (mask), _MASKED_BIT_DISABLE(mask))

#define WA_SET_FIELD_MASKED(addr, mask, value) \
	wa_add(dev_priv, (addr), (mask), _MASKED_FIELD(mask, value))

static void gen8_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	WA_SET_BIT_MASKED(INSTPM, INSTPM_FORCE_ORDERING);

	/* WaDisableAsyncFlipPerfMode:bdw,chv */
	WA_SET_BIT_MASKED(MI_MODE, ASYNC_FLIP_PERF_DISABLE);

	/* WaDisablePartialInstShootdown:bdw,chv */
	WA_SET_BIT_MASKED(GEN8_ROW_CHICKEN,
			  PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE);

	/* Use Force Non-Coherent whenever executing a 3D context. This is a
	 * workaround for for a possible hang in the unlikely event a TLB
	 * invalidation occurs during a PSD flush.
	 */
	/* WaForceEnableNonCoherent:bdw,chv */
	/* WaHdcDisableFetchWhenMasked:bdw,chv */
	WA_SET_BIT_MASKED(HDC_CHICKEN0,
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
	WA_CLR_BIT_MASKED(CACHE_MODE_0_GEN7, HIZ_RAW_STALL_OPT_DISABLE);

	/* Wa4x4STCOptimizationDisable:bdw,chv */
	WA_SET_BIT_MASKED(CACHE_MODE_1, GEN8_4x4_STC_OPTIMIZATION_DISABLE);

	/*
	 * BSpec recommends 8x4 when MSAA is used,
	 * however in practice 16x4 seems fastest.
	 *
	 * Note that PS/WM thread counts depend on the WIZ hashing
	 * disable bit, which we don't touch here, but it's good
	 * to keep in mind (see 3DSTATE_PS and 3DSTATE_WM).
	 */
	WA_SET_FIELD_MASKED(GEN7_GT_MODE,
			    GEN6_WIZ_HASHING_MASK,
			    GEN6_WIZ_HASHING_16x4);
}

static void bdw_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen8_ctx_workarounds_apply(dev_priv);

	/* WaDisableThreadStallDopClockGating:bdw (pre-production) */
	WA_SET_BIT_MASKED(GEN8_ROW_CHICKEN, STALL_DOP_GATING_DISABLE);

	/* WaDisableDopClockGating:bdw
	 *
	 * Also see the related UCGTCL1 write in broadwell_init_clock_gating()
	 * to disable EUTC clock gating.
	 */
	WA_SET_BIT_MASKED(GEN7_ROW_CHICKEN2,
			  DOP_CLOCK_GATING_DISABLE);

	WA_SET_BIT_MASKED(HALF_SLICE_CHICKEN3,
			  GEN8_SAMPLER_POWER_BYPASS_DIS);

	WA_SET_BIT_MASKED(HDC_CHICKEN0,
			  /* WaForceContextSaveRestoreNonCoherent:bdw */
			  HDC_FORCE_CONTEXT_SAVE_RESTORE_NON_COHERENT |
			  /* WaDisableFenceDestinationToSLM:bdw (pre-prod) */
			  (IS_BDW_GT3(dev_priv) ? HDC_FENCE_DEST_SLM_DISABLE : 0));
}

static void chv_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen8_ctx_workarounds_apply(dev_priv);

	/* WaDisableThreadStallDopClockGating:chv */
	WA_SET_BIT_MASKED(GEN8_ROW_CHICKEN, STALL_DOP_GATING_DISABLE);

	/* Improve HiZ throughput on CHV. */
	WA_SET_BIT_MASKED(HIZ_CHICKEN, CHV_HZ_8X8_MODE_IN_1X);
}

static void gen9_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	if (HAS_LLC(dev_priv)) {
		/* WaCompressedResourceSamplerPbeMediaNewHashMode:skl,kbl
		 *
		 * Must match Display Engine. See
		 * WaCompressedResourceDisplayNewHashMode.
		 */
		WA_SET_BIT_MASKED(COMMON_SLICE_CHICKEN2,
				  GEN9_PBE_COMPRESSED_HASH_SELECTION);
		WA_SET_BIT_MASKED(GEN9_HALF_SLICE_CHICKEN7,
				  GEN9_SAMPLER_HASH_COMPRESSED_READ_ADDR);
	}

	/* WaClearFlowControlGpgpuContextSave:skl,bxt,kbl,glk,cfl */
	/* WaDisablePartialInstShootdown:skl,bxt,kbl,glk,cfl */
	WA_SET_BIT_MASKED(GEN8_ROW_CHICKEN,
			  FLOW_CONTROL_ENABLE |
			  PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE);

	/* Syncing dependencies between camera and graphics:skl,bxt,kbl */
	if (!IS_COFFEELAKE(dev_priv))
		WA_SET_BIT_MASKED(HALF_SLICE_CHICKEN3,
				  GEN9_DISABLE_OCL_OOB_SUPPRESS_LOGIC);

	/* WaEnableYV12BugFixInHalfSliceChicken7:skl,bxt,kbl,glk,cfl */
	/* WaEnableSamplerGPGPUPreemptionSupport:skl,bxt,kbl,cfl */
	WA_SET_BIT_MASKED(GEN9_HALF_SLICE_CHICKEN7,
			  GEN9_ENABLE_YV12_BUGFIX |
			  GEN9_ENABLE_GPGPU_PREEMPTION);

	/* Wa4x4STCOptimizationDisable:skl,bxt,kbl,glk,cfl */
	/* WaDisablePartialResolveInVc:skl,bxt,kbl,cfl */
	WA_SET_BIT_MASKED(CACHE_MODE_1, (GEN8_4x4_STC_OPTIMIZATION_DISABLE |
					 GEN9_PARTIAL_RESOLVE_IN_VC_DISABLE));

	/* WaCcsTlbPrefetchDisable:skl,bxt,kbl,glk,cfl */
	WA_CLR_BIT_MASKED(GEN9_HALF_SLICE_CHICKEN5,
			  GEN9_CCS_TLB_PREFETCH_ENABLE);

	/* WaForceContextSaveRestoreNonCoherent:skl,bxt,kbl,cfl */
	WA_SET_BIT_MASKED(HDC_CHICKEN0,
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
	WA_SET_BIT_MASKED(HDC_CHICKEN0,
			  HDC_FORCE_NON_COHERENT);

	/* WaDisableSamplerPowerBypassForSOPingPong:skl,bxt,kbl,cfl */
	if (IS_SKYLAKE(dev_priv) ||
	    IS_KABYLAKE(dev_priv) ||
	    IS_COFFEELAKE(dev_priv))
		WA_SET_BIT_MASKED(HALF_SLICE_CHICKEN3,
				  GEN8_SAMPLER_POWER_BYPASS_DIS);

	/* WaDisableSTUnitPowerOptimization:skl,bxt,kbl,glk,cfl */
	WA_SET_BIT_MASKED(HALF_SLICE_CHICKEN2, GEN8_ST_PO_DISABLE);

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
	WA_CLR_BIT_MASKED(GEN8_CS_CHICKEN1, GEN9_PREEMPT_3D_OBJECT_LEVEL);

	/* WaDisableGPGPUMidCmdPreemption:skl,bxt,blk,cfl,[cnl] */
	WA_SET_FIELD_MASKED(GEN8_CS_CHICKEN1, GEN9_PREEMPT_GPGPU_LEVEL_MASK,
			    GEN9_PREEMPT_GPGPU_COMMAND_LEVEL);
}

static void skl_tune_iz_hashing(struct drm_i915_private *dev_priv)
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
		return;

	/* Tune IZ hashing. See intel_device_info_runtime_init() */
	WA_SET_FIELD_MASKED(GEN7_GT_MODE,
			    GEN9_IZ_HASHING_MASK(2) |
			    GEN9_IZ_HASHING_MASK(1) |
			    GEN9_IZ_HASHING_MASK(0),
			    GEN9_IZ_HASHING(2, vals[2]) |
			    GEN9_IZ_HASHING(1, vals[1]) |
			    GEN9_IZ_HASHING(0, vals[0]));
}

static void skl_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen9_ctx_workarounds_apply(dev_priv);

	skl_tune_iz_hashing(dev_priv);
}

static void bxt_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen9_ctx_workarounds_apply(dev_priv);

	/* WaDisableThreadStallDopClockGating:bxt */
	WA_SET_BIT_MASKED(GEN8_ROW_CHICKEN,
			  STALL_DOP_GATING_DISABLE);

	/* WaToEnableHwFixForPushConstHWBug:bxt */
	WA_SET_BIT_MASKED(COMMON_SLICE_CHICKEN2,
			  GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION);
}

static void kbl_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen9_ctx_workarounds_apply(dev_priv);

	/* WaDisableFenceDestinationToSLM:kbl (pre-prod) */
	if (IS_KBL_REVID(dev_priv, KBL_REVID_A0, KBL_REVID_A0))
		WA_SET_BIT_MASKED(HDC_CHICKEN0,
				  HDC_FENCE_DEST_SLM_DISABLE);

	/* WaToEnableHwFixForPushConstHWBug:kbl */
	if (IS_KBL_REVID(dev_priv, KBL_REVID_C0, REVID_FOREVER))
		WA_SET_BIT_MASKED(COMMON_SLICE_CHICKEN2,
				  GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION);

	/* WaDisableSbeCacheDispatchPortSharing:kbl */
	WA_SET_BIT_MASKED(
		GEN7_HALF_SLICE_CHICKEN1,
		GEN7_SBE_SS_CACHE_DISPATCH_PORT_SHARING_DISABLE);
}

static void glk_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen9_ctx_workarounds_apply(dev_priv);

	/* WaToEnableHwFixForPushConstHWBug:glk */
	WA_SET_BIT_MASKED(COMMON_SLICE_CHICKEN2,
			  GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION);
}

static void cfl_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen9_ctx_workarounds_apply(dev_priv);

	/* WaToEnableHwFixForPushConstHWBug:cfl */
	WA_SET_BIT_MASKED(COMMON_SLICE_CHICKEN2,
			  GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION);

	/* WaDisableSbeCacheDispatchPortSharing:cfl */
	WA_SET_BIT_MASKED(
		GEN7_HALF_SLICE_CHICKEN1,
		GEN7_SBE_SS_CACHE_DISPATCH_PORT_SHARING_DISABLE);
}

static void cnl_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	/* WaForceContextSaveRestoreNonCoherent:cnl */
	WA_SET_BIT_MASKED(CNL_HDC_CHICKEN0,
			  HDC_FORCE_CONTEXT_SAVE_RESTORE_NON_COHERENT);

	/* WaThrottleEUPerfToAvoidTDBackPressure:cnl(pre-prod) */
	if (IS_CNL_REVID(dev_priv, CNL_REVID_B0, CNL_REVID_B0))
		WA_SET_BIT_MASKED(GEN8_ROW_CHICKEN, THROTTLE_12_5);

	/* WaDisableReplayBufferBankArbitrationOptimization:cnl */
	WA_SET_BIT_MASKED(COMMON_SLICE_CHICKEN2,
			  GEN8_SBE_DISABLE_REPLAY_BUF_OPTIMIZATION);

	/* WaDisableEnhancedSBEVertexCaching:cnl (pre-prod) */
	if (IS_CNL_REVID(dev_priv, 0, CNL_REVID_B0))
		WA_SET_BIT_MASKED(COMMON_SLICE_CHICKEN2,
				  GEN8_CSC2_SBE_VUE_CACHE_CONSERVATIVE);

	/* WaPushConstantDereferenceHoldDisable:cnl */
	WA_SET_BIT_MASKED(GEN7_ROW_CHICKEN2, PUSH_CONSTANT_DEREF_DISABLE);

	/* FtrEnableFastAnisoL1BankingFix:cnl */
	WA_SET_BIT_MASKED(HALF_SLICE_CHICKEN3, CNL_FAST_ANISO_L1_BANKING_FIX);

	/* WaDisable3DMidCmdPreemption:cnl */
	WA_CLR_BIT_MASKED(GEN8_CS_CHICKEN1, GEN9_PREEMPT_3D_OBJECT_LEVEL);

	/* WaDisableGPGPUMidCmdPreemption:cnl */
	WA_SET_FIELD_MASKED(GEN8_CS_CHICKEN1, GEN9_PREEMPT_GPGPU_LEVEL_MASK,
			    GEN9_PREEMPT_GPGPU_COMMAND_LEVEL);

	/* WaDisableEarlyEOT:cnl */
	WA_SET_BIT_MASKED(GEN8_ROW_CHICKEN, DISABLE_EARLY_EOT);
}

void intel_ctx_workarounds_apply(struct drm_i915_private *dev_priv)
{
	dev_priv->workarounds.count = 0;

	if (INTEL_GEN(dev_priv) < 8)
		return;
	else if (IS_BROADWELL(dev_priv))
		bdw_ctx_workarounds_apply(dev_priv);
	else if (IS_CHERRYVIEW(dev_priv))
		chv_ctx_workarounds_apply(dev_priv);
	else if (IS_SKYLAKE(dev_priv))
		skl_ctx_workarounds_apply(dev_priv);
	else if (IS_BROXTON(dev_priv))
		bxt_ctx_workarounds_apply(dev_priv);
	else if (IS_KABYLAKE(dev_priv))
		kbl_ctx_workarounds_apply(dev_priv);
	else if (IS_GEMINILAKE(dev_priv))
		glk_ctx_workarounds_apply(dev_priv);
	else if (IS_COFFEELAKE(dev_priv))
		cfl_ctx_workarounds_apply(dev_priv);
	else if (IS_CANNONLAKE(dev_priv))
		cnl_ctx_workarounds_apply(dev_priv);
	else
		MISSING_CASE(INTEL_GEN(dev_priv));

	DRM_DEBUG_DRIVER("Number of context specific w/a: %d\n",
			 dev_priv->workarounds.count);
}

static void bdw_gt_workarounds_apply(struct drm_i915_private *dev_priv)
{
}

static void chv_gt_workarounds_apply(struct drm_i915_private *dev_priv)
{
}

static void gen9_gt_workarounds_apply(struct drm_i915_private *dev_priv)
{
	/* WaContextSwitchWithConcurrentTLBInvalidate:skl,bxt,kbl,glk,cfl */
	I915_WRITE(GEN9_CSFE_CHICKEN1_RCS,
		   _MASKED_BIT_ENABLE(GEN9_PREEMPT_GPGPU_SYNC_SWITCH_DISABLE));

	/* WaEnableLbsSlaRetryTimerDecrement:skl,bxt,kbl,glk,cfl */
	I915_WRITE(BDW_SCRATCH1, I915_READ(BDW_SCRATCH1) |
		   GEN9_LBS_SLA_RETRY_TIMER_DECREMENT_ENABLE);

	/* WaDisableKillLogic:bxt,skl,kbl */
	if (!IS_COFFEELAKE(dev_priv))
		I915_WRITE(GAM_ECOCHK, I915_READ(GAM_ECOCHK) |
			   ECOCHK_DIS_TLB);

	if (HAS_LLC(dev_priv)) {
		/* WaCompressedResourceSamplerPbeMediaNewHashMode:skl,kbl
		 *
		 * Must match Display Engine. See
		 * WaCompressedResourceDisplayNewHashMode.
		 */
		I915_WRITE(MMCD_MISC_CTRL,
			   I915_READ(MMCD_MISC_CTRL) |
			   MMCD_PCLA |
			   MMCD_HOTSPOT_EN);
	}

	/* WaDisableHDCInvalidation:skl,bxt,kbl,cfl */
	I915_WRITE(GAM_ECOCHK, I915_READ(GAM_ECOCHK) |
		   BDW_DISABLE_HDC_INVALIDATION);

	/* WaProgramL3SqcReg1DefaultForPerf:bxt,glk */
	if (IS_GEN9_LP(dev_priv)) {
		u32 val = I915_READ(GEN8_L3SQCREG1);

		val &= ~L3_PRIO_CREDITS_MASK;
		val |= L3_GENERAL_PRIO_CREDITS(62) | L3_HIGH_PRIO_CREDITS(2);
		I915_WRITE(GEN8_L3SQCREG1, val);
	}

	/* WaOCLCoherentLineFlush:skl,bxt,kbl,cfl */
	I915_WRITE(GEN8_L3SQCREG4, (I915_READ(GEN8_L3SQCREG4) |
				    GEN8_LQSC_FLUSH_COHERENT_LINES));

	/* WaEnablePreemptionGranularityControlByUMD:skl,bxt,kbl,cfl,[cnl] */
	I915_WRITE(GEN7_FF_SLICE_CS_CHICKEN1,
		   _MASKED_BIT_ENABLE(GEN9_FFSC_PERCTX_PREEMPT_CTRL));
}

static void skl_gt_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen9_gt_workarounds_apply(dev_priv);

	/* WaEnableGapsTsvCreditFix:skl */
	I915_WRITE(GEN8_GARBCNTL, (I915_READ(GEN8_GARBCNTL) |
				   GEN9_GAPS_TSV_CREDIT_DISABLE));

	/* WaDisableGafsUnitClkGating:skl */
	I915_WRITE(GEN7_UCGCTL4, (I915_READ(GEN7_UCGCTL4) |
				  GEN8_EU_GAUNIT_CLOCK_GATE_DISABLE));

	/* WaInPlaceDecompressionHang:skl */
	if (IS_SKL_REVID(dev_priv, SKL_REVID_H0, REVID_FOREVER))
		I915_WRITE(GEN9_GAMT_ECO_REG_RW_IA,
			   (I915_READ(GEN9_GAMT_ECO_REG_RW_IA) |
			    GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS));
}

static void bxt_gt_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen9_gt_workarounds_apply(dev_priv);

	/* WaDisablePooledEuLoadBalancingFix:bxt */
	I915_WRITE(FF_SLICE_CS_CHICKEN2,
		   _MASKED_BIT_ENABLE(GEN9_POOLED_EU_LOAD_BALANCING_FIX_DISABLE));

	/* WaInPlaceDecompressionHang:bxt */
	I915_WRITE(GEN9_GAMT_ECO_REG_RW_IA,
		   (I915_READ(GEN9_GAMT_ECO_REG_RW_IA) |
		    GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS));
}

static void kbl_gt_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen9_gt_workarounds_apply(dev_priv);

	/* WaEnableGapsTsvCreditFix:kbl */
	I915_WRITE(GEN8_GARBCNTL, (I915_READ(GEN8_GARBCNTL) |
				   GEN9_GAPS_TSV_CREDIT_DISABLE));

	/* WaDisableDynamicCreditSharing:kbl */
	if (IS_KBL_REVID(dev_priv, 0, KBL_REVID_B0))
		I915_WRITE(GAMT_CHKN_BIT_REG,
			   (I915_READ(GAMT_CHKN_BIT_REG) |
			    GAMT_CHKN_DISABLE_DYNAMIC_CREDIT_SHARING));

	/* WaDisableGafsUnitClkGating:kbl */
	I915_WRITE(GEN7_UCGCTL4, (I915_READ(GEN7_UCGCTL4) |
				  GEN8_EU_GAUNIT_CLOCK_GATE_DISABLE));

	/* WaInPlaceDecompressionHang:kbl */
	I915_WRITE(GEN9_GAMT_ECO_REG_RW_IA,
		   (I915_READ(GEN9_GAMT_ECO_REG_RW_IA) |
		    GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS));
}

static void glk_gt_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen9_gt_workarounds_apply(dev_priv);
}

static void cfl_gt_workarounds_apply(struct drm_i915_private *dev_priv)
{
	gen9_gt_workarounds_apply(dev_priv);

	/* WaEnableGapsTsvCreditFix:cfl */
	I915_WRITE(GEN8_GARBCNTL, (I915_READ(GEN8_GARBCNTL) |
				   GEN9_GAPS_TSV_CREDIT_DISABLE));

	/* WaDisableGafsUnitClkGating:cfl */
	I915_WRITE(GEN7_UCGCTL4, (I915_READ(GEN7_UCGCTL4) |
				  GEN8_EU_GAUNIT_CLOCK_GATE_DISABLE));

	/* WaInPlaceDecompressionHang:cfl */
	I915_WRITE(GEN9_GAMT_ECO_REG_RW_IA,
		   (I915_READ(GEN9_GAMT_ECO_REG_RW_IA) |
		    GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS));
}

static void cnl_gt_workarounds_apply(struct drm_i915_private *dev_priv)
{
	/* WaDisableI2mCycleOnWRPort:cnl (pre-prod) */
	if (IS_CNL_REVID(dev_priv, CNL_REVID_B0, CNL_REVID_B0))
		I915_WRITE(GAMT_CHKN_BIT_REG,
			   (I915_READ(GAMT_CHKN_BIT_REG) |
			    GAMT_CHKN_DISABLE_I2M_CYCLE_ON_WR_PORT));

	/* WaInPlaceDecompressionHang:cnl */
	I915_WRITE(GEN9_GAMT_ECO_REG_RW_IA,
		   (I915_READ(GEN9_GAMT_ECO_REG_RW_IA) |
		    GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS));

	/* WaEnablePreemptionGranularityControlByUMD:cnl */
	I915_WRITE(GEN7_FF_SLICE_CS_CHICKEN1,
		   _MASKED_BIT_ENABLE(GEN9_FFSC_PERCTX_PREEMPT_CTRL));
}

void intel_gt_workarounds_apply(struct drm_i915_private *dev_priv)
{
	if (INTEL_GEN(dev_priv) < 8)
		return;
	else if (IS_BROADWELL(dev_priv))
		bdw_gt_workarounds_apply(dev_priv);
	else if (IS_CHERRYVIEW(dev_priv))
		chv_gt_workarounds_apply(dev_priv);
	else if (IS_SKYLAKE(dev_priv))
		skl_gt_workarounds_apply(dev_priv);
	else if (IS_BROXTON(dev_priv))
		bxt_gt_workarounds_apply(dev_priv);
	else if (IS_KABYLAKE(dev_priv))
		kbl_gt_workarounds_apply(dev_priv);
	else if (IS_GEMINILAKE(dev_priv))
		glk_gt_workarounds_apply(dev_priv);
	else if (IS_COFFEELAKE(dev_priv))
		cfl_gt_workarounds_apply(dev_priv);
	else if (IS_CANNONLAKE(dev_priv))
		cnl_gt_workarounds_apply(dev_priv);
	else
		MISSING_CASE(INTEL_GEN(dev_priv));
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
	int ret = gen9_whitelist_workarounds_apply(engine);
	if (ret)
		return ret;

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

	/* WA #0862: Userspace has to set "Barrier Mode" to avoid hangs. */
	ret = wa_ring_whitelist_reg(engine, GEN9_SLICE_COMMON_ECO_CHICKEN1);
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

int intel_whitelist_workarounds_apply(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	int err;

	WARN_ON(engine->id != RCS);

	dev_priv->workarounds.hw_whitelist_count[engine->id] = 0;

	if (INTEL_GEN(dev_priv) < 9) {
		WARN(1, "No whitelisting in Gen%u\n", INTEL_GEN(dev_priv));
		err = 0;
	} else if (IS_SKYLAKE(dev_priv))
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
	else {
		MISSING_CASE(INTEL_GEN(dev_priv));
		err = 0;
	}
	if (err)
		return err;

	DRM_DEBUG_DRIVER("%s: Number of whitelist w/a: %d\n", engine->name,
			 dev_priv->workarounds.hw_whitelist_count[engine->id]);
	return 0;
}

/*
 * In this WA we need to set GEN8_L3SQCREG4[21:21] and reset it after
 * PIPE_CONTROL instruction. This is required for the flush to happen correctly
 * but there is a slight complication as this is applied in WA batch where the
 * values are only initialized once so we cannot take register value at the
 * beginning and reuse it further; hence we save its value to memory, upload a
 * constant value with bit21 set and then we restore it back with the saved value.
 * To simplify the WA, a constant value is formed by using the default value
 * of this register. This shouldn't be a problem because we are only modifying
 * it for a short period and this batch in non-premptible. We can ofcourse
 * use additional instructions that read the actual value of the register
 * at that time and set our bit of interest but it makes the WA complicated.
 *
 * This WA is also required for Gen9 so extracting as a function avoids
 * code duplication.
 */
static u32 *
gen8_emit_flush_coherentl3_wa(struct intel_engine_cs *engine, u32 *batch)
{
	*batch++ = MI_STORE_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
	*batch++ = i915_mmio_reg_offset(GEN8_L3SQCREG4);
	*batch++ = i915_ggtt_offset(engine->scratch) + 256;
	*batch++ = 0;

	*batch++ = MI_LOAD_REGISTER_IMM(1);
	*batch++ = i915_mmio_reg_offset(GEN8_L3SQCREG4);
	*batch++ = 0x40400000 | GEN8_LQSC_FLUSH_COHERENT_LINES;

	batch = gen8_emit_pipe_control(batch,
				       PIPE_CONTROL_CS_STALL |
				       PIPE_CONTROL_DC_FLUSH_ENABLE,
				       0);

	*batch++ = MI_LOAD_REGISTER_MEM_GEN8 | MI_SRM_LRM_GLOBAL_GTT;
	*batch++ = i915_mmio_reg_offset(GEN8_L3SQCREG4);
	*batch++ = i915_ggtt_offset(engine->scratch) + 256;
	*batch++ = 0;

	return batch;
}

/*
 * Typically we only have one indirect_ctx and per_ctx batch buffer which are
 * initialized at the beginning and shared across all contexts but this field
 * helps us to have multiple batches at different offsets and select them based
 * on a criteria. At the moment this batch always start at the beginning of the page
 * and at this point we don't have multiple wa_ctx batch buffers.
 *
 * The number of WA applied are not known at the beginning; we use this field
 * to return the no of DWORDS written.
 *
 * It is to be noted that this batch does not contain MI_BATCH_BUFFER_END
 * so it adds NOOPs as padding to make it cacheline aligned.
 * MI_BATCH_BUFFER_END will be added to perctx batch and both of them together
 * makes a complete batch buffer.
 */
static u32 *gen8_init_indirectctx_bb(struct intel_engine_cs *engine, u32 *batch)
{
	/* WaDisableCtxRestoreArbitration:bdw,chv */
	*batch++ = MI_ARB_ON_OFF | MI_ARB_DISABLE;

	/* WaFlushCoherentL3CacheLinesAtContextSwitch:bdw */
	if (IS_BROADWELL(engine->i915))
		batch = gen8_emit_flush_coherentl3_wa(engine, batch);

	/* WaClearSlmSpaceAtContextSwitch:bdw,chv */
	/* Actual scratch location is at 128 bytes offset */
	batch = gen8_emit_pipe_control(batch,
				       PIPE_CONTROL_FLUSH_L3 |
				       PIPE_CONTROL_GLOBAL_GTT_IVB |
				       PIPE_CONTROL_CS_STALL |
				       PIPE_CONTROL_QW_WRITE,
				       i915_ggtt_offset(engine->scratch) +
				       2 * CACHELINE_BYTES);

	*batch++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;

	/* Pad to end of cacheline */
	while ((unsigned long)batch % CACHELINE_BYTES)
		*batch++ = MI_NOOP;

	/*
	 * MI_BATCH_BUFFER_END is not required in Indirect ctx BB because
	 * execution depends on the length specified in terms of cache lines
	 * in the register CTX_RCS_INDIRECT_CTX
	 */

	return batch;
}

static u32 *gen9_init_indirectctx_bb(struct intel_engine_cs *engine, u32 *batch)
{
	*batch++ = MI_ARB_ON_OFF | MI_ARB_DISABLE;

	/* WaFlushCoherentL3CacheLinesAtContextSwitch:skl,bxt,glk */
	batch = gen8_emit_flush_coherentl3_wa(engine, batch);

	/* WaDisableGatherAtSetShaderCommonSlice:skl,bxt,kbl,glk */
	*batch++ = MI_LOAD_REGISTER_IMM(1);
	*batch++ = i915_mmio_reg_offset(COMMON_SLICE_CHICKEN2);
	*batch++ = _MASKED_BIT_DISABLE(
			GEN9_DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE);
	*batch++ = MI_NOOP;

	/* WaClearSlmSpaceAtContextSwitch:kbl */
	/* Actual scratch location is at 128 bytes offset */
	if (IS_KBL_REVID(engine->i915, 0, KBL_REVID_A0)) {
		batch = gen8_emit_pipe_control(batch,
					       PIPE_CONTROL_FLUSH_L3 |
					       PIPE_CONTROL_GLOBAL_GTT_IVB |
					       PIPE_CONTROL_CS_STALL |
					       PIPE_CONTROL_QW_WRITE,
					       i915_ggtt_offset(engine->scratch)
					       + 2 * CACHELINE_BYTES);
	}

	/* WaMediaPoolStateCmdInWABB:bxt,glk */
	if (HAS_POOLED_EU(engine->i915)) {
		/*
		 * EU pool configuration is setup along with golden context
		 * during context initialization. This value depends on
		 * device type (2x6 or 3x6) and needs to be updated based
		 * on which subslice is disabled especially for 2x6
		 * devices, however it is safe to load default
		 * configuration of 3x6 device instead of masking off
		 * corresponding bits because HW ignores bits of a disabled
		 * subslice and drops down to appropriate config. Please
		 * see render_state_setup() in i915_gem_render_state.c for
		 * possible configurations, to avoid duplication they are
		 * not shown here again.
		 */
		*batch++ = GEN9_MEDIA_POOL_STATE;
		*batch++ = GEN9_MEDIA_POOL_ENABLE;
		*batch++ = 0x00777000;
		*batch++ = 0;
		*batch++ = 0;
		*batch++ = 0;
	}

	*batch++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;

	/* Pad to end of cacheline */
	while ((unsigned long)batch % CACHELINE_BYTES)
		*batch++ = MI_NOOP;

	return batch;
}

static u32 *
gen10_init_indirectctx_bb(struct intel_engine_cs *engine, u32 *batch)
{
	int i;

	/*
	 * WaPipeControlBefore3DStateSamplePattern: cnl
	 *
	 * Ensure the engine is idle prior to programming a
	 * 3DSTATE_SAMPLE_PATTERN during a context restore.
	 */
	batch = gen8_emit_pipe_control(batch,
				       PIPE_CONTROL_CS_STALL,
				       0);
	/*
	 * WaPipeControlBefore3DStateSamplePattern says we need 4 dwords for
	 * the PIPE_CONTROL followed by 12 dwords of 0x0, so 16 dwords in
	 * total. However, a PIPE_CONTROL is 6 dwords long, not 4, which is
	 * confusing. Since gen8_emit_pipe_control() already advances the
	 * batch by 6 dwords, we advance the other 10 here, completing a
	 * cacheline. It's not clear if the workaround requires this padding
	 * before other commands, or if it's just the regular padding we would
	 * already have for the workaround bb, so leave it here for now.
	 */
	for (i = 0; i < 10; i++)
		*batch++ = MI_NOOP;

	/* Pad to end of cacheline */
	while ((unsigned long)batch % CACHELINE_BYTES)
		*batch++ = MI_NOOP;

	return batch;
}

#define CTX_WA_BB_OBJ_SIZE (PAGE_SIZE)

static int lrc_setup_wa_ctx(struct intel_engine_cs *engine)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	int err;

	obj = i915_gem_object_create(engine->i915, CTX_WA_BB_OBJ_SIZE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	vma = i915_vma_instance(obj, &engine->i915->ggtt.base, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto err;
	}

	err = i915_vma_pin(vma, 0, PAGE_SIZE, PIN_GLOBAL | PIN_HIGH);
	if (err)
		goto err;

	engine->wa_ctx.vma = vma;
	return 0;

err:
	i915_gem_object_put(obj);
	return err;
}

static void lrc_destroy_wa_ctx(struct intel_engine_cs *engine)
{
	i915_vma_unpin_and_release(&engine->wa_ctx.vma);
}

typedef u32 *(*wa_bb_func_t)(struct intel_engine_cs *engine, u32 *batch);

int intel_engine_init_bb_workarounds(struct intel_engine_cs *engine)
{
	struct i915_ctx_workarounds *wa_ctx = &engine->wa_ctx;
	struct i915_wa_ctx_bb *wa_bb[2] = { &wa_ctx->indirect_ctx,
					    &wa_ctx->per_ctx };
	wa_bb_func_t wa_bb_fn[2];
	struct page *page;
	void *batch, *batch_ptr;
	unsigned int i;
	int ret;

	if (GEM_WARN_ON(engine->id != RCS))
		return -EINVAL;

	switch (INTEL_GEN(engine->i915)) {
	case 10:
		wa_bb_fn[0] = gen10_init_indirectctx_bb;
		wa_bb_fn[1] = NULL;
		break;
	case 9:
		wa_bb_fn[0] = gen9_init_indirectctx_bb;
		wa_bb_fn[1] = NULL;
		break;
	case 8:
		wa_bb_fn[0] = gen8_init_indirectctx_bb;
		wa_bb_fn[1] = NULL;
		break;
	default:
		MISSING_CASE(INTEL_GEN(engine->i915));
		return 0;
	}

	ret = lrc_setup_wa_ctx(engine);
	if (ret) {
		DRM_DEBUG_DRIVER("Failed to setup context WA page: %d\n", ret);
		return ret;
	}

	page = i915_gem_object_get_dirty_page(wa_ctx->vma->obj, 0);
	batch = batch_ptr = kmap_atomic(page);

	/*
	 * Emit the two workaround batch buffers, recording the offset from the
	 * start of the workaround batch buffer object for each and their
	 * respective sizes.
	 */
	for (i = 0; i < ARRAY_SIZE(wa_bb_fn); i++) {
		wa_bb[i]->offset = batch_ptr - batch;
		if (GEM_WARN_ON(!IS_ALIGNED(wa_bb[i]->offset,
					    CACHELINE_BYTES))) {
			ret = -EINVAL;
			break;
		}
		if (wa_bb_fn[i])
			batch_ptr = wa_bb_fn[i](engine, batch_ptr);
		wa_bb[i]->size = batch_ptr - (batch + wa_bb[i]->offset);
	}

	BUG_ON(batch_ptr - batch > CTX_WA_BB_OBJ_SIZE);

	kunmap_atomic(batch);
	if (ret)
		lrc_destroy_wa_ctx(engine);

	return ret;
}

void intel_engine_fini_bb_workarounds(struct intel_engine_cs *engine)
{
	lrc_destroy_wa_ctx(engine);
}
