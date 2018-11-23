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

#include <linux/prime_numbers.h>

#include "../i915_selftest.h"
#include "i915_random.h"
#include "igt_flush_test.h"

static int cpu_set(struct drm_i915_gem_object *obj,
		   unsigned long offset,
		   u32 v)
{
	unsigned int needs_clflush;
	struct page *page;
	void *map;
	u32 *cpu;
	int err;

	err = i915_gem_obj_prepare_shmem_write(obj, &needs_clflush);
	if (err)
		return err;

	page = i915_gem_object_get_page(obj, offset >> PAGE_SHIFT);
	map = kmap_atomic(page);
	cpu = map + offset_in_page(offset);

	if (needs_clflush & CLFLUSH_BEFORE)
		drm_clflush_virt_range(cpu, sizeof(*cpu));

	*cpu = v;

	if (needs_clflush & CLFLUSH_AFTER)
		drm_clflush_virt_range(cpu, sizeof(*cpu));

	kunmap_atomic(map);
	i915_gem_obj_finish_shmem_access(obj);

	return 0;
}

static int cpu_get(struct drm_i915_gem_object *obj,
		   unsigned long offset,
		   u32 *v)
{
	unsigned int needs_clflush;
	struct page *page;
	void *map;
	u32 *cpu;
	int err;

	err = i915_gem_obj_prepare_shmem_read(obj, &needs_clflush);
	if (err)
		return err;

	page = i915_gem_object_get_page(obj, offset >> PAGE_SHIFT);
	map = kmap_atomic(page);
	cpu = map + offset_in_page(offset);

	if (needs_clflush & CLFLUSH_BEFORE)
		drm_clflush_virt_range(cpu, sizeof(*cpu));

	*v = *cpu;

	kunmap_atomic(map);
	i915_gem_obj_finish_shmem_access(obj);

	return 0;
}

static int gtt_set(struct drm_i915_gem_object *obj,
		   unsigned long offset,
		   u32 v)
{
	struct i915_vma *vma;
	u32 __iomem *map;
	int err;

	err = i915_gem_object_set_to_gtt_domain(obj, true);
	if (err)
		return err;

	vma = i915_gem_object_ggtt_pin(obj, NULL, 0, 0, PIN_MAPPABLE);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	map = i915_vma_pin_iomap(vma);
	i915_vma_unpin(vma);
	if (IS_ERR(map))
		return PTR_ERR(map);

	iowrite32(v, &map[offset / sizeof(*map)]);
	i915_vma_unpin_iomap(vma);

	return 0;
}

static int gtt_get(struct drm_i915_gem_object *obj,
		   unsigned long offset,
		   u32 *v)
{
	struct i915_vma *vma;
	u32 __iomem *map;
	int err;

	err = i915_gem_object_set_to_gtt_domain(obj, false);
	if (err)
		return err;

	vma = i915_gem_object_ggtt_pin(obj, NULL, 0, 0, PIN_MAPPABLE);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	map = i915_vma_pin_iomap(vma);
	i915_vma_unpin(vma);
	if (IS_ERR(map))
		return PTR_ERR(map);

	*v = ioread32(&map[offset / sizeof(*map)]);
	i915_vma_unpin_iomap(vma);

	return 0;
}

static int wc_set(struct drm_i915_gem_object *obj,
		  unsigned long offset,
		  u32 v)
{
	u32 *map;
	int err;

	err = i915_gem_object_set_to_wc_domain(obj, true);
	if (err)
		return err;

	map = i915_gem_object_pin_map(obj, I915_MAP_WC);
	if (IS_ERR(map))
		return PTR_ERR(map);

	map[offset / sizeof(*map)] = v;
	i915_gem_object_unpin_map(obj);

	return 0;
}

static int wc_get(struct drm_i915_gem_object *obj,
		  unsigned long offset,
		  u32 *v)
{
	u32 *map;
	int err;

	err = i915_gem_object_set_to_wc_domain(obj, false);
	if (err)
		return err;

	map = i915_gem_object_pin_map(obj, I915_MAP_WC);
	if (IS_ERR(map))
		return PTR_ERR(map);

	*v = map[offset / sizeof(*map)];
	i915_gem_object_unpin_map(obj);

	return 0;
}

static int gpu_set(struct drm_i915_gem_object *obj,
		   unsigned long offset,
		   u32 v)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_request *rq;
	struct i915_vma *vma;
	u32 *cs;
	int err;

	err = i915_gem_object_set_to_gtt_domain(obj, true);
	if (err)
		return err;

	vma = i915_gem_object_ggtt_pin(obj, NULL, 0, 0, 0);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	rq = i915_request_alloc(i915->engine[RCS], i915->kernel_context);
	if (IS_ERR(rq)) {
		i915_vma_unpin(vma);
		return PTR_ERR(rq);
	}

	cs = intel_ring_begin(rq, 4);
	if (IS_ERR(cs)) {
		i915_request_add(rq);
		i915_vma_unpin(vma);
		return PTR_ERR(cs);
	}

	if (INTEL_GEN(i915) >= 8) {
		*cs++ = MI_STORE_DWORD_IMM_GEN4 | 1 << 22;
		*cs++ = lower_32_bits(i915_ggtt_offset(vma) + offset);
		*cs++ = upper_32_bits(i915_ggtt_offset(vma) + offset);
		*cs++ = v;
	} else if (INTEL_GEN(i915) >= 4) {
		*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
		*cs++ = 0;
		*cs++ = i915_ggtt_offset(vma) + offset;
		*cs++ = v;
	} else {
		*cs++ = MI_STORE_DWORD_IMM | MI_MEM_VIRTUAL;
		*cs++ = i915_ggtt_offset(vma) + offset;
		*cs++ = v;
		*cs++ = MI_NOOP;
	}
	intel_ring_advance(rq, cs);

	err = i915_vma_move_to_active(vma, rq, EXEC_OBJECT_WRITE);
	i915_vma_unpin(vma);

	i915_request_add(rq);

	return err;
}

static bool always_valid(struct drm_i915_private *i915)
{
	return true;
}

static bool needs_fence_registers(struct drm_i915_private *i915)
{
	return !i915_terminally_wedged(&i915->gpu_error);
}

static bool needs_mi_store_dword(struct drm_i915_private *i915)
{
	if (i915_terminally_wedged(&i915->gpu_error))
		return false;

	return intel_engine_can_store_dword(i915->engine[RCS]);
}

static const struct igt_coherency_mode {
	const char *name;
	int (*set)(struct drm_i915_gem_object *, unsigned long offset, u32 v);
	int (*get)(struct drm_i915_gem_object *, unsigned long offset, u32 *v);
	bool (*valid)(struct drm_i915_private *i915);
} igt_coherency_mode[] = {
	{ "cpu", cpu_set, cpu_get, always_valid },
	{ "gtt", gtt_set, gtt_get, needs_fence_registers },
	{ "wc", wc_set, wc_get, always_valid },
	{ "gpu", gpu_set, NULL, needs_mi_store_dword },
	{ },
};

static int igt_gem_coherency(void *arg)
{
	const unsigned int ncachelines = PAGE_SIZE/64;
	I915_RND_STATE(prng);
	struct drm_i915_private *i915 = arg;
	const struct igt_coherency_mode *read, *write, *over;
	struct drm_i915_gem_object *obj;
	unsigned long count, n;
	u32 *offsets, *values;
	int err = 0;

	/* We repeatedly write, overwrite and read from a sequence of
	 * cachelines in order to try and detect incoherency (unflushed writes
	 * from either the CPU or GPU). Each setter/getter uses our cache
	 * domain API which should prevent incoherency.
	 */

	offsets = kmalloc_array(ncachelines, 2*sizeof(u32), GFP_KERNEL);
	if (!offsets)
		return -ENOMEM;
	for (count = 0; count < ncachelines; count++)
		offsets[count] = count * 64 + 4 * (count % 16);

	values = offsets + ncachelines;

	mutex_lock(&i915->drm.struct_mutex);
	intel_runtime_pm_get(i915);
	for (over = igt_coherency_mode; over->name; over++) {
		if (!over->set)
			continue;

		if (!over->valid(i915))
			continue;

		for (write = igt_coherency_mode; write->name; write++) {
			if (!write->set)
				continue;

			if (!write->valid(i915))
				continue;

			for (read = igt_coherency_mode; read->name; read++) {
				if (!read->get)
					continue;

				if (!read->valid(i915))
					continue;

				for_each_prime_number_from(count, 1, ncachelines) {
					obj = i915_gem_object_create_internal(i915, PAGE_SIZE);
					if (IS_ERR(obj)) {
						err = PTR_ERR(obj);
						goto unlock;
					}

					i915_random_reorder(offsets, ncachelines, &prng);
					for (n = 0; n < count; n++)
						values[n] = prandom_u32_state(&prng);

					for (n = 0; n < count; n++) {
						err = over->set(obj, offsets[n], ~values[n]);
						if (err) {
							pr_err("Failed to set stale value[%ld/%ld] in object using %s, err=%d\n",
							       n, count, over->name, err);
							goto put_object;
						}
					}

					for (n = 0; n < count; n++) {
						err = write->set(obj, offsets[n], values[n]);
						if (err) {
							pr_err("Failed to set value[%ld/%ld] in object using %s, err=%d\n",
							       n, count, write->name, err);
							goto put_object;
						}
					}

					for (n = 0; n < count; n++) {
						u32 found;

						err = read->get(obj, offsets[n], &found);
						if (err) {
							pr_err("Failed to get value[%ld/%ld] in object using %s, err=%d\n",
							       n, count, read->name, err);
							goto put_object;
						}

						if (found != values[n]) {
							pr_err("Value[%ld/%ld] mismatch, (overwrite with %s) wrote [%s] %x read [%s] %x (inverse %x), at offset %x\n",
							       n, count, over->name,
							       write->name, values[n],
							       read->name, found,
							       ~values[n], offsets[n]);
							err = -EINVAL;
							goto put_object;
						}
					}

					__i915_gem_object_release_unless_active(obj);
				}
			}
		}
	}
unlock:
	intel_runtime_pm_put(i915);
	mutex_unlock(&i915->drm.struct_mutex);
	kfree(offsets);
	return err;

put_object:
	__i915_gem_object_release_unless_active(obj);
	goto unlock;
}

#define DW_PER_PAGE (PAGE_SIZE / sizeof(u32))

struct live_test {
	struct drm_i915_private *i915;
	const char *func;
	const char *name;

	unsigned int reset_global;
	unsigned int reset_engine[I915_NUM_ENGINES];
};

static int begin_live_test(struct live_test *t,
			   struct drm_i915_private *i915,
			   const char *func,
			   const char *name)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err;

	t->i915 = i915;
	t->func = func;
	t->name = name;

	err = i915_gem_wait_for_idle(i915,
				     I915_WAIT_LOCKED,
				     MAX_SCHEDULE_TIMEOUT);
	if (err) {
		pr_err("%s(%s): failed to idle before, with err=%d!",
		       func, name, err);
		return err;
	}

	i915->gpu_error.missed_irq_rings = 0;
	t->reset_global = i915_reset_count(&i915->gpu_error);

	for_each_engine(engine, i915, id)
		t->reset_engine[id] =
		i915_reset_engine_count(&i915->gpu_error, engine);

	return 0;
}

static int end_live_test(struct live_test *t)
{
	struct drm_i915_private *i915 = t->i915;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	if (igt_flush_test(i915, I915_WAIT_LOCKED))
		return -EIO;

	if (t->reset_global != i915_reset_count(&i915->gpu_error)) {
		pr_err("%s(%s): GPU was reset %d times!\n",
		       t->func, t->name,
		       i915_reset_count(&i915->gpu_error) - t->reset_global);
		return -EIO;
	}

	for_each_engine(engine, i915, id) {
		if (t->reset_engine[id] ==
		    i915_reset_engine_count(&i915->gpu_error, engine))
			continue;

		pr_err("%s(%s): engine '%s' was reset %d times!\n",
		       t->func, t->name, engine->name,
		       i915_reset_engine_count(&i915->gpu_error, engine) -
		       t->reset_engine[id]);
		return -EIO;
	}

	if (i915->gpu_error.missed_irq_rings) {
		pr_err("%s(%s): Missed interrupts on engines %lx\n",
		       t->func, t->name, i915->gpu_error.missed_irq_rings);
		return -EIO;
	}

	return 0;
}

static int cpu_fill(struct drm_i915_gem_object *obj, u32 value)
{
	const bool has_llc = HAS_LLC(to_i915(obj->base.dev));
	unsigned int n, need_flush;
	int err;

	err = i915_gem_obj_prepare_shmem_write(obj, &need_flush);
	if (err)
		return err;

	for (n = 0; n < obj->base.size >> PAGE_SHIFT; n++) {
		u32 *map;

		map = kmap_atomic(i915_gem_object_get_page(obj, n));
		memset32(map, value, DW_PER_PAGE);
		if (!has_llc)
			drm_clflush_virt_range(map, PAGE_SIZE);
		kunmap_atomic(map);
	}

	i915_gem_obj_finish_shmem_access(obj);
	obj->read_domains = I915_GEM_DOMAIN_GTT | I915_GEM_DOMAIN_CPU;
	obj->write_domain = 0;
	return 0;
}

static struct drm_i915_gem_object *
create_test_object(struct drm_i915_private *i915,
		   unsigned int num_pages,
		   struct drm_file *file,
		   struct list_head *objects)
{
	struct drm_i915_gem_object *obj;
	int err;

	obj = i915_gem_object_create_internal(i915, num_pages << PAGE_SHIFT);
	if (IS_ERR(obj))
		return obj;

	err = i915_gem_object_pin_pages(obj);
	if (err)
		goto err_put;

	err = idr_alloc(&file->object_idr, &obj->base, 1, 0, GFP_KERNEL);
	if (err < 0)
		goto err_unpin;

	obj->base.handle_count++;
	obj->scratch = err;

	err = cpu_fill(obj, STACK_MAGIC);
	if (err)
		goto err_remove;

	err = i915_gem_object_set_to_gtt_domain(obj, false);
	if (err)
		goto err_remove;

	list_add_tail(&obj->st_link, objects);
	return obj;

err_remove:
	idr_remove(&file->object_idr, obj->scratch);
err_unpin:
	i915_gem_object_unpin_pages(obj);
err_put:
	i915_gem_object_put(obj);
	return ERR_PTR(err);
}

static struct i915_vma *
gpu_fill_dw(struct i915_vma *vma, u64 offset, unsigned long count, u32 value)
{
	struct drm_i915_gem_object *obj;
	const int gen = INTEL_GEN(vma->vm->i915);
	unsigned long n, size;
	u32 *cmd;
	int err;

	size = (4 * count + 1) * sizeof(u32);
	size = round_up(size, PAGE_SIZE);
	obj = i915_gem_object_create_internal(vma->vm->i915, size);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	cmd = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(cmd)) {
		err = PTR_ERR(cmd);
		goto err;
	}

	GEM_BUG_ON(offset + (count - 1) * PAGE_SIZE > vma->node.size);
	offset += vma->node.start;

	for (n = 0; n < count; n++) {
		if (gen >= 8) {
			*cmd++ = MI_STORE_DWORD_IMM_GEN4;
			*cmd++ = lower_32_bits(offset);
			*cmd++ = upper_32_bits(offset);
			*cmd++ = value;
		} else if (gen >= 4) {
			*cmd++ = MI_STORE_DWORD_IMM_GEN4 |
				(gen < 6 ? MI_USE_GGTT : 0);
			*cmd++ = 0;
			*cmd++ = offset;
			*cmd++ = value;
		} else {
			*cmd++ = MI_STORE_DWORD_IMM | MI_MEM_VIRTUAL;
			*cmd++ = offset;
			*cmd++ = value;
		}
		offset += PAGE_SIZE;
	}
	*cmd = MI_BATCH_BUFFER_END;
	i915_gem_object_unpin_map(obj);

	err = i915_gem_object_set_to_gtt_domain(obj, false);
	if (err)
		goto err;

	vma = i915_vma_instance(obj, vma->vm, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto err;
	}

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (err)
		goto err;

	return vma;

err:
	i915_gem_object_put(obj);
	return ERR_PTR(err);
}

static int gpu_fill(struct drm_i915_gem_object *obj,
		    struct i915_gem_context *ctx,
		    struct intel_engine_cs *engine,
		    unsigned int dw)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_address_space *vm =
		ctx->ppgtt ? &ctx->ppgtt->vm : &i915->ggtt.vm;
	struct i915_request *rq;
	struct i915_vma *vma;
	struct i915_vma *batch;
	unsigned int flags;
	int err;

	GEM_BUG_ON(obj->base.size > vm->total);
	GEM_BUG_ON(!intel_engine_can_store_dword(engine));

	vma = i915_vma_instance(obj, vm, NULL);
	if (IS_ERR(vma))
		return PTR_ERR(vma);

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (err)
		return err;

	batch = gpu_fill_dw(vma,
			    dw * sizeof(u32),
			    obj->base.size >> PAGE_SHIFT,
			    engine->id << 16 | dw);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		goto err_vma;
	}

	rq = i915_request_alloc(engine, ctx);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		goto err_batch;
	}

	flags = 0;
	if (INTEL_GEN(vm->i915) <= 5)
		flags |= I915_DISPATCH_SECURE;

	err = engine->emit_bb_start(rq,
				    batch->node.start, batch->node.size,
				    flags);
	if (err)
		goto err_request;

	err = i915_vma_move_to_active(batch, rq, 0);
	if (err)
		goto skip_request;

	err = i915_vma_move_to_active(vma, rq, EXEC_OBJECT_WRITE);
	if (err)
		goto skip_request;

	i915_gem_chipset_flush(vm->i915);
	i915_request_add(rq);

	i915_gem_object_set_active_reference(batch->obj);
	i915_vma_unpin(batch);
	i915_vma_close(batch);

	i915_vma_unpin(vma);

	return 0;

skip_request:
	i915_request_skip(rq, err);
err_request:
	i915_request_add(rq);
err_batch:
	i915_vma_unpin(batch);
	i915_vma_put(batch);
err_vma:
	i915_vma_unpin(vma);
	return err;
}

static int coherency_check(struct drm_i915_gem_object *obj,
			   unsigned int idx, unsigned int max)
{
	unsigned long n, npages = obj->base.size >> PAGE_SHIFT;
	unsigned int m, needs_flush;
	unsigned int errors = 0;
	int err;

	err = i915_gem_obj_prepare_shmem_read(obj, &needs_flush);
	if (err)
		return err;

	for (n = 0; n < npages; n++) {
		u32 *map;

		map = kmap_atomic(i915_gem_object_get_page(obj, n));
		if (needs_flush & CLFLUSH_BEFORE)
			drm_clflush_virt_range(map, PAGE_SIZE);

		for (m = 0; m < max; m++) {
			u32 x = map[m];

			if ((x & 0xffff) != m) {
				if (errors++ < 5) {
					pr_err("Invalid value at page %d:%ld/%ld, offset %d: found %x expected %x\n",
					       idx, n, npages, m, x, m);
				}
				err = -EINVAL;
			}
		}

		for (; m < DW_PER_PAGE; m++) {
			u32 x = map[m];

			if (x != STACK_MAGIC) {
				if (errors++ < 5) {
					pr_err("Invalid value at page %d:%ld/%ld, offset %d: found %x expected %x\n",
					       idx, n, npages, m, x, STACK_MAGIC);
				}
				err = -EINVAL;
			}
		}

		kunmap_atomic(map);
		if (errors) {
			pr_err("Found %d errors on page %d:%ld/%ld\n",
			       errors, idx, n, npages);
			break;
		}
	}

	i915_gem_obj_finish_shmem_access(obj);
	return err;
}

struct igt_mi_store_dw {
	struct drm_i915_private *i915;
	struct i915_gem_context *ctx;
	struct drm_file *file;
};

static int igt_mi_store_dw__engine(struct igt_mi_store_dw *igt,
				   struct intel_engine_cs *engine)
{
	unsigned long timeout;
	unsigned long npages;
	struct live_test t;
	int err = 0;

	npages = 0;
	for (timeout = 1;
	     !err && timeout < i915_selftest.timeout_jiffies;
	     timeout = next_prime_number(2 * timeout)) {
		unsigned long end_time = jiffies + timeout;
		struct drm_i915_gem_object *obj = NULL, *on;
		unsigned long ndwords, width, dw, id;
		LIST_HEAD(objects);

		err = begin_live_test(&t, igt->i915, __func__, "");
		if (err)
			break;

		dw = 0;
		width = 0;
		ndwords = 0;
		while (!time_after(jiffies, end_time)) {
			if (!obj) {
				struct i915_address_space *vm =
					igt->ctx->ppgtt ?
					&igt->ctx->ppgtt->vm :
					&igt->i915->ggtt.vm;

				npages = next_prime_number(2 * npages);
				if (npages > vm->total >> PAGE_SHIFT)
					goto done;

				obj = create_test_object(igt->i915,
							 npages,
							 igt->file,
							 &objects);
				if (IS_ERR(obj)) {
					err = PTR_ERR(obj);
					goto free;
				}
			}

			intel_runtime_pm_get(igt->i915);
			err = gpu_fill(obj, igt->ctx, engine, dw);
			intel_runtime_pm_put(igt->i915);
			if (err) {
				pr_err("Failed to fill dword %lu [%lu] with gpu (%s), err=%d\n",
				       ndwords, dw, engine->name, err);
				goto free;
			}

			if (++dw == DW_PER_PAGE) {
				obj = NULL;
				dw = 0;
			}

			ndwords += npages;
			width++;
		}
done:
		pr_info("Submitted %lu/%lu dwords to %s in %lu jiffies\n",
			ndwords, width, engine->name, timeout);

free:
		dw = 0;
		id = 0;
		list_for_each_entry_safe(obj, on, &objects, st_link) {
			unsigned int num_writes =
				min_t(unsigned int, width - dw, DW_PER_PAGE);

			if (err == 0)
				err = coherency_check(obj, id++, num_writes);

			dw += num_writes;

			GEM_BUG_ON(--obj->base.handle_count);
			idr_remove(&igt->file->object_idr, obj->scratch);
			i915_gem_object_unpin_pages(obj);
			i915_gem_object_put(obj);
		}

		if (end_live_test(&t))
			err = -EIO;

		i915_retire_requests(igt->i915);
	}

	return err;
}

static int igt_mi_store_dw__all(struct igt_mi_store_dw *igt)
{
	unsigned long timeout;
	unsigned long npages;
	struct live_test t;
	int err = 0;

	npages = 0;
	for (timeout = 1;
	     !err && timeout < i915_selftest.timeout_jiffies;
	     timeout = next_prime_number(2 * timeout)) {
		unsigned long end_time = jiffies + timeout;
		struct drm_i915_gem_object *obj = NULL, *on;
		struct intel_engine_cs *engine;
		unsigned long ndwords, width, dw, id;
		LIST_HEAD(objects);

		err = begin_live_test(&t, igt->i915, __func__, "");
		if (err)
			break;

		dw = 0;
		width = 0;
		ndwords = 0;
		while (!time_after(jiffies, end_time)) {
			for_each_engine(engine, igt->i915, id) {
				if (!intel_engine_can_store_dword(engine))
					continue;

				if (!obj) {
					struct i915_address_space *vm =
						igt->ctx->ppgtt ?
						&igt->ctx->ppgtt->vm :
						&igt->i915->ggtt.vm;

					npages = next_prime_number(2 * npages);
					if (npages > vm->total >> PAGE_SHIFT)
						goto done;

					obj = create_test_object(igt->i915,
								 npages,
								 igt->file,
								 &objects);
					if (IS_ERR(obj)) {
						err = PTR_ERR(obj);
						goto free;
					}
				}

				intel_runtime_pm_get(igt->i915);
				err = gpu_fill(obj, igt->ctx, engine, dw);
				intel_runtime_pm_put(igt->i915);
				if (err) {
					pr_err("Failed to fill dword %lu [%lu] with gpu (%s), err=%d\n",
					       ndwords, dw, engine->name, err);
					goto free;
				}

				if (++dw == DW_PER_PAGE) {
					obj = NULL;
					dw = 0;
				}

				ndwords += npages;
				width++;
			}
		}
done:
		dw = 0;
		for_each_engine(engine, igt->i915, id)
			dw += intel_engine_can_store_dword(engine);
		pr_info("Submitted %lu/%lu dwords (across %lu engines) in %lu jiffies\n", ndwords, width, dw, timeout);

free:
		dw = 0;
		id = 0;
		list_for_each_entry_safe(obj, on, &objects, st_link) {
			unsigned int num_writes =
				min_t(unsigned int, width - dw, DW_PER_PAGE);

			if (err == 0)
				err = coherency_check(obj, id++, num_writes);

			dw += num_writes;

			GEM_BUG_ON(--obj->base.handle_count);
			idr_remove(&igt->file->object_idr, obj->scratch);
			i915_gem_object_unpin_pages(obj);
			i915_gem_object_put(obj);
		}

		if (end_live_test(&t))
			err = -EIO;

		i915_retire_requests(igt->i915);
	}

	return err;
}

static int igt_mi_store_dw(void *arg)
{
	struct igt_mi_store_dw igt = { .i915 = arg };
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err;

	igt.file = mock_file(igt.i915);
	if (IS_ERR(igt.file))
		return PTR_ERR(igt.file);

	mutex_lock(&igt.i915->drm.struct_mutex);

	igt.ctx = live_context(igt.i915, igt.file);
	if (IS_ERR(igt.ctx)) {
		err = PTR_ERR(igt.ctx);
		goto out_unlock;
	}

	for_each_engine(engine, igt.i915, id) {
		if (!intel_engine_can_store_dword(engine))
			continue;

		err = igt_mi_store_dw__engine(&igt, engine);
		if (err)
			goto out_unlock;
	}

	err = igt_mi_store_dw__all(&igt);

out_unlock:
	mutex_unlock(&igt.i915->drm.struct_mutex);

	mock_file_free(igt.i915, igt.file);
	return err;
}

int i915_gem_coherency_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_gem_coherency),
		SUBTEST(igt_mi_store_dw),
	};

	return i915_subtests(tests, i915);
}
