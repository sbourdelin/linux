/*
 * Copyright (c) 2017 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef _EXYNOS_DRM_FBPROC_H_
#define _EXYNOS_DRM_FBPORC_H_

#include <drm/drmP.h>

struct exynos_drm_pp;
struct exynos_drm_pp_task;

/**
 * struct exynos_drm_pp_funcs - exynos_drm_pp control functions
 */
struct exynos_drm_pp_funcs {
	/**
	 * @check:
	 *
	 * This is the optional hook to validate an pp task. This function
	 * must reject any task which the hardware or driver doesn't support.
	 * This includes but is of course not limited to:
	 *
	 *  - Checking that the framebuffers, scaling and placement
	 *    requirements and so on are within the limits of the hardware.
	 *
	 *  - The driver does not need to repeat basic input validation like
	 *    done in the exynos_drm_pp_check_only() function. The core does
	 *    that before calling this hook.
	 *
	 * RETURNS:
	 *
	 * 0 on success or one of the below negative error codes:
	 *
	 *  - -EINVAL, if any of the above constraints are violated.
	 */
	int (*check)(struct exynos_drm_pp *pp,
		     struct exynos_drm_pp_task *task);

	/**
	 * @commit:
	 *
	 * This is the main entry point to start framebuffer processing
	 * in the hardware. The exynos_drm_pp_task has been already validated.
	 * This function must not wait until the device finishes processing.
	 * When the driver finishes processing, it has to call
	 * exynos_exynos_drm_pp_task_done() function.
	 *
	 * RETURNS:
	 *
	 * 0 on success or negative error codes in case of failure.
	 */
	int (*commit)(struct exynos_drm_pp *pp,
		      struct exynos_drm_pp_task *task);

	/**
	 * @abort:
	 *
	 * Informs the driver that it has to abort the currently running
	 * task as soon as possible (i.e. as soon as it can stop the device
	 * safely), even if the task would not have been finished by then.
	 * After the driver performs the necessary steps, it has to call
	 * exynos_drm_pp_task_done() (as if the task ended normally).
	 * This function does not have to (and will usually not) wait
	 * until the device enters a state when it can be stopped.
	 */
	void (*abort)(struct exynos_drm_pp *pp,
		      struct exynos_drm_pp_task *task);
};

/**
 * struct exynos_drm_pp - central picture processor module structure
 */
struct exynos_drm_pp {
	struct drm_device *dev;
	struct list_head head;

	char *name;
	struct drm_mode_object base;
	const struct exynos_drm_pp_funcs *funcs;
	unsigned int capabilities;
	atomic_t sequence;

	spinlock_t lock;
	struct exynos_drm_pp_task *task;
	struct list_head todo_list;
	wait_queue_head_t done_wq;

	uint32_t *src_format_types;
	unsigned int src_format_count;
	uint32_t *dst_format_types;
	unsigned int dst_format_count;

	struct drm_object_properties properties;

	struct drm_property *rotation_property;
};

/**
 * struct exynos_drm_pp_task - a structure describing transformation that
 * has to be performed by the picture processor hardware module
 */
struct exynos_drm_pp_task {
	struct drm_device *dev;
	struct exynos_drm_pp *pp;
	struct list_head head;

	struct drm_framebuffer *src_fb;

	/* Source values are 16.16 fixed point */
	uint32_t src_x, src_y;
	uint32_t src_h, src_w;

	struct drm_framebuffer *dst_fb;

	/* Destination values are 16.16 fixed point */
	uint32_t dst_x, dst_y;
	uint32_t dst_h, dst_w;

	unsigned int rotation;

	struct work_struct cleanup_work;
	unsigned int flags;
	int ret;

	struct drm_pending_exynos_pp_event *event;
};

#define DRM_EXYNOS_PP_TASK_DONE		(1 << 0)
#define DRM_EXYNOS_PP_TASK_ASYNC	(1 << 1)

int exynos_drm_pp_init(struct drm_device *dev);

int exynos_drm_pp_register(struct drm_device *dev, struct exynos_drm_pp *pp,
		    const struct exynos_drm_pp_funcs *funcs, unsigned int caps,
		    const uint32_t *src_fmts, unsigned int src_fmt_count,
		    const uint32_t *dst_fmts, unsigned int dst_fmt_count,
		    unsigned int rotation, const char *name, ...);
void exynos_drm_pp_unregister(struct drm_device *dev, struct exynos_drm_pp *pp);

void exynos_drm_pp_task_done(struct exynos_drm_pp_task *task, int ret);

int exynos_drm_pp_get_res(struct drm_device *dev, void *data,
			  struct drm_file *file_priv);
int exynos_drm_pp_get(struct drm_device *dev, void *data,
		      struct drm_file *file_priv);
int exynos_drm_pp_commit(struct drm_device *dev,
			 void *data, struct drm_file *file_priv);

#endif
