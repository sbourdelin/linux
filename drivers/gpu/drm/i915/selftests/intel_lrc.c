/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2018 Intel Corporation
 */

#include <linux/prime_numbers.h>

#include "../i915_selftest.h"
#include "igt_flush_test.h"

#include "mock_context.h"

struct spinner {
	struct drm_i915_private *i915;
	struct drm_i915_gem_object *hws;
	struct drm_i915_gem_object *obj;
	u32 *batch;
	void *seqno;
};

static int spinner_init(struct spinner *spin, struct drm_i915_private *i915)
{
	unsigned int mode;
	void *vaddr;
	int err;

	GEM_BUG_ON(INTEL_GEN(i915) < 8);

	memset(spin, 0, sizeof(*spin));
	spin->i915 = i915;

	spin->hws = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(spin->hws)) {
		err = PTR_ERR(spin->hws);
		goto err;
	}

	spin->obj = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(spin->obj)) {
		err = PTR_ERR(spin->obj);
		goto err_hws;
	}

	i915_gem_object_set_cache_level(spin->hws, I915_CACHE_LLC);
	vaddr = i915_gem_object_pin_map(spin->hws, I915_MAP_WB);
	if (IS_ERR(vaddr)) {
		err = PTR_ERR(vaddr);
		goto err_obj;
	}
	spin->seqno = memset(vaddr, 0xff, PAGE_SIZE);

	mode = HAS_LLC(i915) ? I915_MAP_WB : I915_MAP_WC;
	vaddr = i915_gem_object_pin_map(spin->obj, mode);
	if (IS_ERR(vaddr)) {
		err = PTR_ERR(vaddr);
		goto err_unpin_hws;
	}
	spin->batch = vaddr;

	return 0;

err_unpin_hws:
	i915_gem_object_unpin_map(spin->hws);
err_obj:
	i915_gem_object_put(spin->obj);
err_hws:
	i915_gem_object_put(spin->hws);
err:
	return err;
}

static unsigned int seqno_offset(u64 fence)
{
	return offset_in_page(sizeof(u32) * fence);
}

static u64 hws_address(const struct i915_vma *hws,
		       const struct i915_request *rq)
{
	return hws->node.start + seqno_offset(rq->fence.context);
}

static int emit_recurse_batch(struct spinner *spin,
			      struct i915_request *rq,
			      u32 arbitration_command)
{
	struct i915_address_space *vm = &rq->gem_context->ppgtt->vm;
	struct i915_vma *hws, *vma;
	u32 *batch;
	int err;

	vma = i915_vma_instance(spin->obj, vm, NULL);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	hws = i915_vma_instance(spin->hws, vm, NULL);
	if (IS_ERR(hws))
		return PTR_ERR(hws);

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (err)
		return err;

	err = i915_vma_pin(hws, 0, 0, PIN_USER);
	if (err)
		goto unpin_vma;

	err = i915_vma_move_to_active(vma, rq, 0);
	if (err)
		goto unpin_hws;

	if (!i915_gem_object_has_active_reference(vma->obj)) {
		i915_gem_object_get(vma->obj);
		i915_gem_object_set_active_reference(vma->obj);
	}

	err = i915_vma_move_to_active(hws, rq, 0);
	if (err)
		goto unpin_hws;

	if (!i915_gem_object_has_active_reference(hws->obj)) {
		i915_gem_object_get(hws->obj);
		i915_gem_object_set_active_reference(hws->obj);
	}

	batch = spin->batch;

	*batch++ = MI_STORE_DWORD_IMM_GEN4;
	*batch++ = lower_32_bits(hws_address(hws, rq));
	*batch++ = upper_32_bits(hws_address(hws, rq));
	*batch++ = rq->fence.seqno;

	*batch++ = arbitration_command;

	*batch++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
	*batch++ = lower_32_bits(vma->node.start);
	*batch++ = upper_32_bits(vma->node.start);
	*batch++ = MI_BATCH_BUFFER_END; /* not reached */

	i915_gem_chipset_flush(spin->i915);

	err = rq->engine->emit_bb_start(rq, vma->node.start, PAGE_SIZE, 0);

unpin_hws:
	i915_vma_unpin(hws);
unpin_vma:
	i915_vma_unpin(vma);
	return err;
}

static struct i915_request *
spinner_create_request(struct spinner *spin,
		       struct i915_gem_context *ctx,
		       struct intel_engine_cs *engine,
		       u32 arbitration_command)
{
	struct i915_request *rq;
	int err;

	rq = i915_request_alloc(engine, ctx);
	if (IS_ERR(rq))
		return rq;

	err = emit_recurse_batch(spin, rq, arbitration_command);
	if (err) {
		i915_request_add(rq);
		return ERR_PTR(err);
	}

	return rq;
}

static u32 hws_seqno(const struct spinner *spin, const struct i915_request *rq)
{
	u32 *seqno = spin->seqno + seqno_offset(rq->fence.context);

	return READ_ONCE(*seqno);
}

static void spinner_end(struct spinner *spin)
{
	*spin->batch = MI_BATCH_BUFFER_END;
	i915_gem_chipset_flush(spin->i915);
}

static void spinner_fini(struct spinner *spin)
{
	spinner_end(spin);

	i915_gem_object_unpin_map(spin->obj);
	i915_gem_object_put(spin->obj);

	i915_gem_object_unpin_map(spin->hws);
	i915_gem_object_put(spin->hws);
}

static bool wait_for_spinner(struct spinner *spin, struct i915_request *rq)
{
	if (!wait_event_timeout(rq->execute,
				READ_ONCE(rq->global_seqno),
				msecs_to_jiffies(10)))
		return false;

	return !(wait_for_us(i915_seqno_passed(hws_seqno(spin, rq),
					       rq->fence.seqno),
			     10) &&
		 wait_for(i915_seqno_passed(hws_seqno(spin, rq),
					    rq->fence.seqno),
			  1000));
}

static int live_sanitycheck(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_engine_cs *engine;
	struct i915_gem_context *ctx;
	enum intel_engine_id id;
	struct spinner spin;
	int err = -ENOMEM;

	if (!HAS_LOGICAL_RING_CONTEXTS(i915))
		return 0;

	mutex_lock(&i915->drm.struct_mutex);

	if (spinner_init(&spin, i915))
		goto err_unlock;

	ctx = kernel_context(i915);
	if (!ctx)
		goto err_spin;

	for_each_engine(engine, i915, id) {
		struct i915_request *rq;

		rq = spinner_create_request(&spin, ctx, engine, MI_NOOP);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err_ctx;
		}

		i915_request_add(rq);
		if (!wait_for_spinner(&spin, rq)) {
			GEM_TRACE("spinner failed to start\n");
			GEM_TRACE_DUMP();
			i915_gem_set_wedged(i915);
			err = -EIO;
			goto err_ctx;
		}

		spinner_end(&spin);
		if (igt_flush_test(i915, I915_WAIT_LOCKED)) {
			err = -EIO;
			goto err_ctx;
		}
	}

	err = 0;
err_ctx:
	kernel_context_close(ctx);
err_spin:
	spinner_fini(&spin);
err_unlock:
	igt_flush_test(i915, I915_WAIT_LOCKED);
	mutex_unlock(&i915->drm.struct_mutex);
	return err;
}

static int live_preempt(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_gem_context *ctx_hi, *ctx_lo;
	struct spinner spin_hi, spin_lo;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err = -ENOMEM;

	if (!HAS_LOGICAL_RING_PREEMPTION(i915))
		return 0;

	mutex_lock(&i915->drm.struct_mutex);

	if (spinner_init(&spin_hi, i915))
		goto err_unlock;

	if (spinner_init(&spin_lo, i915))
		goto err_spin_hi;

	ctx_hi = kernel_context(i915);
	if (!ctx_hi)
		goto err_spin_lo;
	ctx_hi->sched.priority =
		I915_USER_PRIORITY(I915_CONTEXT_MAX_USER_PRIORITY);

	ctx_lo = kernel_context(i915);
	if (!ctx_lo)
		goto err_ctx_hi;
	ctx_lo->sched.priority =
		I915_USER_PRIORITY(I915_CONTEXT_MIN_USER_PRIORITY);

	for_each_engine(engine, i915, id) {
		struct i915_request *rq;

		rq = spinner_create_request(&spin_lo, ctx_lo, engine,
					    MI_ARB_CHECK);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err_ctx_lo;
		}

		i915_request_add(rq);
		if (!wait_for_spinner(&spin_lo, rq)) {
			GEM_TRACE("lo spinner failed to start\n");
			GEM_TRACE_DUMP();
			i915_gem_set_wedged(i915);
			err = -EIO;
			goto err_ctx_lo;
		}

		rq = spinner_create_request(&spin_hi, ctx_hi, engine,
					    MI_ARB_CHECK);
		if (IS_ERR(rq)) {
			spinner_end(&spin_lo);
			err = PTR_ERR(rq);
			goto err_ctx_lo;
		}

		i915_request_add(rq);
		if (!wait_for_spinner(&spin_hi, rq)) {
			GEM_TRACE("hi spinner failed to start\n");
			GEM_TRACE_DUMP();
			i915_gem_set_wedged(i915);
			err = -EIO;
			goto err_ctx_lo;
		}

		spinner_end(&spin_hi);
		spinner_end(&spin_lo);
		if (igt_flush_test(i915, I915_WAIT_LOCKED)) {
			err = -EIO;
			goto err_ctx_lo;
		}
	}

	err = 0;
err_ctx_lo:
	kernel_context_close(ctx_lo);
err_ctx_hi:
	kernel_context_close(ctx_hi);
err_spin_lo:
	spinner_fini(&spin_lo);
err_spin_hi:
	spinner_fini(&spin_hi);
err_unlock:
	igt_flush_test(i915, I915_WAIT_LOCKED);
	mutex_unlock(&i915->drm.struct_mutex);
	return err;
}

static int live_late_preempt(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_gem_context *ctx_hi, *ctx_lo;
	struct spinner spin_hi, spin_lo;
	struct intel_engine_cs *engine;
	struct i915_sched_attr attr = {};
	enum intel_engine_id id;
	int err = -ENOMEM;

	if (!HAS_LOGICAL_RING_PREEMPTION(i915))
		return 0;

	mutex_lock(&i915->drm.struct_mutex);

	if (spinner_init(&spin_hi, i915))
		goto err_unlock;

	if (spinner_init(&spin_lo, i915))
		goto err_spin_hi;

	ctx_hi = kernel_context(i915);
	if (!ctx_hi)
		goto err_spin_lo;

	ctx_lo = kernel_context(i915);
	if (!ctx_lo)
		goto err_ctx_hi;

	for_each_engine(engine, i915, id) {
		struct i915_request *rq;

		rq = spinner_create_request(&spin_lo, ctx_lo, engine,
					    MI_ARB_CHECK);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			goto err_ctx_lo;
		}

		i915_request_add(rq);
		if (!wait_for_spinner(&spin_lo, rq)) {
			pr_err("First context failed to start\n");
			goto err_wedged;
		}

		rq = spinner_create_request(&spin_hi, ctx_hi, engine, MI_NOOP);
		if (IS_ERR(rq)) {
			spinner_end(&spin_lo);
			err = PTR_ERR(rq);
			goto err_ctx_lo;
		}

		i915_request_add(rq);
		if (wait_for_spinner(&spin_hi, rq)) {
			pr_err("Second context overtook first?\n");
			goto err_wedged;
		}

		attr.priority = I915_USER_PRIORITY(I915_PRIORITY_MAX);
		engine->schedule(rq, &attr);

		if (!wait_for_spinner(&spin_hi, rq)) {
			pr_err("High priority context failed to preempt the low priority context\n");
			GEM_TRACE_DUMP();
			goto err_wedged;
		}

		spinner_end(&spin_hi);
		spinner_end(&spin_lo);
		if (igt_flush_test(i915, I915_WAIT_LOCKED)) {
			err = -EIO;
			goto err_ctx_lo;
		}
	}

	err = 0;
err_ctx_lo:
	kernel_context_close(ctx_lo);
err_ctx_hi:
	kernel_context_close(ctx_hi);
err_spin_lo:
	spinner_fini(&spin_lo);
err_spin_hi:
	spinner_fini(&spin_hi);
err_unlock:
	igt_flush_test(i915, I915_WAIT_LOCKED);
	mutex_unlock(&i915->drm.struct_mutex);
	return err;

err_wedged:
	spinner_end(&spin_hi);
	spinner_end(&spin_lo);
	i915_gem_set_wedged(i915);
	err = -EIO;
	goto err_ctx_lo;
}

struct live_test {
	struct drm_i915_private *i915;
	const char *func;
	const char *name;

	unsigned int reset_count;
	bool wedge;
};

static int begin_live_test(struct live_test *t,
			   struct drm_i915_private *i915,
			   const char *func,
			   const char *name)
{
	t->i915 = i915;
	t->func = func;
	t->name = name;

	if (igt_flush_test(i915, I915_WAIT_LOCKED))
		return -EIO;

	i915->gpu_error.missed_irq_rings = 0;
	t->reset_count = i915_reset_count(&i915->gpu_error);

	return 0;
}

static int end_live_test(struct live_test *t)
{
	struct drm_i915_private *i915 = t->i915;

	if (igt_flush_test(i915, I915_WAIT_LOCKED))
		return -EIO;

	if (t->reset_count != i915_reset_count(&i915->gpu_error)) {
		pr_err("%s(%s): GPU was reset %d times!\n",
		       t->func, t->name,
		       i915_reset_count(&i915->gpu_error) - t->reset_count);
		return -EIO;
	}

	if (i915->gpu_error.missed_irq_rings) {
		pr_err("%s(%s): Missed interrupts on engines %lx\n",
		       t->func, t->name, i915->gpu_error.missed_irq_rings);
		return -EIO;
	}

	return 0;
}

static int nop_virtual_engine(struct drm_i915_private *i915,
			      struct intel_engine_cs **siblings,
			      unsigned int nsibling,
			      unsigned int nctx)
{
	IGT_TIMEOUT(end_time);
	struct i915_request *request[16];
	struct i915_gem_context *ctx[16];
	struct intel_engine_cs *ve[16];
	unsigned long n, prime, nc;
	ktime_t times[2] = {};
	struct live_test t;
	int err;

	GEM_BUG_ON(!nctx || nctx > ARRAY_SIZE(ctx));

	for (n = 0; n < nctx; n++) {
		ctx[n] = kernel_context(i915);
		if (!ctx[n])
			return -ENOMEM;

		ve[n] = intel_execlists_create_virtual(ctx[n],
						       siblings, nsibling);
		if (IS_ERR(ve[n]))
			return PTR_ERR(ve[n]);
	}

	err = begin_live_test(&t, i915, __func__, ve[0]->name);
	if (err)
		goto out;

	for_each_prime_number_from(prime, 1, 8192) {
		times[1] = ktime_get_raw();

		for (nc = 0; nc < nctx; nc++) {
			for (n = 0; n < prime; n++) {
				request[nc] =
					i915_request_alloc(ve[nc], ctx[nc]);
				if (IS_ERR(request[nc])) {
					err = PTR_ERR(request[nc]);
					goto out;
				}

				i915_request_add(request[nc]);
			}
		}

		for (nc = 0; nc < nctx; nc++)
			i915_request_wait(request[nc],
					  I915_WAIT_LOCKED,
					  MAX_SCHEDULE_TIMEOUT);

		times[1] = ktime_sub(ktime_get_raw(), times[1]);
		if (prime == 1)
			times[0] = times[1];

		if (__igt_timeout(end_time, NULL))
			break;
	}

	err = end_live_test(&t);
	if (err)
		goto out;

	pr_info("Requestx%d latencies on %s: 1 = %lluns, %lu = %lluns\n",
		nctx, ve[0]->name, ktime_to_ns(times[0]),
		prime, div64_u64(ktime_to_ns(times[1]), prime));

out:
	if (igt_flush_test(i915, I915_WAIT_LOCKED))
		err = -EIO;

	for (nc = 0; nc < nctx; nc++) {
		intel_virtual_engine_put(ve[nc]);
		kernel_context_close(ctx[nc]);
	}
	return err;
}

static int live_virtual_engine(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_engine_cs *siblings[MAX_ENGINE_INSTANCE + 1];
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	unsigned int class, inst;
	int err = -ENODEV;

	mutex_lock(&i915->drm.struct_mutex);

	for_each_engine(engine, i915, id) {
		err = nop_virtual_engine(i915, &engine, 1, 1);
		if (err) {
			pr_err("Failed to wrap engine %s: err=%d\n",
			       engine->name, err);
			goto out_unlock;
		}
	}

	for (class = 0; class <= MAX_ENGINE_CLASS; class++) {
		int nsibling, n;

		nsibling = 0;
		for (inst = 0; inst <= MAX_ENGINE_INSTANCE; inst++) {
			if (!i915->engine_class[class][inst])
				break;

			siblings[nsibling++] = i915->engine_class[class][inst];
		}
		if (nsibling < 2)
			continue;

		for (n = 1; n <= nsibling + 1; n++) {
			err = nop_virtual_engine(i915, siblings, nsibling, n);
			if (err)
				goto out_unlock;
		}
	}

out_unlock:
	mutex_unlock(&i915->drm.struct_mutex);
	return err;
}

int intel_execlists_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(live_sanitycheck),
		SUBTEST(live_preempt),
		SUBTEST(live_late_preempt),
		SUBTEST(live_virtual_engine),
	};

	if (!HAS_EXECLISTS(i915))
		return 0;

	return i915_subtests(tests, i915);
}
