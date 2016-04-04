/*
 * Copyright © 2008-2015 Intel Corporation
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

#include <linux/oom.h>
#include <linux/shmem_fs.h>
#include <linux/migrate.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <drm/drmP.h>
#include <drm/i915_drm.h>

#include "i915_drv.h"
#include "i915_trace.h"

static bool mutex_is_locked_by(struct mutex *mutex, struct task_struct *task)
{
	if (!mutex_is_locked(mutex))
		return false;

#if defined(CONFIG_SMP) || defined(CONFIG_DEBUG_MUTEXES)
	return mutex->owner == task;
#else
	/* Since UP may be pre-empted, we cannot assume that we own the lock */
	return false;
#endif
}

static int num_vma_bound(struct drm_i915_gem_object *obj)
{
	struct i915_vma *vma;
	int count = 0;

	list_for_each_entry(vma, &obj->vma_list, obj_link) {
		if (drm_mm_node_allocated(&vma->node))
			count++;
		if (vma->pin_count)
			count++;
	}

	return count;
}

static bool swap_available(void)
{
	return get_nr_swap_pages() > 0;
}

static bool can_release_pages(struct drm_i915_gem_object *obj)
{
	/* Only report true if by unbinding the object and putting its pages
	 * we can actually make forward progress towards freeing physical
	 * pages.
	 *
	 * If the pages are pinned for any other reason than being bound
	 * to the GPU, simply unbinding from the GPU is not going to succeed
	 * in releasing our pin count on the pages themselves.
	 */
	if (obj->pages_pin_count != num_vma_bound(obj))
		return false;

	/* We can only return physical pages to the system if we can either
	 * discard the contents (because the user has marked them as being
	 * purgeable) or if we can move their contents out to swap.
	 */
	return swap_available() || obj->madv == I915_MADV_DONTNEED;
}

static bool can_migrate_page(struct drm_i915_gem_object *obj)
{
	/* Avoid the migration of page if being actively used by GPU */
	if (obj->active)
		return false;

	/* Skip the migration for purgeable objects otherwise there
	 * will be a deadlock when shmem will try to lock the page for
	 * truncation, which is already locked by the caller before
	 * migration.
	 */
	if (obj->madv == I915_MADV_DONTNEED)
		return false;

	/* Skip the migration for a pinned object */
	if (obj->pages_pin_count != num_vma_bound(obj))
		return false;

	return true;
}

static int
unsafe_drop_pages(struct drm_i915_gem_object *obj)
{
	struct i915_vma *vma, *next;
	int ret;

	drm_gem_object_reference(&obj->base);
	list_for_each_entry_safe(vma, next, &obj->vma_list, obj_link)
		if (i915_vma_unbind(vma))
			break;

	ret = i915_gem_object_put_pages(obj);
	drm_gem_object_unreference(&obj->base);

	return ret;
}

static int
do_migrate_page(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *dev_priv = to_i915(obj->base.dev);
	int ret = 0;

	if (!can_migrate_page(obj))
		return -EBUSY;

	/* HW access would be required for a GGTT bound object, for which
	 * device has to be kept awake. But a deadlock scenario can arise if
	 * the attempt is made to resume the device, when either a suspend
	 * or a resume operation is already happening concurrently from some
	 * other path and that only also triggers compaction. So only unbind
	 * if the device is currently awake.
	 */
	if (!intel_runtime_pm_get_if_in_use(dev_priv))
		return -EBUSY;

	if (unsafe_drop_pages(obj))
		ret = -EBUSY;

	intel_runtime_pm_put(dev_priv);
	return ret;
}

/**
 * i915_gem_shrink - Shrink buffer object caches
 * @dev_priv: i915 device
 * @target: amount of memory to make available, in pages
 * @flags: control flags for selecting cache types
 *
 * This function is the main interface to the shrinker. It will try to release
 * up to @target pages of main memory backing storage from buffer objects.
 * Selection of the specific caches can be done with @flags. This is e.g. useful
 * when purgeable objects should be removed from caches preferentially.
 *
 * Note that it's not guaranteed that released amount is actually available as
 * free system memory - the pages might still be in-used to due to other reasons
 * (like cpu mmaps) or the mm core has reused them before we could grab them.
 * Therefore code that needs to explicitly shrink buffer objects caches (e.g. to
 * avoid deadlocks in memory reclaim) must fall back to i915_gem_shrink_all().
 *
 * Also note that any kind of pinning (both per-vma address space pins and
 * backing storage pins at the buffer object level) result in the shrinker code
 * having to skip the object.
 *
 * Returns:
 * The number of pages of backing storage actually released.
 */
unsigned long
i915_gem_shrink(struct drm_i915_private *dev_priv,
		unsigned long target, unsigned flags)
{
	const struct {
		struct list_head *list;
		unsigned int bit;
	} phases[] = {
		{ &dev_priv->mm.unbound_list, I915_SHRINK_UNBOUND },
		{ &dev_priv->mm.bound_list, I915_SHRINK_BOUND },
		{ NULL, 0 },
	}, *phase;
	unsigned long count = 0;

	trace_i915_gem_shrink(dev_priv, target, flags);
	i915_gem_retire_requests(dev_priv->dev);

	/*
	 * As we may completely rewrite the (un)bound list whilst unbinding
	 * (due to retiring requests) we have to strictly process only
	 * one element of the list at the time, and recheck the list
	 * on every iteration.
	 *
	 * In particular, we must hold a reference whilst removing the
	 * object as we may end up waiting for and/or retiring the objects.
	 * This might release the final reference (held by the active list)
	 * and result in the object being freed from under us. This is
	 * similar to the precautions the eviction code must take whilst
	 * removing objects.
	 *
	 * Also note that although these lists do not hold a reference to
	 * the object we can safely grab one here: The final object
	 * unreferencing and the bound_list are both protected by the
	 * dev->struct_mutex and so we won't ever be able to observe an
	 * object on the bound_list with a reference count equals 0.
	 */
	for (phase = phases; phase->list; phase++) {
		struct list_head still_in_list;

		if ((flags & phase->bit) == 0)
			continue;

		INIT_LIST_HEAD(&still_in_list);
		while (count < target && !list_empty(phase->list)) {
			struct drm_i915_gem_object *obj;

			obj = list_first_entry(phase->list,
					       typeof(*obj), global_list);
			list_move_tail(&obj->global_list, &still_in_list);

			if (flags & I915_SHRINK_PURGEABLE &&
			    obj->madv != I915_MADV_DONTNEED)
				continue;

			if ((flags & I915_SHRINK_ACTIVE) == 0 && obj->active)
				continue;

			if (!can_release_pages(obj))
				continue;

			if (unsafe_drop_pages(obj) == 0)
				count += obj->base.size >> PAGE_SHIFT;
		}
		list_splice(&still_in_list, phase->list);
	}

	i915_gem_retire_requests(dev_priv->dev);

	return count;
}

/**
 * i915_gem_shrink_all - Shrink buffer object caches completely
 * @dev_priv: i915 device
 *
 * This is a simple wraper around i915_gem_shrink() to aggressively shrink all
 * caches completely. It also first waits for and retires all outstanding
 * requests to also be able to release backing storage for active objects.
 *
 * This should only be used in code to intentionally quiescent the gpu or as a
 * last-ditch effort when memory seems to have run out.
 *
 * Returns:
 * The number of pages of backing storage actually released.
 */
unsigned long i915_gem_shrink_all(struct drm_i915_private *dev_priv)
{
	return i915_gem_shrink(dev_priv, -1UL,
			       I915_SHRINK_BOUND |
			       I915_SHRINK_UNBOUND |
			       I915_SHRINK_ACTIVE);
}

static bool i915_gem_shrinker_lock(struct drm_device *dev, bool *unlock)
{
	if (!mutex_trylock(&dev->struct_mutex)) {
		if (!mutex_is_locked_by(&dev->struct_mutex, current))
			return false;

		if (to_i915(dev)->mm.shrinker_no_lock_stealing)
			return false;

		*unlock = false;
	} else
		*unlock = true;

	return true;
}

static unsigned long
i915_gem_shrinker_count(struct shrinker *shrinker, struct shrink_control *sc)
{
	struct drm_i915_private *dev_priv =
		container_of(shrinker, struct drm_i915_private, mm.shrinker);
	struct drm_device *dev = dev_priv->dev;
	struct drm_i915_gem_object *obj;
	unsigned long count;
	bool unlock;

	if (!i915_gem_shrinker_lock(dev, &unlock))
		return 0;

	count = 0;
	list_for_each_entry(obj, &dev_priv->mm.unbound_list, global_list)
		if (obj->pages_pin_count == 0)
			count += obj->base.size >> PAGE_SHIFT;

	list_for_each_entry(obj, &dev_priv->mm.bound_list, global_list) {
		if (!obj->active && can_release_pages(obj))
			count += obj->base.size >> PAGE_SHIFT;
	}

	if (unlock)
		mutex_unlock(&dev->struct_mutex);

	return count;
}

static unsigned long
i915_gem_shrinker_scan(struct shrinker *shrinker, struct shrink_control *sc)
{
	struct drm_i915_private *dev_priv =
		container_of(shrinker, struct drm_i915_private, mm.shrinker);
	struct drm_device *dev = dev_priv->dev;
	unsigned long freed;
	bool unlock;

	if (!i915_gem_shrinker_lock(dev, &unlock))
		return SHRINK_STOP;

	freed = i915_gem_shrink(dev_priv,
				sc->nr_to_scan,
				I915_SHRINK_BOUND |
				I915_SHRINK_UNBOUND |
				I915_SHRINK_PURGEABLE);
	if (freed < sc->nr_to_scan)
		freed += i915_gem_shrink(dev_priv,
					 sc->nr_to_scan - freed,
					 I915_SHRINK_BOUND |
					 I915_SHRINK_UNBOUND);
	if (unlock)
		mutex_unlock(&dev->struct_mutex);

	return freed;
}

static int
i915_gem_shrinker_oom(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct drm_i915_private *dev_priv =
		container_of(nb, struct drm_i915_private, mm.oom_notifier);
	struct drm_device *dev = dev_priv->dev;
	struct drm_i915_gem_object *obj;
	unsigned long timeout = msecs_to_jiffies(5000) + 1;
	unsigned long pinned, bound, unbound, freed_pages;
	bool was_interruptible;
	bool unlock;

	while (!i915_gem_shrinker_lock(dev, &unlock) && --timeout) {
		schedule_timeout_killable(1);
		if (fatal_signal_pending(current))
			return NOTIFY_DONE;
	}
	if (timeout == 0) {
		pr_err("Unable to purge GPU memory due lock contention.\n");
		return NOTIFY_DONE;
	}

	was_interruptible = dev_priv->mm.interruptible;
	dev_priv->mm.interruptible = false;

	freed_pages = i915_gem_shrink_all(dev_priv);

	dev_priv->mm.interruptible = was_interruptible;

	/* Because we may be allocating inside our own driver, we cannot
	 * assert that there are no objects with pinned pages that are not
	 * being pointed to by hardware.
	 */
	unbound = bound = pinned = 0;
	list_for_each_entry(obj, &dev_priv->mm.unbound_list, global_list) {
		if (!obj->base.filp) /* not backed by a freeable object */
			continue;

		if (obj->pages_pin_count)
			pinned += obj->base.size;
		else
			unbound += obj->base.size;
	}
	list_for_each_entry(obj, &dev_priv->mm.bound_list, global_list) {
		if (!obj->base.filp)
			continue;

		if (obj->pages_pin_count)
			pinned += obj->base.size;
		else
			bound += obj->base.size;
	}

	if (unlock)
		mutex_unlock(&dev->struct_mutex);

	if (freed_pages || unbound || bound)
		pr_info("Purging GPU memory, %lu bytes freed, %lu bytes still pinned.\n",
			freed_pages << PAGE_SHIFT, pinned);
	if (unbound || bound)
		pr_err("%lu and %lu bytes still available in the "
		       "bound and unbound GPU page lists.\n",
		       bound, unbound);

	*(unsigned long *)ptr += freed_pages;
	return NOTIFY_DONE;
}

#ifdef CONFIG_MIGRATION
static int i915_gem_shrinker_migratepage(struct address_space *mapping,
					 struct page *newpage,
					 struct page *page,
					 enum migrate_mode mode,
					 void *dev_priv_data)
{
	struct drm_i915_private *dev_priv = dev_priv_data;
	struct drm_device *dev = dev_priv->dev;
	unsigned long timeout = msecs_to_jiffies(10) + 1;
	bool unlock;
	int ret;

	WARN((page_count(newpage) != 1), "Unexpected ref count for newpage\n");

	/*
	 * Clear the private field of the new target page as it could have a
	 * stale value in the private field. Otherwise later on if this page
	 * itself gets migrated, without getting referred by the Driver
	 * in between, the stale value would cause the i915_migratepage
	 * function to go for a toss as object pointer is derived from it.
	 * This should be safe since at the time of migration, private field
	 * of the new page (which is actually an independent free 4KB page now)
	 * should be like a don't care for the kernel.
	 */
	set_page_private(newpage, 0);

	if (!page_private(page))
		goto migrate;

	/*
	 * Check the page count, if Driver also has a reference then it should
	 * be more than 2, as shmem will have one reference and one reference
	 * would have been taken by the migration path itself. So if reference
	 * is <=2, we can directly invoke the migration function.
	 */
	if (page_count(page) <= 2)
		goto migrate;

	/*
	 * Use trylock here, with a timeout, for struct_mutex as
	 * otherwise there is a possibility of deadlock due to lock
	 * inversion. This path, which tries to migrate a particular
	 * page after locking that page, can race with a path which
	 * truncate/purge pages of the corresponding object (after
	 * acquiring struct_mutex). Since page truncation will also
	 * try to lock the page, a scenario of deadlock can arise.
	 */
	while (!i915_gem_shrinker_lock(dev, &unlock) && --timeout)
		schedule_timeout_killable(1);
	if (timeout == 0) {
		DRM_DEBUG_DRIVER("Unable to acquire device mutex.\n");
		return -EBUSY;
	}

	ret = 0;
	if (!PageSwapCache(page) && page_private(page)) {
		struct drm_i915_gem_object *obj =
			(struct drm_i915_gem_object *)page_private(page);

		ret = do_migrate_page(obj);
	}

	if (unlock)
		mutex_unlock(&dev->struct_mutex);

	if (ret)
		return ret;

	/*
	 * Ideally here we don't expect the page count to be > 2, as driver
	 * would have dropped its reference, but occasionally it has been seen
	 * coming as 3 & 4. This leads to a situation of unexpected page count,
	 * causing migration failure, with -EGAIN error. This then leads to
	 * multiple attempts by the kernel to migrate the same set of pages.
	 * And sometimes the repeated attempts proves detrimental for stability.
	 * Also since we don't know who is the other owner, and for how long its
	 * gonna keep the reference, its better to return -EBUSY.
	 */
	if (page_count(page) > 2)
		return -EBUSY;

migrate:
	ret = migrate_page(mapping, newpage, page, mode);
	if (ret)
		DRM_DEBUG_DRIVER("page=%p migration returned %d\n", page, ret);

	return ret;
}
#endif

/**
 * i915_gem_shrinker_init - Initialize i915 shrinker
 * @dev_priv: i915 device
 *
 * This function registers and sets up the i915 shrinker and OOM handler.
 */
void i915_gem_shrinker_init(struct drm_i915_private *dev_priv)
{
	dev_priv->mm.shrinker.scan_objects = i915_gem_shrinker_scan;
	dev_priv->mm.shrinker.count_objects = i915_gem_shrinker_count;
	dev_priv->mm.shrinker.seeks = DEFAULT_SEEKS;
	WARN_ON(register_shrinker(&dev_priv->mm.shrinker));

	dev_priv->mm.oom_notifier.notifier_call = i915_gem_shrinker_oom;
	WARN_ON(register_oom_notifier(&dev_priv->mm.oom_notifier));

	dev_priv->mm.shmem_info.dev_private_data = dev_priv;
#ifdef CONFIG_MIGRATION
	dev_priv->mm.shmem_info.dev_migratepage = i915_gem_shrinker_migratepage;
#endif
}

/**
 * i915_gem_shrinker_cleanup - Clean up i915 shrinker
 * @dev_priv: i915 device
 *
 * This function unregisters the i915 shrinker and OOM handler.
 */
void i915_gem_shrinker_cleanup(struct drm_i915_private *dev_priv)
{
	WARN_ON(unregister_oom_notifier(&dev_priv->mm.oom_notifier));
	unregister_shrinker(&dev_priv->mm.shrinker);
}
