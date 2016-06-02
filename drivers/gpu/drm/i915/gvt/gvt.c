/*
 * Copyright(c) 2011-2016 Intel Corporation. All rights reserved.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/types.h>
#include <xen/xen.h>
#include <linux/kthread.h>

#include "gvt.h"

struct intel_gvt_host intel_gvt_host;

static const char * const supported_hypervisors[] = {
	[INTEL_GVT_HYPERVISOR_XEN] = "XEN",
	[INTEL_GVT_HYPERVISOR_KVM] = "KVM",
};

#define MB(x) (x * 1024ULL * 1024ULL)
#define GB(x) (x * MB(1024))

/* Load MPT modules and detect if we're running in host */
static int init_gvt_host(void)
{
	if (intel_gvt_host.initialized)
		return 0;

	/* Xen DOM U */
	if (xen_domain() && !xen_initial_domain())
		return -ENODEV;

	/* Try to load MPT modules for hypervisors */
	if (xen_initial_domain()) {
		/* In Xen dom0 */
		intel_gvt_host.mpt = try_then_request_module(
				symbol_get(xengt_mpt), "xengt");
		intel_gvt_host.hypervisor_type = INTEL_GVT_HYPERVISOR_XEN;
	} else {
		/* not in Xen. Try KVMGT */
		intel_gvt_host.mpt = try_then_request_module(
				symbol_get(kvmgt_mpt), "kvm");
		intel_gvt_host.hypervisor_type = INTEL_GVT_HYPERVISOR_KVM;
	}

	/* Fail to load MPT modules - bail out */
	if (!intel_gvt_host.mpt)
		return -EINVAL;

	/* Try to detect if we're running in host instead of VM. */
	if (!intel_gvt_hypervisor_detect_host())
		return -ENODEV;

	gvt_dbg_core("Running with hypervisor %s in host mode\n",
			supported_hypervisors[intel_gvt_host.hypervisor_type]);

	idr_init(&intel_gvt_host.gvt_idr);
	mutex_init(&intel_gvt_host.gvt_idr_lock);
	intel_gvt_host.initialized = true;
	return 0;
}

static void init_device_info(struct intel_gvt *gvt)
{
	if (IS_BROADWELL(gvt->dev_priv))
		gvt->device_info.max_support_vgpus = 8;
	/* This function will grow large in GVT device model patches. */
}

static void free_gvt_device(struct intel_gvt *gvt)
{
	mutex_lock(&intel_gvt_host.gvt_idr_lock);
	idr_remove(&intel_gvt_host.gvt_idr, gvt->id);
	mutex_unlock(&intel_gvt_host.gvt_idr_lock);

	vfree(gvt);
}

static struct intel_gvt *alloc_gvt_device(struct drm_i915_private *dev_priv)
{
	struct intel_gvt *gvt;
	int ret;

	/*
	 * This data structure will grow large in future, use vzalloc() at
	 * the beginning.
	 */
	gvt = vzalloc(sizeof(*gvt));
	if (!gvt)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&intel_gvt_host.gvt_idr_lock);
	ret = idr_alloc(&intel_gvt_host.gvt_idr, gvt, 0, 0, GFP_KERNEL);
	mutex_unlock(&intel_gvt_host.gvt_idr_lock);

	if (ret < 0)
		goto err;

	gvt->id = ret;
	mutex_init(&gvt->lock);
	gvt->dev_priv = dev_priv;
	idr_init(&gvt->vgpu_idr);

	return gvt;
err:
	free_gvt_device(gvt);
	return ERR_PTR(ret);
}

/**
 * intel_gvt_destroy_device - destroy a GVT device
 * @gvt: intel gvt device
 *
 * This function is called at the driver unloading stage, to destroy a
 * GVT device and free the related resources.
 *
 */
void intel_gvt_destroy_device(struct intel_gvt *gvt)
{
	/* Another de-initialization of GVT components will be introduced. */
	free_gvt_device(gvt);
}

/**
 * intel_gvt_create_device - create a GVT device
 * @dev_priv: drm i915 private data
 *
 * This function is called at the initialization stage, to create a
 * GVT device and initialize necessary GVT components for it.
 *
 * Returns:
 * pointer to the intel gvt device structure, error pointer if failed.
 *
 */
struct intel_gvt *intel_gvt_create_device(struct drm_i915_private *dev_priv)
{
	struct intel_gvt *gvt;
	int ret;

	ret = init_gvt_host();
	if (ret)
		return ERR_PTR(ret);

	gvt_dbg_core("create new gvt device\n");

	gvt = alloc_gvt_device(dev_priv);
	if (IS_ERR(gvt)) {
		ret = PTR_ERR(gvt);
		goto out_err;
	}

	gvt_dbg_core("init gvt device, id %d\n", gvt->id);

	init_device_info(gvt);
	/*
	 * Other initialization of GVT components will be called here.
	 */
	gvt_dbg_core("gvt device creation is done, id %d\n", gvt->id);

	return gvt;

out_err:
	return ERR_PTR(ret);
}
