/*
 * Copyright (C) 2017 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * DOC: VC4 V3D performance monitor module
 *
 * The V3D block provides 16 hardware counters which can count various events.
 */

#include "vc4_drv.h"
#include "vc4_regs.h"

void vc4_perfmon_get(struct vc4_perfmon *perfmon)
{
	if (perfmon)
		refcount_inc(&perfmon->refcnt);
}

void vc4_perfmon_put(struct vc4_perfmon *perfmon)
{
	if (perfmon && refcount_dec_and_test(&perfmon->refcnt))
		kfree(perfmon);
}

void vc4_perfmon_start(struct vc4_dev *vc4, struct vc4_perfmon *perfmon)
{
	unsigned int i;
	u32 mask;

	if (!perfmon || WARN_ON(vc4->perfmon_active))
		return;

	for (i = 0; i < perfmon->ncounters; i++)
		V3D_WRITE(V3D_PCTRS(i), perfmon->events[i]);

	mask = GENMASK(perfmon->ncounters - 1, 0);
	V3D_WRITE(V3D_PCTRE, V3D_PCTRE_EN | mask);
	V3D_WRITE(V3D_PCTRC, mask);
	vc4->perfmon_active = true;
}

void vc4_perfmon_stop(struct vc4_dev *vc4, struct vc4_perfmon *perfmon,
		      bool capture)
{
	unsigned int i;

	if (!perfmon || WARN_ON(!vc4->perfmon_active))
		return;

	if (capture) {
		for (i = 0; i < perfmon->ncounters; i++)
			perfmon->counters[i] += V3D_READ(V3D_PCTR(i));
	}

	V3D_WRITE(V3D_PCTRE, 0);
	vc4->perfmon_active = false;
}

struct vc4_perfmon *vc4_perfmon_find(struct vc4_file *vc4file, int id)
{
	struct vc4_perfmon *perfmon;

	mutex_lock(&vc4file->perfmon.lock);
	perfmon = idr_find(&vc4file->perfmon.idr, id);
	vc4_perfmon_get(perfmon);
	mutex_unlock(&vc4file->perfmon.lock);

	return perfmon;
}

void vc4_perfmon_open_file(struct vc4_file *vc4file)
{
	mutex_init(&vc4file->perfmon.lock);
	idr_init(&vc4file->perfmon.idr);
}

static int vc4_perfmon_idr_del(int id, void *elem, void *data)
{
	struct vc4_perfmon *perfmon = elem;

	vc4_perfmon_put(perfmon);

	return 0;
}

void vc4_perfmon_close_file(struct vc4_file *vc4file)
{
	mutex_lock(&vc4file->perfmon.lock);
	idr_for_each(&vc4file->perfmon.idr, vc4_perfmon_idr_del, NULL);
	idr_destroy(&vc4file->perfmon.idr);
	mutex_unlock(&vc4file->perfmon.lock);
}

int vc4_perfmon_create_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file_priv)
{
	struct vc4_file *vc4file = file_priv->driver_priv;
	struct drm_vc4_perfmon_create *req = data;
	struct vc4_perfmon *perfmon;
	unsigned int i;
	int ret;

	/* Number of monitored counters cannot exceed HW limits. */
	if (req->ncounters > DRM_VC4_MAX_PERF_COUNTERS ||
	    !req->ncounters)
		return -EINVAL;

	/* Make sure all events are valid. */
	for (i = 0; i < req->ncounters; i++) {
		if (req->events[i] >= VC4_PERFCNT_NUM_EVENTS)
			return -EINVAL;
	}

	perfmon = kzalloc(sizeof(*perfmon) + (req->ncounters * sizeof(u64)),
			  GFP_KERNEL);
	if (!perfmon)
		return -ENOMEM;

	for (i = 0; i < req->ncounters; i++)
		perfmon->events[i] = req->events[i];

	perfmon->ncounters = req->ncounters;

	refcount_set(&perfmon->refcnt, 1);

	mutex_lock(&vc4file->perfmon.lock);
	ret = idr_alloc(&vc4file->perfmon.idr, perfmon, 0, INT_MAX,
			GFP_KERNEL);
	mutex_unlock(&vc4file->perfmon.lock);

	if (ret < 0) {
		kfree(perfmon);
		return ret;
	}

	req->id = ret;
	return 0;
}

int vc4_perfmon_destroy_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct vc4_file *vc4file = file_priv->driver_priv;
	struct drm_vc4_perfmon_destroy *req = data;
	struct vc4_perfmon *perfmon;

	mutex_lock(&vc4file->perfmon.lock);
	perfmon = idr_remove(&vc4file->perfmon.idr, req->id);
	mutex_unlock(&vc4file->perfmon.lock);

	if (!perfmon)
		return -EINVAL;

	vc4_perfmon_put(perfmon);
	return 0;
}

int vc4_perfmon_get_values_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	struct vc4_file *vc4file = file_priv->driver_priv;
	struct drm_vc4_perfmon_get_values *req = data;
	struct vc4_perfmon *perfmon;
	int ret;

	mutex_lock(&vc4file->perfmon.lock);
	perfmon = idr_find(&vc4file->perfmon.idr, req->id);
	vc4_perfmon_get(perfmon);
	mutex_unlock(&vc4file->perfmon.lock);

	if (!perfmon)
		return -EINVAL;

	if (copy_to_user(u64_to_user_ptr(req->values_ptr), perfmon->counters,
			 perfmon->ncounters * sizeof(u64)))
		ret = -EFAULT;
	else
		ret = 0;

	vc4_perfmon_put(perfmon);
	return ret;
}
