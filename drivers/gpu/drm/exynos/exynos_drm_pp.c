/*
 * Copyright (C) 2017 Samsung Electronics Co.Ltd
 * Authors:
 *	Marek Szyprowski <m.szyprowski@samsung.com>
 *
 * Exynos DRM Picture Processor (PP) related functions
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include <drm/drmP.h>
#include <drm/drm_mode.h>
#include <uapi/drm/exynos_drm.h>

#include "exynos_drm_drv.h"
#include "exynos_drm_pp.h"

struct drm_pending_exynos_pp_event {
	struct drm_pending_event base;
	struct drm_exynos_pp_event event;
};

/**
 * exynos_drm_pp_create_properties - Initialize Picture Processor extension
 * @dev: DRM device
 */
int exynos_drm_pp_init(struct drm_device *dev)
{
	struct exynos_drm_private *priv = dev->dev_private;
	struct drm_property *prop;

	INIT_LIST_HEAD(&priv->pp_list);

	prop = drm_property_create_object(dev, DRM_MODE_PROP_VENDOR,
			"SRC_FB_ID", DRM_MODE_OBJECT_FB);
	if (!prop)
		return -ENOMEM;
	priv->pp_src_fb = prop;

	prop = drm_property_create_range(dev, DRM_MODE_PROP_VENDOR,
			"SRC_X", 0, UINT_MAX);
	if (!prop)
		return -ENOMEM;
	priv->pp_src_x = prop;

	prop = drm_property_create_range(dev, DRM_MODE_PROP_VENDOR,
			"SRC_Y", 0, UINT_MAX);
	if (!prop)
		return -ENOMEM;
	priv->pp_src_y = prop;

	prop = drm_property_create_range(dev, DRM_MODE_PROP_VENDOR,
			"SRC_W", 0, UINT_MAX);
	if (!prop)
		return -ENOMEM;
	priv->pp_src_w = prop;

	prop = drm_property_create_range(dev, DRM_MODE_PROP_VENDOR,
			"SRC_H", 0, UINT_MAX);
	if (!prop)
		return -ENOMEM;
	priv->pp_src_h = prop;

	prop = drm_property_create_object(dev, DRM_MODE_PROP_VENDOR,
			"DST_FB_ID", DRM_MODE_OBJECT_FB);
	if (!prop)
		return -ENOMEM;
	priv->pp_dst_fb = prop;

	prop = drm_property_create_range(dev, DRM_MODE_PROP_VENDOR,
			"DST_X", 0, UINT_MAX);
	if (!prop)
		return -ENOMEM;
	priv->pp_dst_x = prop;

	prop = drm_property_create_range(dev, DRM_MODE_PROP_VENDOR,
			"DST_Y", 0, UINT_MAX);
	if (!prop)
		return -ENOMEM;
	priv->pp_dst_y = prop;

	prop = drm_property_create_range(dev, DRM_MODE_PROP_VENDOR,
			"DST_W", 0, UINT_MAX);
	if (!prop)
		return -ENOMEM;
	priv->pp_dst_w = prop;

	prop = drm_property_create_range(dev, DRM_MODE_PROP_VENDOR,
			"DST_H", 0, UINT_MAX);
	if (!prop)
		return -ENOMEM;
	priv->pp_dst_h = prop;

	return 0;
}

/**
 * exynos_drm_pp_register - Register a new picture processor hardware module
 * @dev: DRM device
 * @pp: pp module to init
 * @funcs: callbacks for the new pp object
 * @caps: bitmask of pp capabilities (%DRM_EXYNOS_PP_CAP_*)
 * @src_fmts: array of supported source fb formats (%DRM_FORMAT_*)
 * @src_fmt_count: number of elements in @src_fmts
 * @dst_fmts: array of supported destination fb formats (%DRM_FORMAT_*)
 * @dst_fmt_count: number of elements in @dst_fmts
 * @rotation: a set of supported rotation transformations
 * @name: printf style format string, or NULL for the default name
 *
 * Initializes a pp module.
 *
 * Returns:
 * Zero on success, error code on failure.
 */
int exynos_drm_pp_register(struct drm_device *dev, struct exynos_drm_pp *pp,
		    const struct exynos_drm_pp_funcs *funcs, unsigned int caps,
		    const uint32_t *src_fmts, unsigned int src_fmt_count,
		    const uint32_t *dst_fmts, unsigned int dst_fmt_count,
		    unsigned int rotation, const char *name, ...)
{
	static const struct drm_prop_enum_list props[] = {
		{ __builtin_ffs(DRM_ROTATE_0) - 1,   "rotate-0" },
		{ __builtin_ffs(DRM_ROTATE_90) - 1,  "rotate-90" },
		{ __builtin_ffs(DRM_ROTATE_180) - 1, "rotate-180" },
		{ __builtin_ffs(DRM_ROTATE_270) - 1, "rotate-270" },
		{ __builtin_ffs(DRM_REFLECT_X) - 1,  "reflect-x" },
		{ __builtin_ffs(DRM_REFLECT_Y) - 1,  "reflect-y" },
	};
	struct exynos_drm_private *priv = dev->dev_private;
	struct drm_property *prop;
	int ret;

	ret = drm_mode_object_add(dev, &pp->base, DRM_EXYNOS_OBJECT_PP);
	if (ret)
		return ret;

	spin_lock_init(&pp->lock);
	INIT_LIST_HEAD(&pp->todo_list);
	init_waitqueue_head(&pp->done_wq);
	pp->base.properties = &pp->properties;
	pp->dev = dev;
	pp->funcs = funcs;
	pp->capabilities = caps;
	pp->src_format_count = src_fmt_count;
	pp->dst_format_count = dst_fmt_count;

	if (name) {
		va_list ap;

		va_start(ap, name);
		pp->name = kvasprintf(GFP_KERNEL, name, ap);
		va_end(ap);
	} else {
		pp->name = kasprintf(GFP_KERNEL, "pp-%d",
					 priv->num_pp);
	}
	if (!pp->name)
		goto free;

	pp->src_format_types = kmemdup(src_fmts,
				  sizeof(uint32_t) * src_fmt_count, GFP_KERNEL);
	if (!pp->src_format_types)
		goto free;

	pp->dst_format_types = kmemdup(dst_fmts,
				  sizeof(uint32_t) * dst_fmt_count, GFP_KERNEL);
	if (!pp->dst_format_types)
		goto free;

	prop = drm_property_create_bitmask(dev, DRM_MODE_PROP_VENDOR,
					   "rotation", props, ARRAY_SIZE(props),
					   rotation);
	if (!prop)
		goto free;

	pp->rotation_property = prop;

	list_add_tail(&pp->head, &priv->pp_list);

	drm_object_attach_property(&pp->base, priv->pp_src_fb, 0);
	drm_object_attach_property(&pp->base, priv->pp_src_x, 0);
	drm_object_attach_property(&pp->base, priv->pp_src_y, 0);
	drm_object_attach_property(&pp->base, priv->pp_src_w, 0);
	drm_object_attach_property(&pp->base, priv->pp_src_h, 0);
	drm_object_attach_property(&pp->base, priv->pp_dst_fb, 0);
	drm_object_attach_property(&pp->base, priv->pp_dst_x, 0);
	drm_object_attach_property(&pp->base, priv->pp_dst_y, 0);
	drm_object_attach_property(&pp->base, priv->pp_dst_w, 0);
	drm_object_attach_property(&pp->base, priv->pp_dst_h, 0);
	drm_object_attach_property(&pp->base, prop, DRM_ROTATE_0);

	priv->num_pp++;
	DRM_DEBUG_DRIVER("Registered pp %d\n", pp->base.id);

	return 0;

free:
	kfree(pp->dst_format_types);
	kfree(pp->src_format_types);
	kfree(pp->name);
	drm_mode_object_unregister(dev, &pp->base);
	return -ENOMEM;
}

/**
 * exynos_drm_pp_unregister - Unregister the picture processor module
 * @dev: DRM device
 * @pp: pp module
 */
void exynos_drm_pp_unregister(struct drm_device *dev, struct exynos_drm_pp *pp)
{
	BUG_ON(pp->task);
	BUG_ON(!list_empty(&pp->todo_list));

	kfree(pp->dst_format_types);
	kfree(pp->src_format_types);
	kfree(pp->name);
	drm_mode_object_unregister(dev, &pp->base);
}

/**
 * exynos_drm_pp_get_res - enumerate all pp modules
 * @dev: DRM device
 * @data: ioctl data
 * @file_priv: DRM file info
 *
 * Construct a list of pp ids to return to the user.
 *
 * Called by the user via ioctl.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int exynos_drm_pp_get_res(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct exynos_drm_private *priv = dev->dev_private;
	struct drm_exynos_pp_get_res *resp = data;
	struct exynos_drm_pp *pp;
	uint32_t __user *pp_ptr;
	unsigned int count = priv->num_pp, copied = 0;

	/*
	 * This ioctl is called twice, once to determine how much space is
	 * needed, and the 2nd time to fill it.
	 */
	if (count && resp->count_pps >= count) {
		pp_ptr = (uint32_t __user *)
					(unsigned long)resp->pp_id_ptr;

		list_for_each_entry(pp, &priv->pp_list, head) {
			if (put_user(pp->base.id, pp_ptr + copied))
				return -EFAULT;
			copied++;
		}
	}
	resp->count_pps = count;

	return 0;
}

static inline struct exynos_drm_pp *exynos_drm_pp_find(struct drm_device *dev,
						       uint32_t id)
{
	struct exynos_drm_private *priv = dev->dev_private;
	struct exynos_drm_pp *pp;

	list_for_each_entry(pp, &priv->pp_list, head) {
		if (pp->base.id == id)
			return pp;
	}
	return NULL;
}

/**
 * exynos_drm_pp_get - get picture processor module parameters
 * @dev: DRM device
 * @data: ioctl data
 * @file_priv: DRM file info
 *
 * Construct a pp configuration structure to return to the user.
 *
 * Called by the user via ioctl.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int exynos_drm_pp_get(struct drm_device *dev, void *data,
		      struct drm_file *file_priv)
{
	struct drm_exynos_pp_get *resp = data;
	struct exynos_drm_pp *pp;
	uint32_t __user *format_ptr;

	pp = exynos_drm_pp_find(dev, resp->pp_id);
	if (!pp)
		return -ENOENT;

	resp->pp_id = pp->base.id;
	resp->capabilities = pp->capabilities;

	/*
	 * This ioctl is called twice, once to determine how much space is
	 * needed, and the 2nd time to fill it.
	 */
	if (pp->src_format_count &&
	    (resp->src_format_count >= pp->src_format_count)) {
		format_ptr = (uint32_t __user *)
				(unsigned long)resp->src_format_type_ptr;
		if (copy_to_user(format_ptr, pp->src_format_types,
				 sizeof(uint32_t) * pp->src_format_count))
			return -EFAULT;
	}
	if (pp->dst_format_count &&
	    (resp->dst_format_count >= pp->dst_format_count)) {
		format_ptr = (uint32_t __user *)
				(unsigned long)resp->dst_format_type_ptr;
		if (copy_to_user(format_ptr, pp->dst_format_types,
				 sizeof(uint32_t) * pp->dst_format_count))
			return -EFAULT;
	}
	resp->src_format_count = pp->src_format_count;
	resp->dst_format_count = pp->dst_format_count;

	return 0;
}

static inline struct exynos_drm_pp_task *
	exynos_drm_pp_task_alloc(struct exynos_drm_pp *pp)
{
	struct exynos_drm_pp_task *task;

	task = kzalloc(sizeof(*task), GFP_KERNEL);
	if (!task)
		return NULL;

	task->dev = pp->dev;
	task->pp = pp;
	task->src_w = task->dst_w = UINT_MAX;
	task->src_h = task->dst_h = UINT_MAX;
	task->rotation = DRM_ROTATE_0;

	DRM_DEBUG_DRIVER("Allocated task %pK\n", task);

	return task;
}

static void exynos_drm_pp_task_free(struct exynos_drm_pp *pp,
				 struct exynos_drm_pp_task *task)
{
	DRM_DEBUG_DRIVER("Freeing task %pK\n", task);

	task->pp = NULL;

	if (task->src_fb) {
		drm_framebuffer_unreference(task->src_fb);
		task->src_fb = NULL;
	}
	if (task->dst_fb) {
		drm_framebuffer_unreference(task->dst_fb);
		task->dst_fb = NULL;
	}
	if (task->event) {
		drm_event_cancel_free(pp->dev, &task->event->base);
		task->event = NULL;
	}
	kfree(task);
}

static int exynos_drm_pp_task_set_property(struct exynos_drm_pp_task *task,
		struct drm_property *prop, uint64_t prop_value)
{
	struct drm_device *dev = task->dev;
	struct exynos_drm_private *priv = dev->dev_private;
	struct exynos_drm_pp *pp = task->pp;
	struct drm_framebuffer *fb;
	int ret = 0;

	if (prop == priv->pp_src_fb) {
		fb = drm_framebuffer_lookup(dev, prop_value);
		if (task->src_fb)
			drm_framebuffer_unreference(task->src_fb);
		task->src_fb = fb;
	} else if (prop == priv->pp_src_x) {
		task->src_x = prop_value;
	} else if (prop == priv->pp_src_y) {
		task->src_y = prop_value;
	} else if (prop == priv->pp_src_w) {
		task->src_w = prop_value;
	} else if (prop == priv->pp_src_h) {
		task->src_h = prop_value;
	} else if (prop == priv->pp_dst_fb) {
		fb = drm_framebuffer_lookup(dev, prop_value);
		if (task->dst_fb)
			drm_framebuffer_unreference(task->dst_fb);
		task->dst_fb = fb;
	} else if (prop == priv->pp_dst_x) {
		task->dst_x = prop_value;
	} else if (prop == priv->pp_dst_y) {
		task->dst_y = prop_value;
	} else if (prop == priv->pp_dst_w) {
		task->dst_w = prop_value;
	} else if (prop == priv->pp_dst_h) {
		task->dst_h = prop_value;
	} else if (prop == pp->rotation_property) {
		task->rotation = prop_value;
	} else {
		ret = -EINVAL;
	}

	return ret;
}

static struct drm_pending_exynos_pp_event *exynos_drm_pp_event_create(
			struct drm_device *dev, struct drm_file *file_priv,
			uint64_t user_data)
{
	struct drm_pending_exynos_pp_event *e = NULL;
	int ret;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return NULL;

	e->event.base.type = DRM_EXYNOS_PP_EVENT;
	e->event.base.length = sizeof(e->event);
	e->event.user_data = user_data;

	if (file_priv) {
		ret = drm_event_reserve_init(dev, file_priv, &e->base,
					     &e->event.base);
		if (ret) {
			kfree(e);
			return NULL;
		}
	}

	return e;
}

static void exynos_drm_pp_event_send(struct drm_device *dev,
				  struct exynos_drm_pp *pp,
				  struct drm_pending_exynos_pp_event *e)
{
	struct timeval now = ktime_to_timeval(ktime_get());

	e->event.tv_sec = now.tv_sec;
	e->event.tv_usec = now.tv_usec;
	e->event.sequence = atomic_inc_return(&pp->sequence);

	drm_send_event(dev, &e->base);
}

static inline bool drm_fb_check_format(struct drm_framebuffer *fb,
				const uint32_t *formats, int format_counts)
{
	while (format_counts--)
		if (*formats++ == fb->format->format)
			return true;
	return false;
}

static int exynos_drm_pp_task_check(struct exynos_drm_pp_task *task)
{
	struct exynos_drm_pp *pp = task->pp;
	int ret = 0;

	DRM_DEBUG_DRIVER("checking %pK\n", task);

	if (!task->src_fb || !task->dst_fb)
		return -EINVAL;

	if (!drm_fb_check_format(task->src_fb, pp->src_format_types,
				 pp->src_format_count))
		return -EINVAL;

	if (!drm_fb_check_format(task->dst_fb, pp->dst_format_types,
				 pp->dst_format_count))
		return -EINVAL;

	if (task->src_w == UINT_MAX)
		task->src_w = task->src_fb->width << 16;
	if (task->src_h == UINT_MAX)
		task->src_h = task->src_fb->height << 16;
	if (task->dst_w == UINT_MAX)
		task->dst_w = task->dst_fb->width << 16;
	if (task->dst_h == UINT_MAX)
		task->dst_h = task->dst_fb->height << 16;

	if (task->src_x + task->src_w > (task->src_fb->width << 16) ||
	    task->src_y + task->src_h > (task->src_fb->height << 16) ||
	    task->dst_x + task->dst_w > (task->dst_fb->width << 16) ||
	    task->dst_y + task->dst_h > (task->dst_fb->height << 16))
		return -EINVAL;

	if (!(pp->capabilities & DRM_EXYNOS_PP_CAP_CROP) &&
	    (task->src_x || task->src_y || task->dst_x || task->dst_y))
		return -EINVAL;

	if (!(pp->capabilities & DRM_EXYNOS_PP_CAP_ROTATE) &&
	    task->rotation != DRM_ROTATE_0)
		return -EINVAL;

	if (!(pp->capabilities & DRM_EXYNOS_PP_CAP_SCALE) &&
	    (!drm_rotation_90_or_270(task->rotation) &&
	     (task->src_w != task->dst_w || task->src_h != task->dst_h)) &&
	    (drm_rotation_90_or_270(task->rotation) &&
	     (task->src_w != task->dst_h || task->src_h != task->dst_w)))
		return -EINVAL;

	if (!(pp->capabilities & DRM_EXYNOS_PP_CAP_CONVERT) &&
	    task->src_fb->format->format != task->dst_fb->format->format)
		return -EINVAL;

	if (!(pp->capabilities & DRM_EXYNOS_PP_CAP_FB_MODIFIERS) &&
	    ((task->src_fb->flags & DRM_MODE_FB_MODIFIERS) ||
	     (task->dst_fb->flags & DRM_MODE_FB_MODIFIERS)))
		return -EINVAL;

	if (pp->funcs->check)
		ret = pp->funcs->check(pp, task);

	return ret;
}

static int exynos_drm_pp_task_cleanup(struct exynos_drm_pp_task *task)
{
	int ret = task->ret;

	if (ret == 0 && task->event) {
		exynos_drm_pp_event_send(task->dev, task->pp, task->event);
		/* ensure event won't be canceled on task free */
		task->event = NULL;
	}

	exynos_drm_pp_task_free(task->pp, task);
	return ret;
}

static void exynos_drm_pp_cleanup_work(struct work_struct *work)
{
	struct exynos_drm_pp_task *task = container_of(work,
				struct exynos_drm_pp_task, cleanup_work);

	exynos_drm_pp_task_cleanup(task);
}

static void exynos_drm_pp_next_task(struct exynos_drm_pp *pp);

void exynos_drm_pp_task_done(struct exynos_drm_pp_task *task, int ret)
{
	struct exynos_drm_pp *pp = task->pp;
	unsigned long flags;

	DRM_DEBUG_DRIVER("pp: %d, task %pK done\n", pp->base.id,
			 task);

	spin_lock_irqsave(&pp->lock, flags);
	if (pp->task == task)
		pp->task = NULL;
	task->flags |= DRM_EXYNOS_PP_TASK_DONE;
	task->ret = ret;
	spin_unlock_irqrestore(&pp->lock, flags);

	exynos_drm_pp_next_task(pp);
	wake_up(&pp->done_wq);

	if (task->flags & DRM_EXYNOS_PP_TASK_ASYNC) {
		INIT_WORK(&task->cleanup_work, exynos_drm_pp_cleanup_work);
		schedule_work(&task->cleanup_work);
	}
}

static void exynos_drm_pp_next_task(struct exynos_drm_pp *pp)
{
	struct exynos_drm_pp_task *task;
	unsigned long flags;
	int ret;

	DRM_DEBUG_DRIVER("pp: %d, try to run new task\n", pp->base.id);

	spin_lock_irqsave(&pp->lock, flags);

	if (pp->task || list_empty(&pp->todo_list)) {
		spin_unlock_irqrestore(&pp->lock, flags);
		return;
	}

	task = list_first_entry(&pp->todo_list, struct exynos_drm_pp_task,
				head);
	list_del_init(&task->head);
	pp->task = task;

	spin_unlock_irqrestore(&pp->lock, flags);

	DRM_DEBUG_DRIVER("pp: %d, selected task %pK to run\n",
			 pp->base.id, task);

	ret = pp->funcs->commit(pp, task);
	if (ret)
		exynos_drm_pp_task_done(task, ret);
}

static void exynos_drm_pp_schedule_task(struct exynos_drm_pp *pp,
				     struct exynos_drm_pp_task *task)
{
	unsigned long flags;

	spin_lock_irqsave(&pp->lock, flags);
	list_add(&task->head, &pp->todo_list);
	spin_unlock_irqrestore(&pp->lock, flags);

	exynos_drm_pp_next_task(pp);
}

static void exynos_drm_pp_task_abort(struct exynos_drm_pp *pp,
				  struct exynos_drm_pp_task *task)
{
	unsigned long flags;

	spin_lock_irqsave(&pp->lock, flags);
	if (task->flags & DRM_EXYNOS_PP_TASK_DONE) {
		/* already completed task */
		exynos_drm_pp_task_cleanup(task);
	} else if (pp->task != task) {
		/* task has not been scheduled for execution yet */
		list_del_init(&task->head);
		exynos_drm_pp_task_cleanup(task);
	} else {
		/*
		 * currently processed task, call abort() and perform
		 * cleanup with async worker
		 */
		task->flags |= DRM_EXYNOS_PP_TASK_ASYNC;
		if (pp->funcs->abort)
			pp->funcs->abort(pp, task);
	}
	spin_unlock_irqrestore(&pp->lock, flags);
}

/**
 * exynos_drm_pp_ioctl - perform operation on framebuffer processor object
 * @dev: DRM device
 * @data: ioctl data
 * @file_priv: DRM file info
 *
 * Construct a pp task from the set of properties provided from the user
 * and try to schedule it to framebuffer processor hardware.
 *
 * Called by the user via ioctl.
 *
 * Returns:
 * Zero on success, negative errno on failure.
 */
int exynos_drm_pp_commit(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_exynos_pp_commit *arg = data;
	uint32_t __user *props_ptr =
		(uint32_t __user *)(unsigned long)(arg->props_ptr);
	uint64_t __user *prop_values_ptr =
		(uint64_t __user *)(unsigned long)(arg->prop_values_ptr);
	struct exynos_drm_pp *pp;
	struct exynos_drm_pp_task *task;
	int ret = 0;
	unsigned int i;

	if (arg->flags & ~DRM_EXYNOS_PP_FLAGS)
		return -EINVAL;

	if (arg->reserved)
		return -EINVAL;

	/* can't test and expect an event at the same time */
	if ((arg->flags & DRM_EXYNOS_PP_FLAG_TEST_ONLY) &&
			(arg->flags & DRM_EXYNOS_PP_FLAG_EVENT))
		return -EINVAL;

	pp = exynos_drm_pp_find(dev, arg->pp_id);
	if (!pp)
		return -ENOENT;

	task = exynos_drm_pp_task_alloc(pp);
	if (!task) {
		ret = -ENOMEM;
		goto free;
	}

	for (i = 0; i < arg->count_props; i++) {
		uint32_t prop_id;
		uint64_t prop_value;
		struct drm_property *prop;

		if (get_user(prop_id, props_ptr + i)) {
			ret = -EFAULT;
			goto free;
		}

		prop = drm_property_find(dev, prop_id);
		if (!prop) {
			ret = -ENOENT;
			goto free;
		}

		if (copy_from_user(&prop_value, prop_values_ptr + i,
				   sizeof(prop_value))) {
			ret = -EFAULT;
			goto free;
		}

		ret = exynos_drm_pp_task_set_property(task, prop, prop_value);
		if (ret)
			goto free;
	}

	if (arg->flags & DRM_EXYNOS_PP_FLAG_EVENT) {
		struct drm_pending_exynos_pp_event *e;

		e = exynos_drm_pp_event_create(dev, file_priv, arg->user_data);
		if (!e) {
			ret = -ENOMEM;
			goto free;
		}
		task->event = e;
	}

	ret = exynos_drm_pp_task_check(task);
	if (ret || arg->flags & DRM_EXYNOS_PP_FLAG_TEST_ONLY)
		goto free;

	/*
	 * Queue task for processing on the hardware. task object will be
	 * then freed after exynos_drm_pp_task_done()
	 */
	if (arg->flags & DRM_EXYNOS_PP_FLAG_NONBLOCK) {
		DRM_DEBUG_DRIVER("pp: %d, nonblocking processing task %pK\n",
				 task->pp->base.id, task);

		task->flags |= DRM_EXYNOS_PP_TASK_ASYNC;
		exynos_drm_pp_schedule_task(task->pp, task);
		ret = 0;
	} else {
		DRM_DEBUG_DRIVER("pp: %d, processing task %pK\n", pp->base.id,
				 task);
		exynos_drm_pp_schedule_task(pp, task);
		ret = wait_event_interruptible(pp->done_wq,
					task->flags & DRM_EXYNOS_PP_TASK_DONE);
		if (ret)
			exynos_drm_pp_task_abort(pp, task);
		else
			ret = exynos_drm_pp_task_cleanup(task);
	}
	return ret;
free:
	exynos_drm_pp_task_free(pp, task);

	return ret;
}
