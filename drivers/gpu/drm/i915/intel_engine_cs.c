/*
 * Copyright © 2016 Intel Corporation
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
#include "intel_ringbuffer.h"
#include "intel_lrc.h"

/* Haswell does have the CXT_SIZE register however it does not appear to be
 * valid. Now, docs explain in dwords what is in the context object. The full
 * size is 70720 bytes, however, the power context and execlist context will
 * never be saved (power context is stored elsewhere, and execlists don't work
 * on HSW) - so the final size, including the extra state required for the
 * Resource Streamer, is 66944 bytes, which rounds to 17 pages.
 */
#define HSW_CXT_TOTAL_SIZE		(17 * PAGE_SIZE)
/* Same as Haswell, but 72064 bytes now. */
#define GEN8_CXT_TOTAL_SIZE		(18 * PAGE_SIZE)

#define GEN8_LR_CONTEXT_RENDER_SIZE	(20 * PAGE_SIZE)
#define GEN9_LR_CONTEXT_RENDER_SIZE	(22 * PAGE_SIZE)
#define GEN10_LR_CONTEXT_RENDER_SIZE	(18 * PAGE_SIZE)

#define GEN8_LR_CONTEXT_OTHER_SIZE	( 2 * PAGE_SIZE)

struct engine_class_info {
	const char *name;
	int (*init_legacy)(struct intel_engine_cs *engine);
	int (*init_execlists)(struct intel_engine_cs *engine);
};

static const struct engine_class_info intel_engine_classes[] = {
	[RENDER_CLASS] = {
		.name = "rcs",
		.init_execlists = logical_render_ring_init,
		.init_legacy = intel_init_render_ring_buffer,
	},
	[COPY_ENGINE_CLASS] = {
		.name = "bcs",
		.init_execlists = logical_xcs_ring_init,
		.init_legacy = intel_init_blt_ring_buffer,
	},
	[VIDEO_DECODE_CLASS] = {
		.name = "vcs",
		.init_execlists = logical_xcs_ring_init,
		.init_legacy = intel_init_bsd_ring_buffer,
	},
	[VIDEO_ENHANCEMENT_CLASS] = {
		.name = "vecs",
		.init_execlists = logical_xcs_ring_init,
		.init_legacy = intel_init_vebox_ring_buffer,
	},
};

struct engine_info {
	unsigned int hw_id;
	unsigned int uabi_id;
	u8 class;
	u8 instance;
	u32 mmio_base;
	unsigned irq_shift;
};

static const struct engine_info intel_engines[] = {
	[RCS] = {
		.hw_id = RCS_HW,
		.uabi_id = I915_EXEC_RENDER,
		.class = RENDER_CLASS,
		.instance = 0,
		.mmio_base = RENDER_RING_BASE,
		.irq_shift = GEN8_RCS_IRQ_SHIFT,
	},
	[BCS] = {
		.hw_id = BCS_HW,
		.uabi_id = I915_EXEC_BLT,
		.class = COPY_ENGINE_CLASS,
		.instance = 0,
		.mmio_base = BLT_RING_BASE,
		.irq_shift = GEN8_BCS_IRQ_SHIFT,
	},
	[VCS] = {
		.hw_id = VCS_HW,
		.uabi_id = I915_EXEC_BSD,
		.class = VIDEO_DECODE_CLASS,
		.instance = 0,
		.mmio_base = GEN6_BSD_RING_BASE,
		.irq_shift = GEN8_VCS1_IRQ_SHIFT,
	},
	[VCS2] = {
		.hw_id = VCS2_HW,
		.uabi_id = I915_EXEC_BSD,
		.class = VIDEO_DECODE_CLASS,
		.instance = 1,
		.mmio_base = GEN8_BSD2_RING_BASE,
		.irq_shift = GEN8_VCS2_IRQ_SHIFT,
	},
	[VECS] = {
		.hw_id = VECS_HW,
		.uabi_id = I915_EXEC_VEBOX,
		.class = VIDEO_ENHANCEMENT_CLASS,
		.instance = 0,
		.mmio_base = VEBOX_RING_BASE,
		.irq_shift = GEN8_VECS_IRQ_SHIFT,
	},
};

/**
 * ___intel_engine_context_size() - return the size of the context for an engine
 * @dev_priv: i915 device private
 * @class: engine class
 *
 * Each engine class may require a different amount of space for a context
 * image.
 *
 * Return: size (in bytes) of an engine class specific context image
 *
 * Note: this size includes the HWSP, which is part of the context image
 * in LRC mode, but does not include the "shared data page" used with
 * GuC submission. The caller should account for this if using the GuC.
 */
static u32
__intel_engine_context_size(struct drm_i915_private *dev_priv, u8 class)
{
	u32 cxt_size;

	BUILD_BUG_ON(I915_GTT_PAGE_SIZE != PAGE_SIZE);

	switch (class) {
	case RENDER_CLASS:
		switch (INTEL_GEN(dev_priv)) {
		default:
			MISSING_CASE(INTEL_GEN(dev_priv));
		case 10:
			return GEN10_LR_CONTEXT_RENDER_SIZE;
		case 9:
			return GEN9_LR_CONTEXT_RENDER_SIZE;
		case 8:
			return i915_modparams.enable_execlists ?
			       GEN8_LR_CONTEXT_RENDER_SIZE :
			       GEN8_CXT_TOTAL_SIZE;
		case 7:
			if (IS_HASWELL(dev_priv))
				return HSW_CXT_TOTAL_SIZE;

			cxt_size = I915_READ(GEN7_CXT_SIZE);
			return round_up(GEN7_CXT_TOTAL_SIZE(cxt_size) * 64,
					PAGE_SIZE);
		case 6:
			cxt_size = I915_READ(CXT_SIZE);
			return round_up(GEN6_CXT_TOTAL_SIZE(cxt_size) * 64,
					PAGE_SIZE);
		case 5:
		case 4:
		case 3:
		case 2:
		/* For the special day when i810 gets merged. */
		case 1:
			return 0;
		}
		break;
	default:
		MISSING_CASE(class);
	case VIDEO_DECODE_CLASS:
	case VIDEO_ENHANCEMENT_CLASS:
	case COPY_ENGINE_CLASS:
		if (INTEL_GEN(dev_priv) < 8)
			return 0;
		return GEN8_LR_CONTEXT_OTHER_SIZE;
	}
}

static int
intel_engine_setup(struct drm_i915_private *dev_priv,
		   enum intel_engine_id id)
{
	const struct engine_info *info = &intel_engines[id];
	const struct engine_class_info *class_info;
	struct intel_engine_cs *engine;

	GEM_BUG_ON(info->class >= ARRAY_SIZE(intel_engine_classes));
	class_info = &intel_engine_classes[info->class];

	GEM_BUG_ON(dev_priv->engine[id]);
	engine = kzalloc(sizeof(*engine), GFP_KERNEL);
	if (!engine)
		return -ENOMEM;

	engine->id = id;
	engine->i915 = dev_priv;
	WARN_ON(snprintf(engine->name, sizeof(engine->name), "%s%u",
			 class_info->name, info->instance) >=
		sizeof(engine->name));
	engine->uabi_id = info->uabi_id;
	engine->hw_id = engine->guc_id = info->hw_id;
	engine->mmio_base = info->mmio_base;
	engine->irq_shift = info->irq_shift;
	engine->class = info->class;
	engine->instance = info->instance;

	engine->context_size = __intel_engine_context_size(dev_priv,
							   engine->class);
	if (WARN_ON(engine->context_size > BIT(20)))
		engine->context_size = 0;

	/* Nothing to do here, execute in order of dependencies */
	engine->schedule = NULL;

	ATOMIC_INIT_NOTIFIER_HEAD(&engine->context_status_notifier);

	dev_priv->engine[id] = engine;
	return 0;
}

/**
 * intel_engines_init_mmio() - allocate and prepare the Engine Command Streamers
 * @dev_priv: i915 device private
 *
 * Return: non-zero if the initialization failed.
 */
int intel_engines_init_mmio(struct drm_i915_private *dev_priv)
{
	struct intel_device_info *device_info = mkwrite_device_info(dev_priv);
	const unsigned int ring_mask = INTEL_INFO(dev_priv)->ring_mask;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	unsigned int mask = 0;
	unsigned int i;
	int err;

	WARN_ON(ring_mask == 0);
	WARN_ON(ring_mask &
		GENMASK(sizeof(mask) * BITS_PER_BYTE - 1, I915_NUM_ENGINES));

	for (i = 0; i < ARRAY_SIZE(intel_engines); i++) {
		if (!HAS_ENGINE(dev_priv, i))
			continue;

		err = intel_engine_setup(dev_priv, i);
		if (err)
			goto cleanup;

		mask |= ENGINE_MASK(i);
	}

	/*
	 * Catch failures to update intel_engines table when the new engines
	 * are added to the driver by a warning and disabling the forgotten
	 * engines.
	 */
	if (WARN_ON(mask != ring_mask))
		device_info->ring_mask = mask;

	/* We always presume we have at least RCS available for later probing */
	if (WARN_ON(!HAS_ENGINE(dev_priv, RCS))) {
		err = -ENODEV;
		goto cleanup;
	}

	device_info->num_rings = hweight32(mask);

	return 0;

cleanup:
	for_each_engine(engine, dev_priv, id)
		kfree(engine);
	return err;
}

/**
 * intel_engines_init() - init the Engine Command Streamers
 * @dev_priv: i915 device private
 *
 * Return: non-zero if the initialization failed.
 */
int intel_engines_init(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id, err_id;
	int err;

	for_each_engine(engine, dev_priv, id) {
		const struct engine_class_info *class_info =
			&intel_engine_classes[engine->class];
		int (*init)(struct intel_engine_cs *engine);

		if (i915_modparams.enable_execlists)
			init = class_info->init_execlists;
		else
			init = class_info->init_legacy;

		err = -EINVAL;
		err_id = id;

		if (GEM_WARN_ON(!init))
			goto cleanup;

		err = init(engine);
		if (err)
			goto cleanup;

		GEM_BUG_ON(!engine->submit_request);
	}

	return 0;

cleanup:
	for_each_engine(engine, dev_priv, id) {
		if (id >= err_id) {
			kfree(engine);
			dev_priv->engine[id] = NULL;
		} else {
			dev_priv->gt.cleanup_engine(engine);
		}
	}
	return err;
}

void intel_engine_init_global_seqno(struct intel_engine_cs *engine, u32 seqno)
{
	struct drm_i915_private *dev_priv = engine->i915;

	/* Our semaphore implementation is strictly monotonic (i.e. we proceed
	 * so long as the semaphore value in the register/page is greater
	 * than the sync value), so whenever we reset the seqno,
	 * so long as we reset the tracking semaphore value to 0, it will
	 * always be before the next request's seqno. If we don't reset
	 * the semaphore value, then when the seqno moves backwards all
	 * future waits will complete instantly (causing rendering corruption).
	 */
	if (IS_GEN6(dev_priv) || IS_GEN7(dev_priv)) {
		I915_WRITE(RING_SYNC_0(engine->mmio_base), 0);
		I915_WRITE(RING_SYNC_1(engine->mmio_base), 0);
		if (HAS_VEBOX(dev_priv))
			I915_WRITE(RING_SYNC_2(engine->mmio_base), 0);
	}
	if (dev_priv->semaphore) {
		struct page *page = i915_vma_first_page(dev_priv->semaphore);
		void *semaphores;

		/* Semaphores are in noncoherent memory, flush to be safe */
		semaphores = kmap_atomic(page);
		memset(semaphores + GEN8_SEMAPHORE_OFFSET(engine->id, 0),
		       0, I915_NUM_ENGINES * gen8_semaphore_seqno_size);
		drm_clflush_virt_range(semaphores + GEN8_SEMAPHORE_OFFSET(engine->id, 0),
				       I915_NUM_ENGINES * gen8_semaphore_seqno_size);
		kunmap_atomic(semaphores);
	}

	intel_write_status_page(engine, I915_GEM_HWS_INDEX, seqno);
	clear_bit(ENGINE_IRQ_BREADCRUMB, &engine->irq_posted);

	/* After manually advancing the seqno, fake the interrupt in case
	 * there are any waiters for that seqno.
	 */
	intel_engine_wakeup(engine);

	GEM_BUG_ON(intel_engine_get_seqno(engine) != seqno);
}

static void intel_engine_init_timeline(struct intel_engine_cs *engine)
{
	engine->timeline = &engine->i915->gt.global_timeline.engine[engine->id];
}

static bool csb_force_mmio(struct drm_i915_private *i915)
{
	/* GVT emulation depends upon intercepting CSB mmio */
	if (intel_vgpu_active(i915))
		return true;

	/*
	 * IOMMU adds unpredictable latency causing the CSB write (from the
	 * GPU into the HWSP) to only be visible some time after the interrupt
	 * (missed breadcrumb syndrome).
	 */
	if (intel_vtd_active())
		return true;

	return false;
}

static void intel_engine_init_execlist(struct intel_engine_cs *engine)
{
	struct intel_engine_execlists * const execlists = &engine->execlists;

	execlists->csb_use_mmio = csb_force_mmio(engine->i915);

	execlists->port_mask = 1;
	BUILD_BUG_ON_NOT_POWER_OF_2(execlists_num_ports(execlists));
	GEM_BUG_ON(execlists_num_ports(execlists) > EXECLIST_MAX_PORTS);

	execlists->queue = RB_ROOT;
	execlists->first = NULL;
}

/**
 * intel_engines_setup_common - setup engine state not requiring hw access
 * @engine: Engine to setup.
 *
 * Initializes @engine@ structure members shared between legacy and execlists
 * submission modes which do not require hardware access.
 *
 * Typically done early in the submission mode specific engine setup stage.
 */
void intel_engine_setup_common(struct intel_engine_cs *engine)
{
	intel_engine_init_execlist(engine);

	intel_engine_init_timeline(engine);
	intel_engine_init_hangcheck(engine);
	i915_gem_batch_pool_init(engine, &engine->batch_pool);

	intel_engine_init_cmd_parser(engine);
}

int intel_engine_create_scratch(struct intel_engine_cs *engine, int size)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	int ret;

	WARN_ON(engine->scratch);

	obj = i915_gem_object_create_stolen(engine->i915, size);
	if (!obj)
		obj = i915_gem_object_create_internal(engine->i915, size);
	if (IS_ERR(obj)) {
		DRM_ERROR("Failed to allocate scratch page\n");
		return PTR_ERR(obj);
	}

	vma = i915_vma_instance(obj, &engine->i915->ggtt.base, NULL);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto err_unref;
	}

	ret = i915_vma_pin(vma, 0, 4096, PIN_GLOBAL | PIN_HIGH);
	if (ret)
		goto err_unref;

	engine->scratch = vma;
	DRM_DEBUG_DRIVER("%s pipe control offset: 0x%08x\n",
			 engine->name, i915_ggtt_offset(vma));
	return 0;

err_unref:
	i915_gem_object_put(obj);
	return ret;
}

static void intel_engine_cleanup_scratch(struct intel_engine_cs *engine)
{
	i915_vma_unpin_and_release(&engine->scratch);
}

static void cleanup_phys_status_page(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;

	if (!dev_priv->status_page_dmah)
		return;

	drm_pci_free(&dev_priv->drm, dev_priv->status_page_dmah);
	engine->status_page.page_addr = NULL;
}

static void cleanup_status_page(struct intel_engine_cs *engine)
{
	struct i915_vma *vma;
	struct drm_i915_gem_object *obj;

	vma = fetch_and_zero(&engine->status_page.vma);
	if (!vma)
		return;

	obj = vma->obj;

	i915_vma_unpin(vma);
	i915_vma_close(vma);

	i915_gem_object_unpin_map(obj);
	__i915_gem_object_release_unless_active(obj);
}

static int init_status_page(struct intel_engine_cs *engine)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	unsigned int flags;
	void *vaddr;
	int ret;

	obj = i915_gem_object_create_internal(engine->i915, PAGE_SIZE);
	if (IS_ERR(obj)) {
		DRM_ERROR("Failed to allocate status page\n");
		return PTR_ERR(obj);
	}

	ret = i915_gem_object_set_cache_level(obj, I915_CACHE_LLC);
	if (ret)
		goto err;

	vma = i915_vma_instance(obj, &engine->i915->ggtt.base, NULL);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto err;
	}

	flags = PIN_GLOBAL;
	if (!HAS_LLC(engine->i915))
		/* On g33, we cannot place HWS above 256MiB, so
		 * restrict its pinning to the low mappable arena.
		 * Though this restriction is not documented for
		 * gen4, gen5, or byt, they also behave similarly
		 * and hang if the HWS is placed at the top of the
		 * GTT. To generalise, it appears that all !llc
		 * platforms have issues with us placing the HWS
		 * above the mappable region (even though we never
		 * actually map it).
		 */
		flags |= PIN_MAPPABLE;
	else
		flags |= PIN_HIGH;
	ret = i915_vma_pin(vma, 0, 4096, flags);
	if (ret)
		goto err;

	vaddr = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(vaddr)) {
		ret = PTR_ERR(vaddr);
		goto err_unpin;
	}

	engine->status_page.vma = vma;
	engine->status_page.ggtt_offset = i915_ggtt_offset(vma);
	engine->status_page.page_addr = memset(vaddr, 0, PAGE_SIZE);

	DRM_DEBUG_DRIVER("%s hws offset: 0x%08x\n",
			 engine->name, i915_ggtt_offset(vma));
	return 0;

err_unpin:
	i915_vma_unpin(vma);
err:
	i915_gem_object_put(obj);
	return ret;
}

static int init_phys_status_page(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;

	GEM_BUG_ON(engine->id != RCS);

	dev_priv->status_page_dmah =
		drm_pci_alloc(&dev_priv->drm, PAGE_SIZE, PAGE_SIZE);
	if (!dev_priv->status_page_dmah)
		return -ENOMEM;

	engine->status_page.page_addr = dev_priv->status_page_dmah->vaddr;
	memset(engine->status_page.page_addr, 0, PAGE_SIZE);

	return 0;
}

/**
 * intel_engines_init_common - initialize cengine state which might require hw access
 * @engine: Engine to initialize.
 *
 * Initializes @engine@ structure members shared between legacy and execlists
 * submission modes which do require hardware access.
 *
 * Typcally done at later stages of submission mode specific engine setup.
 *
 * Returns zero on success or an error code on failure.
 */
int intel_engine_init_common(struct intel_engine_cs *engine)
{
	struct intel_ring *ring;
	int ret;

	engine->set_default_submission(engine);

	/* We may need to do things with the shrinker which
	 * require us to immediately switch back to the default
	 * context. This can cause a problem as pinning the
	 * default context also requires GTT space which may not
	 * be available. To avoid this we always pin the default
	 * context.
	 */
	ring = engine->context_pin(engine, engine->i915->kernel_context);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	/*
	 * Similarly the preempt context must always be available so that
	 * we can interrupt the engine at any time.
	 */
	if (INTEL_INFO(engine->i915)->has_logical_ring_preemption) {
		ring = engine->context_pin(engine,
					   engine->i915->preempt_context);
		if (IS_ERR(ring)) {
			ret = PTR_ERR(ring);
			goto err_unpin_kernel;
		}
	}

	ret = intel_engine_init_breadcrumbs(engine);
	if (ret)
		goto err_unpin_preempt;

	ret = i915_gem_render_state_init(engine);
	if (ret)
		goto err_breadcrumbs;

	if (HWS_NEEDS_PHYSICAL(engine->i915))
		ret = init_phys_status_page(engine);
	else
		ret = init_status_page(engine);
	if (ret)
		goto err_rs_fini;

	return 0;

err_rs_fini:
	i915_gem_render_state_fini(engine);
err_breadcrumbs:
	intel_engine_fini_breadcrumbs(engine);
err_unpin_preempt:
	if (INTEL_INFO(engine->i915)->has_logical_ring_preemption)
		engine->context_unpin(engine, engine->i915->preempt_context);
err_unpin_kernel:
	engine->context_unpin(engine, engine->i915->kernel_context);
	return ret;
}

/**
 * intel_engines_cleanup_common - cleans up the engine state created by
 *                                the common initiailizers.
 * @engine: Engine to cleanup.
 *
 * This cleans up everything created by the common helpers.
 */
void intel_engine_cleanup_common(struct intel_engine_cs *engine)
{
	intel_engine_cleanup_scratch(engine);

	if (HWS_NEEDS_PHYSICAL(engine->i915))
		cleanup_phys_status_page(engine);
	else
		cleanup_status_page(engine);

	i915_gem_render_state_fini(engine);
	intel_engine_fini_breadcrumbs(engine);
	intel_engine_cleanup_cmd_parser(engine);
	i915_gem_batch_pool_fini(&engine->batch_pool);

	if (INTEL_INFO(engine->i915)->has_logical_ring_preemption)
		engine->context_unpin(engine, engine->i915->preempt_context);
	engine->context_unpin(engine, engine->i915->kernel_context);
}

u64 intel_engine_get_active_head(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	u64 acthd;

	if (INTEL_GEN(dev_priv) >= 8)
		acthd = I915_READ64_2x32(RING_ACTHD(engine->mmio_base),
					 RING_ACTHD_UDW(engine->mmio_base));
	else if (INTEL_GEN(dev_priv) >= 4)
		acthd = I915_READ(RING_ACTHD(engine->mmio_base));
	else
		acthd = I915_READ(ACTHD);

	return acthd;
}

u64 intel_engine_get_last_batch_head(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	u64 bbaddr;

	if (INTEL_GEN(dev_priv) >= 8)
		bbaddr = I915_READ64_2x32(RING_BBADDR(engine->mmio_base),
					  RING_BBADDR_UDW(engine->mmio_base));
	else
		bbaddr = I915_READ(RING_BBADDR(engine->mmio_base));

	return bbaddr;
}

const char *i915_cache_level_str(struct drm_i915_private *i915, int type)
{
	switch (type) {
	case I915_CACHE_NONE: return " uncached";
	case I915_CACHE_LLC: return HAS_LLC(i915) ? " LLC" : " snooped";
	case I915_CACHE_L3_LLC: return " L3+LLC";
	case I915_CACHE_WT: return " WT";
	default: return "";
	}
}

static inline uint32_t
read_subslice_reg(struct drm_i915_private *dev_priv, int slice,
		  int subslice, i915_reg_t reg)
{
	uint32_t mcr;
	uint32_t ret;
	enum forcewake_domains fw_domains;

	fw_domains = intel_uncore_forcewake_for_reg(dev_priv, reg,
						    FW_REG_READ);
	fw_domains |= intel_uncore_forcewake_for_reg(dev_priv,
						     GEN8_MCR_SELECTOR,
						     FW_REG_READ | FW_REG_WRITE);

	spin_lock_irq(&dev_priv->uncore.lock);
	intel_uncore_forcewake_get__locked(dev_priv, fw_domains);

	mcr = I915_READ_FW(GEN8_MCR_SELECTOR);
	/*
	 * The HW expects the slice and sublice selectors to be reset to 0
	 * after reading out the registers.
	 */
	WARN_ON_ONCE(mcr & (GEN8_MCR_SLICE_MASK | GEN8_MCR_SUBSLICE_MASK));
	mcr &= ~(GEN8_MCR_SLICE_MASK | GEN8_MCR_SUBSLICE_MASK);
	mcr |= GEN8_MCR_SLICE(slice) | GEN8_MCR_SUBSLICE(subslice);
	I915_WRITE_FW(GEN8_MCR_SELECTOR, mcr);

	ret = I915_READ_FW(reg);

	mcr &= ~(GEN8_MCR_SLICE_MASK | GEN8_MCR_SUBSLICE_MASK);
	I915_WRITE_FW(GEN8_MCR_SELECTOR, mcr);

	intel_uncore_forcewake_put__locked(dev_priv, fw_domains);
	spin_unlock_irq(&dev_priv->uncore.lock);

	return ret;
}

/* NB: please notice the memset */
void intel_engine_get_instdone(struct intel_engine_cs *engine,
			       struct intel_instdone *instdone)
{
	struct drm_i915_private *dev_priv = engine->i915;
	u32 mmio_base = engine->mmio_base;
	int slice;
	int subslice;

	memset(instdone, 0, sizeof(*instdone));

	switch (INTEL_GEN(dev_priv)) {
	default:
		instdone->instdone = I915_READ(RING_INSTDONE(mmio_base));

		if (engine->id != RCS)
			break;

		instdone->slice_common = I915_READ(GEN7_SC_INSTDONE);
		for_each_instdone_slice_subslice(dev_priv, slice, subslice) {
			instdone->sampler[slice][subslice] =
				read_subslice_reg(dev_priv, slice, subslice,
						  GEN7_SAMPLER_INSTDONE);
			instdone->row[slice][subslice] =
				read_subslice_reg(dev_priv, slice, subslice,
						  GEN7_ROW_INSTDONE);
		}
		break;
	case 7:
		instdone->instdone = I915_READ(RING_INSTDONE(mmio_base));

		if (engine->id != RCS)
			break;

		instdone->slice_common = I915_READ(GEN7_SC_INSTDONE);
		instdone->sampler[0][0] = I915_READ(GEN7_SAMPLER_INSTDONE);
		instdone->row[0][0] = I915_READ(GEN7_ROW_INSTDONE);

		break;
	case 6:
	case 5:
	case 4:
		instdone->instdone = I915_READ(RING_INSTDONE(mmio_base));

		if (engine->id == RCS)
			/* HACK: Using the wrong struct member */
			instdone->slice_common = I915_READ(GEN4_INSTDONE1);
		break;
	case 3:
	case 2:
		instdone->instdone = I915_READ(GEN2_INSTDONE);
		break;
	}
}

static bool ring_is_idle(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	bool idle = true;

	intel_runtime_pm_get(dev_priv);

	/* First check that no commands are left in the ring */
	if ((I915_READ_HEAD(engine) & HEAD_ADDR) !=
	    (I915_READ_TAIL(engine) & TAIL_ADDR))
		idle = false;

	/* No bit for gen2, so assume the CS parser is idle */
	if (INTEL_GEN(dev_priv) > 2 && !(I915_READ_MODE(engine) & MODE_IDLE))
		idle = false;

	intel_runtime_pm_put(dev_priv);

	return idle;
}

/**
 * intel_engine_is_idle() - Report if the engine has finished process all work
 * @engine: the intel_engine_cs
 *
 * Return true if there are no requests pending, nothing left to be submitted
 * to hardware, and that the engine is idle.
 */
bool intel_engine_is_idle(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;

	/* More white lies, if wedged, hw state is inconsistent */
	if (i915_terminally_wedged(&dev_priv->gpu_error))
		return true;

	/* Any inflight/incomplete requests? */
	if (!i915_seqno_passed(intel_engine_get_seqno(engine),
			       intel_engine_last_submit(engine)))
		return false;

	if (I915_SELFTEST_ONLY(engine->breadcrumbs.mock))
		return true;

	/* Interrupt/tasklet pending? */
	if (test_bit(ENGINE_IRQ_EXECLIST, &engine->irq_posted))
		return false;

	/* Both ports drained, no more ELSP submission? */
	if (port_request(&engine->execlists.port[0]))
		return false;

	/* ELSP is empty, but there are ready requests? */
	if (READ_ONCE(engine->execlists.first))
		return false;

	/* Ring stopped? */
	if (!ring_is_idle(engine))
		return false;

	return true;
}

bool intel_engines_are_idle(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	if (READ_ONCE(dev_priv->gt.active_requests))
		return false;

	/* If the driver is wedged, HW state may be very inconsistent and
	 * report that it is still busy, even though we have stopped using it.
	 */
	if (i915_terminally_wedged(&dev_priv->gpu_error))
		return true;

	for_each_engine(engine, dev_priv, id) {
		if (!intel_engine_is_idle(engine))
			return false;
	}

	return true;
}

void intel_engines_reset_default_submission(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	for_each_engine(engine, i915, id)
		engine->set_default_submission(engine);
}

void intel_engines_mark_idle(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	for_each_engine(engine, i915, id) {
		intel_engine_disarm_breadcrumbs(engine);
		i915_gem_batch_pool_fini(&engine->batch_pool);
		tasklet_kill(&engine->execlists.irq_tasklet);
		engine->execlists.no_priolist = false;
	}
}

bool intel_engine_can_store_dword(struct intel_engine_cs *engine)
{
	switch (INTEL_GEN(engine->i915)) {
	case 2:
		return false; /* uses physical not virtual addresses */
	case 3:
		/* maybe only uses physical not virtual addresses */
		return !(IS_I915G(engine->i915) || IS_I915GM(engine->i915));
	case 6:
		return engine->class != VIDEO_DECODE_CLASS; /* b0rked */
	default:
		return true;
	}
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftests/mock_engine.c"
#endif
